#include "aim/aim.h"
#include "aim_x_filter_reset_fixture.h"
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
void actual_crossing(bool mirror) {
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
        if (s.sequence == 7625) {
            expect(direction * r.command.dx_counts < 0,
                   "过零前须存在旧向命令，不能退化成无历史夹具");
        } else {
            expect(r.control.evaluated && r.target.matched_observation_valid &&
                       r.control.background_motion_use_x == AimBackgroundMotionUse::CONSUMED &&
                       direction * error > config.deadzone_pixels,
                   "过零后须有新鲜同帧背景与死区外反侧误差");
            if (s.sequence == 7626) {
                expect(r.command.dx_counts == 0, "清理旧向记忆当帧仍必须经过零输出");
                expect(r.control.filter_reset_x &&
                           r.control.pre_eligibility_filtered_x_counts == 0.0f &&
                           r.control.filtered_integral_x_counts == 0.0f &&
                           direction * r.control.history_adjusted_x_counts > 0.0f,
                       "账本区分合法滤波输入、清理当帧零状态与末段下一步seed");
            } else {
                expect(direction * r.command.dx_counts > 0,
                       "已清理旧向记忆后不得丢弃首帧更新而再次空发");
                expect(!r.control.filter_reset_x &&
                           direction * r.control.pre_eligibility_filtered_x_counts > 0.0f,
                       "账本必须在下一帧显示新向滤波状态，不沿用清理帧标记");
            }
        }
        std::cout << "mirror=" << mirror << " seq=" << s.sequence
                  << " error=" << error << " q=" << r.command.dx_counts << '\n';
    }
    expect(checked == 3, "必须完整覆盖过零前、零帧与后续新向更新");
}
}
int main() {
    actual_crossing(false);
    actual_crossing(true);
    std::cout << "失败数：" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
