#include "auto_stop/auto_stop.h"

#include <algorithm>
#include <cmath>
#include <limits>

const char* AutoStopStatusName(AutoStopStatus status) noexcept {
    switch (status) {
        case AutoStopStatus::DISABLED: return "DISABLED";
        case AutoStopStatus::UNSUPPORTED_BACKEND: return "UNSUPPORTED_BACKEND";
        case AutoStopStatus::UNBOUND: return "UNBOUND";
        case AutoStopStatus::AWAITING_VALIDATION: return "AWAITING_VALIDATION";
        case AutoStopStatus::PAUSED: return "PAUSED";
        case AutoStopStatus::READY: return "READY";
        case AutoStopStatus::WAITING_INPUT: return "WAITING_INPUT";
        case AutoStopStatus::BRAKING: return "BRAKING";
        case AutoStopStatus::ESTIMATED: return "ESTIMATED";
        case AutoStopStatus::CANCELED: return "CANCELED";
        case AutoStopStatus::FAULT: return "FAULT";
    }
    return "UNKNOWN";
}

AutoStopSnapshot assess_auto_stop_availability(
        const AutoStopConfig& config, bool kmbox_backend,
        bool protocol_available, bool paused) noexcept {
    AutoStopSnapshot snapshot;
    snapshot.device_protocol_available = kmbox_backend && protocol_available;
    if (!config.enabled) return snapshot;
    if (!kmbox_backend) {
        snapshot.status = AutoStopStatus::UNSUPPORTED_BACKEND;
    } else if (paused) {
        snapshot.status = AutoStopStatus::PAUSED;
    } else if (config.activation_virtual_key == 0) {
        snapshot.status = AutoStopStatus::UNBOUND;
    } else {
        snapshot.status = AutoStopStatus::AWAITING_VALIDATION;
    }
    return snapshot;
}

void WasdInputHistory::reset() noexcept {
    state_ = {};
    epoch_ = sequence_ = 0;
    received_at_ns_ = 0;
    synchronized_ = false;
}

WasdMotionIntent WasdInputHistory::observe(
        std::uint8_t held_mask, std::uint64_t epoch,
        std::uint64_t sequence, std::int64_t received_at_ns,
        bool input_valid, bool sequence_gap) noexcept {
    if (epoch == 0 || sequence == 0 || received_at_ns <= 0 || epoch < epoch_) {
        state_.history_valid = false;
        state_.held_since_ns = {};
        synchronized_ = false;
        return state_;
    }
    if (epoch != epoch_) {
        reset();
        epoch_ = epoch;
    }
    if (!input_valid || sequence_gap || held_mask > 15 ||
        sequence < sequence_ || received_at_ns < received_at_ns_ ||
        (sequence == sequence_ && held_mask != state_.held_mask)) {
        // 键态未知不等于事件未发生。保留已见水位，防止旧释放越过
        // 一个无效的新报告重新建立输入历史。
        sequence_ = std::max(sequence_, sequence);
        received_at_ns_ = std::max(received_at_ns_, received_at_ns);
        state_.history_valid = false;
        state_.held_since_ns = {};
        synchronized_ = false;
        return state_;
    }
    if (sequence == sequence_) return state_;
    const auto previous = state_.held_mask;
    sequence_ = sequence;
    received_at_ns_ = received_at_ns;
    state_.epoch = epoch;
    state_.sequence = sequence;
    state_.received_at_ns = received_at_ns;
    state_.held_mask = held_mask;
    state_.conflicting = (held_mask & 5U) == 5U ||
                         (held_mask & 10U) == 10U;
    state_.horizontal = ((held_mask & 8U) != 0) - ((held_mask & 2U) != 0);
    state_.longitudinal = ((held_mask & 1U) != 0) - ((held_mask & 4U) != 0);
    if (state_.conflicting) {
        synchronized_ = false;
        state_.held_since_ns = {};
    } else if (held_mask == 0) {
        synchronized_ = true;
        state_.held_since_ns = {};
    } else if (synchronized_) {
        for (unsigned index = 0; index < 4; ++index) {
            const auto bit = 1U << index;
            if ((held_mask & bit) == 0) state_.held_since_ns[index] = 0;
            else if ((previous & bit) == 0)
                state_.held_since_ns[index] = received_at_ns;
        }
    }
    state_.history_valid = synchronized_;
    return state_;
}

namespace {
std::array<double, 2> direction(std::uint8_t mask) noexcept {
    std::array<double, 2> value{double(bool(mask & 1)) - double(bool(mask & 4)),
                              double(bool(mask & 8)) - double(bool(mask & 2))};
    const double length = std::hypot(value[0], value[1]);
    if (length != 0) { value[0] /= length; value[1] /= length; }
    return value;
}
}
bool AutoStopController::active() const noexcept {
    return decision_.phase == AutoStopPhase::WAITING_ACK || decision_.phase == AutoStopPhase::BRAKING;
}
void AutoStopController::invalidate() noexcept {
    synchronized_ = false;
    decision_.phase = AutoStopPhase::INVALID;
    decision_.desired_mask = 0;
    decision_.axis_deadline_ns = {};
    ++decision_.command_id;
}
bool AutoStopController::advance(std::int64_t now_ns) noexcept {
    if (now_ns <= 0 || now_ns < time_ns_) { invalidate(); return false; }
    if (synchronized_ && time_ns_ != 0) {
        const auto target = direction(output_started_ ? applied_mask_ : input_.held_mask);
        const double dt = double(now_ns - time_ns_) / 1e9;
        for (int axis = 0; axis < 2; ++axis) {
            const double tau = target[axis] == 0 ? .110 :
                (target[axis] * state_[axis] < 0 ? .180 : .230);
            state_[axis] = target[axis] + (state_[axis] - target[axis]) * std::exp(-dt / tau);
        }
    }
    time_ns_ = now_ns;
    return true;
}
void AutoStopController::issue(std::uint8_t mask) noexcept {
    decision_.desired_mask = mask;
    decision_.phase = AutoStopPhase::WAITING_ACK;
    decision_.axis_deadline_ns = {};
    ++decision_.command_id;
}
void AutoStopController::deadlines(std::int64_t now_ns) noexcept {
    const auto target = direction(applied_mask_);
    decision_.axis_deadline_ns = {};
    for (int axis = 0; axis < 2; ++axis) if (target[axis] != 0) {
        const double seconds = target[axis] * state_[axis] < 0 ?
            .180 * std::log1p(std::abs(state_[axis] / target[axis])) : 0;
        const auto duration = static_cast<std::int64_t>(std::ceil(seconds * 1e9));
        if (now_ns > std::numeric_limits<std::int64_t>::max() - duration) { invalidate(); return; }
        decision_.axis_deadline_ns[axis] = now_ns + duration;
    }
}
AutoStopDecision AutoStopController::observe(const WasdMotionIntent& intent, std::int64_t now_ns) noexcept {
    const bool changed = intent.held_mask != input_.held_mask;
    const bool epoch_changed = input_.epoch != 0 && intent.epoch != input_.epoch;
    bool edges_valid = true;
    for (unsigned key = 0; key < 4; ++key) {
        const auto edge = intent.held_since_ns[key];
        if ((intent.held_mask & (1U << key)) ? edge <= 0 || edge > intent.received_at_ns : edge != 0)
            edges_valid = false;
    }
    if (!intent.history_valid || intent.conflicting || intent.epoch == 0 || intent.sequence == 0 ||
        !edges_valid || intent.longitudinal != int(bool(intent.held_mask & 1)) - int(bool(intent.held_mask & 4)) ||
        intent.horizontal != int(bool(intent.held_mask & 8)) - int(bool(intent.held_mask & 2)) ||
        intent.received_at_ns <= 0 || intent.received_at_ns > now_ns || intent.held_mask > 15 ||
        (intent.held_mask & 5) == 5 || (intent.held_mask & 10) == 10 ||
        (input_.epoch && intent.epoch < input_.epoch) ||
        (!epoch_changed && (intent.sequence < input_.sequence ||
                           (synchronized_ && input_.sequence != 0 && intent.sequence > input_.sequence &&
                            intent.sequence - input_.sequence > 1) ||
                           intent.received_at_ns < input_.received_at_ns ||
                           (intent.sequence == input_.sequence && changed)))) {
        invalidate(); return decision_;
    }
    if (epoch_changed || !synchronized_) {
        const bool was_active = active();
        invalidate();
        input_ = intent;
        if (now_ns <= 0 || now_ns < time_ns_) return decision_;
        if (was_active) return decision_;
        // 缺口后已持键不补历史，只有明确释放可以开始新的意图估计。
        if (intent.held_mask == 0) {
            state_ = {}; synchronized_ = true; output_started_ = false; applied_mask_ = 0;
            time_ns_ = intent.received_at_ns;
            decision_.phase = AutoStopPhase::IDLE;
        }
        return decision_;
    }
    if (intent.sequence == input_.sequence) {
        return decision_;
    }
    if (active() && changed) {
        // 输入队列可以晚于一次轮询交付。真实改向先取消，不将旧边沿夹到now伪造时间。
        input_ = intent;
        decision_.phase = AutoStopPhase::CANCELLED;
        decision_.desired_mask = 0; decision_.axis_deadline_ns = {};
        ++decision_.command_id;
        synchronized_ = false;
        return decision_;
    }
    if (active() && intent.received_at_ns < time_ns_ && !changed) {
        input_ = intent;
        return decision_;
    }
    // 事件以接收时间积分；迟到到已推进时间之前的事件无法精确重放。
    if (!advance(intent.received_at_ns)) return decision_;
    input_ = intent;
    return decision_;
}
AutoStopDecision AutoStopController::request(std::uint64_t id, std::int64_t now_ns) noexcept {
    if (id == 0 || id <= request_watermark_ || active()) return decision_;
    request_watermark_ = id;
    if (!advance(now_ns) || !synchronized_) { invalidate(); return decision_; }
    decision_.request_id = id;
    std::uint8_t mask = 0;
    if (state_[0] > 0) mask |= 4; else if (state_[0] < 0) mask |= 1;
    if (state_[1] > 0) mask |= 2; else if (state_[1] < 0) mask |= 8;
    issue(mask);
    return decision_;
}
AutoStopDecision AutoStopController::tick(std::int64_t now_ns) noexcept {
    if (!active()) {
        if (now_ns <= 0 || now_ns < time_ns_) invalidate();
        return decision_;
    }
    if (!advance(now_ns)) return decision_;
    if (decision_.phase != AutoStopPhase::BRAKING) return decision_;
    auto mask = applied_mask_;
    if (decision_.axis_deadline_ns[0] && now_ns >= decision_.axis_deadline_ns[0]) mask = static_cast<std::uint8_t>(mask & ~5U);
    if (decision_.axis_deadline_ns[1] && now_ns >= decision_.axis_deadline_ns[1]) mask = static_cast<std::uint8_t>(mask & ~10U);
    if (mask != applied_mask_) issue(mask);
    return decision_;
}
AutoStopDecision AutoStopController::acknowledge(std::uint64_t id, std::uint64_t command,
        std::uint8_t mask, std::int64_t now_ns) noexcept {
    if (decision_.phase != AutoStopPhase::WAITING_ACK || id != decision_.request_id ||
        command != decision_.command_id || mask != decision_.desired_mask) return decision_;
    if (!advance(now_ns)) return decision_;
    output_started_ = true;
    applied_mask_ = mask;
    if (mask == 0) {
        decision_.phase = AutoStopPhase::COMPLETE_ESTIMATED;
        decision_.axis_deadline_ns = {};
        // 真实剩余速度未知；下一请求必须重新观察完整释放建立历史。
        synchronized_ = false;
    } else {
        decision_.phase = AutoStopPhase::BRAKING;
        deadlines(now_ns);
    }
    return decision_;
}
AutoStopDecision AutoStopController::cancel(std::uint64_t id, std::int64_t now_ns) noexcept {
    if (id != decision_.request_id || id == 0) return decision_;
    if (!advance(now_ns)) return decision_;
    decision_.phase = AutoStopPhase::CANCELLED;
    decision_.desired_mask = 0; decision_.axis_deadline_ns = {};
    ++decision_.command_id;
    synchronized_ = input_.history_valid && !input_.conflicting && input_.held_mask == 0;
    if (synchronized_) { state_ = {}; output_started_ = false; applied_mask_ = 0; }
    return decision_;
}
