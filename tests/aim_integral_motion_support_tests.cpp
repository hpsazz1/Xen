#include "aim/aim.h"
#include "aim_integral_motion_support_fixture.h"
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
void actual_integral_tail(bool mirror, bool ambiguous_motion = false) {
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
    float previous_left = 0.0f, previous_right = 0.0f;
    const int direction = mirror ? 1 : -1;
    for (const auto& s : aim_integral_motion_support_fixture::kSamples) {
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
        if (ambiguous_motion && s.sequence == 1974) {
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
        if (s.sequence != 1974 && s.sequence != 1976) continue;
        ++checked;
        const float error = r.target.base_aim_x - f.control_center_x;
        const float world_left = r.control.reverse_translation_raw_left_x_roi_pixels -
            f.background_motion_x.dx_roi_pixels;
        const float world_right = r.control.reverse_translation_raw_right_x_roi_pixels -
            f.background_motion_x.dx_roi_pixels;
        const float observation_dt = std::chrono::duration<float>(
            f.background_motion_x.captured_at - f.background_motion_x.previous_captured_at).count();
        const float current_bound = std::min(std::fabs(world_left), std::fabs(world_right)) /
            (0.2216375f / config.counts_per_pixel_x) * r.control.controller_dt_ms / 1000.0f / observation_dt;
        const float observer_bound = std::fabs(r.control.observer_target_velocity_x_counts_per_second) *
            r.control.controller_dt_ms / 1000.0f;
        if (!ambiguous_motion && r.control.target_motion_maintenance_x_counts * error < 0.0f)
            expect(world_left * world_right > 0.0f &&
                       std::fabs(r.control.target_motion_maintenance_x_counts) <= current_bound + .0003f &&
                       std::fabs(r.control.target_motion_maintenance_x_counts) <= observer_bound + .0003f,
                   "实际逆误差维护不得超过当前双边及observer各自步预算");
        expect(r.control.evaluated && r.target.matched_observation_valid &&
                   r.control.background_motion_use_x == AimBackgroundMotionUse::CONSUMED,
               "真实回归必须消费同帧背景和有效目标");
        if (s.sequence == 1974) {
            expect(direction * error < 0.0f && std::fabs(error) < config.deadzone_pixels &&
                       direction * r.control.observer_target_velocity_x_counts_per_second > 0.0f &&
                       direction * r.control.filtered_integral_x_counts < 0.0f &&
                       r.control.proportional_x_counts == 0.0f,
                   "旧侧积分与当前运动估计相反，不能把位置残留当同向维护");
            expect(direction * r.control.filtered_x_counts <= 0.0f &&
                       std::fabs(r.control.filtered_x_counts) < 0.5f &&
                       std::fabs(r.control.filtered_x_counts) <=
                           std::fabs(error) / (0.2216375f / config.counts_per_pixel_x) + .0003f,
                   "旧向积分尾部仍受当前位置额度限制，不能独自补一count");
            if (ambiguous_motion) {
                expect(r.command.dx_counts == 0 &&
                           r.control.target_motion_maintenance_x_counts == 0.0f &&
                           r.control.modelled_response_x_counts == 0.0f,
                       "无共同运动支持时保持原零输出保护");
            } else {
                expect(direction * r.control.target_motion_maintenance_x_counts > 0.0f &&
                           direction * r.command.dx_counts > 0 &&
                           std::fabs(r.control.shaped_x_counts -
                               r.control.filtered_x_counts -
                               r.control.modelled_response_x_counts) < 0.0003f,
                       "实际反向维护必须来自fresh运动预算，与受限PI净合成");
            }
        } else {
            expect(direction * error > config.deadzone_pixels,
                   "下一纠正须有死区外新侧误差");
            expect(direction * r.command.dx_counts > 0,
                   "撤销无支持旧向尾部后，1976不得继续空发而推迟有效纠正");
        }
        std::cout << "mirror=" << mirror << " seq=" << s.sequence
                  << " error=" << error << " q=" << r.command.dx_counts << '\n';
    }
    expect(checked == 2, "必须覆盖旧向尾部和后续有效纠正");
}
}
int main() {
    actual_integral_tail(false);
    actual_integral_tail(true);
    actual_integral_tail(false, true);
    actual_integral_tail(true, true);
    std::cout << "失败数：" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
