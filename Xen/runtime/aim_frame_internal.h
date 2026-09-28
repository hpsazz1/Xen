#ifndef RUNTIME_AIM_FRAME_INTERNAL_H
#define RUNTIME_AIM_FRAME_INTERNAL_H

#include <limits>
#include "auto_stop/auto_stop_worker.h"
#include "runtime/camera_motion_internal.h"
#include "runtime/runtime_internal.h"
#include "runtime/weapon_context_internal.h"
#include "runtime/team_filter_internal.h"

namespace runtime::detail {

struct PreparedAimFrame {
    AimFrame frame;
    bool reset_aim = false;
    double background_motion_ms = 0.0;
};

struct AimWeaponSessionDecision {
    bool allowed = false;
    bool reset_aim = false;
    std::uint64_t generation = 0;
    weapon::Team team = weapon::Team::UNKNOWN;
    std::uint64_t team_epoch = 0;
    bool team_valid = false;
    weapon::GamePhase game_phase = weapon::GamePhase::UNKNOWN;
};

// 仅由 pipeline 线程推进。普通武器变化撤销旧目标/命令；信任断点才撤销持键恢复资格。
class AimWeaponSessionGate final {
public:
    AimWeaponSessionDecision update(const weapon::WeaponSnapshot& weapon, bool weapon_required,
            const source_context::SourceContextSnapshot& source, bool focus_required,
            bool hold_active, weapon::Clock::time_point now, bool team_filter_required = false) {
        const bool team_allowed = !team_filter_required || (weapon_required && team_context_valid(weapon, now));
        const auto team_epoch = team_filter_required ? weapon.team_epoch : 0;
        const auto team = team_filter_required ? weapon.team : weapon::Team::UNKNOWN;
        const auto phase = team_filter_required ? weapon.game_phase : weapon::GamePhase::UNKNOWN;
        const bool trusted = !weapon_required || weapon_session_trusted(weapon, now);
        const bool ready = !weapon_required || weapon_ready(weapon, now);
        const bool focused = !focus_required || (source.available && source.focused && source.session_id != 0);
        const auto trust_epoch = weapon_required ? weapon.control_safety_epoch : 0;
        const auto weapon_epoch = weapon_required ? weapon.source_epoch : 0;
        const auto focus_session = focus_required ? source.session_id : 0;
        const std::string_view weapon_id = weapon_required ? weapon.canonical_id : std::string_view{};
        const bool trust_changed = initialized_ && (trust_epoch_ != trust_epoch || focus_session_ != focus_session);
        const bool previous_release_required = release_required_;
        if (!trusted || !focused || trust_changed) release_required_ = true;
        // 不健康期间看到的松键不作为新的许可；信任恢复后才接受真实松键。
        if (trusted && focused && !hold_active) release_required_ = false;
        const bool changed = !initialized_ || ready_ != ready || trusted_ != trusted || focused_ != focused ||
            weapon_epoch_ != weapon_epoch || weapon_id_ != weapon_id || trust_changed ||
            previous_release_required != release_required_ || team_allowed_ != team_allowed ||
            team_epoch_ != team_epoch || team_ != team || game_phase_ != phase;
        if (changed) {
            if (generation_ == std::numeric_limits<std::uint64_t>::max()) exhausted_ = true;
            else ++generation_;
        }
        initialized_ = true;
        ready_ = ready; trusted_ = trusted; focused_ = focused;
        weapon_epoch_ = weapon_epoch; trust_epoch_ = trust_epoch; focus_session_ = focus_session;
        weapon_id_ = weapon_id;
        team_allowed_ = team_allowed; team_epoch_ = team_epoch; team_ = team; game_phase_ = phase;
        return {ready && trusted && focused && team_allowed && !release_required_ && !exhausted_,
            changed, generation_, team, team_epoch, team_allowed, phase};
    }
private:
    bool initialized_ = false, ready_ = false, trusted_ = false, focused_ = false;
    bool release_required_ = false, exhausted_ = false;
    std::uint64_t weapon_epoch_ = 0, trust_epoch_ = 0, focus_session_ = 0, generation_ = 0;
    std::string weapon_id_;
    bool team_allowed_ = true;
    std::uint64_t team_epoch_ = 0;
    weapon::Team team_ = weapon::Team::UNKNOWN;
    weapon::GamePhase game_phase_ = weapon::GamePhase::UNKNOWN;
};

// 新按键许可不能追溯激活按未输出状态计算的旧帧；当前撤销仍立即生效。
inline bool aim_frame_dispatch_allowed(const AimFrame& frame, bool current_permission) noexcept {
    return frame.lock_active && current_permission;
}

inline bool aim_frame_dispatch_allowed(const AimFrame& frame, bool current_permission,
        const AimWeaponSessionDecision& frame_session, const AimWeaponSessionDecision& current_session) noexcept {
    return aim_frame_dispatch_allowed(frame, current_permission) && frame_session.allowed &&
        current_session.allowed && frame_session.generation == current_session.generation;
}

inline std::chrono::steady_clock::time_point aim_output_slot_deadline(
        const AimFrame& frame, std::chrono::milliseconds backend_budget) noexcept {
    return std::min(frame.control_at + backend_budget, frame.captured_at + AimFrame::kObservationHorizon);
}

struct AimOutputSlot {
    std::unique_lock<std::timed_mutex> guard;
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
    AimDispatchRejectionRecord rejection;
    bool reset_aim_only = false;
};

inline void reject_aim_output(AimFrame& frame, SafetyGate& gate, AimOutputSlot& slot,
        AimDispatchRejection reason, std::chrono::steady_clock::time_point now) noexcept {
    slot.rejection.sequence = frame.sequence;
    slot.rejection.reason = reason;
    slot.rejection.observation_age_ms = std::chrono::duration<double, std::milli>(now - frame.captured_at).count();
    frame.lock_active = false;
    if (reason == AimDispatchRejection::OUTPUT_FAULT) gate.emergency_stop();
    // 普通时限拒绝只约束本观察；既有硬阻断既不新增，也不清除。
    else slot.reset_aim_only = true;
}

inline void finish_aim_output_slot(Aim& aim, const AimOutputSlot& slot) noexcept {
    // 必须在本帧未发送回执结算后调用。拒绝旧帧不是图像源切换，
    // 不能重置 CameraMotionEstimator 或重建 Trigger 观察世代。
    if (slot.reset_aim_only) aim.reset();
}

// 同一准入入口供 Runtime 与无设备回归使用；锁仍覆盖计算和回执。
inline AimOutputSlot acquire_aim_output_slot(AimFrame& frame, SafetyGate& gate,
        AutoStopOutputArbiter& arbiter, std::chrono::milliseconds backend_budget) noexcept {
    AimOutputSlot slot;
    if (!aim_frame_dispatch_allowed(frame, gate.can_dispatch())) return slot;
    slot.deadline = aim_output_slot_deadline(frame, backend_budget);
    const auto started = std::chrono::steady_clock::now();
    OutputArbiterRejection reason = OutputArbiterRejection::NONE;
    slot.guard = arbiter.enter_aim_until(slot.deadline, &reason);
    const auto finished = std::chrono::steady_clock::now();
    slot.rejection.wait_ms = std::chrono::duration<double, std::milli>(finished - started).count();
    if (!slot.guard.owns_lock() || finished > slot.deadline) {
        const auto classified = reason == OutputArbiterRejection::OUTPUT_FAULT
            ? AimDispatchRejection::OUTPUT_FAULT
            : started >= slot.deadline ? AimDispatchRejection::ENTRY_DEADLINE_EXPIRED
            : slot.guard.owns_lock() ? AimDispatchRejection::ACQUIRED_DEADLINE_EXPIRED
            : AimDispatchRejection::WAIT_DEADLINE_EXPIRED;
        reject_aim_output(frame, gate, slot, classified, finished);
    }
    return slot;
}

inline bool aim_output_fresh_before_send(AimFrame& frame, SafetyGate& gate,
        AimOutputSlot& slot, std::chrono::steady_clock::time_point now) noexcept {
    if (now <= slot.deadline) return true;
    reject_aim_output(frame, gate, slot, AimDispatchRejection::COMPUTE_DEADLINE_EXPIRED, now);
    return false;
}

inline void record_aim_dispatch_rejection(AimDispatchRejectionSummary& summary,
        const AimDispatchRejectionRecord& record) noexcept {
    if (record.reason == AimDispatchRejection::NONE) return;
    const auto increment = [](std::uint64_t& value) {
        if (value != (std::numeric_limits<std::uint64_t>::max)()) ++value;
    };
    if (summary.total == 0) summary.first = record;
    increment(summary.total);
    summary.last = record;
    switch (record.reason) {
    case AimDispatchRejection::ENTRY_DEADLINE_EXPIRED: increment(summary.entry_deadline_expired); break;
    case AimDispatchRejection::WAIT_DEADLINE_EXPIRED: increment(summary.wait_deadline_expired); break;
    case AimDispatchRejection::ACQUIRED_DEADLINE_EXPIRED: increment(summary.acquired_deadline_expired); break;
    case AimDispatchRejection::COMPUTE_DEADLINE_EXPIRED: increment(summary.compute_deadline_expired); break;
    case AimDispatchRejection::OUTPUT_FAULT: increment(summary.output_fault); break;
    default: break;
    }
}

// Runtime 与离线验证共用实际组装入口。measure_background 的 false 仅供
// 无输出性能基线，不暴露为产品配置；生产调用始终使用默认值。
inline PreparedAimFrame prepare_aim_frame(
        const CapturedFrame& captured, std::vector<Detection> detections,
        RuntimeObservationClock& clock, CameraMotionEstimator& estimator,
        bool lock_active, bool measure_background = true) {
    PreparedAimFrame result;
    auto& frame = result.frame;
    result.reset_aim = clock.apply(captured.timing, frame);
    frame.roi_width = captured.width;
    frame.roi_height = captured.height;
    frame.control_center_x = static_cast<float>(
        (captured.source_width * 0.5 - captured.roi_x) /
        captured.source_pixels_per_pixel_x);
    frame.control_center_y = static_cast<float>(
        (captured.source_height * 0.5 - captured.roi_y) /
        captured.source_pixels_per_pixel_y);
    frame.source_pixels_per_roi_pixel_x =
        static_cast<float>(captured.source_pixels_per_pixel_x);
    frame.source_pixels_per_roi_pixel_y =
        static_cast<float>(captured.source_pixels_per_pixel_y);
    frame.lock_active = lock_active;
    frame.ease_first_activation = true;
    frame.detections = std::move(detections);
    if (measure_background) {
        const auto measured = estimator.observe(captured, frame, result.reset_aim);
        frame.observation_epoch = measured.observation_epoch;
        frame.background_motion_x = measured.motion;
        result.background_motion_ms = measured.elapsed_ms;
    }
    // 图像测量完成后才取控制时间，新增 CPU 成本必须进入真实帧龄。
    frame.control_at = std::chrono::steady_clock::now();
    return result;
}

} // namespace runtime::detail

#endif
