#include "aim/aim.h"
#include "aim_unified_maintenance_fixture.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>

namespace {
int failures = 0;
void expect(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "失败：" << message << '\n'; }
}
auto at(std::int64_t ns) {
    return std::chrono::steady_clock::time_point{
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::nanoseconds(ns))};
}
void actual_unified(bool mirror, int mode) {
    AimConfig config;
    config.person_class_ids = {0, 2};
    config.head_class_ids = {1, 3};
    config.high_confidence = 0.25f;
    config.low_confidence = 0.1f;
    config.min_confirmed_hits = 2;
    config.max_lost_frames = 8;
    config.min_iou = 0.1f;
    config.max_center_distance = 0.25f;
    config.switch_margin = 0.2f;
    config.switch_confirm_frames = 3;
    config.switch_cooldown_frames = 5;
    config.acquisition_range_percent = 90.0f;
    config.body_aim_height_ratio = 0.35f;
    config.body_aim_range_percent = 50.0f;
    config.deadzone_pixels = 1.5f;
    config.smoothing = 0.475f;
    config.counts_per_pixel_x = 0.425f;
    config.counts_per_pixel_y = 0.4f;
    config.max_counts_per_frame = 14.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 44.0f;
    config.max_delay_compensation_percent = 15.0f;
    config.enable_prediction = false;
    config.max_prediction_lead_percent = 35.0f;
    config.predicted_gain = 0.5f;
    Aim aim(config);
    const int direction = mirror ? -1 : 1;
    int checked = 0;
    float previous_left = 0.0f, previous_right = 0.0f;
    AimFrame last;
    for (const auto& s : aim_unified_maintenance_fixture::kSamples) {
        if (s.observation_clock_reset) aim.reset();
        AimFrame f;
        f.sequence = s.sequence;
        f.captured_at = at(s.source_ns);
        f.control_at = at(s.control_ns);
        f.roi_width = s.roi_width;
        f.roi_height = s.roi_height;
        f.control_center_x = s.center_x;
        f.control_center_y = s.center_y;
        f.source_pixels_per_roi_pixel_x = s.scale_x;
        f.source_pixels_per_roi_pixel_y = s.scale_y;
        f.lock_active = s.lock_active;
        f.observation_epoch = s.epoch;
        for (int i = 0; i < s.count; ++i) {
            auto d = s.detections[static_cast<std::size_t>(i)];
            if (mirror) {
                const float left = d.x1;
                d.x1 = 2.0f * s.center_x - d.x2;
                d.x2 = 2.0f * s.center_x - left;
            }
            f.detections.push_back(d);
        }
        f.background_motion_x = {s.status, s.previous_sequence, s.background_sequence,
            at(s.previous_ns), at(s.background_ns), s.background_epoch,
            mirror ? -s.bg_dx : s.bg_dx, s.response, s.disagreement, s.patches};
        {
            if (mode == 1) f.background_motion_x = {};
            if (mode == 2) f.background_motion_x.dx_roi_pixels =
                ((f.detections[0].x1 - previous_left) +
                 (f.detections[0].x2 - previous_right)) * 0.5f;
        }
        previous_left = f.detections[0].x1;
        previous_right = f.detections[0].x2;
        last = f;
        const auto r = aim.process(f);
        const auto& c = r.control;
        expect(r.status == AimStatus::SUCCESS, "实际22帧短前缀输入须正常处理");
        const float scalars[] = {c.proportional_x_counts, c.feedforward_x_counts,
            c.desired_before_reverse_x_counts, c.filtered_x_counts,
            c.modelled_response_x_counts, c.shaped_x_counts,
            c.pending_net_x_counts, c.pending_absolute_x_counts,
            c.residual_before_quantization_x_counts,
            c.history_adjusted_x_counts, c.pre_eligibility_filtered_x_counts,
            c.filtered_integral_x_counts, c.target_motion_maintenance_x_counts,
            c.observer_target_velocity_x_counts_per_second,
            c.error_derivative_x_source_pixels_per_second, c.opening_weight_x};
        for (const float value : scalars)
            expect(std::isfinite(value), "原字段和分阶段维护诊断必须有限");
        expect(c.opening_weight_x >= 0.0f && c.opening_weight_x <= 1.0f,
               "opening权重保持有效范围");
        expect(r.command.dy_counts == s.expected_y, "原向与镜像都必须保持原Y输出");
        expect(std::hypot(static_cast<float>(r.command.dx_counts),
                          static_cast<float>(r.command.dy_counts)) <= 14.0f,
               "维护更新不得突破二维14上限");
        if (r.has_command)
            expect(aim.record_backend_completed_command(f.sequence, at(s.backend_ns),
                       r.command.dx_counts, r.command.dy_counts),
                   "每个分支只能确认自身产生的命令");
        if (s.sequence < 2158) continue;
        ++checked;
        const float error = r.target.base_aim_x - f.control_center_x;
        expect(c.filtered_x_counts * error >= 0.0f,
               "位置PI仍只能朝当前误差，维护不改PI保护");
        if (mode != 0) {
            expect(r.command.dx_counts == direction * s.fallback_x[mode - 1],
                   "无BG或无共同运动沿用冻结基线回退，不要求全部零");
            continue;
        }
        expect(c.background_motion_use_x == AimBackgroundMotionUse::CONSUMED &&
                   !c.filter_reset_x, "核心须为同源非Reset观测");
        const float left = c.reverse_translation_raw_left_x_roi_pixels - f.background_motion_x.dx_roi_pixels;
        const float right = c.reverse_translation_raw_right_x_roi_pixels - f.background_motion_x.dx_roi_pixels;
        expect(direction * left > 0.0f && direction * right > 0.0f,
               "实际双边证据必须支持同一世界方向");
        const float observation_dt = std::chrono::duration<float>(
            f.background_motion_x.captured_at - f.background_motion_x.previous_captured_at).count();
        const float current_budget = std::min(std::fabs(left), std::fabs(right)) *
            f.source_pixels_per_roi_pixel_x / (0.2216375f / config.counts_per_pixel_x) *
            (c.controller_dt_ms / 1000.0f) / observation_dt;
        const float observer_budget = std::fabs(c.observer_target_velocity_x_counts_per_second) *
            c.controller_dt_ms / 1000.0f;
        const float budget = std::min(current_budget, observer_budget);
        const float integral_credit = std::min(
            std::max(0.0f, direction * c.filtered_integral_x_counts),
            std::max(0.0f, direction * c.filtered_x_counts));
        const float remaining = std::max(0.0f, budget - integral_credit);
        expect(integral_credit <= std::fabs(c.filtered_x_counts) + .0003f,
               "只扣实际eligible PI内的同向积分份额");
        // 该真实窗口未触及14，model_add为D阻尼之前的实际维护追加。
        expect(std::fabs(direction * c.modelled_response_x_counts - remaining) < .001f,
               "同侧与跨侧必须按同一当前预算减实际积分份额支付维护");
        if (s.sequence == 2158 || s.sequence == 2161) {
            expect(direction * error > 0.0f && c.opening_weight_x == 0.0f &&
                       direction * c.reverse_translation_raw_left_x_roi_pixels < 0.0f &&
                       direction * c.reverse_translation_raw_right_x_roi_pixels < 0.0f,
                   "真实主帧同误差侧但图像closing，世界运动仍同向");
            expect(direction * c.modelled_response_x_counts > 1.0f,
                   "足够当前维护预算不能被位置H压成近零追加");
        }
    }
    expect(checked == 4, "必须覆盖四帧同侧跨侧转换与回退");
    ++last.sequence;
    last.captured_at += std::chrono::milliseconds(10);
    last.control_at = last.captured_at + std::chrono::milliseconds(3);
    last.lock_active = false;
    const auto released = aim.process(last);
    // Aim松键时仍可预计算位置/Y请求；实际发送由Runtime锁键门控制。
    // 本单元仅验证松键不能保留运动维护与量化记忆。
    expect(released.control.observer_target_velocity_x_counts_per_second == 0.0f &&
               released.control.target_motion_maintenance_x_counts == 0.0f &&
               released.control.modelled_response_x_counts == 0.0f &&
               released.control.residual_before_quantization_x_counts == 0.0f &&
               released.control.background_motion_use_x != AimBackgroundMotionUse::CONSUMED,
           "松键须清除观察器维护及余数，不将预计算位置请求误认为实发");

}
}
int main() {
    for (int mode = 0; mode < 3; ++mode) { actual_unified(false, mode); actual_unified(true, mode); }
    std::cout << "失败数：" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
