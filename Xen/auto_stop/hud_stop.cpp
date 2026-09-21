#include "auto_stop/hud_stop.h"
#include "reference_assessment/basic_motion_internal.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
std::array<int, 2> axes(std::uint8_t mask) noexcept {
    return {int(bool(mask & 1)) - int(bool(mask & 4)),
            int(bool(mask & 8)) - int(bool(mask & 2))};
}
bool conflict(std::uint8_t mask) noexcept { return (mask & 5) == 5 || (mask & 10) == 10; }
}
HudStopController::HudStopController(const AutoStopConfig& config) noexcept
    : after_release_ns_(std::int64_t(config.shot_after_release_ms) * 1000000),
      timing_valid_(config.shot_after_release_ms >= 0 && config.shot_after_release_ms <= 200) {}

bool HudStopController::active() const noexcept {
    return decision_.phase == AutoStopPhase::WAITING_ACK || decision_.phase == AutoStopPhase::BRAKING ||
        decision_.phase == AutoStopPhase::SETTLING;
}
void HudStopController::invalidate() noexcept {
    synchronized_ = false;
    decision_.phase = AutoStopPhase::INVALID;
    decision_.desired_mask = 0;
    decision_.axis_deadline_ns = {};
    decision_.completion_ready_ns = 0;
    ++decision_.command_id;
}
void HudStopController::issue(std::uint8_t mask) noexcept {
    decision_.phase = AutoStopPhase::WAITING_ACK;
    decision_.desired_mask = mask;
    decision_.axis_deadline_ns = {};
    ++decision_.command_id;
}
bool HudStopController::valid_intent(const WasdMotionIntent& intent, std::int64_t now) const noexcept {
    if (!intent.input_continuous || intent.conflicting || intent.held_mask > 15 || conflict(intent.held_mask) ||
        intent.epoch == 0 || intent.sequence == 0 || intent.received_at_ns <= 0 ||
        intent.received_at_ns > now || now <= 0) return false;
    const auto direction = axes(intent.held_mask);
    if (intent.longitudinal != direction[0] || intent.horizontal != direction[1]) return false;
    if (intent.history_valid) for (unsigned key = 0; key < 4; ++key) {
        const auto edge = intent.held_since_ns[key];
        if ((intent.held_mask & (1U << key)) ? edge <= 0 || edge > intent.received_at_ns : edge != 0) return false;
    }
    return true;
}
bool HudStopController::ordered(const WasdMotionIntent& intent) const noexcept {
    if (intent.epoch < seen_epoch_ || intent.received_at_ns < seen_time_ns_) return false;
    if (intent.epoch != seen_epoch_) return true;
    if ((intent.sequence == seen_sequence_ && input_.sequence != seen_sequence_) ||
        intent.sequence < seen_sequence_ ||
        (seen_sequence_ && intent.sequence > seen_sequence_ && intent.sequence - seen_sequence_ > 1)) return false;
    if (intent.sequence == input_.sequence && intent.epoch == input_.epoch &&
        (intent.held_mask != input_.held_mask || intent.received_at_ns != input_.received_at_ns ||
         intent.held_since_ns != input_.held_since_ns)) return false;
    return true;
}
void HudStopController::remember(const WasdMotionIntent& intent) noexcept {
    if (intent.epoch > seen_epoch_) { seen_epoch_ = intent.epoch; seen_sequence_ = intent.sequence; }
    else if (intent.epoch == seen_epoch_) seen_sequence_ = std::max(seen_sequence_, intent.sequence);
    seen_time_ns_ = std::max(seen_time_ns_, intent.received_at_ns);
}
bool HudStopController::advance(std::int64_t now) noexcept {
    if (now <= 0 || now < time_ns_) { invalidate(); return false; }
    if (synchronized_ && time_ns_ != 0) {
        const auto target = axes(output_started_ ? applied_mask_ : input_.held_mask);
        const double dt = double(now - time_ns_) / 1e9;
        for (unsigned axis = 0; axis < 2; ++axis)
            xen::reference_assessment::detail::advance_axis(telemetry_.velocity[axis], target[axis], dt);
    }
    time_ns_ = now;
    return true;
}
AutoStopDecision HudStopController::observe(const WasdMotionIntent& intent, std::int64_t now) noexcept {
    const bool epoch_changed = input_.epoch && input_.epoch != intent.epoch;
    if (!valid_intent(intent, now) || !ordered(intent) || now < time_ns_) {
        remember(intent); invalidate(); return decision_;
    }
    remember(intent);
    const bool changed = intent.held_mask != input_.held_mask;
    if (epoch_changed || !synchronized_) {
        const bool was_output = active() || output_started_;
        invalidate();
        input_ = intent;
        // 首次持键不具备历史，必须真实全松；清理后持续持键走显式重建接口。
        if (!was_output && intent.held_mask == 0 && intent.history_valid) {
            telemetry_ = {}; synchronized_ = true; applied_mask_ = 0;
            time_ns_ = intent.received_at_ns;
            decision_.phase = AutoStopPhase::IDLE;
        }
        return decision_;
    }
    if (intent.sequence == input_.sequence) return decision_;
    if (active() && changed) {
        // 接管后物理变化只取消计划，不能驱动被屏蔽的模型。
        if (intent.received_at_ns >= time_ns_) advance(intent.received_at_ns);
        input_ = intent;
        decision_.phase = AutoStopPhase::CANCELLED;
        decision_.desired_mask = 0; decision_.axis_deadline_ns = {};
        decision_.completion_ready_ns = 0; ++decision_.command_id;
        return decision_;
    }
    if (intent.received_at_ns < time_ns_ && !changed) { input_ = intent; return decision_; }
    if (!advance(intent.received_at_ns)) return decision_;
    input_ = intent;
    return decision_;
}
std::uint8_t HudStopController::reverse_mask() const noexcept {
    std::uint8_t mask = 0;
    if (telemetry_.velocity[0] > 0) mask |= 4; else if (telemetry_.velocity[0] < 0) mask |= 1;
    if (telemetry_.velocity[1] > 0) mask |= 2; else if (telemetry_.velocity[1] < 0) mask |= 8;
    return mask;
}
AutoStopDecision HudStopController::request(std::uint64_t id, std::int64_t now) noexcept {
    if (id == 0 || id <= request_watermark_ || active() || output_started_ ||
        decision_.phase == AutoStopPhase::CANCELLED) return decision_;
    request_watermark_ = id;
    if (!timing_valid_ || !synchronized_ || !advance(now)) { invalidate(); return decision_; }
    decision_.request_id = id; decision_.completion_ready_ns = 0;
    telemetry_.planned_ms = {}; deadlines_ = {};
    if (reverse_mask() == 0) { invalidate(); return decision_; }
    initial_zero_ = true;
    issue(0);
    return decision_;
}
AutoStopDecision HudStopController::request_manual_release(
        std::uint64_t id, std::uint8_t released, std::int64_t now) noexcept {
    if (released == 0 || released > 15 || conflict(released) || input_.held_mask != 0 ||
        !input_.history_valid || !input_.input_continuous) return decision_;
    return request(id, now);
}
AutoStopDecision HudStopController::tick(std::int64_t now) noexcept {
    // IDLE 由事件时间积分；轮询不能先于尚在队列中的真实边沿推进模型。
    if (!active()) { if (now <= 0 || now < time_ns_) invalidate(); return decision_; }
    if (!advance(now)) return decision_;
    if (decision_.phase == AutoStopPhase::SETTLING) {
        if (now >= decision_.completion_ready_ns) decision_.phase = AutoStopPhase::COMPLETE_ESTIMATED;
        return decision_;
    }
    if (decision_.phase != AutoStopPhase::BRAKING) return decision_;
    auto mask = applied_mask_;
    if (deadlines_[0] && now >= deadlines_[0]) mask = std::uint8_t(mask & ~5U);
    if (deadlines_[1] && now >= deadlines_[1]) mask = std::uint8_t(mask & ~10U);
    if (mask != applied_mask_) issue(mask);
    return decision_;
}
AutoStopDecision HudStopController::acknowledge(std::uint64_t id, std::uint64_t command,
        std::uint8_t mask, std::int64_t now) noexcept {
    if (decision_.phase != AutoStopPhase::WAITING_ACK || id != decision_.request_id ||
        command != decision_.command_id || mask != decision_.desired_mask) return decision_;
    if (!advance(now)) return decision_;
    const auto previous_mask = applied_mask_;
    output_started_ = true; applied_mask_ = mask;
    if (initial_zero_) {
        initial_zero_ = false;
        const auto reverse = reverse_mask();
        if (reverse == 0) { invalidate(); return decision_; }
        issue(reverse); return decision_;
    }
    if (mask == 0) {
        if (now > std::numeric_limits<std::int64_t>::max() - after_release_ns_) { invalidate(); return decision_; }
        decision_.phase = AutoStopPhase::SETTLING;
        decision_.completion_ready_ns = now + after_release_ns_;
        decision_.axis_deadline_ns = {};
        return tick(now);
    }
    const auto target = axes(mask);
    for (unsigned axis = 0; axis < 2; ++axis) {
        if (target[axis] == 0) { deadlines_[axis] = 0; continue; }
        const auto axis_bits = axis == 0 ? 5U : 10U;
        if ((previous_mask & axis_bits) == (mask & axis_bits) && deadlines_[axis]) continue;
        const auto speed = std::abs(telemetry_.velocity[axis]);
        // 上游 tiny 分支不是14：abs(v)<=.001的反向仍以5.5推进至零。
        const double rate = speed > .001 && target[axis] * telemetry_.velocity[axis] < 0 ? 14.0 : 5.5;
        const double seconds = target[axis] * telemetry_.velocity[axis] < 0 ? speed / rate : 0;
        const auto duration = std::int64_t(std::ceil(seconds * 1e9));
        telemetry_.planned_ms[axis] = seconds * 1000.0;
        if (now > std::numeric_limits<std::int64_t>::max() - duration) { invalidate(); return decision_; }
        deadlines_[axis] = now + duration;
    }
    decision_.phase = AutoStopPhase::BRAKING;
    decision_.axis_deadline_ns = deadlines_;
    return tick(now);
}
AutoStopDecision HudStopController::cancel(std::uint64_t id, std::int64_t now) noexcept {
    if (id == 0 || id != decision_.request_id) return decision_;
    if (!advance(now)) return decision_;
    decision_.phase = AutoStopPhase::CANCELLED; decision_.desired_mask = 0;
    decision_.axis_deadline_ns = {}; decision_.completion_ready_ns = 0;
    ++decision_.command_id;
    return decision_;
}
bool HudStopController::restart_after_cleanup(const WasdMotionIntent& intent, std::int64_t cleanup) noexcept {
    // 接管期间由 worker 逐事件检查连续性；此处允许其已核验快照跨多步序号。
    if (!timing_valid_ || !valid_intent(intent, cleanup) || cleanup < time_ns_ ||
        intent.epoch < seen_epoch_ || intent.received_at_ns < seen_time_ns_ ||
        (input_.epoch && intent.epoch != input_.epoch) ||
        (intent.epoch == seen_epoch_ && intent.sequence < seen_sequence_) ||
        (intent.epoch == input_.epoch && intent.sequence == input_.sequence &&
         (intent.held_mask != input_.held_mask || intent.received_at_ns != input_.received_at_ns ||
          intent.held_since_ns != input_.held_since_ns))) return false;
    const bool trusted = synchronized_;
    if (!advance(cleanup)) return false;
    if (!trusted) {
        const auto direction = axes(intent.held_mask);
        // 明确的实验适配：历史已失信但清理ACK及当前方向连续时按逐轴上界重建。
        // 这是保守估计，不是旧完成的继承，也不是实际停稳证据。
        telemetry_.velocity = {double(direction[0]), double(direction[1])};
        telemetry_.seeded = intent.held_mask != 0;
    }
    remember(intent); input_ = intent;
    synchronized_ = true; output_started_ = false; applied_mask_ = 0; initial_zero_ = false;
    const auto command = decision_.command_id;
    decision_ = {}; decision_.command_id = command;
    deadlines_ = {}; telemetry_.planned_ms = {};
    return true;
}
bool HudStopController::resume_after_masked_hold(const WasdMotionIntent& intent, std::int64_t released) noexcept {
    if (decision_.phase != AutoStopPhase::COMPLETE_ESTIMATED || !synchronized_ ||
        !output_started_ || applied_mask_ != 0 || decision_.desired_mask != 0) return false;
    return restart_after_cleanup(intent, released);
}
