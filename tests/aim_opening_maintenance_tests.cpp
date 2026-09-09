#include "aim/aim.h"
#include "aim_opening_maintenance_fixture.h"
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
void actual_opening_maintenance(bool mirror, bool ambiguous_motion = false) {
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
    for (const auto& s : aim_opening_maintenance_fixture::kSamples) {
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
        if (ambiguous_motion && s.sequence == 3171) {
            f.background_motion_x.dx_roi_pixels = ((f.detections[0].x1 - previous_left) +
                (f.detections[0].x2 - previous_right)) * 0.5f;
        }
        previous_left = f.detections[0].x1;
        previous_right = f.detections[0].x2;
        const auto r = aim.process(f);
        const auto& c = r.control;
        expect(r.status == AimStatus::SUCCESS, "实际15帧最小输入须正常处理");
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
        if (r.command.dx_counts != 0)
            expect(r.command.dx_counts * (r.target.base_aim_x - f.control_center_x) > 0.0f,
                   "非零X请求必须朝当前固定base误差方向");
        if (r.has_command)
            expect(aim.record_backend_completed_command(f.sequence, at(s.backend_ns),
                       r.command.dx_counts, r.command.dy_counts),
                   "每个分支只能确认自身产生的命令");
        if (s.sequence != 3171 && s.sequence != 3172) continue;
        ++checked;
        expect(c.evaluated && r.target.matched_observation_valid &&
                   c.background_motion_use_x == AimBackgroundMotionUse::CONSUMED &&
                   !c.filter_reset_x,
               "实际主窗须有同源观测且不是Reset");
        if (s.sequence == 3171) {
            expect(c.opening_weight_x == 0.0f,
                   "图像收拢的原opening判定不受世界运动维护改变");
            if (ambiguous_motion) {
                std::cout << "closing mirror=" << mirror << " q=" << r.command.dx_counts
                          << " pi=" << c.filtered_x_counts << " add=" << c.modelled_response_x_counts
                          << " shaped=" << c.shaped_x_counts << " residual=" << c.residual_before_quantization_x_counts
                          << " error=" << r.target.base_aim_x - f.control_center_x << '\n';
                expect((c.reverse_translation_raw_left_x_roi_pixels - f.background_motion_x.dx_roi_pixels) *
                           (c.reverse_translation_raw_right_x_roi_pixels - f.background_motion_x.dx_roi_pixels) <= 0.0f,
                       "负控必须实际消除校正双边的共同方向");
                const float position_headroom = std::fabs(r.target.base_aim_x - f.control_center_x) *
                    f.source_pixels_per_roi_pixel_x / (0.2216375f / config.counts_per_pixel_x);
                expect(direction * r.command.dx_counts < 0 &&
                           std::fabs(c.filtered_x_counts + c.modelled_response_x_counts) <= position_headroom + 0.0003f &&
                           std::fabs(r.command.dx_counts - c.shaped_x_counts -
                               c.residual_before_quantization_x_counts) <= 0.5003f,
                       "无当前共同运动时仍受原位置额度约束，净整数只含本分支舍入余额");
            } else {
                const float left = c.reverse_translation_raw_left_x_roi_pixels -
                    f.background_motion_x.dx_roi_pixels;
                const float right = c.reverse_translation_raw_right_x_roi_pixels -
                    f.background_motion_x.dx_roi_pixels;
                const float dt = std::chrono::duration<float>(
                    f.background_motion_x.captured_at -
                    f.background_motion_x.previous_captured_at).count();
                const float bound = std::fabs(0.5f * (left + right)) *
                    f.source_pixels_per_roi_pixel_x / (0.2216375f / config.counts_per_pixel_x) *
                    c.controller_dt_ms / 1000.0f / dt;
                expect(direction * left < 0.0f && direction * right < 0.0f &&
                           direction * c.target_motion_maintenance_x_counts < 0.0f &&
                           std::fabs(c.target_motion_maintenance_x_counts) <= bound + 0.0003f,
                       "图像收拢但世界目标仍同向时，维护由当前同向中心幅度支持");
                expect(direction * c.filtered_x_counts < 0.0f &&
                           direction * r.command.dx_counts < -3,
                       "位置纠偏保持方向，同向维护不能仍被收拢位置额度压掉");
            }
        } else {
            expect(c.opening_weight_x > 0.0f &&
                       direction * c.target_motion_maintenance_x_counts < 0.0f,
                   "实际误差扩大且目标维护方向一致");
            expect(direction * r.command.dx_counts <= -3,
                   "3172同向追赶不能仍被纯位置额度压在两个counts以内");
        }
    }
    expect(checked == 2, "必须覆盖收拢负控和同向追赶主帧");
}
}
int main() {
    actual_opening_maintenance(false);
    actual_opening_maintenance(true);
    actual_opening_maintenance(false, true);
    actual_opening_maintenance(true, true);
    std::cout << "失败数：" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
