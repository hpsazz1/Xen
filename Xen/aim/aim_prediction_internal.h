#ifndef AIM_PREDICTION_INTERNAL_H
#define AIM_PREDICTION_INTERNAL_H

#include <algorithm>
#include <cmath>

namespace aim::detail {

// prediction 的世界速度状态需要同时满足两个相反要求：持续运动期间抑制
// 检测框动画造成的速度起伏，真实停止时又能及时归零。基础前馈的单位是
// counts/frame，必须先按本帧真实 dt 归一化为 counts/second 再低通；否则
// NDI 成批交付造成的短帧间隔会在后续除以 dt 时把提前量单帧放大到上限。
// 停止证据仍保留 counts/frame 口径，避免改变已经实机验收的静止门槛。
inline void update_prediction_velocity_axis(
        float feedforward_counts_per_frame,
        float stop_measurement_counts_per_frame, float delta_seconds,
        float minimum_counts, int release_confirm_frames,
        float build_gain_per_second, float release_gain_per_second,
        float& prediction_velocity_counts_per_second,
        int& low_measurement_frames) noexcept {
    if (std::fabs(stop_measurement_counts_per_frame) <= minimum_counts) {
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
    const float safe_delta_seconds = std::max(delta_seconds, 0.001f);
    const float target_velocity_counts_per_second = release_confirmed
        ? 0.0f
        : feedforward_counts_per_frame / safe_delta_seconds;
    const float alpha = 1.0f - std::exp(-gain * safe_delta_seconds);
    prediction_velocity_counts_per_second +=
        (target_velocity_counts_per_second -
         prediction_velocity_counts_per_second) * alpha;
}

// 历史 prediction 方向的反拉保护不能无条件覆盖当前最终点。只有命令
// 确实朝当前最终点，且命令补偿后的世界运动测量超过噪声门槛并支持历史
// 方向时，才允许该轴继续输出。超额在途命令已经由控制锚提前制动，因此
// 保持带外也必须服从当前最终点；停止、反馈低谷和真实反向仍不会误放行。
inline bool prediction_pullback_command_allowed(
        float desired_counts, float final_error_pixels,
        float world_measurement_counts, float prediction_direction,
        float minimum_world_counts) noexcept {
    return desired_counts * final_error_pixels > 0.0f &&
        std::fabs(world_measurement_counts) > minimum_world_counts &&
        std::fabs(prediction_direction) > 0.001f &&
        world_measurement_counts * prediction_direction > 0.0f;
}

// 反向库存制动只允许用最小整数命令短促消化超额 pending。更大命令或超过
// 连续帧预算后必须停发，等待已经在途的命令反馈；调用者只在公开最终点
// 保持带外且命令确实背离该点时使用本门禁。
inline bool prediction_inventory_brake_allowed(
        int command_counts, int previous_brake_frames,
        int maximum_brake_frames) noexcept {
    return std::abs(command_counts) == 1 &&
        previous_brake_frames >= 0 &&
        previous_brake_frames < maximum_brake_frames;
}

} // namespace aim::detail

#endif // AIM_PREDICTION_INTERNAL_H
