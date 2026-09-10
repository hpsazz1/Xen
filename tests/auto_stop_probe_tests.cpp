#include <iostream>
#include "auto_stop_probe/probe_internal.h"
#include "auto_stop_probe/session_internal.h"
#include "auto_stop_probe/mask_internal.h"

namespace {
using namespace auto_stop_probe_detail;
class Fake final : public IMouseController {
public:
    bool open() noexcept override { return true; }
    MouseMoveReceipt move(const MouseMoveCommand&) noexcept override { ++forbidden; return {}; }
    bool poll_input(InputSnapshot& value) noexcept override {
        ++polls;
        value = {}; value.state_valid = true; value.status = InputMonitorStatus::READY;
        if (!statuses.empty()) value.status = statuses[std::min<std::size_t>(polls - 1, statuses.size() - 1)];
        const bool end = cancel || commands.size() >= cancel_after;
        if (end) value.status = InputMonitorStatus::READY;
        value.state_valid = value.status == InputMonitorStatus::READY;
        value.virtual_keys[0x23] = end;
        return !poll_failure;
    }
    KeyboardReceipt set_wasd_keyboard(std::uint8_t mask) noexcept override {
        commands.push_back(mask);
        KeyboardReceipt result;
        result.disposition = commands.size() == fail_at ? KeyboardDisposition::APPLICATION_UNKNOWN : KeyboardDisposition::ACKNOWLEDGED;
        result.datagram_sent = true;
        result.backend_completed_at = Clock::now();
        return result;
    }
    KeyboardReceipt cleanup_wasd_keyboard() noexcept override {
        ++cleanup_count; KeyboardReceipt result; result.disposition = KeyboardDisposition::ACKNOWLEDGED; return result;
    }
    void close() noexcept override { ++close_count; }
    MouseStatus status() const noexcept override { return MouseStatus::READY; }
    std::string last_error() const override { return {}; }
    std::vector<int> commands;
    std::vector<InputMonitorStatus> statuses;
    std::size_t fail_at = 999;
    std::size_t cancel_after = 999;
    bool cancel = false, poll_failure = false;
    int polls = 0;
    int cleanup_count = 0, close_count = 0, forbidden = 0;
};
void require(bool condition) { if (!condition) throw std::runtime_error("急停实测工具回归失败"); }
class MaskFake final : public IMouseController {
public:
    bool open() noexcept override { return true; }
    MouseMoveReceipt move(const MouseMoveCommand&) noexcept override { ++forbidden; return {}; }
    KeyboardReceipt set_wasd_keyboard(std::uint8_t) noexcept override { ++forbidden; return {}; }
    bool poll_input(InputSnapshot& input) noexcept override {
        ++polls;
        input = {}; input.state_valid = true; input.status = InputMonitorStatus::READY;
        if (!masked || !freeze_sequence) ++sequence;
        input.sequence = sequence;
        input.virtual_keys['W'] = (start_held || polls >= 3) && !(masked && release_while_masked);
        input.virtual_keys[0x11] = masked;
        return true;
    }
    bool set_wasd_event_subscription(bool) noexcept override { return true; }
    bool read_wasd_events(WasdEventCursor&, WasdEventBatch& batch) noexcept override {
        batch = {}; batch.subscribed = true; batch.count = 1;
        batch.events[0] = {static_cast<std::uint8_t>(polls >= 3 ? 1 : 0), true, 1, sequence, ns(Clock::now())}; return true;
    }
    KeyboardReceipt set_wasd_mask(std::uint8_t key, bool value) noexcept override {
        if (key != 1) ++forbidden;
        masks.push_back(value); masked = value;
        KeyboardReceipt result;
        result.disposition = fail_mask ? KeyboardDisposition::APPLICATION_UNKNOWN : KeyboardDisposition::ACKNOWLEDGED;
        result.datagram_sent = true; return result;
    }
    KeyboardReceipt cleanup_wasd_keyboard() noexcept override {
        ++cleanup_count; masked = false; KeyboardReceipt r; r.disposition = KeyboardDisposition::ACKNOWLEDGED; return r;
    }
    void close() noexcept override { ++close_count; }
    MouseStatus status() const noexcept override { return MouseStatus::READY; }
    std::string last_error() const override { return {}; }
    std::vector<bool> masks;
    std::uint64_t sequence = 0;
    int polls = 0, forbidden = 0, cleanup_count = 0, close_count = 0;
    bool masked = false, release_while_masked = false, freeze_sequence = false, fail_mask = false, start_held = false;
};
}
int main() {
    try {
        require(parse_monitor_ready_timeout("0") == 0 && parse_monitor_ready_timeout("1") == 1 && parse_monitor_ready_timeout("30000") == 30000);
        require(parse_session_seconds("1") == 1 && parse_session_seconds("600") == 600);
        for (const auto* invalid : {"0", "601", "-1"}) {
            bool rejected = false;
            try { parse_session_seconds(invalid); } catch (...) { rejected = true; }
            require(rejected);
        }
        for (const auto* invalid : {"30001", "-1", "1ms", "", "999999999999999999999"}) {
            bool rejected = false;
            try { parse_monitor_ready_timeout(invalid); } catch (...) { rejected = true; }
            require(rejected);
        }
        const auto plan = parse_plan({{"steps", {{{"held_mask", 1}, {"hold_ms", 0}}, {{"held_mask", 3}, {"hold_ms", 0}}}}});
        Fake mouse;
        const auto result = execute(mouse, plan);
        require(result["success"] == true && mouse.commands == std::vector<int>({0, 1, 3, 0}));
        require(mouse.cleanup_count == 1 && mouse.close_count == 1 && mouse.forbidden == 0);
        require(result["monitor_ready_timeout_ms"] == 0 &&
            result["ready_finished_steady_ns"] == result["action_started_steady_ns"]);
        Fake failure; failure.fail_at = 2;
        const auto failed = execute(failure, plan);
        require(failed["success"] == false && failure.commands.size() == 2 && failure.cleanup_count == 1);
        Fake cancel; cancel.cancel = true;
        const auto canceled = execute(cancel, plan, 30000);
        require(canceled["failure"] == "END_CANCELED" && canceled["action_started_steady_ns"].is_null() &&
            cancel.commands.empty() && cancel.cleanup_count == 1);
        Fake waiting; waiting.statuses = {InputMonitorStatus::WAITING};
        const auto waiting_result = execute(waiting, plan);
        require(waiting_result["success"] == true && waiting_result["monitor_initial_state_known"] == false && waiting_result["monitor_ever_ready"] == false);
        Fake delayed_end; delayed_end.statuses = {InputMonitorStatus::WAITING}; delayed_end.cancel_after = 1;
        require(execute(delayed_end, plan)["failure"] == "END_CANCELED" && delayed_end.commands == std::vector<int>({0}));
        Fake regressed; regressed.statuses = {InputMonitorStatus::WAITING, InputMonitorStatus::READY, InputMonitorStatus::WAITING};
        require(execute(regressed, plan)["failure"] == "MONITOR_INVALID" && regressed.commands == std::vector<int>({0}));
        Fake failed_monitor; failed_monitor.poll_failure = true;
        require(execute(failed_monitor, plan)["failure"] == "MONITOR_INVALID" && failed_monitor.commands.empty());
        Fake observed_wait; observed_wait.statuses = {InputMonitorStatus::WAITING};
        require(execute(observed_wait, plan, 1)["success"] == true);
        for (const int bad_mask : {5, 10, 15, 16, -1}) {
            bool rejected = false;
            try { parse_plan({{"steps", {{{"held_mask", bad_mask}, {"hold_ms", 1}}}}}); } catch (...) { rejected = true; }
            require(rejected);
        }
        bool rejected = false;
        try { parse_plan({{"steps", {{{"held_mask", 1}, {"hold_ms", 2001}}}}}); } catch (...) { rejected = true; }
        require(rejected);
        Json too_long = {{"steps", Json::array()}};
        for (int i = 0; i < 8; ++i) too_long["steps"].push_back({{"held_mask", 1}, {"hold_ms", 2000}});
        rejected = false;
        try { parse_plan(too_long); } catch (...) { rejected = true; }
        require(rejected);
        too_long["steps"] = Json::array();
        for (int i = 0; i < 65; ++i) too_long["steps"].push_back({{"held_mask", 0}, {"hold_ms", 0}});
        rejected = false;
        try { parse_plan(too_long); } catch (...) { rejected = true; }
        require(rejected);
        Fake unwound;
        try { CleanupGuard guard(unwound); throw std::runtime_error("模拟异常"); } catch (...) {}
        require(unwound.cleanup_count == 1 && unwound.close_count == 1);
        Fake retained;
        require(execute(retained, plan, 1000, false)["success"] == true && retained.close_count == 0 && retained.cleanup_count == 1);
        Fake expired;
        const auto deadline_canceled = execute(expired, plan, 1000, false, [&]() {
            return expired.commands.empty() ? std::string{} : std::string("SESSION_EXPIRED");
        });
        require(deadline_canceled["failure"] == "SESSION_EXPIRED" && expired.commands == std::vector<int>({0}) && expired.cleanup_count == 1);
        const auto session_dir = std::filesystem::current_path() / ("auto-stop-probe-test-" + std::to_string(ns(Clock::now())));
        std::filesystem::create_directory(session_dir);
        write_session_json(session_dir / "001.plan.json", {{"steps", {{{"held_mask", 1}, {"hold_ms", 0}}}}}, false);
        Fake session_mouse;
        session_mouse.statuses = {InputMonitorStatus::WAITING};
        session_mouse.cancel_after = 5;
        const auto session_result = execute_session(session_mouse, session_dir, 1, 0);
        require(session_result["state"] == "finished" && session_result["completed_plans"] == 1 &&
            session_mouse.close_count == 1 && session_mouse.cleanup_count == 3 &&
            std::filesystem::exists(session_dir / "001.result.json"));
        std::filesystem::remove(session_dir / "001.plan.json");
        std::filesystem::remove(session_dir / "001.result.json");
        std::filesystem::remove(session_dir / "session.status.json");
        std::filesystem::remove(session_dir);
        const MaskCheckTiming quick{30, 0, 1, 0, 100};
        MaskFake masked;
        masked.start_held = true;
        const auto mask_result = execute_mask_check(masked, quick);
        require(mask_result["success"] == true && mask_result["raw_monitor_observed"] == false && mask_result["physical_w_down_time_ns"].is_null() &&
            masked.masks == std::vector<bool>({true, false}) && masked.forbidden == 0 && masked.cleanup_count == 1 && masked.close_count == 1);
        MaskFake stale; stale.freeze_sequence = true; stale.release_while_masked = true;
        require(execute_mask_check(stale, quick)["raw_monitor_observed"] == false);
        MaskFake released; released.release_while_masked = true;
        const auto released_result = execute_mask_check(released, quick);
        require(released_result["success"] == true && released_result["raw_monitor_observed"] == true &&
            released_result["return_observation_requested"] == false && released.cleanup_count == 1 && released.masks.size() == 2);
        MaskFake unknown; unknown.fail_mask = true;
        require(execute_mask_check(unknown, quick)["failure"] == "MASK_NOT_ACKNOWLEDGED" && unknown.cleanup_count == 1 && unknown.masks.size() == 1);
        std::cout << "急停实测工具专项通过\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
