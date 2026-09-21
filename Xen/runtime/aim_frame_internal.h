#ifndef RUNTIME_AIM_FRAME_INTERNAL_H
#define RUNTIME_AIM_FRAME_INTERNAL_H

#include "runtime/camera_motion_internal.h"
#include "runtime/runtime_internal.h"
#include "runtime/weapon_context_internal.h"

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
};

// 仅由 pipeline 线程推进。普通武器变化撤销旧目标/命令；信任断点才撤销持键恢复资格。
class AimWeaponSessionGate final {
public:
    AimWeaponSessionDecision update(const weapon::WeaponSnapshot& weapon, bool weapon_required,
            const source_context::SourceContextSnapshot& source, bool focus_required,
            bool hold_active, weapon::Clock::time_point now) {
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
            previous_release_required != release_required_;
        if (changed) {
            if (generation_ == std::numeric_limits<std::uint64_t>::max()) exhausted_ = true;
            else ++generation_;
        }
        initialized_ = true;
        ready_ = ready; trusted_ = trusted; focused_ = focused;
        weapon_epoch_ = weapon_epoch; trust_epoch_ = trust_epoch; focus_session_ = focus_session;
        weapon_id_ = weapon_id;
        return {ready && trusted && focused && !release_required_ && !exhausted_, changed, generation_};
    }
private:
    bool initialized_ = false, ready_ = false, trusted_ = false, focused_ = false;
    bool release_required_ = false, exhausted_ = false;
    std::uint64_t weapon_epoch_ = 0, trust_epoch_ = 0, focus_session_ = 0, generation_ = 0;
    std::string weapon_id_;
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
