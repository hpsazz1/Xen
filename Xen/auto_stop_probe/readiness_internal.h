#ifndef AUTO_STOP_COUNTERPULSE_READINESS_INTERNAL_H
#define AUTO_STOP_COUNTERPULSE_READINESS_INTERNAL_H

#include <cstdint>
#include <nlohmann/json.hpp>
#include "mouse/mouse.h"
#include "source_context/source_context.h"

namespace auto_stop_probe_detail {
inline const char* counterpulse_monitor_status(InputMonitorStatus status) noexcept {
    switch (status) {
    case InputMonitorStatus::CLOSED: return "CLOSED";
    case InputMonitorStatus::UNVERIFIED: return "UNVERIFIED";
    case InputMonitorStatus::WAITING: return "WAITING";
    case InputMonitorStatus::READY: return "READY";
    case InputMonitorStatus::STALE: return "STALE";
    case InputMonitorStatus::FAILURE: return "FAILURE";
    default: return "UNKNOWN";
    }
}

inline nlohmann::json evaluate_counterpulse_readiness(
    const source_context::SourceContextSnapshot& focus, const InputSnapshot& input,
    bool poll_ok, bool cancelled) {
    // 只记录白名单：位0至8依次是W/A/S/D及五个鼠标键，不保存其他按键或配置。
    constexpr int keys[]{0x57, 0x41, 0x53, 0x44, 1, 2, 4, 5, 6};
    unsigned blocked = 0;
    for (unsigned i = 0; i < 9; ++i) if (input.virtual_keys[keys[i]]) blocked |= 1U << i;
    const bool end = poll_ok && input.state_valid && input.status == InputMonitorStatus::READY && input.virtual_keys[0x23];
    const bool cancel = cancelled || end;
    const bool identity_invalid = !focus.session_id || !focus.sequence;
    const bool monitor_waiting = poll_ok && input.status == InputMonitorStatus::WAITING;
    const bool monitor_invalid = !poll_ok || input.status != InputMonitorStatus::READY || !input.state_valid || !input.sequence;
    const bool ready = !cancel && focus.available && focus.focused && !identity_invalid && !monitor_invalid && !blocked;
    const char* reason = "READY";
    if (cancel) reason = "USER_CANCELLED";
    else if (!focus.available) reason = "SOURCE_UNAVAILABLE";
    else if (identity_invalid) reason = "SOURCE_IDENTITY_INVALID";
    else if (!focus.focused) reason = "SOURCE_NOT_FOCUSED";
    else if (!poll_ok) reason = "MONITOR_POLL_FAILED";
    else if (monitor_waiting) reason = "MONITOR_WAITING";
    else if (monitor_invalid) reason = "MONITOR_INVALID";
    else if (blocked) reason = "PHYSICAL_KEYS_HELD";
    return {{"ready", ready}, {"instant_ready", ready}, {"reason", reason},
        {"flags", {{"cancelled", cancel}, {"source_unavailable", !focus.available},
            {"source_not_focused", !focus.focused}, {"source_identity_invalid", identity_invalid},
            {"monitor_poll_failed", !poll_ok}, {"monitor_waiting", monitor_waiting},
            {"monitor_invalid", monitor_invalid}, {"physical_keys_held", blocked != 0}}},
        {"focus", {{"available", focus.available}, {"focused", focus.focused},
            {"session_id", focus.session_id}, {"sequence", focus.sequence}, {"age_ms", focus.age_ms}}},
        {"monitor", {{"poll_ok", poll_ok}, {"status", static_cast<int>(input.status)},
            {"code", counterpulse_monitor_status(input.status)}, {"state_valid", input.state_valid}, {"sequence", input.sequence}}},
        {"blocked_keys", blocked}};
}

class CounterpulseReadinessAccumulator {
public:
    // 连续许可窗口只防止瞬时准入，不证明人物静止，也不把change-only静默当心跳。
    static constexpr std::int64_t required_stable_ns = 300000000;
    nlohmann::json update(const source_context::SourceContextSnapshot& focus,
        const InputSnapshot& input, bool poll_ok, bool cancelled, std::int64_t now_ns) {
        if (!started_) { started_ = true; started_ns_ = last_ns_ = now_ns; }
        const bool regressed = now_ns < last_ns_;
        const bool identity_changed = session_ && session_ != focus.session_id;
        const bool sequence_regressed = !identity_changed &&
            ((focus.sequence && focus.sequence < focus_sequence_) || (input.sequence && input.sequence < input_sequence_));
        auto result = evaluate_counterpulse_readiness(focus, input, poll_ok, cancelled || cancelled_);
        cancelled_ = result["flags"]["cancelled"].get<bool>();
        const bool instant = result["instant_ready"].get<bool>();
        if (!instant || regressed || identity_changed || sequence_regressed) stable_ = false;
        if (instant && !regressed && !sequence_regressed) {
            if (!stable_) { stable_ = true; stable_since_ns_ = now_ns; }
        }
        const auto continuous = stable_ ? now_ns - stable_since_ns_ : 0;
        result["ready"] = instant && stable_ && continuous >= required_stable_ns;
        if (!cancelled_) {
            if (regressed) result["reason"] = "CLOCK_REGRESSION";
            else if (sequence_regressed) result["reason"] = "READINESS_SEQUENCE_REGRESSION";
            else if (instant && !result["ready"].get<bool>()) result["reason"] = "STABILIZING";
        }
        result["flags"]["clock_regression"] = regressed;
        result["flags"]["sequence_regression"] = sequence_regressed;
        result["flags"]["source_session_changed"] = identity_changed;
        result["elapsed_ns"] = now_ns >= started_ns_ ? now_ns - started_ns_ : 0;
        result["continuous_ready_ns"] = continuous;
        result["required_stable_ns"] = required_stable_ns;
        last_ns_ = now_ns;
        session_ = focus.session_id;
        focus_sequence_ = focus.sequence;
        input_sequence_ = input.sequence;
        return result;
    }
private:
    bool started_ = false, stable_ = false, cancelled_ = false;
    std::int64_t started_ns_ = 0, last_ns_ = 0, stable_since_ns_ = 0;
    std::uint64_t session_ = 0, focus_sequence_ = 0, input_sequence_ = 0;
};
}
#endif
