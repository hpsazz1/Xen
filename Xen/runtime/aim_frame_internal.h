#ifndef RUNTIME_AIM_FRAME_INTERNAL_H
#define RUNTIME_AIM_FRAME_INTERNAL_H

#include "runtime/camera_motion_internal.h"
#include "runtime/runtime_internal.h"

namespace runtime::detail {

struct PreparedAimFrame {
    AimFrame frame;
    bool reset_aim = false;
    double background_motion_ms = 0.0;
};

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
