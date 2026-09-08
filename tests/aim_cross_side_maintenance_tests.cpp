#include "aim/aim.h"
#include "aim_cross_side_maintenance_fixture.h"
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
void actual_cross_side(bool mirror, int mode) {
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
    for (const auto& s : aim_cross_side_maintenance_fixture::kSamples) {
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
        if (s.sequence >= 3168) {
            if (mode == 1) f.background_motion_x = {};
            if (mode == 2) --f.background_motion_x.previous_sequence;
            if (mode == 3) f.background_motion_x.dx_roi_pixels =
                ((f.detections[0].x1 - previous_left) +
                 (f.detections[0].x2 - previous_right)) * 0.5f;
        }
        previous_left = f.detections[0].x1;
        previous_right = f.detections[0].x2;
        last = f;
        const auto r = aim.process(f);
        const auto& c = r.control;
        expect(r.status == AimStatus::SUCCESS, "实际10帧最小输入须正常处理");
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
        if (s.sequence < 3168) continue;
        ++checked;
        const float error = r.target.base_aim_x - f.control_center_x;
        expect(c.evaluated && r.target.matched_observation_valid &&
                   direction * error > 0.0f && !c.filter_reset_x,
               "实际核心帧仍在正误差侧且不是Reset");
        expect(c.filtered_x_counts * error >= 0.0f,
               "位置纠偏份额不得因维护方向改变而反向");
        if (mode == 0) {
            const float left = c.reverse_translation_raw_left_x_roi_pixels -
                f.background_motion_x.dx_roi_pixels;
            const float right = c.reverse_translation_raw_right_x_roi_pixels -
                f.background_motion_x.dx_roi_pixels;
            const float observation_dt = std::chrono::duration<float>(
                f.background_motion_x.captured_at -
                f.background_motion_x.previous_captured_at).count();
            const float current_bound = std::min(std::fabs(left), std::fabs(right)) *
                f.source_pixels_per_roi_pixel_x / (0.2216375f / config.counts_per_pixel_x) *
                (c.controller_dt_ms / 1000.0f) / observation_dt;
            const float observer_bound = std::fabs(c.observer_target_velocity_x_counts_per_second) *
                c.controller_dt_ms / 1000.0f;
            expect(c.background_motion_use_x == AimBackgroundMotionUse::CONSUMED &&
                       direction * left < 0.0f && direction * right < 0.0f,
                   "逆误差维护必须有同帧双边一致的运动支持");
            expect(direction * c.target_motion_maintenance_x_counts < 0.0f &&
                       direction * r.command.dx_counts < 0,
                   "目标仍在移动时，误差换侧不得整项丢掉受支持的维护");
            expect(std::fabs(c.target_motion_maintenance_x_counts) <= current_bound + 0.0003f &&
                       std::fabs(c.target_motion_maintenance_x_counts) <= observer_bound + 0.0003f,
                   "维护同时受当前双边位移和observer步预算限制");
            expect(std::fabs(c.shaped_x_counts - c.filtered_x_counts -
                       c.modelled_response_x_counts) < 0.0003f,
                   "位置和维护按原净请求合成，不分别执行两个整数");
        } else {
            expect(r.command.dx_counts == 0 && c.modelled_response_x_counts == 0.0f,
                   "缺测、错帧对或无共同运动时，不能凭旧observer逆误差发令");
        }
    }
    expect(checked == 2, "必须覆盖连续两帧跨侧维护及对应负控");
    ++last.sequence;
    last.captured_at += std::chrono::milliseconds(10);
    last.control_at = last.captured_at + std::chrono::milliseconds(3);
    last.lock_active = false;
    const auto released = aim.process(last);
    expect(released.command.dx_counts == 0 && released.command.dy_counts == 0,
           "跨侧维护后松键必须立即停发，不等待余数或观察器耗尽");

}
// 当前图像点固定在软死区一侧，BG说明目标仍在反向移动。
// 净请求不足半count，必须复用单一余数累计，不能每帧独立舍入成零。
void subpixel_maintenance(bool mirror) {
    AimConfig c;
    c.min_confirmed_hits = 1;
    c.counts_per_pixel_x = 0.425f;
    c.counts_per_pixel_y = 0.4f;
    c.smoothing = 0.475f;
    c.enable_delay_compensation = true;
    c.control_delay_ms = 15.0f;
    c.max_delay_compensation_ms = 44.0f;
    c.enable_prediction = false;
    c.max_counts_per_frame = 14.0f;
    Aim aim(c), neutral(c);
    const float direction = mirror ? -1.0f : 1.0f;
    float requested = 0.0f;
    int issued = 0;
    AimFrame f;
    for (int i = 0; i < 64; ++i) {
        f = {};
        f.sequence = 100 + i;
        f.observation_epoch = 17;
        f.captured_at = at(10000000000LL + 4000000LL * i);
        f.control_at = f.captured_at + std::chrono::milliseconds(3);
        f.roi_width = f.roi_height = 320;
        f.control_center_x = f.control_center_y = 160.0f;
        f.lock_active = true;
        f.detections.push_back({140.0f + direction * .3f, 140.0f,
                               180.0f + direction * .3f, 200.0f, .95f, 0});
        f.background_motion_x = {AimBackgroundMotionStatus::VALID,
            f.sequence - 1, f.sequence,
            f.captured_at - std::chrono::milliseconds(4), f.captured_at,
            17, direction * .15f, .9f, 0.0f, 2};
        const auto r = aim.process(f);
        auto no_camera = f;
        no_camera.background_motion_x.dx_roi_pixels = 0.0f;
        const auto y_control = neutral.process(no_camera);
        expect(r.command.dy_counts == y_control.command.dy_counts,
               "跨侧小数维护不得改变相同几何的Y控制");
        expect(std::fabs(r.control.shaped_x_counts) < .5f,
               "本场景每步维护必须不足半count");
        requested += r.control.shaped_x_counts;
        issued += r.command.dx_counts;
        if (r.has_command)
            aim.record_backend_completed_command(f.sequence, f.control_at,
                r.command.dx_counts, r.command.dy_counts);
        if (y_control.has_command)
            neutral.record_backend_completed_command(f.sequence, f.control_at,
                y_control.command.dx_counts, y_control.command.dy_counts);
    }
    std::cout << "small mirror=" << mirror << " requested=" << requested << " issued=" << issued << '\n';
    expect(direction * issued < 0 && std::fabs(requested - issued) <= .5003f,
           "有效亚计数维护须累计发出，累计误差不超过一次舍入余数");
    ++f.sequence;
    f.captured_at += std::chrono::milliseconds(4);
    f.control_at += std::chrono::milliseconds(4);
    f.lock_active = false;
    const auto released = aim.process(f);
    expect(released.command.dx_counts == 0 && released.command.dy_counts == 0,
           "小数维护累计中松键仍须立即停发");
}
}
int main() {
    for (int mode = 0; mode < 4; ++mode) {
        actual_cross_side(false, mode);
        actual_cross_side(true, mode);
    }
    subpixel_maintenance(false);
    subpixel_maintenance(true);
    std::cout << "失败数：" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
