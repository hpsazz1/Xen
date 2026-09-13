#ifndef AUTO_STOP_COUNTERPULSE_INTERNAL_H
#define AUTO_STOP_COUNTERPULSE_INTERNAL_H

#include "auto_stop_probe/probe_internal.h"

namespace auto_stop_probe_detail {
struct CounterpulsePlan {
    std::string baseline = "counter";
    int shots = 8;
    int shot_interval_ms = 280;
    int move_ms = 120;
    int counter_hold_ms = 30;
    int shot_hold_ms = 5;
    int late_tolerance_ms = 5;
    std::uint8_t direction = 2;
};

inline CounterpulsePlan parse_counterpulse_plan(const Json& input) {
    if (!input.is_object()) throw std::runtime_error("反冲计划必须为对象");
    const std::vector<std::string> fields{"baseline", "shots", "shot_interval_ms", "move_ms",
        "counter_hold_ms", "shot_hold_ms", "late_tolerance_ms", "direction"};
    for (const auto& [key, value] : input.items()) {
        if (std::find(fields.begin(), fields.end(), key) == fields.end())
            throw std::runtime_error("反冲计划存在未知字段");
        if (key != "baseline" && (!value.is_number_integer() || value.get<double>() < 0 || value.get<double>() > 10000))
            throw std::runtime_error("时序字段必须为有界非负整数");
    }
    CounterpulsePlan p;
    p.baseline = input.value("baseline", p.baseline);
    p.shots = input.value("shots", p.shots);
    p.shot_interval_ms = input.value("shot_interval_ms", p.shot_interval_ms);
    p.move_ms = input.value("move_ms", p.move_ms);
    p.counter_hold_ms = input.value("counter_hold_ms", p.counter_hold_ms);
    p.shot_hold_ms = input.value("shot_hold_ms", p.shot_hold_ms);
    p.late_tolerance_ms = input.value("late_tolerance_ms", p.late_tolerance_ms);
    const int direction = input.value("direction", static_cast<int>(p.direction));
    if ((p.baseline != "counter" && p.baseline != "stationary" && p.baseline != "no_counter") ||
        (p.shots != 7 && p.shots != 8) || p.shot_interval_ms != 280 ||
        (direction != 2 && direction != 8) || p.move_ms < 1 || p.move_ms > 250 ||
        p.counter_hold_ms < 1 || p.counter_hold_ms > 200 || p.shot_hold_ms < 1 || p.shot_hold_ms > 20 ||
        p.late_tolerance_ms > 10 ||
        (p.baseline != "stationary" && p.shot_hold_ms + p.move_ms +
            (p.baseline == "counter" ? p.counter_hold_ms : 0) >= p.shot_interval_ms))
        throw std::runtime_error("反冲计划越界或280毫秒预算不足");
    p.direction = static_cast<std::uint8_t>(direction);
    return p;
}

inline Json counterpulse_plan_json(const CounterpulsePlan& p) {
    return {{"baseline", p.baseline}, {"shots", p.shots}, {"shot_interval_ms", p.shot_interval_ms},
        {"move_ms", p.move_ms}, {"counter_hold_ms", p.counter_hold_ms}, {"shot_hold_ms", p.shot_hold_ms},
        {"late_tolerance_ms", p.late_tolerance_ms}, {"direction", p.direction}};
}

struct CounterpulseClock {
    std::function<Clock::time_point()> now = [] { return Clock::now(); };
    std::function<void(Clock::time_point)> sleep_until = [](auto deadline) { std::this_thread::sleep_until(deadline); };
};

// 清理不服从取消：未知ACK也可能已按下，必须尝试左键和WASD归零。
class CounterpulseCleanup {
public:
    explicit CounterpulseCleanup(IMouseController& mouse) : mouse_(mouse) {}
    ~CounterpulseCleanup() { if (!finished_) { mouse_.set_left_button(false); mouse_.cleanup_wasd_keyboard(); } }
    bool finish(Json& report) {
        const auto button = mouse_.set_left_button(false);
        const auto keyboard = mouse_.cleanup_wasd_keyboard();
        finished_ = true;
        report["cleanup"] = {{"button_disposition", static_cast<int>(button.disposition)},
            {"button_ack_ns", ns(button.protocol_ack_received_at)}, {"keyboard", receipt_json(keyboard)}};
        return button.disposition == ButtonDisposition::ACKNOWLEDGED &&
            keyboard.disposition == KeyboardDisposition::ACKNOWLEDGED && !mouse_.left_button_cleanup_required();
    }
private:
    IMouseController& mouse_;
    bool finished_ = false;
};

inline Json execute_counterpulse(IMouseController& mouse, const CounterpulsePlan& proposed,
    const std::function<std::string()>& external_cancel = {}, CounterpulseClock clock = {}, bool require_event_history = false) {
    const auto p = parse_counterpulse_plan(counterpulse_plan_json(proposed));
    Json report{{"schema_version", 1}, {"input_source", "TEST_SCRIPT"}, {"plan", counterpulse_plan_json(p)},
        {"commands", Json::array()}, {"success", false}, {"settled", nullptr},
        {"physical_effect_observed", false}, {"shot_down_attempts", 0}};
    if (!mouse.output_owner_exclusive() || !mouse.supports_wasd_keyboard() || !mouse.supports_left_button()) {
        report["failure"] = "CAPABILITY_OR_OWNER_REQUIRED"; return report;
    }
    CounterpulseCleanup cleanup(mouse);
    std::string failure;
    WasdEventCursor cursor;
    std::uint64_t epoch = 0, last_sequence = 0;
    auto last_clock = clock.now();
    const auto run_deadline = last_clock + std::chrono::seconds(3);
    auto check = [&]() -> bool {
        if (!failure.empty()) return false;
        const auto current = clock.now();
        if (current < last_clock) { failure = "CLOCK_REGRESSION"; return false; }
        last_clock = current;
        if (current > run_deadline) { failure = "RUN_DEADLINE_EXCEEDED"; return false; }
        if (external_cancel) failure = external_cancel();
        if (!failure.empty()) return false;
        if (mouse.status() != MouseStatus::READY || mouse.left_button_faulted()) {
            failure = "OUTPUT_NOT_READY"; return false;
        }
        InputSnapshot snapshot;
        if (!mouse.poll_input(snapshot) || snapshot.status != InputMonitorStatus::READY || !snapshot.state_valid)
            failure = "MONITOR_INVALID";
        else if (snapshot.sequence < last_sequence) failure = "INPUT_SEQUENCE_REGRESSION";
        else {
            last_sequence = snapshot.sequence;
            for (const int key : {0x01, 0x02, 0x04, 0x05, 0x06, 0x23, 0x57, 0x41, 0x53, 0x44})
                if (snapshot.virtual_keys[key]) failure = "PHYSICAL_INPUT_CANCELED";
        }
        WasdEventBatch batch;
        if (mouse.read_wasd_events(cursor, batch)) {
            if (batch.count > batch.events.size()) failure = "INPUT_HISTORY_INVALID";
            if (require_event_history && (!batch.subscribed || !cursor.epoch)) failure = "INPUT_HISTORY_UNAVAILABLE";
            if (batch.gap) failure = "INPUT_EVENT_GAP";
            if (epoch && cursor.epoch != epoch) failure = "INPUT_EPOCH_CHANGED";
            epoch = cursor.epoch;
            for (std::size_t i = 0; i < batch.count && i < batch.events.size(); ++i)
                if (!batch.events[i].state_valid || batch.events[i].held_mask) failure = "PHYSICAL_EVENT_CANCELED";
        } else if (require_event_history) failure = "INPUT_HISTORY_UNAVAILABLE";
        report["last_input_sequence"] = last_sequence;
        report["last_input_epoch"] = epoch;
        return failure.empty();
    };
    auto wait = [&](Clock::time_point deadline) -> bool {
        while (clock.now() < deadline) {
            if (!check()) return false;
            const auto before = clock.now();
            clock.sleep_until(std::min(deadline, before + std::chrono::milliseconds(1)));
            if (clock.now() <= before) { failure = "CLOCK_NOT_ADVANCING"; return false; }
        }
        return check();
    };
    Clock::time_point ack{};
    Clock::time_point next_shot_deadline{};
    auto command = [&](bool button, int value, int shot, Clock::time_point planned) -> bool {
        if (!check()) return false;
        const auto submit = clock.now();
        const bool late = submit > planned + std::chrono::milliseconds(p.late_tolerance_ms);
        if (value && late) {
            failure = button ? "SHOT_DEADLINE_MISSED" : "COMMAND_DEADLINE_MISSED"; return false;
        }
        if (!button && value) {
            const int remaining_hold = value == p.direction ? p.move_ms + (p.baseline == "counter" ? p.counter_hold_ms : 0) : p.counter_hold_ms;
            if (submit + std::chrono::milliseconds(remaining_hold) > next_shot_deadline) {
                failure = "ACTION_BUDGET_EXCEEDED"; return false;
            }
        }
        Json entry{{"kind", button ? "left_button" : "wasd"}, {"value", value}, {"shot_index", shot},
            {"planned_ns", ns(planned)}, {"submit_ns", ns(submit)}, {"input_sequence", last_sequence}, {"input_epoch", epoch}};
        bool accepted = false;
        Clock::time_point completed;
        if (button) {
            if (value) {
                const int attempts = report["shot_down_attempts"].get<int>();
                if (attempts >= p.shots) { failure = "SHOT_BUDGET_EXHAUSTED"; return false; }
                report["shot_down_attempts"] = attempts + 1;
            }
            const auto receipt = mouse.set_left_button(value != 0);
            accepted = receipt.disposition == ButtonDisposition::ACKNOWLEDGED;
            ack = receipt.protocol_ack_received_at; completed = receipt.backend_completed_at;
            entry["disposition"] = static_cast<int>(receipt.disposition);
            entry["datagram_sent"] = receipt.datagram_sent;
        } else {
            const auto receipt = mouse.set_wasd_keyboard(static_cast<std::uint8_t>(value));
            accepted = receipt.disposition == KeyboardDisposition::ACKNOWLEDGED;
            ack = receipt.protocol_ack_received_at; completed = receipt.backend_completed_at;
            entry["disposition"] = static_cast<int>(receipt.disposition);
            entry["datagram_sent"] = receipt.datagram_sent;
        }
        const auto returned = clock.now();
        entry["ack_received_ns"] = ns(ack); entry["backend_completed_ns"] = ns(completed); entry["returned_ns"] = ns(returned);
        report["commands"].push_back(std::move(entry));
        if (!accepted) failure = "COMMAND_NOT_ACKNOWLEDGED";
        else if (ack < submit || ack > returned || completed < ack || completed > returned) failure = "COMMAND_TIME_INVALID";
        else if (late) failure = "RELEASE_DEADLINE_MISSED";
        else if (returned > run_deadline) failure = "RUN_DEADLINE_EXCEEDED";
        return failure.empty();
    };
    try {
        // 首枪之前显式建立软件全松状态，不把软件报告写入物理事件链。
        const auto start = clock.now();
        if (command(true, 0, 0, start) && command(false, 0, 0, clock.now())) {
            const auto first = clock.now();
            for (int shot = 1; shot <= p.shots && failure.empty(); ++shot) {
                const auto deadline = first + std::chrono::milliseconds((shot - 1) * p.shot_interval_ms);
                if (!wait(deadline)) break;
                if (clock.now() > deadline + std::chrono::milliseconds(p.late_tolerance_ms)) { failure = "SHOT_DEADLINE_MISSED"; break; }
                if (!command(true, 1, shot, deadline)) break;
                const auto button_release = ack + std::chrono::milliseconds(p.shot_hold_ms);
                if (!wait(button_release) || !command(true, 0, shot, button_release)) break;
                if (shot == p.shots || p.baseline == "stationary") continue;
                next_shot_deadline = deadline + std::chrono::milliseconds(p.shot_interval_ms);
                if (!command(false, p.direction, shot + 1, clock.now())) break;
                const auto move_release = ack + std::chrono::milliseconds(p.move_ms);
                if (!wait(move_release) || !command(false, 0, shot + 1, move_release)) break;
                if (p.baseline == "counter") {
                    if (!command(false, p.direction == 2 ? 8 : 2, shot + 1, clock.now())) break;
                    const auto counter_release = ack + std::chrono::milliseconds(p.counter_hold_ms);
                    if (!wait(counter_release) || !command(false, 0, shot + 1, counter_release)) break;
                }
                // 动作已超出下一枪计划时取消；绝不压缩动作或补射。
                if (clock.now() > deadline + std::chrono::milliseconds(p.shot_interval_ms)) failure = "ACTION_BUDGET_EXCEEDED";
            }
        }
    } catch (const std::exception&) { failure = "EXECUTION_EXCEPTION"; }
    const bool cleaned = cleanup.finish(report);
    if (!cleaned && failure.empty()) failure = "CLEANUP_NOT_ACKNOWLEDGED";
    report["failure"] = failure;
    report["success"] = failure.empty() && report["shot_down_attempts"].get<int>() == p.shots;
    return report;
}
} // namespace auto_stop_probe_detail
#endif
