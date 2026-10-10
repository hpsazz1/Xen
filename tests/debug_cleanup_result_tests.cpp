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
#include <atomic>
#include <fstream>
#include <iostream>
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
    std::function<void()> cancel;
    bool open() noexcept override { ++opens; return true; }
    void close() noexcept override { ++closes; }
    MouseMoveReceipt move(const MouseMoveCommand&) noexcept override { ++moves; return {}; }
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
        passed &= run_case(root, "healthy", 0, false, false);
        passed &= run_case(root, "partial-known", 1, false, false);
        passed &= run_case(root, "target-known", 2, false, false);
        passed &= run_case(root, "cancel-partial", 1, false, true);
        passed &= run_case(root, "unknown-partial", 1, true, false);
        passed &= run_case(root, "unknown-control", 0, true, false);
        std::cout << "retained=" << root << '\n'; WSACleanup(); return passed ? 0 : 1;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; WSACleanup(); return 1; }
}
