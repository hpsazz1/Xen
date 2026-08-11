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

// tracking 的额外水平时域直接从“本帧世界位移 / 本帧 dt”估计速度。
// 不能把已经跨帧低通的 counts/frame 基础前馈再除以当前 dt：NDI 以 4/12 ms
// 交替交付时，历史位移会被错误套用到当前采样周期，形成慢速追赶和回摆。
// 同向变化仍使用低增益抑制检测噪声；确认的反向测量使用释放增益快速穿零，
// 避免旧方向在一个完整低通时间常数内继续推动准星。
inline void update_tracking_projection_velocity_axis(
        float world_measurement_counts_per_frame, float delta_seconds,
        float minimum_counts, int release_confirm_frames,
        float build_gain_per_second, float release_gain_per_second,
        float reversal_gain_per_second,
        float& projection_velocity_counts_per_second,
        int& low_measurement_frames) noexcept {
    if (std::fabs(world_measurement_counts_per_frame) <= minimum_counts) {
        low_measurement_frames = std::min(
            low_measurement_frames + 1, release_confirm_frames);
    } else {
        low_measurement_frames = 0;
    }

    const bool release_confirmed =
        low_measurement_frames >= release_confirm_frames;
    const float safe_delta_seconds = std::max(delta_seconds, 0.001f);
    const float target_velocity_counts_per_second = release_confirmed
        ? 0.0f
        : world_measurement_counts_per_frame / safe_delta_seconds;
    const bool reversing =
        projection_velocity_counts_per_second *
            target_velocity_counts_per_second < 0.0f;
    const float gain = release_confirmed
        ? release_gain_per_second
        : (reversing ? reversal_gain_per_second : build_gain_per_second);
    const float alpha = 1.0f - std::exp(-gain * safe_delta_seconds);
    projection_velocity_counts_per_second +=
        (target_velocity_counts_per_second -
         projection_velocity_counts_per_second) * alpha;
}

// tracking 的总 X 延迟必须作为一个状态统一限速。只限速额外 24 ms 分量时，
// 基础 16 ms 分量仍会随 track.vx 和 Y 占用的二维预算直接阶跃，绕过扩展限速。
// 首次进入从本帧基础延迟开始，保留已有 tracking 响应；预算收缩时立即回写以
// 满足总向量上限，预算扩张和后续基础速度变化则统一服从单帧渐变上限。
inline float update_tracking_delay_output(
        float desired_output_x, float base_delay_x,
        float remaining_x_limit, float maximum_output_step,
        bool& initialized, float& output_x) noexcept {
    const float safe_remaining_x_limit =
        std::max(remaining_x_limit, 0.0f);
    if (!initialized) {
        output_x = std::clamp(
            base_delay_x, -safe_remaining_x_limit, safe_remaining_x_limit);
        initialized = true;
    } else {
        output_x = std::clamp(
            output_x, -safe_remaining_x_limit, safe_remaining_x_limit);
    }
    const float reachable_desired_output = std::clamp(
        desired_output_x, -safe_remaining_x_limit, safe_remaining_x_limit);
    const float safe_maximum_step = std::max(maximum_output_step, 0.0f);
    output_x += std::clamp(
        reachable_desired_output - output_x,
        -safe_maximum_step, safe_maximum_step);
    output_x = std::clamp(
        output_x, -safe_remaining_x_limit, safe_remaining_x_limit);
    return output_x;
}

// 跳跃期间逐步把水平内窗扩展到完整人物框，使已滤波的 Track 瞄点不再被
// 25%/75% 移动边界夹住。这里只改变 clamp 可达范围，不建立绝对屏幕位置
// 状态，因此不会滞后真实水平运动；起跳和落地使用同一速率渐变，避免切换跳变。
inline float update_tracking_horizontal_half_range(
        float configured_half_range, bool jump_active, float delta_seconds,
        float slew_per_second, bool& initialized,
        float& current_half_range) noexcept {
    const float safe_configured_half_range = std::clamp(
        configured_half_range, 0.0f, 0.5f);
    if (!initialized) {
        current_half_range = safe_configured_half_range;
        initialized = true;
    }
    const float target_half_range = jump_active
        ? 0.5f : safe_configured_half_range;
    const float maximum_step = std::max(
        0.0f, slew_per_second * std::max(delta_seconds, 0.0f));
    current_half_range += std::clamp(
        target_half_range - current_half_range,
        -maximum_step, maximum_step);
    current_half_range = std::clamp(
        current_half_range, safe_configured_half_range, 0.5f);
    return current_half_range;
}

// 延迟窗口内的旧方向命令不能无条件阻塞真实反向。只有新方向纠偏需求小于
// 预计库存影响时才暂停；目标已经沿新方向拉开足够误差后必须恢复输出，避免
// “等库存全清空 -> 大误差重新打满”的新极限环。门禁只作用于跳跃 X 轴。
inline bool tracking_jump_reversal_command_allowed(
        int command_counts, float pending_counts,
        float control_error_counts, float pending_response,
        bool jump_active) noexcept {
    const bool opposite_inventory =
        static_cast<float>(command_counts) * pending_counts < 0.0f;
    const float expected_pending_effect = std::fabs(pending_counts) *
        std::clamp(pending_response, 0.0f, 1.0f);
    return !jump_active || command_counts == 0 ||
        !opposite_inventory ||
        std::fabs(control_error_counts) >= expected_pending_effect;
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
