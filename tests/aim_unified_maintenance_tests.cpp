#include "aim_consumer_contract_support.h"
#include "aim/aim.h"
#include "aim_unified_maintenance_fixture.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {
struct Snapshot { int x, y; float velocity; };
std::vector<Snapshot> snapshots[4][2];
int failures = 0;
std::string context;
void expect(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "失败：" << context << "：" << message << '\n'; }
}
auto at(std::int64_t ns) {
    return std::chrono::steady_clock::time_point{
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::nanoseconds(ns))};
}
void actual_unified(bool mirror, int mode) {
    context="actual,m="+std::to_string(mode)+",mirror="+std::to_string(mirror);
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
            if (mode == 3) {
                f.background_motion_x = {};
                f.background_motion_x.status = AimBackgroundMotionStatus::VALID;
                f.background_motion_x.dx_roi_pixels = 1000.0f;
            }
            if (mode == 2) f.background_motion_x.dx_roi_pixels =
                ((f.detections[0].x1 - previous_left) +
                 (f.detections[0].x2 - previous_right)) * 0.5f;
        }
        previous_left = f.detections[0].x1;
        previous_right = f.detections[0].x2;
        last = f;
        const auto r = aim.process(f);
        const auto& c = r.control;
        snapshots[mode][mirror].push_back({r.command.dx_counts, r.command.dy_counts,
            c.observer_target_velocity_x_counts_per_second});
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
        // Y采用下方相同X上下文拒绝BG配对及独立闭环，不把旧版本整数当物理真值。
        expect(r.command.dx_counts == std::lround(c.shaped_x_counts +
                   c.residual_before_quantization_x_counts),
               "真实前缀各分支须保持最近整数与单余数契约");
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
        expect(c.proportional_x_counts * error >= 0.0f,
               "源位置P仍朝源误差，执行反馈允许与运动维护抵消");
        if (mode != 0) {
            if (mode == 2) {
                expect(c.background_motion_use_x == AimBackgroundMotionUse::CONSUMED &&
                           std::fabs(c.target_motion_maintenance_x_counts) < .001f &&
                           std::fabs(c.modelled_response_x_counts) < .001f,
                       "同源零世界平移不得保留运动维护；位置纠偏不要求整数全零");
            } else {
                expect(c.background_motion_use_x != AimBackgroundMotionUse::CONSUMED &&
                           !c.residual_background_role_x &&
                           c.execution_world_preview_x_counts == 0.0,
                       "缺失或不合格BG不能授权独立世界预览角色");
            }
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
        const float current_budget = std::fabs(0.5f * (left + right)) *
            f.source_pixels_per_roi_pixel_x / .5215f *
            (c.controller_dt_ms / 1000.0f) / observation_dt;
        expect(direction * c.observer_target_velocity_x_counts_per_second > 0.0f,
               "当前维护仍须获得observer方向支持");
        const float budget = current_budget;
        expect(std::fabs(direction * c.target_motion_maintenance_x_counts - budget) < .001f,
               "当前原始中心位移预算须保留量纲和来源");
        const float nominal = c.observer_target_velocity_x_counts_per_second *
            c.controller_dt_ms / 1000.0f;
        expect(c.residual_role_x && c.residual_background_role_x &&
                   c.modelled_response_x_counts * nominal >= 0.0f &&
                   std::fabs(c.modelled_response_x_counts) <= std::fabs(nominal) + .001f,
               "实际M归属observer控制步预算，允许限幅但不得扩大或翻转");
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
void alternating_displacement(bool mirror, std::int64_t interval_ns, bool alternating_width = false) {
    context="alternating,mirror="+std::to_string(mirror)+",dt="+std::to_string(interval_ns)+",width="+std::to_string(alternating_width);
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.counts_per_pixel_x = 0.2216375f;
    config.deadzone_pixels = 1.5f;
    config.body_aim_height_ratio = 1.0f / 3.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 44.0f;
    config.enable_prediction = false;
    config.max_counts_per_frame = 14.0f;
    Aim aim(config);
    const float direction = mirror ? -1.0f : 1.0f;
    float budget_sum = 0.0f, shaped_sum = 0.0f;
    float first_residual = 0.0f, last_residual = 0.0f;
    int issued_sum = 0;
    // 固定图像基点与等间隔观测，背景平移给出6/2像素交替的同向运动。
    // 固定标定plant为.5215；40帧总位移160，单位与位置增益独立。
    for (int i = 0; i < 70; ++i) {
        const float displacement = direction * (i >= 60 ? 0.25f :
            (alternating_width ? 4.0f : (i % 2 ? 6.0f : 2.0f)));
        // 中心平移保持4，宽度往返只改变双边形变，不应累计扣除平移。
        // 减速前一帧结束形变，使减速检查独立于双边异号回退合同。
        const float width_change = alternating_width && i < 59 && i % 2 ? 1.0f : 0.0f;
        AimFrame f;
        f.sequence = 100 + i;
        f.observation_epoch = 17;
        f.captured_at = at(10000000000LL + interval_ns * i);
        f.control_at = f.captured_at + std::chrono::milliseconds(12);
        f.roi_width = f.roi_height = 320;
        f.control_center_x = f.control_center_y = 160;
        f.lock_active = true;
        f.detections.push_back({140.0f + direction * 0.25f - width_change, 140.0f,
            180.0f + direction * 0.25f + width_change, 200.0f, 0.95f, 0});
        f.background_motion_x = {AimBackgroundMotionStatus::VALID, f.sequence - 1, f.sequence,
            f.captured_at - std::chrono::nanoseconds(interval_ns), f.captured_at, 17,
            -displacement, 0.9f, 0.0f, 2};
        const auto r = aim.process(f);
        const auto& c = r.control;
        if (i >= 20) {
            expect(c.background_motion_use_x == AimBackgroundMotionUse::CONSUMED &&
                       !c.filter_reset_x && direction * c.observer_target_velocity_x_counts_per_second > 0.0f,
                   "交替与减速段须持续同源同向且无Reset");
            expect(std::fabs(r.target.base_aim_x - f.control_center_x) < config.deadzone_pixels &&
                       std::fabs(c.proportional_x_counts) < 0.0003f,
                   "固定死区内位置不产生追赶比例份额");
            expect(r.command.dy_counts == 0 && std::abs(r.command.dx_counts) <= 14 &&
                       std::isfinite(c.shaped_x_counts) && std::isfinite(c.target_motion_maintenance_x_counts),
                   "合成仍保持Y、14上限及有限值");
            if (i < 60) {
                if (i == 20) first_residual = c.residual_before_quantization_x_counts;
                budget_sum += direction * c.target_motion_maintenance_x_counts;
                shaped_sum += c.shaped_x_counts;
                issued_sum += r.command.dx_counts;
                last_residual = c.shaped_x_counts + c.residual_before_quantization_x_counts -
                    static_cast<float>(r.command.dx_counts);
            } else {
                expect(std::fabs(c.target_motion_maintenance_x_counts - displacement / .5215f) < 0.0003f,
                       "减速当帧必须撤回旧observer幅度，只保留本帧小位移预算");
            }
        }
        if (r.has_command)
            expect(aim.record_backend_completed_command(f.sequence, f.control_at,
                       r.command.dx_counts, r.command.dy_counts),
                   "交替分支只确认自己的命令");
    }
    expect(std::fabs(budget_sum - 160.0f / .5215f) < 0.01f,
           alternating_width
               ? "对称宽度往返不能侵蚀已知中心平移的累计维护预算"
               : "交替同向位移不能因低通与当前值取小而持续丢失累计维护预算");
    expect(std::fabs(static_cast<float>(issued_sum) - shaped_sum - first_residual + last_residual) < 0.001f &&
               std::fabs(last_residual) <= 0.5003f,
           "净请求沿用单余数累计守恒，不要求每帧独立整数维护");
}


void startup_model_oracle() {
    AimConfig config;
    config.min_confirmed_hits=1; config.body_aim_height_ratio=1.0f/3.0f;
    config.enable_delay_compensation=true; config.control_delay_ms=15;
    config.counts_per_pixel_x=.425f; config.counts_per_pixel_y=.4f;
    config.acquisition_range_percent=100; config.enable_prediction=false;
    Aim aim(config);
    float expected=0, previous_left=0;
    int checked=0;
    int positive_p = 0, negative_p = 0;
    for(int i=0;i<80;++i) {
        AimFrame f;
        f.sequence=i+1; f.observation_epoch=81; f.lock_active=true;
        f.captured_at=at(30000000000LL+5000000LL*i);
        f.control_at=f.captured_at+std::chrono::milliseconds(i%2?1:2);
        f.roi_width=f.roi_height=320; f.control_center_x=f.control_center_y=160;
        const float left=116.0f+.4f*i;
        f.detections.push_back({left,120,left+48,240,.95f,0});
        const auto r=aim.process(f);
        expect(r.status==AimStatus::SUCCESS && r.has_target,"模型oracle输入处理成功且目标有效");
        const float source_error = left + 24.0f - 160.0f;
        if (std::fabs(source_error) > config.deadzone_pixels) {
            expect(r.control.proportional_x_counts * source_error > 0.0f,
                   "独立已知源误差必须得到非零同向源P，不以死区零值假绿");
            positive_p += r.control.proportional_x_counts > 0.0f;
            negative_p += r.control.proportional_x_counts < 0.0f;
        }
        if(i) {
            const float measurement=(left-previous_left)/.5215f/.005f;
            expected+=.005f/(.008f+.005f)*(measurement-expected);
            expect(std::fabs(r.control.observer_target_velocity_x_counts_per_second-expected)<.03f,
                   "无BG零应用的已知同源平移须匹配独立source-dt模型递推");
            expect(r.control.background_motion_use_x!=AimBackgroundMotionUse::CONSUMED &&
                   std::fabs(r.control.observer_camera_motion_x_source_pixels)<.0001f,
                   "零应用模型不能伪造camera或世界观测资格");
            ++checked;
        }
        if(r.has_command) expect(aim.record_backend_completed_command(f.sequence,f.control_at,0,0),
                                 "模型oracle每条已产生请求只确认合法零应用");
        previous_left=left;
    }
    expect(positive_p > 0 && negative_p > 0,"源P方向负控必须覆盖两侧非零误差");
    expect(checked==79 && expected>0,"模型oracle必须覆盖非空启动及非零速度");
}

void pure_maintenance(int direction) {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.body_aim_height_ratio = 1.0f / 3.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 0.0f;
    config.enable_prediction = false;
    config.deadzone_pixels = 1.5f;
    config.counts_per_pixel_x = .425f;
    config.max_counts_per_frame = 14.0f;
    Aim aim(config);
    int checked = 0;
    for (int i = 0; i < 80; ++i) {
        AimFrame f;
        f.sequence = 800 + i;
        f.observation_epoch = 93;
        f.captured_at = f.control_at = at(80000000000LL + 8000000LL * i);
        f.roi_width = f.roi_height = 320;
        f.control_center_x = f.control_center_y = 160;
        f.lock_active = true;
        f.detections.push_back({140, 140, 180, 200, .95f, 0});
        f.background_motion_x = {AimBackgroundMotionStatus::VALID,
            f.sequence - 1, f.sequence, f.captured_at - std::chrono::milliseconds(8),
            f.captured_at, 93, -2.0f * direction, .9f, 0.0f, 2};
        const auto r = aim.process(f);
        expect(r.status == AimStatus::SUCCESS && r.has_target,
               "维护oracle必须保持有效目标");
        if (i >= 20) {
            ++checked;
            expect(r.control.background_motion_use_x == AimBackgroundMotionUse::CONSUMED &&
                       r.control.residual_background_role_x &&
                       std::fabs(r.control.proportional_x_counts) < .0001f &&
                       std::fabs(r.control.filtered_x_counts) < .0001f,
                   "独立维护oracle必须同源且位置反馈严格为零");
            expect(std::fabs(r.control.modelled_response_x_counts - 2.0f * direction / .5215f) < .001f &&
                       std::fabs(r.control.shaped_x_counts - 2.0f * direction / .5215f) < .001f,
                   "零位置反馈且不饱和时已知世界平移须完整支付，不能少付或多付M");
        }
        if (r.has_command)
            expect(aim.record_backend_completed_command(f.sequence, f.control_at,
                       r.command.dx_counts, r.command.dy_counts), "维护oracle只回执自身输出");
    }
    expect(checked == 60, "独立维护oracle覆盖非空稳定段");
}

} // namespace
int main() {
    for (const std::int64_t interval_ns : {4166667LL, 8000000LL}) {
        alternating_displacement(false, interval_ns);
        alternating_displacement(true, interval_ns);
        alternating_displacement(false, interval_ns, true);
        alternating_displacement(true, interval_ns, true);
    }
    for (int mode = 0; mode < 4; ++mode) { actual_unified(false, mode); actual_unified(true, mode); }
    for (int mirror = 0; mirror < 2; ++mirror) {
        expect(snapshots[1][mirror].size() == 22 && snapshots[3][mirror].size() == 22,
               "拒绝BG的相同X上下文对照必须覆盖完整22帧");
        for (std::size_t i = 0; i < std::min(snapshots[1][mirror].size(), snapshots[3][mirror].size()); ++i) {
            const auto a = snapshots[1][mirror][i];
            const auto b = snapshots[3][mirror][i];
            expect(a.x == b.x && a.y == b.y && std::fabs(a.velocity - b.velocity) < .001f,
                   "毒值无效BG与缺失BG在同X上下文须保持相同X/Y及模型回退");
        }
    }
    startup_model_oracle();
    pure_maintenance(-1); pure_maintenance(1);
    aim_consumer_contract_support::vertical_closed_loop(-1, expect); aim_consumer_contract_support::vertical_closed_loop(1, expect); aim_consumer_contract_support::vertical_closed_loop(0, expect);
    std::cout << "失败数：" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
