#ifndef AIM_PREDICTION_INTERNAL_H
#define AIM_PREDICTION_INTERNAL_H

#include <algorithm>
#include <cmath>

namespace aim::detail {

// prediction 的世界运动状态需要同时满足两个相反要求：持续运动期间抑制
// 检测框动画造成的速度起伏，真实停止时又能及时归零。非零降幅不能使用
// 停止释放增益，否则前探距离会跟随每帧框形变跳动；只有连续低测量才是
// 释放证据。
inline void update_prediction_motion_axis(
        float feedforward, float measurement, float delta_seconds,
        float minimum_counts, int release_confirm_frames,
        float build_gain_per_second, float release_gain_per_second,
        float& prediction_motion, int& low_measurement_frames) noexcept {
    if (std::fabs(measurement) <= minimum_counts) {
        low_measurement_frames = std::min(
            low_measurement_frames + 1, release_confirm_frames);
    } else {
        low_measurement_frames = 0;
    }

    const bool release_confirmed =
        low_measurement_frames >= release_confirm_frames;
    // 所有非零运动变化都使用同一低通增益。这样观察器估计的是持续世界
    // 运动，而不是逐帧复制基础前馈噪声；停止确认后才切换快速释放。
    const float gain = release_confirmed
        ? release_gain_per_second : build_gain_per_second;
    const float target = release_confirmed ? 0.0f : feedforward;
    const float alpha = 1.0f - std::exp(-gain * delta_seconds);
    prediction_motion += (target - prediction_motion) * alpha;
}

} // namespace aim::detail

#endif // AIM_PREDICTION_INTERNAL_H
