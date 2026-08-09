#ifndef AIM_PREDICTION_INTERNAL_H
#define AIM_PREDICTION_INTERNAL_H

#include <algorithm>
#include <cmath>

namespace aim::detail {

// prediction 的世界运动状态需要同时满足两个相反要求：运动幅值下降时快速
// 跟随新值，真实停止时快速归零。下降不等于停止；若把任何一次降幅都朝零
// 释放，检测框动画造成的短时速度低谷会让预测点周期性开关。
inline void update_prediction_motion_axis(
        float feedforward, float measurement, float delta_seconds,
        float minimum_counts, int release_confirm_frames,
        float build_gain_per_second, float release_gain_per_second,
        bool force_release, float& prediction_motion,
        int& low_measurement_frames) noexcept {
    if (std::fabs(measurement) <= minimum_counts) {
        low_measurement_frames = std::min(
            low_measurement_frames + 1, release_confirm_frames);
    } else {
        low_measurement_frames = 0;
    }

    const bool release_confirmed = force_release ||
        low_measurement_frames >= release_confirm_frames;
    const float feedforward_magnitude = std::fabs(feedforward);
    const float prediction_magnitude = std::fabs(prediction_motion);
    const bool same_direction = prediction_motion * feedforward > 0.0f;
    // 同向幅值下降时用快速增益贴近新的非零前馈，而不是错误地朝零释放。
    // 前馈已经低于噪声门槛时继续使用慢增益保持，直到原始测量满足连续
    // 停止确认；这样单帧框形变不会关闭已经建立的 prediction。
    const bool fast_follow_decrease =
        same_direction &&
        feedforward_magnitude > minimum_counts &&
        prediction_magnitude > minimum_counts &&
        feedforward_magnitude < prediction_magnitude;
    const float gain = release_confirmed || fast_follow_decrease
        ? release_gain_per_second : build_gain_per_second;
    const float target = release_confirmed ? 0.0f : feedforward;
    const float alpha = 1.0f - std::exp(-gain * delta_seconds);
    prediction_motion += (target - prediction_motion) * alpha;
}

} // namespace aim::detail

#endif // AIM_PREDICTION_INTERNAL_H
