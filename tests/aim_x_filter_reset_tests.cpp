#include "aim/aim.h"
#include "aim_x_filter_reset_fixture.h"
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
void actual_crossing(bool mirror, bool ambiguous_motion = false) {
    AimConfig config;
    config.person_class_ids = {0, 2};
    config.head_class_ids = {1, 3};
    config.smoothing = 0.475f;
    config.counts_per_pixel_x = 0.425f;
    config.counts_per_pixel_y = 0.4f;
    config.max_counts_per_frame = 14.0f;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 44.0f;
    config.enable_delay_compensation = true;
    config.enable_prediction = false;
    Aim aim(config);
    int checked = 0;
    float reset_request = 0.0f;
    float previous_left = 0.0f, previous_right = 0.0f;
    const int direction = mirror ? 1 : -1;
    for (const auto& s : aim_x_filter_reset_fixture::kSamples) {
        AimFrame f;
        f.sequence = s.sequence;
        f.captured_at = at(s.source_ns);
        f.control_at = at(s.control_ns);
        f.roi_width = f.roi_height = 320;
        f.control_center_x = f.control_center_y = 160.0f;
        f.lock_active = true;
        f.observation_epoch = s.epoch;
        for (int i = 0; i < s.count; ++i) {
            auto d = s.detections[static_cast<std::size_t>(i)];
            if (mirror) {
                const float left = d.x1;
                d.x1 = 320.0f - d.x2;
                d.x2 = 320.0f - left;
            }
            f.detections.push_back(d);
        }
        f.background_motion_x = {s.status, s.previous_sequence, s.sequence,
            at(s.previous_ns), f.captured_at, s.epoch,
            mirror ? -s.bg_dx : s.bg_dx, s.response, s.disagreement,
            static_cast<int>(s.patches)};
        if (ambiguous_motion && s.sequence == 7627) {
            // 相同前缀，仅令末帧校正后的双边跨零，撤销共同运动证据。
            f.background_motion_x.dx_roi_pixels =
                ((f.detections[0].x1 - previous_left) +
                 (f.detections[0].x2 - previous_right)) * 0.5f;
        }
        previous_left = f.detections[0].x1;
        previous_right = f.detections[0].x2;
        const auto r = aim.process(f);
        expect(r.status == AimStatus::SUCCESS, "实际最小化输入须正常处理");
        expect(std::isfinite(r.control.history_adjusted_x_counts) &&
                   std::isfinite(r.control.pre_eligibility_filtered_x_counts) &&
                   std::isfinite(r.control.filtered_integral_x_counts) &&
                   std::isfinite(r.control.target_motion_maintenance_x_counts) &&
                   std::isfinite(r.control.error_derivative_x_source_pixels_per_second) &&
                   r.control.opening_weight_x >= 0.0f &&
                   r.control.opening_weight_x <= 1.0f,
               "实际换向账本必须输出有限分阶段标量");
        if (r.has_command) {
            expect(aim.record_backend_completed_command(
                f.sequence, at(s.backend_ns), r.command.dx_counts, r.command.dy_counts),
                "回归只能确认当前分支自身请求");
        }
        expect(std::hypot(static_cast<float>(r.command.dx_counts),
                          static_cast<float>(r.command.dy_counts)) <= 14.0f,
               "换向更新不得突破二维物理上限");
        if (!mirror) expect(r.command.dy_counts == s.expected_y, "X修复保持原Y请求");
        if (s.sequence < 7625 || s.sequence > 7627) continue;
        ++checked;
        const float error = r.target.base_aim_x - f.control_center_x;
        const float world_left = r.control.reverse_translation_raw_left_x_roi_pixels -
            f.background_motion_x.dx_roi_pixels;
        const float world_right = r.control.reverse_translation_raw_right_x_roi_pixels -
            f.background_motion_x.dx_roi_pixels;
        const float observation_dt = std::chrono::duration<float>(
            f.background_motion_x.captured_at - f.background_motion_x.previous_captured_at).count();
        const float current_bound = std::fabs(0.5f * (world_left + world_right)) /
            (0.2216375f / config.counts_per_pixel_x) * r.control.controller_dt_ms / 1000.0f / observation_dt;
        if (!ambiguous_motion && r.control.target_motion_maintenance_x_counts * error < 0.0f)
            expect(world_left * world_right > 0.0f &&
                       std::fabs(r.control.target_motion_maintenance_x_counts) <= current_bound + .0003f &&
                       r.control.target_motion_maintenance_x_counts *
                           r.control.observer_target_velocity_x_counts_per_second > 0.0f,
                   "实际逆误差维护须获observer同向支持且不超过当前中心步预算");
        if (s.sequence == 7625) {
            expect(direction * r.command.dx_counts < 0,
                   "过零前须存在旧向命令，不能退化成无历史夹具");
        } else {
            expect(r.control.evaluated && r.target.matched_observation_valid &&
                       r.control.background_motion_use_x == AimBackgroundMotionUse::CONSUMED &&
                       direction * error > config.deadzone_pixels,
                   "过零后须有新鲜同帧背景与死区外反侧误差");
            if (s.sequence == 7626) {
                reset_request = r.control.history_adjusted_x_counts;
                expect(r.command.dx_counts == 0, "清理旧向记忆当帧仍必须经过零输出");
                expect(r.control.filter_reset_x &&
                           r.control.pre_eligibility_filtered_x_counts == 0.0f &&
                           r.control.filtered_integral_x_counts == 0.0f &&
                           direction * r.control.history_adjusted_x_counts > 0.0f,
                       "账本区分合法滤波输入、清理当帧零状态与末段下一步seed");
            } else {
                const float expected_filtered =
                    reset_request * config.smoothing * (1.0f - config.smoothing) +
                    r.control.history_adjusted_x_counts * config.smoothing;
                expect(std::fabs(r.control.pre_eligibility_filtered_x_counts -
                                 expected_filtered) < 0.0003f,
                       "Reset当帧的合法输入必须完整进入下一帧滤波，不得丢掉seed");
                expect(direction * r.control.filtered_x_counts > 0.0f,
                       "位置纠偏份额仍须朝新侧误差");
                if (ambiguous_motion) {
                    expect(direction * r.command.dx_counts > 0 &&
                               direction * r.control.modelled_response_x_counts >= 0.0f,
                           "无共同运动支持时，原首新向纠偏不得被丢弃或无据抵消");
                } else {
                    // 前缀自身命令会改变模型回退状态；维护按本分支实际观察器
                    // 方向验证。位置seed由上方独立断言，不拿固定净整数代替。
                    expect(r.control.modelled_response_x_counts *
                               r.control.observer_target_velocity_x_counts_per_second >= 0.0f &&
                               std::fabs(r.control.modelled_response_x_counts -
                                   r.control.target_motion_maintenance_x_counts) < 0.0003f &&
                               std::fabs(r.control.shaped_x_counts -
                                   r.control.filtered_x_counts -
                                   r.control.modelled_response_x_counts) < 0.0003f &&
                               r.command.dx_counts * r.control.shaped_x_counts >= 0.0f &&
                               std::fabs(r.command.dx_counts - r.control.shaped_x_counts -
                                   r.control.residual_before_quantization_x_counts) <= 0.5003f,
                           "实际维护按本分支方向净合成舍入，不强行执行独立PI整数");
                }
                expect(!r.control.filter_reset_x &&
                           direction * r.control.pre_eligibility_filtered_x_counts > 0.0f,
                       "账本必须在下一帧显示新向滤波状态，不沿用清理帧标记");
            }
        }
        std::cout << "mirror=" << mirror << " seq=" << s.sequence
                  << " error=" << error << " q=" << r.command.dx_counts
                  << " pi=" << r.control.filtered_x_counts
                  << " m=" << r.control.target_motion_maintenance_x_counts
                  << " add=" << r.control.modelled_response_x_counts
                  << " shaped=" << r.control.shaped_x_counts
                  << " residual=" << r.control.residual_before_quantization_x_counts << '\n';
    }
    expect(checked == 3, "必须完整覆盖过零前、零帧与后续新向更新");
}
}
int main() {
    actual_crossing(false);
    actual_crossing(true);
    actual_crossing(false, true);
    actual_crossing(true, true);
    std::cout << "失败数：" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
