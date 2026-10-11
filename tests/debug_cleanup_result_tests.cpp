#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef ERROR
#undef ERROR
#endif
#include "debug_session/debug_session.h"
#include "auto_stop_probe/counterpulse_internal.h"
#include "auto_stop_probe/counterpulse_hud.h"
#include "source_context/source_context_internal.h"
#include "runtime/runtime.h"
#include <atomic>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>

// Test link replacement: the display constructor fails before any native UI.
// All Session, executor, source-client and file-writing code remains production.
namespace auto_stop_probe_detail {
class CounterpulseHud::Impl {};
CounterpulseHud::CounterpulseHud(const sampling_detail::SamplingSettings&, bool, bool, UiTheme, bool) {
    throw std::runtime_error("headless fixture display unavailable");
}
CounterpulseHud::~CounterpulseHud() = default;
void CounterpulseHud::observe(const Json&) noexcept { std::abort(); }
void CounterpulseHud::publish(const Json&) noexcept { std::abort(); }
void CounterpulseHud::finish(const Json&) noexcept { std::abort(); }
void CounterpulseHud::set_visible(bool) noexcept { std::abort(); }
void CounterpulseHud::set_theme(UiTheme) noexcept { std::abort(); }
bool CounterpulseHud::begin_session(const sampling_detail::SamplingSettings&, bool, bool) noexcept { std::abort(); }
void CounterpulseHud::end_session() noexcept { std::abort(); }
bool CounterpulseHud::visible() const noexcept { std::abort(); }
bool CounterpulseHud::failed() const noexcept { std::abort(); }
bool CounterpulseHud::task_active() const noexcept { std::abort(); }
std::uint64_t CounterpulseHud::close_sequence() const noexcept { std::abort(); }
bool CounterpulseHud::closed() const noexcept { std::abort(); }
bool CounterpulseHud::stop_requested() const noexcept { std::abort(); }
Json CounterpulseHud::status() const { std::abort(); }
std::shared_ptr<const Json> CounterpulseHud::latest_analysis() const noexcept { std::abort(); }
}

namespace {
using namespace debug_session;
using namespace auto_stop_probe_detail;
using namespace std::chrono_literals;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
class FocusFixture {
public:
    source_context::SourceContextConfig config;
    std::atomic_bool stop{false}, failed{false};
    FocusFixture() {
        config.enabled = true; config.host = "127.0.0.1";
        config.process_name = "fixture.exe"; config.token = std::string(32, 't'); config.ttl_ms = 150;
        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        require(socket_ != INVALID_SOCKET, "fixture socket");
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "fixture bind");
        int length = sizeof(address);
        require(getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &length) == 0, "fixture port");
        config.port = ntohs(address.sin_port);
        worker_ = std::thread([this] {
            try {
                std::uint64_t sequence = 0;
                while (!stop) {
                    fd_set readers; FD_ZERO(&readers); FD_SET(socket_, &readers);
                    timeval timeout{0, 10000};
                    if (select(0, &readers, nullptr, nullptr, &timeout) <= 0) continue;
                    source_context::detail::Packet packet{};
                    sockaddr_in peer{}; int length = sizeof(peer);
                    const auto size = recvfrom(socket_, reinterpret_cast<char*>(packet.data()), static_cast<int>(packet.size()),
                        0, reinterpret_cast<sockaddr*>(&peer), &length);
                    source_context::detail::Message message;
                    require(size == packet.size() && source_context::detail::decode(packet, config.token, message), "fixture decode");
                    message.response = true; message.session = 11; message.sequence = ++sequence;
                    message.focus = source_context::detail::Focus::Foreground;
                    require(source_context::detail::encode(message, config.token, packet), "fixture encode");
                    require(sendto(socket_, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()),
                        0, reinterpret_cast<sockaddr*>(&peer), length) == packet.size(), "fixture send");
                }
            } catch (...) { failed = true; }
        });
    }
    ~FocusFixture() { stop = true; if (worker_.joinable()) worker_.join(); closesocket(socket_); }
private:
    SOCKET socket_ = INVALID_SOCKET;
    std::thread worker_;
};
class RecordingDevice final : public IMouseController {
public:
    int opens = 0, closes = 0, downs = 0, ups = 0, cleanups = 0, moves = 0;
    bool subscribed = false, unknown = false;
    mutable std::atomic_bool admission_entered{false};
    std::atomic_bool hold_admission{false};
    std::function<void()> cancel;
    bool open() noexcept override { ++opens; return true; }
    void close() noexcept override { ++closes; }
    MouseMoveReceipt move(const MouseMoveCommand&) noexcept override { ++moves; return {}; }
    bool poll_input(InputSnapshot& input) noexcept override {
        input = {}; input.status = InputMonitorStatus::READY; input.state_valid = true; input.sequence = 1; return true;
    }
    MouseStatus status() const noexcept override {
        if (hold_admission) {
            admission_entered = true;
            const auto deadline = Clock::now() + 3s;
            while (hold_admission && Clock::now() < deadline)
                std::this_thread::sleep_for(1ms);
        }
        return MouseStatus::READY;
    }
    std::string last_error() const override { return {}; }
    bool output_owner_exclusive() const noexcept override { return true; }
    bool supports_wasd_keyboard() const noexcept override { return true; }
    bool supports_left_button() const noexcept override { return true; }
    bool set_wasd_event_subscription(bool enabled) noexcept override { subscribed = enabled; return true; }
    bool read_wasd_events(WasdEventCursor& cursor, WasdEventBatch& batch) noexcept override {
        batch = {}; batch.subscribed = subscribed; cursor.epoch = 2; return true;
    }
    ButtonReceipt set_left_button(bool down) noexcept override {
        if (down) { ++downs; if (cancel) cancel(); } else ++ups;
        ButtonReceipt receipt; receipt.disposition = ButtonDisposition::ACKNOWLEDGED;
        receipt.datagram_sent = true;
        receipt.backend_completed_at = receipt.protocol_ack_received_at = Clock::now(); return receipt;
    }
    KeyboardReceipt set_wasd_keyboard(std::uint8_t) noexcept override {
        KeyboardReceipt receipt; receipt.disposition = KeyboardDisposition::ACKNOWLEDGED;
        receipt.datagram_sent = true;
        receipt.backend_completed_at = receipt.protocol_ack_received_at = Clock::now(); return receipt;
    }
    KeyboardReceipt cleanup_wasd_keyboard() noexcept override {
        ++cleanups; auto receipt = set_wasd_keyboard(0);
        if (unknown) receipt.disposition = KeyboardDisposition::APPLICATION_UNKNOWN;
        return receipt;
    }
};
void wait_idle(Session& session) {
    const auto deadline = Clock::now() + 8s;
    while (session.busy() && Clock::now() < deadline) std::this_thread::sleep_for(1ms);
    require(!session.busy(), "session timeout"); session.poll();
}
Json read_json(const std::filesystem::path& path) { std::ifstream input(path); return Json::parse(input); }

Context context_for(const std::shared_ptr<RecordingDevice>& device) {
    Context context;
    context.device = device;
    context.runtime_idle = context.input_recording_idle = context.cleanup_known = true;
    context.config.mouse.backend = MouseBackend::KMBOX_NET;
    context.config.mouse.allow_send_input = true;
    return context;
}

void require_no_device_actions(const RecordingDevice& device) {
    require(device.opens == 0 && device.closes == 0 && device.downs == 0 &&
        device.ups == 0 && device.moves == 0 && device.cleanups == 0 && !device.subscribed,
        "准备、离线或输出前失败不得操作借用设备");
}

void test_prepare_cancel_retry_and_shutdown(const std::filesystem::path& root, bool shutdown) {
    auto device = std::make_shared<RecordingDevice>();
    auto context = context_for(device);
    Session session;
    Request request;
    request.show_hud = false;
    request.output_root = (root / (shutdown ? "prepare-shutdown" : "prepare-cancel")).string();
    device->hold_admission = true;
    require(session.dispatch(Action::PREPARE, request, context), "首个准备请求应接收");
    const auto deadline = Clock::now() + 2s;
    while (!device->admission_entered && Clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    require(device->admission_entered, "测试必须进入真实后台准备准入点");
    const auto generation = session.snapshot()->generation;
    require(!session.dispatch(Action::PREPARE, request, context), "准备中重复点击必须拒绝");
    require(session.snapshot()->generation == generation, "拒绝重复点击不能生成另一后台任务");
    if (shutdown) session.request_shutdown();
    else { session.cancel("取消准备 fixture"); session.cancel("重复取消 fixture"); }
    device->hold_admission = false;
    wait_idle(session);
    const auto canceled = session.snapshot();
    require(canceled->state == State::CANCELED && canceled->prepared_id.empty() &&
        !canceled->repeat_ready && !canceled->cleanup_unknown, "后台准备完成不能覆盖取消或制造未知清理");
    if (shutdown) {
        session.request_shutdown();
        require(!session.dispatch(Action::PREPARE, request, context), "关闭后的会话不得重新接收任务");
    } else {
        require(session.dispatch(Action::PREPARE, request, context), "取消后允许重新准备");
        wait_idle(session);
        const auto fresh = session.snapshot();
        require(fresh->state == State::PREPARED && fresh->generation > generation &&
            fresh->report_directory != canceled->report_directory && !fresh->prepared_id.empty(),
            "重试须生成新代次和独立目录");
        require(!session.dispatch(Action::START, request, context, "stale-id"), "旧准备身份不得启动");
        require(!session.dispatch(Action::START, request, context, fresh->prepared_id),
            "准备完成不能替代显式物理输出许可");
        session.cancel();
    }
    require_no_device_actions(*device);
    std::cout << (shutdown ? "prepare-shutdown" : "prepare-cancel-retry") << ": passed\n";
}

void test_offline_failure_and_recovery(const std::filesystem::path& root) {
    auto device = std::make_shared<RecordingDevice>();
    auto context = context_for(device);
    Session session;
    Request request;
    request.show_hud = false;
    request.output_root = (root / "offline-recovery").string();
    request.input_path = (root / "missing-offline-input").string();
    for (const auto mode : {Mode::EVALUATE_MANUAL, Mode::EVALUATE_COMMANDS}) {
        request.mode = mode;
        require(session.dispatch(Action::PREPARE, request, context), "离线准备请求应接收");
        wait_idle(session);
        auto prepared = session.snapshot();
        require(prepared->state == State::PREPARED && !prepared->repeat_ready, "离线准备不能生成物理重复许可");
        require(!session.dispatch(Action::START, request, context, prepared->prepared_id, true, physical_confirmation()),
            "离线任务必须拒绝物理输出许可");
        require(session.dispatch(Action::START, request, context, prepared->prepared_id), "合法离线启动应接收");
        wait_idle(session);
        require(session.snapshot()->state == State::FAILED && !session.snapshot()->cleanup_unknown &&
            !session.snapshot()->repeat_ready, "缺输入应失败，不能伪造设备清理未知");
    }
    request.mode = Mode::DERIVE_DEFAULTS;
    require(session.dispatch(Action::DERIVE_DEFAULTS, request, context), "失败后应允许纯离线默认推导");
    wait_idle(session);
    const auto complete = session.snapshot();
    require(complete->state == State::COMPLETED && complete->result &&
        !complete->cleanup_unknown && !complete->repeat_ready, "离线重试须真正完成并保留禁输出边界");
    const auto index = read_json(std::filesystem::path(complete->report_directory).parent_path() / "result-index.json");
    require(index.at("physical_output") == false && index.at("physical_validation_passed") == false &&
        index.at("success") == true, "离线结果索引不得冒称物理验收");
    require_no_device_actions(*device);
    std::cout << "offline-failure-recovery: passed\n";
}

void test_manual_recording_rejects_unavailable_hud_before_device_use(const std::filesystem::path& root) {
    auto device = std::make_shared<RecordingDevice>();
    auto context = context_for(device);
    Session session;
    Request request;
    request.mode = Mode::MANUAL_RECORDING;
    request.show_hud = false;
    request.output_root = (root / "manual-unavailable-hud").string();
    require(session.dispatch(Action::PREPARE, request, context), "人工记录准备应接收");
    wait_idle(session);
    require(session.snapshot()->state == State::PREPARED, "人工记录仅冻结准备");
    require(session.dispatch(Action::START, request, context, session.snapshot()->prepared_id), "人工记录启动请求应接收");
    wait_idle(session);
    require(session.snapshot()->state == State::FAILED && !session.snapshot()->cleanup_unknown &&
        !session.snapshot()->hud_message.empty(), "HUD不可用须在设备使用前失败且显示原因");
    require_no_device_actions(*device);
    std::cout << "manual-hud-unavailable: passed\n";
}

void test_input_archive_failure_and_replay_recovery(const std::filesystem::path& root) {
    const auto archive = root / "pure-input-archive";
    input_training::Session writer;
    require(writer.start(archive), "合成档案必须由生产writer生成");
    for (std::uint64_t sequence = 1; sequence <= 3; ++sequence) {
        input_training::Event event;
        event.epoch = 1;
        event.sequence = sequence;
        event.received_at_ns = 1'000'000'000 + static_cast<std::int64_t>(sequence) * 1'000'000;
        event.state_valid = true;
        event.left_down = sequence == 2;
        require(writer.submit(event), "生产writer必须接收合成事件");
    }
    writer.stop();
    require(writer.snapshot()->status == input_training::Status::STOPPED,
        "合成档案必须实际封口成功");
    const auto read_manifest = [&] {
        std::ifstream input(archive / "manifest.txt", std::ios::binary);
        require(static_cast<bool>(input), "生产清单应存在");
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    };
    const auto original_manifest = read_manifest();

    Runtime runtime;
    Session session;
    auto device = std::make_shared<RecordingDevice>();
    const auto context = context_for(device);
    require(session.record_inputs(runtime, (root / "unsupported-input-source").string(), context),
        "不支持订阅的记录请求应交给后台显式失败");
    wait_idle(session);
    require(session.snapshot()->state == State::FAILED && !session.snapshot()->cleanup_unknown,
        "订阅未开始不得误报记录成功或未知设备清理");
    require(session.load_inputs(runtime, (root / "missing-input-archive").string()),
        "缺失离线档案请求应接收并报告失败");
    wait_idle(session);
    require(session.snapshot()->state == State::FAILED && !session.snapshot()->cleanup_unknown,
        "离线缺档失败不得锁死为设备清理未知");

    require(session.load_inputs(runtime, archive.string()), "失败后可重试真实writer生成的档案");
    wait_idle(session);
    const auto replay = runtime.snapshot().training;
    require(session.snapshot()->state == State::COMPLETED && replay && replay->replay_source &&
        replay->status == input_training::Status::STOPPED && replay->received_events == 3 &&
        replay->total_holds == 1 && !replay->holds.empty() &&
        !replay->holds.back()->physical_motion_verified,
        "调试会话须贯通Runtime实际离线reader，并保留物理语义未知");
    require(runtime.snapshot().state == RuntimeState::STOPPED && !runtime.snapshot().output_armed,
        "离线回看不得启动检测或武装Runtime");
    require(read_manifest() == original_manifest,
        "离线回看不得改写源档案清单");
    require_no_device_actions(*device);
    std::cout << "input-archive-failure-recovery: passed\n";
}

bool run_case(const std::filesystem::path& root, const char* name, int collision, bool unknown, bool cancel) {
    FocusFixture focus; Session session;
    auto device = std::make_shared<RecordingDevice>(); device->unknown = unknown;
    if (cancel) device->cancel = [&] { session.cancel(); };
    Context context; context.device = device;
    context.runtime_idle = context.input_recording_idle = context.cleanup_known = true;
    context.config.mouse.backend = MouseBackend::KMBOX_NET; context.config.mouse.allow_send_input = true;
    context.config.mouse.kmbox_command_timeout_ms = 50; context.config.source_context = focus.config;
    CounterpulsePlan plan; plan.baseline = "stationary"; plan.capture_enabled = false; plan.shots = 1;
    plan.shot_hold_ms = 10; plan.late_tolerance_ms = 10;
    Request request; request.show_hud = false; request.plan_text = counterpulse_plan_json(plan).dump();
    request.output_root = (root / name).string();
    require(session.dispatch(Action::PREPARE, request, context), "prepare admission"); wait_idle(session);
    auto prepared = session.snapshot(); require(prepared->state == State::PREPARED, "prepare state");
    const auto directory = std::filesystem::path(prepared->report_directory);
    if (collision) std::filesystem::create_directory(directory / (collision == 1 ? "result-index.json.partial" : "result-index.json"));
    require(session.dispatch(Action::START, request, context, prepared->prepared_id, true, physical_confirmation()), "start admission");
    wait_idle(session); const auto result = session.snapshot();
    const auto report = read_json(directory / "run/result.json");
    require(device->downs == 1 && device->cleanups == 1 && device->ups > 0 && !device->subscribed && !focus.failed,
        "actual executor and cleanup did not finish");
    require(device->opens == 0 && device->closes == 0 && device->moves == 0, "borrowed memory device contract");
    require(report.at("cleanup").at("keyboard").at("disposition") == (unknown ? "APPLICATION_UNKNOWN" : "ACKNOWLEDGED"), "cleanup control");
    const auto expected = unknown ? State::CLEANUP_UNKNOWN : cancel ? State::CANCELED : collision ? State::FAILED : State::COMPLETED;
    const bool passed = result->state == expected && result->cleanup_unknown == unknown &&
        (!collision || !std::filesystem::is_regular_file(directory / "result-index.json")) &&
        (!(collision || cancel || unknown) || !result->repeat_ready);
    std::cout << name << ": state=" << state_name(result->state) << " unknown=" << result->cleanup_unknown
        << " expected=" << state_name(expected) << " passed=" << passed << '\n';
    const bool can_prepare = session.dispatch(Action::PREPARE, request, context);
    if (can_prepare) wait_idle(session);
    return passed && can_prepare != unknown;
}
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    WSADATA data{}; if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 2;
    _putenv_s("XEN_SOURCE_CONTEXT_TOKEN", std::string(32, 't').c_str());
    const auto root = std::filesystem::path(argv[1]) / ("cleanup-" + std::to_string(Clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directories(root); bool passed = true;
        test_prepare_cancel_retry_and_shutdown(root, false);
        test_prepare_cancel_retry_and_shutdown(root, true);
        test_offline_failure_and_recovery(root);
        test_manual_recording_rejects_unavailable_hud_before_device_use(root);
        test_input_archive_failure_and_replay_recovery(root);
        passed &= run_case(root, "healthy", 0, false, false);
        passed &= run_case(root, "partial-known", 1, false, false);
        passed &= run_case(root, "target-known", 2, false, false);
        passed &= run_case(root, "cancel-partial", 1, false, true);
        passed &= run_case(root, "unknown-partial", 1, true, false);
        passed &= run_case(root, "unknown-control", 0, true, false);
        std::cout << "retained=" << root << '\n'; WSACleanup(); return passed ? 0 : 1;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; WSACleanup(); return 1; }
}
