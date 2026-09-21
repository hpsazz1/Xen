#include "reference_assessment/reference_assessment.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace xen::reference_assessment {
namespace {
constexpr double kThreshold = 0.34;
constexpr std::size_t kHistoryLimit = 300;
double round3(double value) { return std::round(value * 1000.0) / 1000.0; }
double round1(double value) { return std::round(value * 10.0) / 10.0; }
template<class T> void append(std::vector<T>& history, const T& value) {
    history.push_back(value);
    if (history.size() > kHistoryLimit) history.erase(history.begin());
}
struct AxisResult { bool counter{}; std::optional<double> cross; };
AxisResult advance_axis(double& velocity, int input, double dt) {
    const double start = velocity;
    const bool counter = std::abs(start) > 0.001 && input * start < 0;
    const double decel = input == 0 ? 2.5 : 14.0;
    AxisResult result{counter, std::nullopt};
    if ((input == 0 || counter) && std::abs(start) > kThreshold) {
        const double cross = (std::abs(start) - kThreshold) / decel;
        if (cross <= dt) result.cross = cross;
    }
    if (input == 0) {
        velocity = std::copysign(std::max(0.0, std::abs(start) - decel * dt), start);
    } else if (!counter) {
        velocity += input * 5.5 * dt;
    } else {
        const double to_zero = std::abs(start) / 14.0;
        velocity = to_zero >= dt ? start + input * 14.0 * dt : input * 5.5 * (dt - to_zero);
    }
    velocity = std::clamp(velocity, -1.0, 1.0);
    return result;
}
}
bool Engine::validate_time(TimeMs time, Output& output) {
    if (!std::isfinite(time) || time < 0 || time < last_external_ms_ || time > 1000000000.0) {
        output.accepted = false;
        output.error = "时间必须非负、单调且保留调度余量";
        return false;
    }
    last_external_ms_ = time;
    return true;
}
void Engine::assess(Event event, Output& output) {
    const auto idx = static_cast<std::size_t>(event.key);
    const auto other = idx ^ 1;
    auto& wait = waiting_[idx / 2];
    std::optional<TimeMs> diff;
    Key from = event.key, to = static_cast<Key>(other);
    if (event.down) {
        assessment_press_times_[idx] = event.time_ms;
        if (wait.waiting && (static_cast<std::size_t>(wait.released) ^ 1) == idx) {
            diff = event.time_ms - wait.time_ms;
            from = wait.released;
            to = event.key;
            wait = {};
        }
    } else if (movement_pressed_[other]) {
        diff = assessment_press_times_[other] - event.time_ms;
    } else {
        wait = {true, event.key, event.time_ms};
    }
    if (!diff || std::abs(*diff) > 120 || event.time_ms - last_assessment_ms_ < 50) return;
    *diff = round1(*diff);
    Assessment record{event.time_ms, from, to, *diff,
        std::abs(*diff) <= 2 ? "perfect" : (*diff < 0 ? "early" : "late"),
        std::abs(*diff) <= 2, std::abs(*diff) <= 10};
    last_assessment_ms_ = event.time_ms;
    append(assessments_, record);
    output.assessments.push_back(record);
}
void Engine::advance(TimeMs time) {
    if (movement_time_ms_ < 0) { movement_time_ms_ = time; return; }
    if (time <= movement_time_ms_) return;
    const double dt = static_cast<double>(time - movement_time_ms_) / 1000.0;
    const double previous = std::hypot(velocity_x_, velocity_y_);
    axis_conflict_ = false;
    auto axis = [&](std::size_t neg, std::size_t pos) {
        if (movement_pressed_[neg] && movement_pressed_[pos]) {
            axis_conflict_ = true;
            return press_times_[pos] >= press_times_[neg] ? 1 : -1;
        }
        return static_cast<int>(movement_pressed_[pos]) - static_cast<int>(movement_pressed_[neg]);
    };
    auto x = advance_axis(velocity_x_, axis(2, 3), dt);
    auto y = advance_axis(velocity_y_, axis(0, 1), dt);
    counter_strafe_ = x.counter || y.counter;
    const double speed = std::hypot(velocity_x_, velocity_y_);
    if (speed > kThreshold) stop_time_ms_.reset();
    else if (previous > kThreshold) {
        double cross = dt * std::clamp((previous - kThreshold) / (previous - speed), 0.0, 1.0);
        if (x.cross || y.cross) cross = std::min(x.cross.value_or(dt), y.cross.value_or(dt));
        stop_time_ms_ = static_cast<double>(movement_time_ms_) + std::clamp(cross, 0.0, dt) * 1000.0;
    } else if (!stop_time_ms_) stop_time_ms_ = static_cast<double>(time);
    movement_time_ms_ = time;
}
void Engine::fire_down(TimeMs time) {
    scheduler_active_ = true;
    fire_down_ms_ = time;
    first_due_ms_ = time + 18;
    first_emitted_ = false;
    last_sample_ms_ = -1;
    sequence_index_ = 0;
}
std::optional<TimeMs> Engine::next_due() const {
    if (!scheduler_active_) return std::nullopt;
    return first_emitted_ ? std::max(last_sample_ms_ + 100, fire_down_ms_ + 90) : first_due_ms_;
}
std::optional<Shot> Engine::process(TimeMs time) {
    // 忠实保留上游：30 秒/300 次保护在持续按住时重新开始一个采样序列。
    if (scheduler_active_ && ((first_emitted_ && sequence_index_ >= 300) || time - fire_down_ms_ >= 30000)) {
        if (fire_pressed_) fire_down(time);
        else { scheduler_active_ = false; first_emitted_ = true; return std::nullopt; }
    }
    const auto due = next_due();
    if (!due || time + 0.001 < *due) return std::nullopt;
    advance(*due);
    const bool held = first_emitted_;
    first_emitted_ = true;
    last_sample_ms_ = *due;
    ++sequence_index_;
    return emit(*due, held);
}
Shot Engine::emit(TimeMs time, bool held) {
    Shot result;
    result.time_ms = time;
    result.sequence_index = std::max(1u, sequence_index_);
    result.held = held;
    result.delayed = !held;
    result.crouching = crouching_;
    result.crouch_grace = !crouching_ && crouch_release_ms_ && time - *crouch_release_ms_ <= 45;
    result.axis_conflict = axis_conflict_;
    result.counter_strafe = counter_strafe_;
    for (bool pressed : movement_pressed_) result.movement_keys_down += pressed ? 1u : 0u;
    const double speed = std::hypot(velocity_x_, velocity_y_);
    double ratio = speed / kThreshold;
    result.estimated_speed = round3(speed);
    result.accuracy_threshold = kThreshold;
    result.stop_success_age_ms = stop_time_ms_ ? round1(std::max(0.0, time - *stop_time_ms_)) : 0;
    result.timing_diff_ms = stop_time_ms_ ? round1(time - *stop_time_ms_) : -std::max(0.0, ratio - 1.0) * 100;
    if (result.crouch_grace || crouching_) {
        ratio = 0;
        result.reason = result.crouch_grace ? "crouch_grace" : "crouching";
    } else {
        if (crouch_release_ms_ && ratio > 1) {
            const double blend = std::clamp((static_cast<double>(time - *crouch_release_ms_) - 45) / 90, 0.0, 1.0);
            ratio = 1 + (ratio - 1) * blend;
        }
        bool press_in_window = false;
        for (std::size_t i = 0; i < 4; ++i)
            if (movement_pressed_[i] && press_times_[i] && time - *press_times_[i] <= 180) press_in_window = true;
        if (press_in_window && result.movement_keys_down && !axis_conflict_ && ratio > 1 && ratio <= 2) ratio = 1;
        if (ratio <= 1) {
            const bool recent = last_movement_ms_ && time - *last_movement_ms_ <= 180;
            result.reason = result.movement_keys_down || recent ? "low_speed_movement" : "no_movement";
        } else {
            result.reason = axis_conflict_ ? "axis_conflict" :
                (result.movement_keys_down && !counter_strafe_ ? "single_direction_held" :
                (counter_strafe_ ? "counter_strafe_braking" : "natural_deceleration"));
            result.error = std::clamp((ratio - 1) / 0.5, 0.0, 1.0);
        }
    }
    result.speed_ratio = round3(ratio);
    // 上游稳定判断使用已保留三位的比值和未舍入的误差。
    result.stable = result.speed_ratio <= 1 && result.error <= 0.35 && !result.crouch_grace;
    result.error = round3(result.error);
    append(shots_, result);
    return result;
}
Output Engine::handle_event(Event event) {
    Output output;
    if (static_cast<unsigned>(event.key) > static_cast<unsigned>(Key::Space)) {
        output.accepted = false; output.error = "未知按键"; return output;
    }
    if (!validate_time(event.time_ms, output)) return output;
    const auto idx = static_cast<std::size_t>(event.key);
    bool fire_up_edge = false;
    if (idx < 4) {
        assess(event, output);
        if (event.down != movement_pressed_[idx]) last_movement_ms_ = event.time_ms;
        if (event.down && !movement_pressed_[idx]) press_times_[idx] = event.time_ms;
        if (!event.down) press_times_[idx].reset();
        movement_pressed_[idx] = event.down;
        if (!event.down && movement_pressed_[idx ^ 1]) press_times_[idx ^ 1] = event.time_ms;
    } else if (event.key == Key::Crouch) {
        if (crouching_ && !event.down) crouch_release_ms_ = event.time_ms;
        if (event.down) crouch_release_ms_.reset();
        crouching_ = event.down;
    } else if (event.key == Key::Fire) {
        if (event.down) {
            if (!fire_pressed_ || !(scheduler_active_ && !first_emitted_)) fire_down(event.time_ms);
            fire_pressed_ = true;
        } else if (fire_pressed_) { fire_pressed_ = false; fire_up_edge = true; }
    } else {
        // 默认 Basic 绑定未给 Shift/Space 任何运动物理含义；它们也不推进上游引擎。
        if (event.key == Key::Shift) shift_pressed_ = event.down;
        else space_pressed_ = event.down;
        return output;
    }
    // 故意保留上游事件顺序，不能在移植中偷偷修正为旧键态积分。
    advance(event.time_ms);
    if (fire_up_edge) {
        if (scheduler_active_ && !first_emitted_) {
            advance(first_due_ms_);
            output.shots.push_back(emit(first_due_ms_, false));
            first_emitted_ = true;
            last_sample_ms_ = first_due_ms_;
            sequence_index_ = 1;
        }
        scheduler_active_ = false;
    }
    if (output.shots.empty()) if (auto shot = process(event.time_ms)) output.shots.push_back(*shot);
    return output;
}
Output Engine::tick(TimeMs time) {
    Output output;
    if (!validate_time(time, output)) return output;
    while (auto shot = process(time)) output.shots.push_back(*shot);
    advance(time);
    return output;
}
bool Engine::reset(TimeMs time) {
    Output output;
    if (!validate_time(time, output)) return false;
    auto assessments = std::move(assessments_);
    auto shots = std::move(shots_);
    const auto last_assessment = last_assessment_ms_;
    *this = Engine{};
    assessments_ = std::move(assessments);
    shots_ = std::move(shots);
    last_assessment_ms_ = last_assessment;
    last_external_ms_ = movement_time_ms_ = time;
    return true;
}
Snapshot Engine::snapshot() const {
    return {velocity_x_, velocity_y_, fire_pressed_, shift_pressed_, space_pressed_, next_due(), assessments_, shots_};
}
}
