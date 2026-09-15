#ifndef AUTO_STOP_SAMPLING_ANALYSIS_INTERNAL_H
#define AUTO_STOP_SAMPLING_ANALYSIS_INTERNAL_H
#include "probe_internal.h"
#include <cmath>
#include <optional>

namespace auto_stop_probe_detail {
namespace sampling_detail {
struct SamplingSettings {
    double max_move_speed = 1, clean_shot_speed_ratio = .34, accel_per_sec = 5.5;
    double natural_decel_per_sec = 2.5, counter_strafe_accel_per_sec = 14;
    int fire_sample_delay_ms = 18, tap_max_hold_ms = 90, auto_fire_interval_ms = 100;
    bool hud_enabled = true;
};
inline Json sampling_settings_json(const SamplingSettings& s) {
    return {{"max_move_speed",s.max_move_speed},{"clean_shot_speed_ratio",s.clean_shot_speed_ratio},
        {"accel_per_sec",s.accel_per_sec},{"natural_decel_per_sec",s.natural_decel_per_sec},
        {"counter_strafe_accel_per_sec",s.counter_strafe_accel_per_sec},{"fire_sample_delay_ms",s.fire_sample_delay_ms},
        {"tap_max_hold_ms",s.tap_max_hold_ms},{"auto_fire_interval_ms",s.auto_fire_interval_ms},{"hud_enabled",s.hud_enabled}};
}
inline SamplingSettings parse_sampling_settings(const Json& value) {
    if (!value.is_object()) throw std::runtime_error("采样参数必须是对象");
    SamplingSettings s;
    const auto allowed = sampling_settings_json(s);
    for (auto it = value.begin(); it != value.end(); ++it)
        if (!allowed.contains(it.key())) throw std::runtime_error("未知采样参数:" + it.key());
    const auto number = [&](const char* key, double fallback, double low, double high, bool integral = false) {
        if (!value.contains(key)) return fallback;
        if (!value[key].is_number()) throw std::runtime_error(std::string("采样参数不是数字:") + key);
        const auto n = value[key].get<double>();
        if (!std::isfinite(n) || n < low || n > high || (integral && std::floor(n) != n))
            throw std::runtime_error(std::string("采样参数超范围:") + key);
        return n;
    };
    s.max_move_speed = number("max_move_speed",s.max_move_speed,.5,2);
    s.clean_shot_speed_ratio = number("clean_shot_speed_ratio",s.clean_shot_speed_ratio,.1,1);
    s.accel_per_sec = number("accel_per_sec",s.accel_per_sec,1,20);
    s.natural_decel_per_sec = number("natural_decel_per_sec",s.natural_decel_per_sec,.5,10);
    s.counter_strafe_accel_per_sec = number("counter_strafe_accel_per_sec",s.counter_strafe_accel_per_sec,5,40);
    s.fire_sample_delay_ms = static_cast<int>(number("fire_sample_delay_ms",s.fire_sample_delay_ms,0,120,true));
    s.tap_max_hold_ms = static_cast<int>(number("tap_max_hold_ms",s.tap_max_hold_ms,20,300,true));
    s.auto_fire_interval_ms = static_cast<int>(number("auto_fire_interval_ms",s.auto_fire_interval_ms,40,500,true));
    if (value.contains("hud_enabled")) {
        if (!value["hud_enabled"].is_boolean()) throw std::runtime_error("hud_enabled必须是布尔值");
        s.hud_enabled = value["hud_enabled"].get<bool>();
    }
    return s;
}
struct State { std::int64_t time; double velocity; int direction; bool valid; };
struct SamplingSchedule { std::vector<std::int64_t> times; bool budget_exceeded = false; };
inline SamplingSchedule sample_due_times(std::int64_t down, std::optional<std::int64_t> up,
    std::int64_t horizon, const SamplingSettings& settings, std::size_t budget = 300) {
    SamplingSchedule result;
    if (down < 0 || down > INT64_MAX - 100000000000LL || (up && *up < down)) return result;
    const auto end = up.value_or(horizon);
    auto due = down + static_cast<std::int64_t>(settings.fire_sample_delay_ms) * 1000000;
    budget = std::min<std::size_t>(budget,300);
    while ((result.times.empty() && up.has_value()) || due <= end) {
        if (result.times.size() >= budget) { result.budget_exceeded = true; break; }
        result.times.push_back(due);
        if (due > INT64_MAX - static_cast<std::int64_t>(settings.auto_fire_interval_ms) * 1000000) {
            result.budget_exceeded = true; break;
        }
        due = std::max(down + static_cast<std::int64_t>(settings.tap_max_hold_ms) * 1000000,
            due + static_cast<std::int64_t>(settings.auto_fire_interval_ms) * 1000000);
    }
    return result;
}
inline double advance(double velocity, int direction, double seconds, const SamplingSettings& settings = {}) {
    if (seconds <= 0) return velocity;
    if (!direction) return std::copysign(std::max(0.0, std::abs(velocity) - settings.natural_decel_per_sec * seconds), velocity);
    if (velocity * direction < 0) {
        const auto braking = std::min(seconds, std::abs(velocity) / settings.counter_strafe_accel_per_sec);
        velocity += direction * settings.counter_strafe_accel_per_sec * braking;
        seconds -= braking;
    }
    return std::clamp(velocity + direction * settings.accel_per_sec * seconds, -settings.max_move_speed, settings.max_move_speed);
}
inline Json model_at(const std::vector<State>& states, std::int64_t time, std::int64_t last_time, const SamplingSettings& settings = {}) {
    const State* state = nullptr;
    if (time >= 0 && time <= INT64_MAX - 100000000000LL)
        for (const auto& item : states) { if (item.time > time) break; state = &item; }
    Json value{{"time_ns", time}, {"valid", state && state->valid}, {"extrapolated", time > last_time},
        {"estimated_speed", nullptr}, {"speed_ratio", nullptr}, {"classification", "UNAVAILABLE"}, {"within_model_threshold", nullptr}};
    if (state && state->valid) {
        const auto speed = std::abs(advance(state->velocity, state->direction, (time - state->time) / 1e9, settings));
        const auto ratio = speed / (settings.max_move_speed * settings.clean_shot_speed_ratio);
        value["estimated_speed"] = speed; value["speed_ratio"] = ratio;
        value["classification"] = ratio <= 1 ? "WITHIN_MODEL_THRESHOLD" : ratio <= 1.5 ? "MICRO" : "RUNNING";
        value["within_model_threshold"] = ratio <= 1;
    }
    return value;
}
// 实时与离线共用的ACK代理状态机；查询不会推进状态，避免提前采样污染随后事件。
class SamplingModel {
public:
    explicit SamplingModel(SamplingSettings settings = {}) : settings_(settings) {}
    bool observe_time(std::int64_t time) {
        if (time < previous_ || time < 0 || time > INT64_MAX - 100000000000LL) { invalidate(previous_); return false; }
        if (known_) velocity_ = advance(velocity_, direction_, (time - previous_) / 1e9, settings_);
        previous_ = time; return true;
    }
    bool observe_wasd(std::int64_t time, std::uint8_t mask) {
        if ((mask & ~10) != 0 || !observe_time(time)) { invalidate(previous_); return false; }
        if (!known_ && mask != 0) trusted_ = false;
        known_ = true;
        if (mask == 10) {
            if (!(held_mask_ & 8)) direction_ = 1;
            else if (!(held_mask_ & 2)) direction_ = -1;
        } else direction_ = mask == 2 ? -1 : mask == 8 ? 1 : 0;
        held_mask_ = mask; save(); return valid();
    }
    void invalidate(std::int64_t time) {
        trusted_ = false;
        if (time >= 0 && time <= INT64_MAX - 100000000000LL) previous_ = std::max(previous_, time);
        save();
    }
    bool valid() const { return known_ && trusted_; }
    bool known() const { return known_; }
    std::int64_t last_time() const { return previous_; }
    Json query(std::int64_t time) const { return sampling_detail::model_at(states_, time, previous_, settings_); }
    Json model_at(std::int64_t time) const { return query(time); }
private:
    void save() {
        states_.push_back({previous_,velocity_,direction_,valid()});
        if (states_.size() > 1024) states_.erase(states_.begin());
    }
    SamplingSettings settings_;
    std::vector<State> states_;
    bool known_ = false, trusted_ = true;
    std::uint8_t held_mask_ = 0;
    int direction_ = 0;
    double velocity_ = 0;
    std::int64_t previous_ = 0;
};
inline std::int64_t integer(const Json& item, const char* key) {
    const auto& value = item.at(key);
    if (!value.is_number_integer() || (value.is_number_unsigned() && value.get<std::uint64_t>() > INT64_MAX))
        throw std::runtime_error("字段不是有符号范围整数");
    return value.get<std::int64_t>();
}
}

using sampling_detail::SamplingSettings;
using sampling_detail::parse_sampling_settings;
using sampling_detail::sampling_settings_json;

inline Json analyze_counterpulse_sampling_impl(const Json& report, std::optional<std::int64_t> live_as_of = {}) {
    using namespace sampling_detail;
    if (!report.is_object()) return {{"source", "COMMAND_ACK_PROXY"}, {"physical_validation_passed", false},
        {"game_stability", nullptr}, {"analysis_complete", false}, {"quality_issues", Json::array({"REPORT_NOT_OBJECT"})}};
    Json out{{"source", "COMMAND_ACK_PROXY"}, {"physical_validation_passed", false},
        {"game_stability", nullptr}, {"game_shot_stability", nullptr}, {"success_meaning", "OFFLINE_ANALYSIS_ONLY"},
        {"reference_commit", "e632605f8b6c20ac5ab8ac3284e4fdc33735d431"},
        {"reference_url", "https://github.com/qianjiachun/cs-match-hud/blob/e632605f8b6c20ac5ab8ac3284e4fdc33735d431/src-tauri/src/counter_strafing/engine.rs"},
        {"settings", {{"max_move_speed", 1.0}, {"clean_shot_speed_ratio", 0.34}, {"accel_per_sec", 5.5},
            {"natural_decel_per_sec", 2.5}, {"counter_strafe_accel_per_sec", 14.0}, {"fire_sample_delay_ms", 18},
            {"tap_max_hold_ms", 90}, {"auto_fire_interval_ms", 100}}},
        {"model_scope", "ONE_AXIS_INPUT_MODEL_WITHOUT_HUD_CROUCH_OR_STARTUP_GRACE"},
        {"quality_note", "仅离线单轴模型；不复制蹲下或启动宽限；初始静止是假设；未知回执后不恢复可信速度"},
        {"initial_velocity_assumption", "ZERO_NOT_PHYSICALLY_VERIFIED"},
        {"short_tap_semantics", "DOWN_PLUS_18MS_WITH_COMPLETE_EVENT_ORDER"},
        {"samples_are_bullets", false}, {"actual_plan", report.value("plan", Json(nullptr))},
        {"analysis_mode", live_as_of ? "LIVE_ACK_PROXY" : "OFFLINE_ACK_PROXY"},
        {"execution_success", report.value("success", Json(nullptr))}, {"execution_failure", report.value("failure", Json(nullptr))},
        {"monitor_evidence", report.value("monitor_evidence", Json(nullptr))},
        {"quality_issues", Json::array()}, {"shots", Json::array()}, {"commands", Json::array()},
        {"timings", Json::array()}, {"total_timing_count", 0}, {"timing_source", "COMMAND_ACK_INTERVAL"}};
    const Json plan = out["actual_plan"].is_object() ? out["actual_plan"] : Json::object();
    const auto issue = [&](const std::string& reason) { out["quality_issues"].push_back(reason); };
    SamplingSettings settings;
    try { settings = parse_sampling_settings(report.value("sampling_settings", Json::object())); }
    catch (const std::exception&) {
        issue("SAMPLING_SETTINGS_INVALID"); out["settings"] = nullptr; out["analysis_complete"] = false; return out;
    }
    out["settings"] = sampling_settings_json(settings);
    if (report.contains("original_sampling_settings")) out["original_sampling_settings"] = report["original_sampling_settings"];
    out["sampling_settings_overridden"] = report.value("sampling_settings_overridden", Json(false));
    out["settings_source"] = report.contains("sampling_settings") ? "REPORT_SAMPLING_SETTINGS" : "REFERENCE_DEFAULTS";
    out["short_tap_semantics"] = "DOWN_PLUS_CONFIGURED_DELAY_WITH_COMPLETE_EVENT_ORDER";
    for (const auto* key : {"move_ms", "counter_hold_ms", "counter_delay_ms", "shot_after_release_ms", "shot_hold_ms", "fire_interval_ms"})
        if (!plan.contains(key)) issue(std::string("PLAN_MISSING:") + key);
        else if (!plan[key].is_number_integer() || plan[key] < 0) issue(std::string("PLAN_INVALID:") + key);
    const std::string baseline = plan.contains("baseline") && plan["baseline"].is_string() ?
        plan["baseline"].get<std::string>() : "counter";
    if (!plan.contains("baseline")) issue("PLAN_MISSING:baseline");
    if (!plan.contains("direction")) issue("PLAN_MISSING:direction");
    const int requested_direction = plan.contains("direction") && plan["direction"].is_number_integer() &&
        (plan["direction"] == 2 || plan["direction"] == 8) ? plan["direction"].get<int>() : 2;
    if ((plan.contains("baseline") && !plan["baseline"].is_string()) ||
        (baseline != "counter" && baseline != "no_counter" && baseline != "stationary")) issue("PLAN_INVALID:baseline");
    if (plan.contains("direction") && (!plan["direction"].is_number_integer() ||
        (plan["direction"] != 2 && plan["direction"] != 8))) issue("PLAN_INVALID:direction");
    if (!report.contains("commands") || !report["commands"].is_array()) {
        issue("COMMANDS_MISSING"); out["analysis_complete"] = false; return out;
    }
    const auto& commands = report["commands"];
    const auto count = std::min<std::size_t>(commands.size(), 512);
    if (commands.size() > count) issue("COMMAND_BUDGET_EXCEEDED");
    SamplingModel model(settings);
    struct Shot { std::size_t command; std::int64_t down, submit; std::optional<std::int64_t> up, up_submit; bool valid; };
    std::vector<Shot> shots;
    std::optional<std::size_t> active;
    std::int64_t previous = 0;
    int timing_direction = 0, timing_held = 0;
    std::optional<std::int64_t> timing_release;
    for (std::size_t index = 0; index < count; ++index) {
        const auto& command = commands[index];
        Json item{{"index", index}, {"valid_receipt", false}, {"submit_lateness_ms", nullptr}, {"ack_latency_ms", nullptr}};
        try {
            const auto kind = command.at("kind").get<std::string>();
            const auto value = integer(command, "value"), disposition = integer(command, "disposition");
            const auto submit = integer(command, "submit_ns"), ack = integer(command, "ack_received_ns");
            const auto completed = integer(command, "backend_completed_ns"), returned = integer(command, "returned_ns");
            const bool keyboard = kind == "wasd", button = kind == "left_button";
            const bool valid = (keyboard || button) && disposition == static_cast<int>(KeyboardDisposition::ACKNOWLEDGED) &&
                submit >= 0 && ack > 0 && returned < INT64_MAX - 100000000000LL &&
                submit <= ack && ack <= completed && completed <= returned && ack >= previous &&
                ((keyboard && value >= 0 && value <= 15 && !(value & 5)) || (button && (value == 0 || value == 1)));
            item["kind"] = kind; item["value"] = value; item["submit_ns"] = submit; item["ack_received_ns"] = ack;
            item["shot_index"] = command.value("shot_index", Json(nullptr));
            if (command.contains("planned_ns") && command["planned_ns"].is_number_integer())
                item["submit_lateness_ms"] = (static_cast<double>(submit) - static_cast<double>(integer(command, "planned_ns"))) / 1e6;
            item["ack_latency_ms"] = (static_cast<double>(ack) - static_cast<double>(submit)) / 1e6;
            if (!valid) throw std::runtime_error("无效或不支持的命令回执");
            item["valid_receipt"] = true;
            if (keyboard) {
                // 急停反馈统一保存ACK释放→对向按下间隔，实时及离线共用；不跨开火周期配对。
                if (value == 0) {
                    if (timing_held == 2 || timing_held == 8) timing_release = ack;
                } else if (value == 2 || value == 8) {
                    if (timing_release && timing_direction && value != timing_direction)
                        out["timings"].push_back({{"delta_ms", (ack - *timing_release) / 1e6},
                            {"ordinal", out["timings"].size() + 1}, {"grade", "ACK_INTERVAL"},
                            {"release_ack_ns", *timing_release}, {"press_ack_ns", ack},
                            {"from_mask", timing_direction}, {"to_mask", value}, {"command_index", index}});
                    timing_direction = static_cast<int>(value); timing_release.reset();
                } else { timing_direction = 0; timing_release.reset(); }
                timing_held = static_cast<int>(value);
                if (!model.known() && value != 0) issue("INITIAL_KEYBOARD_STATE_UNKNOWN");
                model.observe_wasd(ack, static_cast<std::uint8_t>(value));
            } else model.observe_time(ack);
            if (button && value == 1) {
                timing_direction = timing_held = 0; timing_release.reset();
                if (active) { shots[*active].valid = false; model.invalidate(ack); issue("DUPLICATE_FIRE_DOWN"); }
                shots.push_back({index, ack, submit, {}, {}, model.valid()}); active = shots.size() - 1;
            } else if (button && active) { shots[*active].up = ack; shots[*active].up_submit = submit; active.reset(); }
            previous = ack;
        } catch (const std::exception&) {
            timing_direction = timing_held = 0; timing_release.reset();
            if (active) shots[*active].valid = false;
            issue("INVALID_RECEIPT_OR_UNSUPPORTED_AXIS:" + std::to_string(index));
            model.invalidate(previous);
        }
        out["commands"].push_back(std::move(item));
    }
    out["total_timing_count"] = out["timings"].size();
    std::size_t total_samples = 0, first_samples = 0, held_samples = 0;
    const bool live_time_valid = !live_as_of || (*live_as_of >= previous && *live_as_of <= INT64_MAX - 100000000000LL);
    if (!live_time_valid) issue("LIVE_HORIZON_INVALID");
    if (live_as_of) {
        out["current_model"] = model.query(*live_as_of);
        if (!live_time_valid) {
            out["current_model"]["valid"] = false;
            out["current_model"]["classification"] = "UNAVAILABLE";
            out["current_model"]["within_model_threshold"] = nullptr;
        }
    }
    for (std::size_t index = 0; index < shots.size(); ++index) {
        const auto& shot = shots[index];
        const bool live_active = live_as_of && live_time_valid && active && *active == index;
        const bool hold_valid = shot.valid && (shot.up.has_value() || live_active);
        Json item{{"ordinal", index + 1}, {"down_command_index", shot.command}, {"down_ack_ns", shot.down},
            {"up_ack_ns", shot.up ? Json(*shot.up) : Json(nullptr)}, {"complete_hold", shot.up.has_value()},
            {"valid_command_hold", hold_valid}, {"active_hold", live_active},
            {"observed_hold_ms", shot.up ? Json((*shot.up - shot.down) / 1e6) : Json(nullptr)},
            {"up_submit_ns", shot.up_submit ? Json(*shot.up_submit) : Json(nullptr)},
            // 按住计划的时钟锚点为DOWN协议ACK，结束命令提交与UP ACK不得混用。
            {"down_ack_to_up_submit_ms", shot.up_submit && *shot.up_submit >= shot.down ?
                Json((*shot.up_submit - shot.down) / 1e6) : Json(nullptr)},
            {"planned_hold_ms", plan.value("shot_hold_ms", Json(nullptr))},
            {"previous_down_submit_interval_ms", index ? Json((shot.submit - shots[index - 1].submit) / 1e6) : Json(nullptr)},
            {"next_down_submit_interval_ms", index + 1 < shots.size() ? Json((shots[index + 1].submit - shot.submit) / 1e6) : Json(nullptr)},
            {"planned_down_interval_ms", plan.value("fire_interval_ms", Json(nullptr))},
            {"down_model", model.query(shot.down)}, {"samples", Json::array()}, {"stages", Json::array()}};
        // 按本次火键之前的原始WASD命令给出阶段观察值，保留其原索引。
        const auto begin = index ? shots[index - 1].command + 1 : 0;
        std::vector<std::size_t> movement;
        for (auto c = begin; c < shot.command; ++c)
            if (out["commands"][c].value("kind", "") == "wasd" && out["commands"][c].value("valid_receipt", false)) movement.push_back(c);
        for (std::size_t m = 0; m < movement.size(); ++m) {
            const auto c = movement[m]; const auto& current = out["commands"][c];
            const auto end = m + 1 < movement.size() ? integer(out["commands"][movement[m + 1]], "ack_received_ns") : shot.down;
            const auto value = integer(current, "value");
            std::string name = "UNCLASSIFIED";
            const auto primary = requested_direction;
            const auto opposite = primary == 2 ? 8 : 2;
            if (baseline != "stationary" && value == primary) name = "move_hold";
            else if (baseline == "counter" && value == opposite) name = "counter_hold";
            else if (!value && m) {
                const auto preceding = integer(out["commands"][movement[m - 1]], "value");
                if (baseline == "counter" && preceding == primary && m + 1 < movement.size() &&
                    integer(out["commands"][movement[m + 1]], "value") == opposite) name = "counter_gap";
                else if (preceding == primary || preceding == opposite) name = "shot_after_release";
            }
            const std::string key = name == "move_hold" ? "move_ms" : name == "counter_hold" ? "counter_hold_ms" :
                name == "counter_gap" ? "counter_delay_ms" : "shot_after_release_ms";
            item["stages"].push_back({{"name", name}, {"command_index", c}, {"held_mask", value},
                {"observed_ms", (end - integer(current, "ack_received_ns")) / 1e6},
                {"planned_ms", name == "UNCLASSIFIED" ? Json(nullptr) : plan.value(key, Json(nullptr))}});
        }
        if (!shot.up && !live_active) issue("MISSING_FIRE_UP:" + std::to_string(index + 1));
        if (!hold_valid) {
            item["down_model"]["valid"] = false;
            item["down_model"]["within_model_threshold"] = nullptr;
        }
        const auto schedule = sample_due_times(shot.down, shot.up,
            live_as_of && live_time_valid ? *live_as_of : previous, settings, 10000 - total_samples);
        std::size_t generated = 0;
        for (const auto due : schedule.times) {
            if (live_as_of && (!live_time_valid || due > *live_as_of)) continue;
            auto sample = model.query(due);
            if (!hold_valid) { sample["valid"] = false; sample["within_model_threshold"] = nullptr; }
            sample["kind"] = generated ? "HELD_MODEL_SAMPLE" : "FIRST_MODEL_SAMPLE";
            sample["valid_command_hold"] = hold_valid;
            sample["future_events_applied"] = true;
            item["samples"].push_back(std::move(sample));
            ++total_samples; if (generated) ++held_samples; else ++first_samples;
            ++generated;
        }
        const bool limited = schedule.budget_exceeded;
        item["sample_budget_exceeded"] = limited;
        if (limited) issue("SAMPLE_BUDGET_EXCEEDED:" + std::to_string(index + 1));
        out["shots"].push_back(std::move(item));
    }
    out["shot_count"] = shots.size(); out["first_sample_count"] = first_samples;
    out["commands_observed"] = commands.size(); out["commands_analyzed"] = count;
    out["commands_truncated"] = commands.size() - count;
    out["held_sample_count"] = held_samples; out["total_sample_count"] = total_samples;
    Json summaries;
    for (const auto* name : {"down", "first", "held"}) summaries[name] =
        {{"observed", 0}, {"valid", 0}, {"within_model_threshold", 0}, {"micro", 0}, {"running", 0}};
    const auto tally = [&](const char* category, const Json& model) {
        auto& summary = summaries[category];
        summary["observed"] = summary["observed"].get<int>() + 1;
        if (!model.value("valid", false)) return;
        summary["valid"] = summary["valid"].get<int>() + 1;
        const auto label = model.value("classification", "UNAVAILABLE");
        const char* field = label == "WITHIN_MODEL_THRESHOLD" ? "within_model_threshold" :
            label == "MICRO" ? "micro" : label == "RUNNING" ? "running" : nullptr;
        if (field) summary[field] = summary[field].get<int>() + 1;
    };
    for (const auto& shot : out["shots"]) {
        tally("down", shot["down_model"]);
        for (const auto& sample : shot["samples"]) tally(sample["kind"] == "FIRST_MODEL_SAMPLE" ? "first" : "held", sample);
    }
    out["model_summary"] = std::move(summaries);
    out["analysis_complete"] = out["quality_issues"].empty();
    return out;
}
inline Json analyze_counterpulse_sampling(const Json& report) { return analyze_counterpulse_sampling_impl(report); }
inline Json analyze_counterpulse_sampling(const Json& report, const sampling_detail::SamplingSettings& settings) {
    if (!report.is_object()) return analyze_counterpulse_sampling(report);
    auto configured = report;
    configured["sampling_settings"] = sampling_detail::sampling_settings_json(settings);
    return analyze_counterpulse_sampling(configured);
}
inline Json analyze_counterpulse_live_sampling(const Json& report, const SamplingSettings& settings, std::int64_t now) {
    if (!report.is_object()) return analyze_counterpulse_sampling(report);
    auto configured = report;
    configured["sampling_settings"] = sampling_settings_json(settings);
    return analyze_counterpulse_sampling_impl(configured,now);
}
}
#endif
