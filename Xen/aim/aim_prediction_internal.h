#ifndef AIM_PREDICTION_INTERNAL_H
#define AIM_PREDICTION_INTERNAL_H

#include <algorithm>
#include <cmath>

namespace aim::detail {

// prediction 首次建立必须累计同向世界运动时长，但真实命令反馈和量化会
// 偶发一个低于门槛的采样。单帧低谷只暂停累计；连续低谷超过预算时立即
// 清零，反向有效测量则从新方向重新计时，不能跨方向继承资格。
inline bool update_prediction_motion_candidate(
        float world_measurement_counts, float minimum_counts,
        float delta_seconds, float establishment_seconds,
        int tolerated_low_measurement_frames, float& candidate_seconds,
        int& low_measurement_frames) noexcept {
    const int low_frame_limit = std::max(
        tolerated_low_measurement_frames, 0);
    if (std::fabs(world_measurement_counts) <= minimum_counts) {
        low_measurement_frames = std::min(
            low_measurement_frames + 1, low_frame_limit + 1);
        if (low_measurement_frames > low_frame_limit) {
            candidate_seconds = 0.0f;
        }
        return false;
    }

    low_measurement_frames = 0;
    const float signed_dt = std::copysign(
        std::max(delta_seconds, 0.0f), world_measurement_counts);
    if (candidate_seconds * world_measurement_counts <= 0.0f) {
        candidate_seconds = signed_dt;
    } else {
        candidate_seconds += signed_dt;
    }
    candidate_seconds = std::clamp(
        candidate_seconds, -establishment_seconds, establishment_seconds);
    return std::fabs(candidate_seconds) >= establishment_seconds;
}

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

// 最终 prediction 偏移按目标对角线/秒限速，并在发布前重新约束“延迟点到
// 最终点”的几何上限。进入 prediction 和退出 prediction 必须使用同一函数，
// 不能在低运动释放时直接把偏移清零。
inline void slew_prediction_offset(
        float target_offset_x, float target_offset_y,
        float delay_offset_x, float delay_offset_y,
        float box_diagonal, float maximum_lead_percent,
        float maximum_slew_diagonals_per_second, float delta_seconds,
        float& offset_x, float& offset_y) noexcept {
    const float delta_x = target_offset_x - offset_x;
    const float delta_y = target_offset_y - offset_y;
    const float distance = std::hypot(delta_x, delta_y);
    const float maximum_delta = std::max(
        0.0f, box_diagonal * maximum_slew_diagonals_per_second *
            std::max(delta_seconds, 0.0f));
    if (distance <= maximum_delta || distance <= 0.0f) {
        offset_x = target_offset_x;
        offset_y = target_offset_y;
    } else {
        const float scale = maximum_delta / distance;
        offset_x += delta_x * scale;
        offset_y += delta_y * scale;
    }

    float lead_x = offset_x - delay_offset_x;
    float lead_y = offset_y - delay_offset_y;
    const float maximum_lead =
        std::max(0.0f, box_diagonal * maximum_lead_percent / 100.0f);
    const float lead_distance = std::hypot(lead_x, lead_y);
    if (lead_distance > maximum_lead && lead_distance > 0.0f) {
        const float scale = maximum_lead / lead_distance;
        lead_x *= scale;
        lead_y *= scale;
    }
    offset_x = delay_offset_x + lead_x;
    offset_y = delay_offset_y + lead_y;
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
