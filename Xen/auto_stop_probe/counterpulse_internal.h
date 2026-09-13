#ifndef AUTO_STOP_COUNTERPULSE_INTERNAL_H
#define AUTO_STOP_COUNTERPULSE_INTERNAL_H

#include "auto_stop_probe/probe_internal.h"

namespace auto_stop_probe_detail {
struct CounterpulsePlan {
    std::string baseline = "counter";
    int shots = 8;
    bool capture_enabled = true;
    int shot_interval_ms = 280;
    int fire_delay_ms = 0; // 单发左键UP ACK起计时，等待期间保持正向键；0兼容旧流程。
    int move_ms = 120;
    int counter_hold_ms = 30;
    int counter_delay_ms = 0; // 正向UP ACK后等待多久再按反向；不同于反向持键时长。
    int brake_window_ms = 60;
    int shot_after_release_ms = 0; // 新模式允许0表示释放ACK后立即单发；全零兼容规则见uses_release_timing。
    int shot_hold_ms = 5;
    int late_tolerance_ms = 5;
    std::uint8_t direction = 2;
    bool uses_release_timing() const {
        return shot_after_release_ms > 0 || fire_delay_ms > 0 || counter_delay_ms > 0;
    }
    int move_tail_ms() const {
        return uses_release_timing() ? shot_after_release_ms +
            (baseline == "counter" ? counter_delay_ms + counter_hold_ms : 0) : brake_window_ms;
    }
    int cycle_budget_ms() const {
        return fire_delay_ms > 0 ? shot_hold_ms + std::max(move_ms, fire_delay_ms) + move_tail_ms() :
            (shot_interval_ms > 0 ? shot_interval_ms : shot_hold_ms + move_ms + move_tail_ms());
    }
};

inline CounterpulsePlan parse_counterpulse_plan(const Json& input) {
    if (!input.is_object()) throw std::runtime_error("反冲计划必须为对象");
    const std::vector<std::string> fields{"baseline", "capture_enabled", "shots", "shot_interval_ms", "fire_delay_ms", "move_ms",
        "counter_hold_ms", "counter_delay_ms", "brake_window_ms", "shot_after_release_ms", "shot_hold_ms", "late_tolerance_ms", "direction"};
    for (const auto& [key, value] : input.items()) {
        if (std::find(fields.begin(), fields.end(), key) == fields.end())
            throw std::runtime_error("反冲计划存在未知字段");
        if (key == "capture_enabled" && !value.is_boolean()) throw std::runtime_error("采集开关必须为布尔值");
        if (key != "baseline" && key != "capture_enabled" && (!value.is_number_integer() || value.get<double>() < 0 || value.get<double>() > 10000))
            throw std::runtime_error("时序字段必须为有界非负整数");
    }
    CounterpulsePlan p;
    p.baseline = input.value("baseline", p.baseline);
    p.capture_enabled = input.value("capture_enabled", p.capture_enabled);
    p.shots = input.value("shots", p.shots);
    p.shot_interval_ms = input.value("shot_interval_ms", p.shot_interval_ms);
    p.fire_delay_ms = input.value("fire_delay_ms", p.fire_delay_ms);
    p.move_ms = input.value("move_ms", p.move_ms);
    p.counter_hold_ms = input.value("counter_hold_ms", p.counter_hold_ms);
    p.counter_delay_ms = input.value("counter_delay_ms", p.counter_delay_ms);
    p.brake_window_ms = input.value("brake_window_ms", p.brake_window_ms);
    p.shot_after_release_ms = input.value("shot_after_release_ms", p.shot_after_release_ms);
    p.shot_hold_ms = input.value("shot_hold_ms", p.shot_hold_ms);
    p.late_tolerance_ms = input.value("late_tolerance_ms", p.late_tolerance_ms);
    const int direction = input.value("direction", static_cast<int>(p.direction));
    if ((p.baseline != "counter" && p.baseline != "stationary" && p.baseline != "no_counter") ||
        (p.shots < 1 || p.shots > 30) || (p.shot_interval_ms != 0 && p.shot_interval_ms < 280) || p.shot_interval_ms > 650 ||
        (p.shot_interval_ms == 0 && (p.baseline == "stationary" || !p.uses_release_timing())) ||
        ((p.fire_delay_ms == 0 || p.capture_enabled) && p.cycle_budget_ms() > 650) ||
        (p.shots - 1) * p.cycle_budget_ms() > (p.capture_enabled ? 12350 : 40000) ||
        p.fire_delay_ms > 2000 ||
        (p.fire_delay_ms > 0 && (p.shot_interval_ms != 0 || p.baseline == "stationary")) ||
        (direction != 2 && direction != 8) || p.move_ms < 1 || p.move_ms > 500 ||
        p.counter_delay_ms > 200 || (p.counter_delay_ms > 0 && p.baseline != "counter") ||
        p.counter_hold_ms < 1 || p.counter_hold_ms > 200 || p.shot_hold_ms < 1 || p.shot_hold_ms > 20 ||
        p.brake_window_ms < 1 || p.brake_window_ms > 200 ||
        (!p.uses_release_timing() && p.baseline == "counter" && p.counter_hold_ms >= p.brake_window_ms) ||
        p.shot_after_release_ms > 20 || p.late_tolerance_ms > 10 ||
        (p.shot_interval_ms > 0 && p.baseline != "stationary" && p.shot_hold_ms + p.move_ms +
            p.move_tail_ms() >= p.shot_interval_ms))
        throw std::runtime_error("反冲计划越界、总时长超限或射击间隔预算不足");
    p.direction = static_cast<std::uint8_t>(direction);
    return p;
}

inline Json counterpulse_plan_json(const CounterpulsePlan& p) {
    return {{"baseline", p.baseline}, {"capture_enabled", p.capture_enabled}, {"shots", p.shots}, {"shot_interval_ms", p.shot_interval_ms}, {"fire_delay_ms", p.fire_delay_ms},
        {"move_ms", p.move_ms}, {"counter_hold_ms", p.counter_hold_ms}, {"counter_delay_ms", p.counter_delay_ms}, {"brake_window_ms", p.brake_window_ms}, {"shot_after_release_ms", p.shot_after_release_ms}, {"shot_hold_ms", p.shot_hold_ms},
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
    Json report{{"schema_version", 2}, {"input_source", "TEST_SCRIPT"}, {"plan", counterpulse_plan_json(p)},
        {"timing_model", p.baseline == "stationary" ? "STATIONARY_INTERVAL" :
            (p.fire_delay_ms > 0 ? "HOLD_MOVE_DURING_FIRE_DELAY" :
                (p.uses_release_timing() ? "DIRECTION_UP_ACK_DELAY" : "MOVE_UP_ACK_FIXED_WINDOW"))}, {"cycles", Json::array()},
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
    const auto run_deadline = last_clock + std::chrono::milliseconds(
        std::max(3000, (p.shots - 1) * p.cycle_budget_ms() + 300));
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
    Clock::time_point last_shot_submit{};
    Clock::time_point minimum_shot{};
    auto budget_failure = [&](const char* site, int shot, Clock::time_point observed, Clock::time_point limit) {
        failure = "ACTION_BUDGET_EXCEEDED";
        report["failure_context"] = {{"site", site}, {"shot_index", shot},
            {"observed_ns", ns(observed)}, {"limit_ns", ns(limit)}};
    };
    auto command = [&](bool button, int value, int shot, Clock::time_point planned) -> bool {
        if (!check()) return false;
        const auto submit = clock.now();
        if (button && value && shot > 1 && p.shot_interval_ms > 0 && p.baseline != "stationary" && submit < minimum_shot) {
            budget_failure("SHOT_RECOVERY_TOO_SHORT", shot, submit, minimum_shot); return false;
        }
        const bool late = submit > planned + std::chrono::milliseconds(p.late_tolerance_ms);
        if (value && late) {
            failure = button ? "SHOT_DEADLINE_MISSED" : "COMMAND_DEADLINE_MISSED"; return false;
        }
        if (!button && value) {
            const int remaining_hold = value == p.direction ? std::max(p.move_ms, p.fire_delay_ms) + p.move_tail_ms() :
                p.counter_hold_ms + p.shot_after_release_ms;
            if (submit + std::chrono::milliseconds(remaining_hold) > next_shot_deadline) {
                budget_failure("REMAINING_HOLD", shot, submit + std::chrono::milliseconds(remaining_hold), next_shot_deadline); return false;
            }
        }
        Json entry{{"kind", button ? "left_button" : "wasd"}, {"value", value}, {"shot_index", shot},
            {"planned_ns", ns(planned)}, {"submit_ns", ns(submit)}, {"input_sequence", last_sequence}, {"input_epoch", epoch}};
        bool accepted = false;
        Clock::time_point completed;
        if (button) {
            if (value) {
                last_shot_submit = submit;
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
            auto deadline = first;
            for (int shot = 1; shot <= p.shots && failure.empty(); ++shot) {
                if (!wait(deadline)) break;
                if (clock.now() > deadline + std::chrono::milliseconds(p.late_tolerance_ms)) { failure = "SHOT_DEADLINE_MISSED"; break; }
                if (!command(true, 1, shot, deadline)) break;
                const auto button_release = ack + std::chrono::milliseconds(p.shot_hold_ms);
                if (!wait(button_release) || !command(true, 0, shot, button_release)) break;
                const auto fire_delay_start = ack; // 当前单发已释放，后续保持A覆盖此间隔。
                if (shot == p.shots) continue;
                if (p.baseline == "stationary") {
                    deadline = first + std::chrono::milliseconds(shot * p.shot_interval_ms);
                    continue;
                }
                // 恢复等待放在移动之前，不能在松键后为了枪间恢复追加等待。
                const auto earliest = last_shot_submit + std::chrono::milliseconds(p.shot_interval_ms);
                const auto move_start = p.shot_interval_ms == 0 ? clock.now() :
                    earliest - std::chrono::milliseconds(p.move_ms + p.move_tail_ms());
                next_shot_deadline = run_deadline;
                minimum_shot = earliest;
                if (!wait(move_start) || !command(false, p.direction, shot + 1, move_start)) break;
                auto move_release = ack + std::chrono::milliseconds(p.move_ms);
                if (p.fire_delay_ms > 0)
                    move_release = std::max(move_release, fire_delay_start + std::chrono::milliseconds(p.fire_delay_ms));
                if (!wait(move_release) || !command(false, 0, shot + 1, move_release)) break;
                const auto move_up_ack = ack;
                const bool after_release = p.uses_release_timing();
                deadline = move_up_ack + std::chrono::milliseconds(p.brake_window_ms);
                // 旧模式的反向动作必须在固定移动松键窗内；新模式先完成动作再确定射击锚点。
                next_shot_deadline = after_release ? run_deadline : deadline;
                if (next_shot_deadline > run_deadline) {
                    budget_failure("PLANNED_SHOT_RUN_LIMIT", shot + 1, next_shot_deadline, run_deadline); break;
                }
                if (p.baseline == "counter") {
                    const auto counter_press = move_up_ack + std::chrono::milliseconds(p.counter_delay_ms);
                    if (!wait(counter_press) || !command(false, p.direction == 2 ? 8 : 2, shot + 1, counter_press)) break;
                    const auto counter_release = ack + std::chrono::milliseconds(p.counter_hold_ms);
                    if (!wait(counter_release) || !command(false, 0, shot + 1, counter_release)) break;
                }
                const auto shot_anchor = after_release ? ack : move_up_ack;
                if (after_release) deadline = shot_anchor + std::chrono::milliseconds(p.shot_after_release_ms);
                if (deadline > run_deadline) {
                    budget_failure("PLANNED_SHOT_RUN_LIMIT", shot + 1, deadline, run_deadline); break;
                }
                report["cycles"].push_back({{"shot_index", shot + 1}, {"move_up_ack_ns", ns(move_up_ack)},
                    {"fire_delay_start_ack_ns", p.fire_delay_ms > 0 ? Json(ns(fire_delay_start)) : Json(nullptr)},
                    {"move_release_deadline_ns", ns(move_release)}, {"shot_anchor_ack_ns", ns(shot_anchor)}, {"shot_deadline_ns", ns(deadline)},
                    {"minimum_shot_ns", p.shot_interval_ms > 0 ? Json(ns(earliest)) : Json(nullptr)}, {"brake_window_ms", p.brake_window_ms},
                    {"shot_after_release_ms", p.shot_after_release_ms}});
                // 旧固定窗拒绝动作超窗；新锚点提交迟到由下一轮相对deadline检查。
                const auto finished = clock.now();
                if (!after_release && finished > deadline) budget_failure("REVERSE_FINISHED", shot + 1, finished, deadline);
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
