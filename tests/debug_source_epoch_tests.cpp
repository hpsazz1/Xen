#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef ERROR
#undef ERROR
#endif
#include "auto_stop_probe/debug_runner.h"
#include "auto_stop_probe/counterpulse_internal.h"
#include "source_context/source_context_internal.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {
using namespace auto_stop_probe_detail;
using namespace std::chrono_literals;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// 仅绑定本机回环，响应合成鉴权报文；从不查询真实前台或启动源服务端。
class FocusFixture {
public:
    source_context::SourceContextConfig config;
    std::atomic<int> phase{-1};
    std::atomic<bool> stop{false}, failed{false};
    explicit FocusFixture(bool discontinuity) : discontinuity_(discontinuity) {
        config.enabled = true; config.host = "127.0.0.1";
        config.process_name = "fixture.exe"; config.token = std::string(32, 't');
        config.ttl_ms = 150;
        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        require(socket_ != INVALID_SOCKET, "fixture socket");
        sockaddr_in address{}; address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "fixture bind");
        int length = sizeof(address);
        require(getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &length) == 0, "fixture port");
        config.port = ntohs(address.sin_port);
        worker_ = std::thread([this] { serve(); });
    }
    ~FocusFixture() {
        stop = true;
        if (worker_.joinable()) worker_.join();
        if (socket_ != INVALID_SOCKET) closesocket(socket_);
    }
    void cross_boundary() {
        phase = 0;
        const auto deadline = Clock::now() + 2s;
        while (phase.load() < 3 && !failed && Clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        require(!failed && phase.load() >= 3, "真实客户端必须消费前两次响应并请求第三次");
    }
private:
    void serve() noexcept {
        try {
            std::uint64_t sequence = 0;
            while (!stop) {
                fd_set readers; FD_ZERO(&readers); FD_SET(socket_, &readers);
                timeval timeout{0, 10000};
                if (select(0, &readers, nullptr, nullptr, &timeout) <= 0) continue;
                source_context::detail::Packet packet{};
                sockaddr_in peer{}; int peer_length = sizeof(peer);
                const auto size = recvfrom(socket_, reinterpret_cast<char*>(packet.data()), static_cast<int>(packet.size()),
                    0, reinterpret_cast<sockaddr*>(&peer), &peer_length);
                source_context::detail::Message message;
                require(size == packet.size() && source_context::detail::decode(packet, config.token, message), "fixture请求鉴权");
                const int current = phase.load();
                message.response = true; message.session = 11; sequence += 7; message.sequence = sequence;
                message.focus = current == 0 && discontinuity_ ? source_context::detail::Focus::Background : source_context::detail::Focus::Foreground;
                require(source_context::detail::encode(message, config.token, packet), "fixture响应编码");
                require(sendto(socket_, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()),
                    0, reinterpret_cast<sockaddr*>(&peer), peer_length) == packet.size(), "fixture响应发送");
                if (current >= 0 && current < 3) phase = current + 1;
            }
        } catch (...) { failed = true; }
    }
    bool discontinuity_;
    SOCKET socket_ = INVALID_SOCKET;
    std::thread worker_;
};

// 所有按钮/键盘调用只写内存；本测试不创建生产鼠标设备。
class RecordingDevice final : public IMouseController {
public:
    int downs = 0, ups = 0, cleanups = 0;
    bool subscribed = false;
    bool open() noexcept override { return true; }
    void close() noexcept override {}
    MouseMoveReceipt move(const MouseMoveCommand&) noexcept override { return {}; }
    bool poll_input(InputSnapshot& input) noexcept override {
        input = {}; input.status = InputMonitorStatus::READY; input.state_valid = true; input.sequence = 1; return true;
    }
    MouseStatus status() const noexcept override { return MouseStatus::READY; }
    std::string last_error() const override { return {}; }
    bool output_owner_exclusive() const noexcept override { return true; }
    bool supports_wasd_keyboard() const noexcept override { return true; }
    bool supports_left_button() const noexcept override { return true; }
    bool set_wasd_event_subscription(bool enabled) noexcept override { subscribed = enabled; return true; }
    bool read_wasd_events(WasdEventCursor& cursor, WasdEventBatch& batch) noexcept override {
        batch = {}; batch.subscribed = subscribed; cursor.epoch = 2; return true;
    }
    ButtonReceipt set_left_button(bool down) noexcept override {
        if (down) ++downs; else ++ups;
        ButtonReceipt receipt; receipt.disposition = ButtonDisposition::ACKNOWLEDGED;
        receipt.backend_completed_at = receipt.protocol_ack_received_at = Clock::now(); return receipt;
    }
    KeyboardReceipt set_wasd_keyboard(std::uint8_t) noexcept override {
        KeyboardReceipt receipt; receipt.disposition = KeyboardDisposition::ACKNOWLEDGED;
        receipt.backend_completed_at = receipt.protocol_ack_received_at = Clock::now(); return receipt;
    }
    KeyboardReceipt cleanup_wasd_keyboard() noexcept override { ++cleanups; return set_wasd_keyboard(0); }
};

void run_case(const std::filesystem::path& root, bool discontinuity) {
    FocusFixture focus(discontinuity);
    auto device = std::make_shared<RecordingDevice>();
    DebugRunRequest request;
    CounterpulsePlan plan; plan.baseline = "stationary"; plan.capture_enabled = false;
    request.plan = counterpulse_plan_json(plan);
    request.output = root / (discontinuity ? "recovered" : "healthy-gap");
    request.config.source_context = focus.config; request.config.mouse.backend = MouseBackend::KMBOX_NET;
    request.device = device; request.allow_physical_output = true; request.confirmation = "AUTO_STOP_COUNTERPULSE";
    DebugRunCallbacks callbacks;
    callbacks.canceled = [&] { return device->downs > 0; };
    bool barrier_entered = false;
    callbacks.publish = [&](const Json& progress) {
        if (progress.value("stage", "") == "EVENT_SUBSCRIPTION") {
            barrier_entered = true; focus.cross_boundary();
        }
    };
    const auto report = run_debug(request, callbacks);
    std::cout << (discontinuity ? "recovered" : "healthy-gap") << ": downs=" << device->downs
        << " cleanup=" << device->cleanups << " failure=" << report.value("failure", "") << '\n';
    require(barrier_entered && !focus.failed && device->cleanups == 1 && device->ups > 0 && !device->subscribed,
        "实际就绪屏障与最终清理必须完成");
    require(!report.value("success", true), "断点或单次控制取消不能成功");
    require(discontinuity ? device->downs == 0 : device->downs == 1,
        "来源失焦恢复必须阻止首次DOWN；健康跳号仍允许控制动作");
}
}

int main() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 2;
    // 固定测试向量，仅此测试子进程；不读取或写入部署凭据。
    _putenv_s("XEN_SOURCE_CONTEXT_TOKEN", std::string(32, 't').c_str());
    const auto root = std::filesystem::temp_directory_path() /
        ("xen-debug-source-" + std::to_string(Clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directory(root);
        run_case(root, false); run_case(root, true);
        WSACleanup(); return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; WSACleanup(); return 1;
    }
}
