#include "aim/aim.h"
#include "aim_world_observation_gap_fixture.h"
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
void actual_gap(bool mirror, int mode) {
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
    float previous_velocity = 0.0f;
    bool observed_world = false;
    int startup_nonzero = 0;
    AimFrame last;
    for (const auto& s : aim_world_observation_gap_fixture::kSamples) {
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
        if (mode == 3 || (mode == 1 && (s.sequence == 4216 || s.sequence == 4218)))
            f.background_motion_x = {};
        if (mode == 2 && (s.sequence == 4216 || s.sequence == 4218)) {
            f.background_motion_x.status = AimBackgroundMotionStatus::VALID;
            --f.background_motion_x.previous_sequence;
        }
        last = f;
        const auto r = aim.process(f);
        const auto& c = r.control;
        expect(r.status == AimStatus::SUCCESS, "实际29帧短前缀输入须正常处理");
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
        const float velocity = c.observer_target_velocity_x_counts_per_second;
        if (mode == 3) {
            expect(c.background_motion_use_x != AimBackgroundMotionUse::CONSUMED,
                   "启动负控整段没有世界观测");
            expect(std::fabs(velocity - direction * s.startup_velocity) <=
                       .003f + .00003f * std::fabs(s.startup_velocity),
                   "从未获得世界观测时必须保留冻结基线模型更新");
            if (velocity != 0.0f) ++startup_nonzero;
        } else {
            if (s.sequence == 4216 || s.sequence == 4218) {
                ++checked;
                expect(observed_world && c.background_motion_use_x != AimBackgroundMotionUse::CONSUMED,
                       "实际缺口必须位于有效世界观测之后");
                expect(std::fabs(velocity - previous_velocity) < .003f,
                       "缺少世界测量不能用backend模型伪观测改写已有目标速度");
                if (mode == 0)
                    expect(s.status == AimBackgroundMotionStatus::INCONSISTENT,
                           "原实际缺口保留INCONSISTENT测量状态");
            }
            if (s.sequence == 4217) {
                ++checked;
                const float dt = std::chrono::duration<float>(
                    f.background_motion_x.captured_at - f.background_motion_x.previous_captured_at).count();
                expect(c.background_motion_use_x == AimBackgroundMotionUse::CONSUMED &&
                           c.reverse_translation_raw_left_x_roi_pixels == 0.0f &&
                           c.reverse_translation_raw_right_x_roi_pixels == 0.0f,
                       "实际重复内容仍作为有效零位移测量，不无限冻结静止证据");
                expect(std::fabs(velocity) < std::fabs(previous_velocity) &&
                           std::fabs(velocity - previous_velocity * .008f / (.008f + dt)) < .003f,
                       "有效零观测继续按原source时间与8ms滤波更新");
            }
        }
        if (c.background_motion_use_x == AimBackgroundMotionUse::CONSUMED) observed_world = true;
        previous_velocity = velocity;
    }
    if (mode == 3) expect(startup_nonzero > 0, "模型启动负控必须实际发生非零更新");
    else expect(checked == 3, "必须覆盖两个缺口与其间有效零观测");
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
    // 相同目标重新按键且仍缺BG，必须恢复尚未建立世界观测的模型启动。
    // 新source/control时刻均严格递增；不借换目标或reset掩盖松键生命周期。
    ++last.sequence;
    last.captured_at += std::chrono::milliseconds(4);
    last.control_at = last.captured_at + std::chrono::milliseconds(3);
    last.lock_active = true;
    last.background_motion_x = {};
    for (auto& detection : last.detections) {
        detection.x1 += 2.0f * direction;
        detection.x2 += 2.0f * direction;
    }
    const auto resumed = aim.process(last);
    std::cout << "repress mode=" << mode << " mirror=" << mirror
        << " status=" << static_cast<int>(resumed.status) << " target=" << resumed.has_target
        << " id=" << resumed.target.track_id << "/" << released.target.track_id
        << " bg=" << static_cast<int>(resumed.control.background_motion_use_x)
        << " v=" << resumed.control.observer_target_velocity_x_counts_per_second
        << " raw=" << resumed.control.reverse_translation_raw_left_x_roi_pixels
        << "/" << resumed.control.reverse_translation_raw_right_x_roi_pixels << '\n';
    // 旧backend命令仍可能处于15ms模型窗口；raw右移不等于world右移。
    // 本帧两边完全同移，模型测量区间退化为该位移；从松键清零状态
    // 更新的闭式结果用于验证来源标记确已清除，而不是要求任意方向。
    const float model_dt = resumed.control.controller_dt_ms / 1000.0f;
    const float expected_model_velocity =
        (resumed.control.reverse_translation_raw_left_x_roi_pixels * last.source_pixels_per_roi_pixel_x -
         resumed.control.observer_camera_motion_x_source_pixels) /
        (0.2216375f / config.counts_per_pixel_x) / (.008f + model_dt);
    expect(resumed.status == AimStatus::SUCCESS && resumed.has_target &&
               resumed.target.track_id == released.target.track_id &&
               resumed.control.background_motion_use_x == AimBackgroundMotionUse::MISSING &&
               resumed.control.reverse_translation_raw_left_x_roi_pixels ==
                   resumed.control.reverse_translation_raw_right_x_roi_pixels &&
               std::isfinite(resumed.control.observer_target_velocity_x_counts_per_second) &&
               expected_model_velocity != 0.0f &&
               std::fabs(resumed.control.observer_target_velocity_x_counts_per_second -
                   expected_model_velocity) < .01f,
           "松键清除来源状态后，同目标缺BG重按必须从零恢复原相机模型更新");

}
}
int main() {
    for (int mode = 0; mode < 4; ++mode) { actual_gap(false, mode); actual_gap(true, mode); }
    std::cout << "失败数：" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
