#include "auto_stop/auto_stop.h"

#include <algorithm>

const char* AutoStopStatusName(AutoStopStatus status) noexcept {
    switch (status) {
        case AutoStopStatus::DISABLED: return "DISABLED";
        case AutoStopStatus::UNSUPPORTED_BACKEND: return "UNSUPPORTED_BACKEND";
        case AutoStopStatus::UNBOUND: return "UNBOUND";
        case AutoStopStatus::AWAITING_VALIDATION: return "AWAITING_VALIDATION";
        case AutoStopStatus::PAUSED: return "PAUSED";
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
