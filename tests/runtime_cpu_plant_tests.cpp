#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <WS2tcpip.h>
#ifdef ERROR
#undef ERROR
#endif
#include "runtime/startup_internal.h"
#include "source_context/source_context_internal.h"
#include "log/log.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class Predicate> void await(Predicate predicate, const char* message) {
    const auto deadline = Clock::now() + 3s;
    while (!predicate()) {
        require(Clock::now() < deadline, message);
        std::this_thread::sleep_for(1ms);
    }
}
struct Plant {
    std::mutex mutex;
    double camera = 0;
    unsigned calls = 0, nonzero = 0;
    int pending = 0;
    bool fail_next_move = false;
    unsigned sequence = 0, closed = 0;
    double last_expected_center = 0;
};
class MemoryMouse final : public IMouseController {
public:
    explicit MemoryMouse(std::shared_ptr<Plant> plant) : plant_(std::move(plant)) {}
    bool open() noexcept override { return true; }
    void close() noexcept override {}
    MouseStatus status() const noexcept override { return MouseStatus::READY; }
    std::string last_error() const override { return {}; }
    bool poll_input(InputSnapshot&) noexcept override { return false; }
    MouseMoveReceipt move(const MouseMoveCommand& command) noexcept override {
        std::lock_guard lock(plant_->mutex);
        ++plant_->calls;
        if (std::exchange(plant_->fail_next_move, false)) return {};
        if (command.dx_counts) ++plant_->nonzero;
        // 独立内存相机，每count产生0.15源像素，下一张图才体现；不调用设备API。
        plant_->camera += command.dx_counts * .15;
        MouseMoveReceipt receipt;
        receipt.succeeded = true;
        receipt.backend_completed_at = Clock::now();
        return receipt;
    }
private:
    std::shared_ptr<Plant> plant_;
};
class SyntheticCapture final : public ICapture {
public:
    explicit SyntheticCapture(std::shared_ptr<Plant> plant) : plant_(std::move(plant)) {}
    bool open() noexcept override { return true; }
    CaptureStatus grab(CapturedFrame& frame) noexcept override {
        std::this_thread::sleep_for(2ms);
        std::lock_guard lock(plant_->mutex);
        if (!plant_->pending) return CaptureStatus::NO_FRAME;
        --plant_->pending;
        try {
            const double center = std::clamp(24.0 - plant_->camera, 4.0, 28.0);
            const auto gray = static_cast<int>(std::lround((center - 4) / 24 * 255));
            plant_->last_expected_center = 4.0 + 24.0 * gray / 255;
            frame = {};
            frame.bgr = cv::Mat(32, 32, CV_8UC3, cv::Scalar(gray, gray, gray));
            frame.width = frame.height = frame.source_width = frame.source_height = 32;
            frame.encoded_width = frame.encoded_height = 32;
            frame.timing.sequence = ++plant_->sequence;
            frame.timing.captured_at = Clock::now();
            return CaptureStatus::FRAME;
        } catch (...) { return CaptureStatus::FAILURE; }
    }
    void close() noexcept override { std::lock_guard lock(plant_->mutex); ++plant_->closed; }
    CaptureStatus status() const noexcept override { return CaptureStatus::READY; }
    std::string last_error() const override { return "合成CPU帧"; }
private:
    std::shared_ptr<Plant> plant_;
};
// 只处理127.0.0.1合成鉴权报文，不查询真实系统焦点。
class FocusFixture {
public:
    source_context::SourceContextConfig config;
    std::atomic<bool> focused{true}, stopping{false}, failed{false};
    std::atomic<unsigned> replies{0};
    FocusFixture() {
        config.enabled = true; config.host = "127.0.0.1";
        config.process_name = "synthetic.exe"; config.token = std::string(32, 't'); config.ttl_ms = 150;
        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        require(socket_ != INVALID_SOCKET, "回环socket失败");
        try {
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "回环绑定失败");
        int length = sizeof(address);
        require(getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &length) == 0, "回环端口失败");
        config.port = ntohs(address.sin_port);
        thread_ = std::thread([this] { serve(); });
        } catch (...) {
            closesocket(socket_);
            throw;
        }
    }
    ~FocusFixture() {
        stopping = true;
        if (thread_.joinable()) thread_.join();
        closesocket(socket_);
    }
    void focus(bool value) {
        focused = value;
        const unsigned before = replies.load();
        // 单请求客户端后两次应答保证至少一个新焦点事实已被客户端消费。
        await([&] { return replies >= before + 3 || failed; }, "源事实未被实际客户端消费");
        require(!failed, "回环源服务失败");
    }
private:
    void serve() noexcept {
        try {
            std::uint64_t sequence = 0;
            while (!stopping) {
                fd_set readers; FD_ZERO(&readers); FD_SET(socket_, &readers);
                timeval timeout{0, 10000};
                if (select(0, &readers, nullptr, nullptr, &timeout) <= 0) continue;
                source_context::detail::Packet packet{};
                sockaddr_in peer{}; int length = sizeof(peer);
                const int size = recvfrom(socket_, reinterpret_cast<char*>(packet.data()),
                    static_cast<int>(packet.size()), 0, reinterpret_cast<sockaddr*>(&peer), &length);
                source_context::detail::Message message;
                require(size == packet.size() && source_context::detail::decode(packet, config.token, message), "请求鉴权失败");
                message.response = true; message.session = 11; message.sequence = ++sequence;
                message.focus = focused ? source_context::detail::Focus::Foreground : source_context::detail::Focus::Background;
                require(source_context::detail::encode(message, config.token, packet), "响应编码失败");
                require(sendto(socket_, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()),
                    0, reinterpret_cast<sockaddr*>(&peer), length) == packet.size(), "回环响应失败");
                ++replies;
            }
        } catch (...) { failed = true; }
    }
    SOCKET socket_ = INVALID_SOCKET;
    std::thread thread_;
};
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data)) return 2;
    LogConfig logging; logging.enable_console = logging.enable_file = logging.enable_ringbuf = false;
    Log::init(logging);
    int result = 0;
    try {
        FocusFixture focus;
        auto plant = std::make_shared<Plant>();
        auto mouse = std::make_shared<MemoryMouse>(plant);
        runtime::detail::StartupAdapter adapter;
        adapter.create_capture = [plant](const CaptureConfig&) { return std::make_unique<SyntheticCapture>(plant); };
        auto runtime = runtime::detail::make_runtime_with_startup_adapter(std::move(adapter));
        AppConfig config;
        config.detector.model_path = argv[1]; config.detector.backend = BackendType::CPU;
        config.detector.output_format = OutputFormat::CHANNEL_FIRST;
        config.capture.roi_width = config.capture.roi_height = 32;
        config.mouse.backend = MouseBackend::WIN32_SEND_INPUT;
        config.mouse.allow_send_input = true; // 只授予内存替身许可，从不构造Win32后端。
        config.source_context = focus.config;
        config.aim.min_confirmed_hits = 1;
        config.aim.enable_prediction = config.aim.enable_delay_compensation = false;
        config.aim.acquisition_range_percent = 100;
        config.aim.counts_per_pixel_x = config.aim.counts_per_pixel_y = 2;
        config.aim.deadzone_pixels = .5f;
        config.runtime.diagnostics_enabled = true;
        config.auto_stop.enabled = config.trigger.enabled = config.recoil.enabled = false;
        config.movement.enabled = config.gsi.enabled = false;
        require(runtime->start(config, mouse), "实际CPU Runtime启动失败");
        require(runtime->snapshot().provider == "CPUExecutionProvider", "严格CPU身份不符");
        focus.focus(true);
        const auto frame = [&] {
            const auto before = runtime->snapshot().processed_frames;
            { std::lock_guard lock(plant->mutex); ++plant->pending; }
            await([&] { return runtime->snapshot().processed_frames > before; }, "实际CPU帧未完成");
            const auto snapshot = runtime->snapshot();
            require(snapshot.last_aim.status == AimStatus::SUCCESS && snapshot.last_aim.has_target &&
                snapshot.last_aim.target.matched_observation_valid, "完整链未得到实测目标");
            std::lock_guard lock(plant->mutex);
            const auto& target = snapshot.last_aim.target;
            require(std::abs((target.matched_observation_x1 + target.matched_observation_x2) / 2 -
                plant->last_expected_center) < .05, "真实Detector结果与像素独立oracle不符");
            std::cout << "帧=" << snapshot.processed_frames << " calls=" << plant->calls
                << " armed=" << snapshot.output_armed << " held=" << snapshot.aim_hold_active
                << " cmd=" << snapshot.last_aim.has_command << " source=" << snapshot.source_context.available
                << " focus=" << snapshot.source_context.focused
                << " epoch=" << snapshot.source_context.control_safety_epoch
                << " x=" << plant->last_expected_center << '\n';
            require(!snapshot.last_profile.mouse_physical_effect_timing_valid &&
                !snapshot.last_profile.mouse_protocol_ack_timing_valid, "内存回执被误报成协议或物理证据");
            return plant->calls;
        };
        require(frame() == 0, "未武装仍派发");
        require(runtime->post_intent({RuntimeIntentType::INPUT_HEALTH_CHANGED, true}) &&
            runtime->post_intent({RuntimeIntentType::ARM_OUTPUT}) &&
            runtime->post_intent({RuntimeIntentType::AIM_HOLD_CHANGED, true}), "内存输出许可设置失败");
        for (int i = 0; i < 12; ++i) frame();
        { std::lock_guard lock(plant->mutex);
          require(plant->nonzero > 0 && plant->camera > 0 && plant->last_expected_center < 24,
            "真实Detector/Aim/派发未驱动非零内存plant");
          std::cout << "非零命令=" << plant->nonzero << " 相机位移=" << plant->camera
                    << " 最新像素中心=" << plant->last_expected_center << '\n'; }
        unsigned before_loss;
        { std::lock_guard lock(plant->mutex); before_loss = plant->calls; }
        focus.focus(false);
        require(frame() == before_loss, "失焦第一帧仍派发");
        require(frame() == before_loss, "失焦后仍派发");
        focus.focus(true);
        require(frame() == before_loss, "同session焦点恢复复用了旧保持键许可");
        const auto release_and_reacquire = [&] {
            unsigned previous;
            { std::lock_guard lock(plant->mutex); previous = plant->calls; }
            require(runtime->post_intent({RuntimeIntentType::AIM_HOLD_CHANGED, false}), "松键意图失败");
            require(frame() == previous, "松键帧仍派发");
            { std::lock_guard lock(plant->mutex); plant->camera = 0; }
            require(runtime->post_intent({RuntimeIntentType::AIM_HOLD_CHANGED, true}), "保持意图失败");
            // 接管平滑及整数化允许首帧无命令；连续新帧须恢复实际非零派发。
            for (int i = 0; i < 4; ++i) frame();
            std::lock_guard lock(plant->mutex);
            require(plant->calls > previous, "新松按后未恢复内存派发");
            return plant->calls;
        };
        const auto reacquired = release_and_reacquire();
        // Pipeline没有消费失焦帧，仍必须看到客户端持久安全代际。
        focus.focus(false); focus.focus(true);
        require(frame() == reacquired, "两帧之间的源断点被健康快照覆盖");
        release_and_reacquire();
        unsigned stopped;
        { std::lock_guard lock(plant->mutex); stopped = plant->calls; }
        runtime->post_intent({RuntimeIntentType::EMERGENCY_STOP});
        require(frame() == stopped, "急停第一帧仍派发");
        require(frame() == stopped && runtime->snapshot().emergency_stopped, "End语义未阻止派发");
        runtime->stop();
        { std::lock_guard lock(plant->mutex); require(plant->closed == 1, "合成采集未关闭"); }
        require(runtime->snapshot().state == RuntimeState::STOPPED, "Runtime未完整停止");
        require(runtime->start(config, mouse), "同实例CPU重新启动失败");
        focus.focus(true);
        require(frame() == stopped && !runtime->snapshot().output_armed, "新会话自动恢复旧武装");
        require(runtime->post_intent({RuntimeIntentType::INPUT_HEALTH_CHANGED, true}) &&
            runtime->post_intent({RuntimeIntentType::ARM_OUTPUT}) &&
            runtime->post_intent({RuntimeIntentType::AIM_HOLD_CHANGED, true}), "重启后显式武装失败");
        { std::lock_guard lock(plant->mutex); plant->camera = 0; plant->fail_next_move = true; }
        for (int i = 0; i < 4; ++i) frame();
        { std::lock_guard lock(plant->mutex);
          require(plant->calls == stopped + 1 && !plant->fail_next_move && plant->camera == 0,
              "失败回执后重试派发或计入内存位移"); }
        require(runtime->snapshot().emergency_stopped && !runtime->snapshot().output_armed,
            "后端失败未锁存急停并撤销武装");
        require(frame() == stopped + 1, "后端失败后下一帧仍派发");
        runtime->stop();
        { std::lock_guard lock(plant->mutex); require(plant->closed == 2, "重启采集未关闭"); }
        require(!focus.failed, "源fixture发生失败");
        std::cout << "完整CPU链、跨帧源断点、保持键、急停、重启及失败回执通过\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    Log::shutdown(); WSACleanup();
    return result;
}
