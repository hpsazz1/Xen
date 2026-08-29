#include "aim_landmark/aim_landmark.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << message << '\n';
}

AimTargetSnapshot current_body_target(std::uint64_t track_id,
                                      float offset_x = 0.0f) {
    AimTargetSnapshot target;
    target.track_id = track_id;
    target.state = TrackState::CONFIRMED;
    target.matched_observation_valid = true;
    target.matched_observation_x1 = 100.0f + offset_x;
    target.matched_observation_y1 = 50.0f;
    target.matched_observation_x2 = 200.0f + offset_x;
    target.matched_observation_y2 = 250.0f;
    target.matched_observation_aim_from_head = true;
    return target;
}

std::array<Detection, 2> body_and_head(float offset_x = 0.0f) {
    return {{
        {100.0f + offset_x, 50.0f, 200.0f + offset_x, 250.0f,
         0.82f, 0},
        {130.0f + offset_x, 60.0f, 170.0f + offset_x, 110.0f,
         0.91f, 1},
    }};
}

void test_unique_current_head_is_reported_without_observer_state() {
    AimConfig config;
    config.person_class_ids = {0, 2};
    config.head_class_ids = {1, 3};
    config.low_confidence = 0.10f;

    AimResult aim_result;
    aim_result.status = AimStatus::SUCCESS;
    aim_result.has_target = true;
    aim_result.target = current_body_target(7, 2.0f);
    const auto detections = body_and_head(2.0f);
    const auto evidence = aim_landmark::inspect_head_landmark(
        101, detections, config, aim_result);
    expect(evidence.status == aim_landmark::Status::VALID &&
               evidence.valid && evidence.fresh &&
               !evidence.control_eligible && evidence.track_id == 7 &&
               evidence.sequence == 101 && evidence.candidate_count == 1 &&
               evidence.semantic_kind ==
                   aim_landmark::SemanticKind::HEAD_BOX_CENTER &&
               evidence.x == 152.0f && evidence.y == 85.0f &&
               evidence.x1 == 132.0f && evidence.y1 == 60.0f &&
               evidence.x2 == 172.0f && evidence.y2 == 110.0f &&
               evidence.confidence == 0.91f && evidence.class_id == 1,
           "同序号、同 Track 的唯一语义 head 应发布当前帧中心");
}

void test_nonfinite_associated_head_fails_closed() {
    AimConfig config;
    config.person_class_ids = {0, 2};
    config.head_class_ids = {1, 3};
    config.low_confidence = 0.10f;
    AimResult aim_result;
    aim_result.status = AimStatus::SUCCESS;
    aim_result.has_target = true;
    aim_result.target = current_body_target(7);
    auto detections = body_and_head();
    detections[1].x1 = std::numeric_limits<float>::quiet_NaN();

    const auto evidence = aim_landmark::inspect_head_landmark(
        102, detections, config, aim_result);
    expect(evidence.status == aim_landmark::Status::INVALID_INPUT &&
               !evidence.valid && !evidence.fresh &&
               !evidence.control_eligible && evidence.candidate_count == 0 &&
               evidence.x == 0.0f && evidence.y == 0.0f &&
               evidence.confidence == 0.0f && evidence.class_id == -1,
           "非有限的同帧 head 不能降级伪装成遮挡或沿用旧坐标");
}

void test_unselected_local_head_is_reported_as_ambiguous() {
    AimConfig config;
    config.person_class_ids = {0, 2};
    config.head_class_ids = {1, 3};
    config.low_confidence = 0.10f;
    AimResult aim_result;
    aim_result.status = AimStatus::SUCCESS;
    aim_result.has_target = true;
    aim_result.target = current_body_target(7);
    aim_result.target.matched_observation_aim_from_head = false;
    const auto detections = body_and_head();

    const auto evidence = aim_landmark::inspect_head_landmark(
        103, detections, config, aim_result);
    expect(evidence.status == aim_landmark::Status::AMBIGUOUS &&
               evidence.semantic_kind ==
                   aim_landmark::SemanticKind::HEAD_BOX_CENTER &&
               !evidence.valid && !evidence.fresh &&
               !evidence.occluded && !evidence.control_eligible &&
               evidence.candidate_count == 1 &&
               evidence.x == 0.0f && evidence.y == 0.0f,
           "当前 body 内存在未被 Aim 选择的 head 时，身份必须显式歧义");
}

void test_missing_declared_head_is_an_association_mismatch() {
    AimConfig config;
    config.person_class_ids = {0, 2};
    config.head_class_ids = {1, 3};
    config.low_confidence = 0.10f;
    AimResult aim_result;
    aim_result.status = AimStatus::SUCCESS;
    aim_result.has_target = true;
    aim_result.target = current_body_target(7);
    const std::array<Detection, 1> body_only{{body_and_head()[0]}};

    const auto evidence = aim_landmark::inspect_head_landmark(
        104, body_only, config, aim_result);
    expect(evidence.status ==
               aim_landmark::Status::ASSOCIATION_MISMATCH &&
               !evidence.valid && !evidence.fresh &&
               !evidence.occluded && evidence.candidate_count == 0 &&
               evidence.x == 0.0f && evidence.y == 0.0f,
           "Aim 已声明 head 关联但同帧找不到候选时必须显式失败");
}

void test_missing_current_observation_keeps_requested_identity_without_point() {
    AimConfig config;
    AimResult aim_result;
    aim_result.status = AimStatus::SUCCESS;
    aim_result.has_target = true;
    aim_result.target = current_body_target(9);
    aim_result.target.matched_observation_valid = false;
    aim_result.target.predicted = true;

    const auto evidence = aim_landmark::inspect_head_landmark(
        105, std::array<Detection, 0>{}, config, aim_result);
    expect(evidence.status ==
               aim_landmark::Status::NO_CURRENT_OBSERVATION &&
               evidence.semantic_kind ==
                   aim_landmark::SemanticKind::HEAD_BOX_CENTER &&
               evidence.sequence == 105 && evidence.track_id == 9 &&
               !evidence.valid && !evidence.fresh &&
               !evidence.control_eligible && evidence.x == 0.0f &&
               evidence.y == 0.0f && evidence.class_id == -1,
           "预测或丢框帧应保留请求的 landmark 身份但绝不沿用旧点");
}

void test_occluded_ambiguous_and_no_target_fail_closed() {
    AimConfig config;
    config.person_class_ids = {0, 2};
    config.head_class_ids = {1, 3};
    config.low_confidence = 0.10f;
    AimResult aim_result;
    aim_result.status = AimStatus::SUCCESS;
    aim_result.has_target = true;
    aim_result.target = current_body_target(11);
    aim_result.target.matched_observation_aim_from_head = false;
    const std::array<Detection, 1> body_only{{body_and_head()[0]}};
    const auto occluded = aim_landmark::inspect_head_landmark(
        106, body_only, config, aim_result);
    expect(occluded.status == aim_landmark::Status::OCCLUDED &&
               occluded.semantic_kind ==
                   aim_landmark::SemanticKind::HEAD_BOX_CENTER &&
               occluded.occluded && !occluded.valid && !occluded.fresh &&
               !occluded.control_eligible && occluded.x == 0.0f,
           "无同帧 head 时只报告未观测到，不得伪造点或控制资格");

    aim_result.target.matched_observation_aim_from_head = true;
    const std::array<Detection, 3> ambiguous{{
        body_and_head()[0], body_and_head()[1],
        {155.0f, 65.0f, 175.0f, 95.0f, 0.88f, 3},
    }};
    const auto ambiguous_result = aim_landmark::inspect_head_landmark(
        107, ambiguous, config, aim_result);
    expect(ambiguous_result.status == aim_landmark::Status::AMBIGUOUS &&
               ambiguous_result.candidate_count == 2 &&
               !ambiguous_result.valid && !ambiguous_result.fresh &&
               !ambiguous_result.control_eligible &&
               ambiguous_result.x == 0.0f,
           "多个合理 head 候选必须显式歧义并清空坐标");

    aim_result.has_target = false;
    const auto no_target = aim_landmark::inspect_head_landmark(
        108, body_only, config, aim_result);
    expect(no_target.status == aim_landmark::Status::NO_TARGET &&
               no_target.semantic_kind == aim_landmark::SemanticKind::NONE &&
               no_target.track_id == 0 && !no_target.control_eligible,
           "无目标帧不得虚构 landmark 身份");
}

bool same_target(const AimTargetSnapshot& left,
                 const AimTargetSnapshot& right) {
    return left.track_id == right.track_id && left.state == right.state &&
        left.x1 == right.x1 && left.y1 == right.y1 &&
        left.x2 == right.x2 && left.y2 == right.y2 &&
        left.matched_observation_valid == right.matched_observation_valid &&
        left.matched_observation_x1 == right.matched_observation_x1 &&
        left.matched_observation_y1 == right.matched_observation_y1 &&
        left.matched_observation_x2 == right.matched_observation_x2 &&
        left.matched_observation_y2 == right.matched_observation_y2 &&
        left.matched_observation_head_only ==
            right.matched_observation_head_only &&
        left.matched_observation_aim_from_head ==
            right.matched_observation_aim_from_head &&
        left.base_aim_x == right.base_aim_x &&
        left.base_aim_y == right.base_aim_y &&
        left.delay_compensated_aim_x == right.delay_compensated_aim_x &&
        left.delay_compensated_aim_y == right.delay_compensated_aim_y &&
        left.prediction_aim_x == right.prediction_aim_x &&
        left.prediction_aim_y == right.prediction_aim_y &&
        left.aim_x == right.aim_x && left.aim_y == right.aim_y &&
        left.velocity_x == right.velocity_x &&
        left.velocity_y == right.velocity_y &&
        left.lead_x == right.lead_x && left.lead_y == right.lead_y &&
        left.delay_compensation_x == right.delay_compensation_x &&
        left.delay_compensation_y == right.delay_compensation_y &&
        left.delay_compensation_ms_x == right.delay_compensation_ms_x &&
        left.delay_compensation_ms_y == right.delay_compensation_ms_y &&
        left.delay_compensation_ms == right.delay_compensation_ms &&
        left.observation_age_ms == right.observation_age_ms &&
        left.confidence == right.confidence &&
        left.lead_active == right.lead_active &&
        left.delay_compensation_active == right.delay_compensation_active &&
        left.predicted == right.predicted;
}

bool same_control(const AimControlDiagnostics& left,
                  const AimControlDiagnostics& right) {
#define AIM_LANDMARK_SAME_CONTROL_FIELD(field) \
    if (left.field != right.field) return false
    AIM_LANDMARK_SAME_CONTROL_FIELD(evaluated);
    AIM_LANDMARK_SAME_CONTROL_FIELD(controller_dt_ms);
    AIM_LANDMARK_SAME_CONTROL_FIELD(proportional_x_counts);
    AIM_LANDMARK_SAME_CONTROL_FIELD(feedforward_x_counts);
    AIM_LANDMARK_SAME_CONTROL_FIELD(desired_before_reverse_x_counts);
    AIM_LANDMARK_SAME_CONTROL_FIELD(desired_x_counts);
    AIM_LANDMARK_SAME_CONTROL_FIELD(filtered_x_counts);
    AIM_LANDMARK_SAME_CONTROL_FIELD(shaped_x_counts);
    AIM_LANDMARK_SAME_CONTROL_FIELD(residual_before_quantization_x_counts);
    AIM_LANDMARK_SAME_CONTROL_FIELD(delayed_command_x_counts);
    AIM_LANDMARK_SAME_CONTROL_FIELD(pending_net_x_counts);
    AIM_LANDMARK_SAME_CONTROL_FIELD(pending_absolute_x_counts);
    AIM_LANDMARK_SAME_CONTROL_FIELD(modelled_response_x_counts);
    AIM_LANDMARK_SAME_CONTROL_FIELD(observer_phase_command_x_counts);
    AIM_LANDMARK_SAME_CONTROL_FIELD(observer_consistency_weight_x);
    AIM_LANDMARK_SAME_CONTROL_FIELD(reverse_output_direction_x);
    AIM_LANDMARK_SAME_CONTROL_FIELD(reverse_evidence_ratio_seconds_x);
    AIM_LANDMARK_SAME_CONTROL_FIELD(reverse_position_ratio_seconds_x);
    AIM_LANDMARK_SAME_CONTROL_FIELD(reverse_position_peak_error_x);
    AIM_LANDMARK_SAME_CONTROL_FIELD(reverse_translation_seconds_x);
    AIM_LANDMARK_SAME_CONTROL_FIELD(
        reverse_translation_raw_left_x_roi_pixels);
    AIM_LANDMARK_SAME_CONTROL_FIELD(
        reverse_translation_raw_right_x_roi_pixels);
    AIM_LANDMARK_SAME_CONTROL_FIELD(
        reverse_translation_raw_common_x_roi_pixels);
    AIM_LANDMARK_SAME_CONTROL_FIELD(
        reverse_translation_control_evidence_x);
    AIM_LANDMARK_SAME_CONTROL_FIELD(reverse_translation_gap_seconds_x);
    AIM_LANDMARK_SAME_CONTROL_FIELD(reverse_deformation_seconds_x);
    AIM_LANDMARK_SAME_CONTROL_FIELD(
        reverse_required_evidence_ratio_seconds_x);
    AIM_LANDMARK_SAME_CONTROL_FIELD(
        reverse_required_position_ratio_seconds_x);
    AIM_LANDMARK_SAME_CONTROL_FIELD(reverse_probe_direction_x);
    AIM_LANDMARK_SAME_CONTROL_FIELD(reverse_probe_age_ms_x);
    AIM_LANDMARK_SAME_CONTROL_FIELD(reverse_translation_reset_reason_x);
    AIM_LANDMARK_SAME_CONTROL_FIELD(pending_positive_x);
    AIM_LANDMARK_SAME_CONTROL_FIELD(pending_negative_x);
    AIM_LANDMARK_SAME_CONTROL_FIELD(reverse_candidate_x);
    AIM_LANDMARK_SAME_CONTROL_FIELD(reverse_previous_direction_pending_x);
    AIM_LANDMARK_SAME_CONTROL_FIELD(
        reverse_partial_semantics_transition_x);
    AIM_LANDMARK_SAME_CONTROL_FIELD(reverse_deformation_active_x);
    AIM_LANDMARK_SAME_CONTROL_FIELD(reverse_evidence_ready_x);
    AIM_LANDMARK_SAME_CONTROL_FIELD(
        reverse_translation_fresh_evidence_x);
    AIM_LANDMARK_SAME_CONTROL_FIELD(reverse_translation_ready_x);
    AIM_LANDMARK_SAME_CONTROL_FIELD(reverse_position_ready_x);
    AIM_LANDMARK_SAME_CONTROL_FIELD(reverse_position_improvement_reset_x);
    AIM_LANDMARK_SAME_CONTROL_FIELD(reverse_gate_blocked_x);
    AIM_LANDMARK_SAME_CONTROL_FIELD(reverse_probe_active_x);
    AIM_LANDMARK_SAME_CONTROL_FIELD(reverse_probe_limited_x);
    AIM_LANDMARK_SAME_CONTROL_FIELD(pending_inventory_hold_blocked_x);
    AIM_LANDMARK_SAME_CONTROL_FIELD(deadzone_quiet);
    AIM_LANDMARK_SAME_CONTROL_FIELD(shaper_direction_reset_x);
    AIM_LANDMARK_SAME_CONTROL_FIELD(post_alignment_sign_change_blocked_x);
    AIM_LANDMARK_SAME_CONTROL_FIELD(post_alignment_growth_limited_x);
    AIM_LANDMARK_SAME_CONTROL_FIELD(closing_response_tapered_x);
    AIM_LANDMARK_SAME_CONTROL_FIELD(integer_direction_blocked_x);
    AIM_LANDMARK_SAME_CONTROL_FIELD(command_sign_change_blocked_x);
    AIM_LANDMARK_SAME_CONTROL_FIELD(quantization_zero_x);
#undef AIM_LANDMARK_SAME_CONTROL_FIELD
    return true;
}

bool same_control_state(const AimResult& left, const AimResult& right) {
    return left.status == right.status &&
        left.has_target == right.has_target &&
        left.has_command == right.has_command &&
        left.acquisition_range_radius == right.acquisition_range_radius &&
        left.active_range_radius == right.active_range_radius &&
        left.range_locked == right.range_locked &&
        left.range_allows_control == right.range_allows_control &&
        same_target(left.target, right.target) &&
        same_control(left.control, right.control) &&
        left.command.sequence == right.command.sequence &&
        left.command.captured_at == right.command.captured_at &&
        left.command.dx_counts == right.command.dx_counts &&
        left.command.dy_counts == right.command.dy_counts;
}

void test_diagnostic_call_preserves_all_aim_control_state() {
    AimConfig config;
    config.person_class_ids = {0, 2};
    config.head_class_ids = {1, 3};
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 0.0f;
    config.smoothing = 0.475f;
    config.counts_per_pixel_x = 0.425f;
    config.counts_per_pixel_y = 0.40f;
    config.max_counts_per_frame = 14.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 7.5f;
    config.max_delay_compensation_ms = 18.0f;
    config.max_delay_compensation_percent = 12.0f;
    config.enable_prediction = false;
    Aim baseline(config);
    Aim diagnostic(config);
    const auto started_at =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    int valid_landmarks = 0;

    for (int index = 0; index < 64; ++index) {
        AimFrame frame;
        frame.sequence = static_cast<std::uint64_t>(index + 1);
        frame.captured_at = started_at +
            std::chrono::microseconds(index * 4167);
        frame.control_at = frame.captured_at + std::chrono::milliseconds(3);
        frame.roi_width = 320;
        frame.roi_height = 320;
        frame.control_center_x = 160.0f;
        frame.control_center_y = 160.0f;
        frame.lock_active = true;
        const float offset = static_cast<float>((index % 9) - 4) * 0.25f;
        const auto detections = body_and_head(offset);
        frame.detections.assign(detections.begin(), detections.end());

        const AimResult baseline_result = baseline.process(frame);
        const AimResult diagnostic_result = diagnostic.process(frame);
        const auto landmark = aim_landmark::inspect_head_landmark(
            frame.sequence, frame.detections, config, diagnostic_result);
        if (landmark.valid) ++valid_landmarks;
        expect(same_control_state(baseline_result, diagnostic_result),
               "调用 diagnostic landmark 前后，Aim target/base/Y/prediction/"
               "控制诊断与整数命令必须逐字段完全等价");

        if (!baseline_result.has_command) continue;
        const auto completed_at =
            frame.control_at + std::chrono::microseconds(400);
        const bool baseline_completed =
            baseline.record_backend_completed_command(
                frame.sequence, completed_at,
                baseline_result.command.dx_counts,
                baseline_result.command.dy_counts);
        const bool diagnostic_completed =
            diagnostic.record_backend_completed_command(
                frame.sequence, completed_at,
                diagnostic_result.command.dx_counts,
                diagnostic_result.command.dy_counts);
        expect(baseline_completed == diagnostic_completed,
               "diagnostic landmark 不得改变 backend completion 合同");
    }
    expect(valid_landmarks >= 60,
           "等价回归必须实际覆盖唯一同帧 head landmark");
}

} // namespace

int main() {
    test_unique_current_head_is_reported_without_observer_state();
    test_nonfinite_associated_head_fails_closed();
    test_unselected_local_head_is_reported_as_ambiguous();
    test_missing_declared_head_is_an_association_mismatch();
    test_missing_current_observation_keeps_requested_identity_without_point();
    test_occluded_ambiguous_and_no_target_fail_closed();
    test_diagnostic_call_preserves_all_aim_control_state();
    if (failures == 0) {
        std::cout << "Aim landmark tests passed\n";
        return 0;
    }
    std::cerr << failures << " aim landmark test(s) failed\n";
    return 1;
}
