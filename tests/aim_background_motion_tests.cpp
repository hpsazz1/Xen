#include "aim/aim.h"
#include "log/log.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>

namespace {
int failures = 0;
void expect(bool ok, const char* message) {
    if (!ok) { ++failures; std::cerr << "[失败] " << message << '\n'; }
}

AimFrame frame(int index) {
    AimFrame f;
    f.sequence = static_cast<std::uint64_t>(index);
    f.captured_at = std::chrono::steady_clock::time_point{
        std::chrono::seconds(10) + std::chrono::milliseconds(4 * index)};
    f.control_at = f.captured_at + std::chrono::milliseconds(2);
    f.roi_width = f.roi_height = 320;
    f.control_center_x = f.control_center_y = 160.0f;
    f.lock_active = true;
    f.observation_epoch = 7;
    f.detections.push_back({150.0f + 2 * index, 140.0f,
                            180.0f + 2 * index, 200.0f, 0.95f, 0});
    f.background_motion_x = {
        AimBackgroundMotionStatus::VALID, f.sequence - 1, f.sequence,
        f.captured_at - std::chrono::milliseconds(4), f.captured_at,
        f.observation_epoch, 2.0f, 0.9f, 0.0f, 2};
    return f;
}

AimConfig config() {
    AimConfig c;
    c.min_confirmed_hits = 1;
    c.enable_delay_compensation = true;
    c.control_delay_ms = 15.0f;
    c.max_delay_compensation_ms = 44.0f;
    c.max_counts_per_frame = 14.0f;
    return c;
}

AimResult second(AimFrame f) {
    Aim aim(config());
    aim.process(frame(1));
    return aim.process(f);
}

void consumption_and_fallback() {
    auto f = frame(2);
    const auto matched = second(f);
    expect(matched.status == AimStatus::SUCCESS, "合法背景帧应成功");
    expect(matched.control.background_motion_use_x == AimBackgroundMotionUse::CONSUMED,
           "真实 Aim 路径必须消费同帧对背景");
    expect(std::fabs(matched.control.observer_camera_motion_x_source_pixels - 2.0f) < 0.001f,
           "背景 ROI 像素只做 source scale 转换，不再乘 plant 或延迟");
    expect(std::fabs(matched.control.observer_target_velocity_x_counts_per_second) < 0.001f,
           "人物与背景同移时，世界运动测量必须为零");

    f.background_motion_x.dx_roi_pixels = 0;
    const auto zero = second(f);
    expect(zero.control.background_motion_use_x == AimBackgroundMotionUse::CONSUMED,
           "有效真零不能回退命令模型");
    expect(zero.control.observer_target_velocity_x_counts_per_second > 0,
           "静止背景下目标右移必须保留世界运动");

    f.background_motion_x.status = AimBackgroundMotionStatus::MISSING;
    const auto missing = second(f);
    for (int kind = 0; kind < 10; ++kind) {
        auto invalid = frame(2);
        auto expected = AimBackgroundMotionUse::PAIR_MISMATCH;
        switch (kind) {
        case 0: ++invalid.background_motion_x.previous_sequence; break;
        case 1: invalid.background_motion_x.previous_captured_at += std::chrono::nanoseconds(1); break;
        case 2: ++invalid.background_motion_x.observation_epoch; break;
        case 3: ++invalid.background_motion_x.sequence; break;
        case 4: invalid.background_motion_x.captured_at += std::chrono::nanoseconds(1); break;
        case 5: invalid.background_motion_x.dx_roi_pixels = std::numeric_limits<float>::quiet_NaN();
                expected = AimBackgroundMotionUse::INVALID; break;
        case 6: invalid.background_motion_x.status = AimBackgroundMotionStatus::LOW_TEXTURE;
                expected = AimBackgroundMotionUse::INVALID; break;
        case 7: invalid.background_motion_x.dx_roi_pixels = (std::numeric_limits<float>::max)();
                expected = AimBackgroundMotionUse::INVALID; break;
        case 8: invalid.background_motion_x.min_response = -1;
                expected = AimBackgroundMotionUse::INVALID; break;
        case 9: invalid.background_motion_x.disagreement_roi_pixels = -1;
                expected = AimBackgroundMotionUse::INVALID; break;
        }
        const auto result = second(invalid);
        expect(result.control.background_motion_use_x == expected, "无效输入必须明确解释回退");
        expect(result.command.dx_counts == missing.command.dx_counts &&
               result.command.dy_counts == missing.command.dy_counts,
               "无效背景回退原路径，不污染 X/Y 输出");
        expect(std::isfinite(result.control.observer_target_velocity_x_counts_per_second),
               "大有限输入不能向后续状态写入无穷");
    }
    expect(matched.command.dy_counts == missing.command.dy_counts,
           "相机来源替换不能修改 Y 请求");

    Aim aim(config());
    aim.process(frame(1));
    auto absent = frame(2);
    absent.detections.clear();
    aim.process(absent);
    const auto reacquired = aim.process(frame(3));
    expect(reacquired.control.background_motion_use_x != AimBackgroundMotionUse::CONSUMED,
           "缺测重获不能把跨缺口的目标边当相邻图像 pair");
    auto released = frame(4);
    released.lock_active = false;
    expect(aim.process(released).control.background_motion_use_x == AimBackgroundMotionUse::NOT_EVALUATED,
           "松键后不能继续背景驱动的世界运动更新");
}

void source_scale_is_applied_once() {
    Aim aim(config());
    auto first = frame(1);
    first.source_pixels_per_roi_pixel_x = 2.0f;
    aim.process(first);
    auto next = frame(2);
    next.source_pixels_per_roi_pixel_x = 2.0f;
    const auto result = aim.process(next);
    expect(result.control.background_motion_use_x == AimBackgroundMotionUse::CONSUMED,
           "非1 source比例的同帧背景必须真实消费");
    expect(std::fabs(result.control.observer_camera_motion_x_source_pixels - 4.0f) < 0.001f,
           "2 ROI像素在scale2时恰为4 source像素，不能重复乘G或delay");
    expect(std::fabs(result.control.observer_target_velocity_x_counts_per_second) < 0.001f,
           "非1比例下人物与背景同移仍须相减为零");
}

void head_semantics_recover_on_the_next_pair() {
    Aim aim(config());
    const auto body = aim.process(frame(1));
    auto head = frame(2);
    head.detections = {{160, 142, 172, 154, 0.95f, 1}};
    const auto switched = aim.process(head);
    expect(switched.has_target && switched.target.track_id == body.target.track_id &&
               switched.target.matched_observation_head_only,
           "body转head测试必须实际延续同一轨迹身份");
    expect(switched.control.background_motion_use_x == AimBackgroundMotionUse::SEMANTICS_MISMATCH,
           "body到head首帧不能把坐标语义变化当同一原始边差分");
    auto next_head = frame(3);
    next_head.detections = {{162, 142, 174, 154, 0.95f, 1}};
    const auto recovered = aim.process(next_head);
    expect(recovered.has_target && recovered.target.track_id == body.target.track_id &&
               recovered.target.matched_observation_head_only,
           "第二张head仍须来自同一轨迹，不能以新建身份冒充恢复");
    expect(recovered.control.background_motion_use_x == AimBackgroundMotionUse::CONSUMED,
           "连续head原始观测恢复后必须消费新pair，不能永久拒绝");
}

void geometry_epoch_rejects_old_pair_and_recovers() {
    Aim aim(config());
    aim.process(frame(1));
    auto changed = frame(2);
    changed.source_pixels_per_roi_pixel_x = 2;
    changed.observation_epoch = 8;
    changed.background_motion_x.observation_epoch = 8;
    const auto rejected = aim.process(changed);
    expect(rejected.control.background_motion_use_x == AimBackgroundMotionUse::PAIR_MISMATCH,
           "即使输入声称VALID，新几何epoch也不能消费旧epoch的目标边");
    auto next = frame(3);
    next.source_pixels_per_roi_pixel_x = 2;
    next.observation_epoch = 8;
    next.background_motion_x.observation_epoch = 8;
    const auto recovered = aim.process(next);
    expect(recovered.control.background_motion_use_x == AimBackgroundMotionUse::CONSUMED,
           "新几何下连续原始pair应恢复，不沿用上上帧epoch");
    expect(std::fabs(recovered.control.observer_camera_motion_x_source_pixels - 4) < 0.001f,
           "恢复后的背景仍按当前几何转换");
}

void finite_overflow_does_not_poison_later_frames() {
    Aim tested(config()), baseline(config());
    auto first = frame(1);
    first.background_motion_x.status = AimBackgroundMotionStatus::MISSING;
    tested.process(first);
    baseline.process(first);
    auto invalid = frame(2);
    invalid.background_motion_x.dx_roi_pixels = (std::numeric_limits<float>::max)();
    const auto rejected = tested.process(invalid);
    auto missing = invalid;
    missing.background_motion_x.status = AimBackgroundMotionStatus::MISSING;
    const auto expected = baseline.process(missing);
    expect(rejected.control.background_motion_use_x == AimBackgroundMotionUse::INVALID &&
               rejected.control.observer_target_velocity_x_counts_per_second ==
                   expected.control.observer_target_velocity_x_counts_per_second,
           "大有限值溢出须在写入状态前回退为原模型结果");
    for (int index = 3; index <= 6; ++index) {
        auto next = frame(index);
        next.background_motion_x.status = AimBackgroundMotionStatus::MISSING;
        const auto actual = tested.process(next);
        const auto original = baseline.process(next);
        expect(std::isfinite(actual.control.observer_target_velocity_x_counts_per_second) &&
                   actual.control.observer_target_velocity_x_counts_per_second ==
                       original.control.observer_target_velocity_x_counts_per_second &&
                   actual.control.shaped_x_counts == original.control.shaped_x_counts &&
                   actual.command.dx_counts == original.command.dx_counts &&
                   actual.command.dy_counts == original.command.dy_counts,
               "异常后的后继帧速度、整形和命令必须与原模型基线一致");
    }
}

// 比较全部确定性公开输出；profile 是实际耗时，不要求两次调用逐位相等。
bool same_prediction_output(const AimResult& a, const AimResult& b) {
    return a.status == b.status &&
           a.has_target == b.has_target &&
           a.has_command == b.has_command &&
           a.acquisition_range_radius == b.acquisition_range_radius &&
           a.active_range_radius == b.active_range_radius &&
           a.range_locked == b.range_locked &&
           a.range_allows_control == b.range_allows_control &&
           a.target.track_id == b.target.track_id &&
           a.target.state == b.target.state &&
           a.target.x1 == b.target.x1 &&
           a.target.y1 == b.target.y1 &&
           a.target.x2 == b.target.x2 &&
           a.target.y2 == b.target.y2 &&
           a.target.matched_observation_valid == b.target.matched_observation_valid &&
           a.target.matched_observation_x1 == b.target.matched_observation_x1 &&
           a.target.matched_observation_y1 == b.target.matched_observation_y1 &&
           a.target.matched_observation_x2 == b.target.matched_observation_x2 &&
           a.target.matched_observation_y2 == b.target.matched_observation_y2 &&
           a.target.matched_observation_head_only == b.target.matched_observation_head_only &&
           a.target.matched_observation_aim_from_head == b.target.matched_observation_aim_from_head &&
           a.target.base_aim_x == b.target.base_aim_x &&
           a.target.base_aim_y == b.target.base_aim_y &&
           a.target.delay_compensated_aim_x == b.target.delay_compensated_aim_x &&
           a.target.delay_compensated_aim_y == b.target.delay_compensated_aim_y &&
           a.target.prediction_aim_x == b.target.prediction_aim_x &&
           a.target.prediction_aim_y == b.target.prediction_aim_y &&
           a.target.aim_x == b.target.aim_x &&
           a.target.aim_y == b.target.aim_y &&
           a.target.velocity_x == b.target.velocity_x &&
           a.target.velocity_y == b.target.velocity_y &&
           a.target.lead_x == b.target.lead_x &&
           a.target.lead_y == b.target.lead_y &&
           a.target.delay_compensation_x == b.target.delay_compensation_x &&
           a.target.delay_compensation_y == b.target.delay_compensation_y &&
           a.target.delay_compensation_ms_x == b.target.delay_compensation_ms_x &&
           a.target.delay_compensation_ms_y == b.target.delay_compensation_ms_y &&
           a.target.delay_compensation_ms == b.target.delay_compensation_ms &&
           a.target.observation_age_ms == b.target.observation_age_ms &&
           a.target.confidence == b.target.confidence &&
           a.target.lead_active == b.target.lead_active &&
           a.target.delay_compensation_active == b.target.delay_compensation_active &&
           a.target.predicted == b.target.predicted &&
           a.control.evaluated == b.control.evaluated &&
           a.control.background_motion_use_x == b.control.background_motion_use_x &&
           a.control.observer_camera_motion_x_source_pixels == b.control.observer_camera_motion_x_source_pixels &&
           a.control.observer_target_velocity_x_counts_per_second == b.control.observer_target_velocity_x_counts_per_second &&
           a.control.controller_dt_ms == b.control.controller_dt_ms &&
           a.control.proportional_x_counts == b.control.proportional_x_counts &&
           a.control.feedforward_x_counts == b.control.feedforward_x_counts &&
           a.control.desired_before_reverse_x_counts == b.control.desired_before_reverse_x_counts &&
           a.control.desired_x_counts == b.control.desired_x_counts &&
           a.control.filtered_x_counts == b.control.filtered_x_counts &&
           a.control.shaped_x_counts == b.control.shaped_x_counts &&
           a.control.residual_before_quantization_x_counts == b.control.residual_before_quantization_x_counts &&
           a.control.delayed_command_x_counts == b.control.delayed_command_x_counts &&
           a.control.pending_net_x_counts == b.control.pending_net_x_counts &&
           a.control.pending_absolute_x_counts == b.control.pending_absolute_x_counts &&
           a.control.modelled_response_x_counts == b.control.modelled_response_x_counts &&
           a.control.observer_phase_command_x_counts == b.control.observer_phase_command_x_counts &&
           a.control.observer_consistency_weight_x == b.control.observer_consistency_weight_x &&
           a.control.reverse_output_direction_x == b.control.reverse_output_direction_x &&
           a.control.reverse_evidence_ratio_seconds_x == b.control.reverse_evidence_ratio_seconds_x &&
           a.control.reverse_position_ratio_seconds_x == b.control.reverse_position_ratio_seconds_x &&
           a.control.reverse_position_peak_error_x == b.control.reverse_position_peak_error_x &&
           a.control.reverse_translation_seconds_x == b.control.reverse_translation_seconds_x &&
           a.control.reverse_translation_raw_left_x_roi_pixels == b.control.reverse_translation_raw_left_x_roi_pixels &&
           a.control.reverse_translation_raw_right_x_roi_pixels == b.control.reverse_translation_raw_right_x_roi_pixels &&
           a.control.reverse_translation_raw_common_x_roi_pixels == b.control.reverse_translation_raw_common_x_roi_pixels &&
           a.control.reverse_translation_control_evidence_x == b.control.reverse_translation_control_evidence_x &&
           a.control.reverse_translation_gap_seconds_x == b.control.reverse_translation_gap_seconds_x &&
           a.control.reverse_deformation_seconds_x == b.control.reverse_deformation_seconds_x &&
           a.control.reverse_required_evidence_ratio_seconds_x == b.control.reverse_required_evidence_ratio_seconds_x &&
           a.control.reverse_required_position_ratio_seconds_x == b.control.reverse_required_position_ratio_seconds_x &&
           a.control.reverse_probe_direction_x == b.control.reverse_probe_direction_x &&
           a.control.reverse_probe_age_ms_x == b.control.reverse_probe_age_ms_x &&
           a.control.reverse_translation_reset_reason_x == b.control.reverse_translation_reset_reason_x &&
           a.control.pending_positive_x == b.control.pending_positive_x &&
           a.control.pending_negative_x == b.control.pending_negative_x &&
           a.control.reverse_candidate_x == b.control.reverse_candidate_x &&
           a.control.reverse_previous_direction_pending_x == b.control.reverse_previous_direction_pending_x &&
           a.control.reverse_partial_semantics_transition_x == b.control.reverse_partial_semantics_transition_x &&
           a.control.reverse_deformation_active_x == b.control.reverse_deformation_active_x &&
           a.control.reverse_evidence_ready_x == b.control.reverse_evidence_ready_x &&
           a.control.reverse_translation_fresh_evidence_x == b.control.reverse_translation_fresh_evidence_x &&
           a.control.reverse_translation_ready_x == b.control.reverse_translation_ready_x &&
           a.control.reverse_position_ready_x == b.control.reverse_position_ready_x &&
           a.control.reverse_position_improvement_reset_x == b.control.reverse_position_improvement_reset_x &&
           a.control.reverse_gate_blocked_x == b.control.reverse_gate_blocked_x &&
           a.control.reverse_probe_active_x == b.control.reverse_probe_active_x &&
           a.control.reverse_probe_limited_x == b.control.reverse_probe_limited_x &&
           a.control.pending_inventory_hold_blocked_x == b.control.pending_inventory_hold_blocked_x &&
           a.control.deadzone_quiet == b.control.deadzone_quiet &&
           a.control.shaper_direction_reset_x == b.control.shaper_direction_reset_x &&
           a.control.post_alignment_sign_change_blocked_x == b.control.post_alignment_sign_change_blocked_x &&
           a.control.post_alignment_growth_limited_x == b.control.post_alignment_growth_limited_x &&
           a.control.closing_response_tapered_x == b.control.closing_response_tapered_x &&
           a.control.integer_direction_blocked_x == b.control.integer_direction_blocked_x &&
           a.control.command_sign_change_blocked_x == b.control.command_sign_change_blocked_x &&
           a.control.quantization_zero_x == b.control.quantization_zero_x &&
           a.command.sequence == b.command.sequence &&
           a.command.captured_at == b.command.captured_at &&
           a.command.dx_counts == b.command.dx_counts &&
           a.command.dy_counts == b.command.dy_counts;
}

void prediction_does_not_consume_background() {
    auto c = config();
    c.enable_prediction = true;
    Aim measured(c), missing(c);
    for (int index = 1; index <= 32; ++index) {
        auto f = frame(index);
        f.background_motion_x.dx_roi_pixels = index % 2 ? 9.0f : -7.0f;
        f.detections[0].y1 += static_cast<float>(index % 5);
        f.detections[0].y2 += static_cast<float>(index % 5);
        f.lock_active = index % 7 != 0;
        const auto a = measured.process(f);
        f.background_motion_x.status = AimBackgroundMotionStatus::MISSING;
        const auto b = missing.process(f);
        expect(a.control.background_motion_use_x == AimBackgroundMotionUse::NOT_EVALUATED,
               "prediction路径不得消费有效背景或修改其世界运动状态");
        expect(same_prediction_output(a, b),
               "prediction的全部确定性公开输出必须与缺失背景逐字段相同");
    }
}

} // namespace

int main() {
    consumption_and_fallback();
    source_scale_is_applied_once();
    head_semantics_recover_on_the_next_pair();
    geometry_epoch_rejects_old_pair_and_recovers();
    finite_overflow_does_not_poison_later_frames();
    prediction_does_not_consume_background();
    if (failures) { std::cerr << "背景输入合同失败数: " << failures << '\n'; return 1; }
    std::cout << "背景输入合同全部通过。\n";
}
