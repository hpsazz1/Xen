#include "aim/aim.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
Clock::time_point at(double seconds) {
    return Clock::time_point(std::chrono::nanoseconds(
        10000000000LL + std::llround(seconds * 1e9)));
}

struct Command { double apply_at; int x; int y; };

AimConfig make_config(bool prediction) {
    AimConfig config;
    config.person_class_ids = {0, 2};
    config.head_class_ids = {1, 3};
    config.high_confidence = 0.25f;
    config.low_confidence = 0.1f;
    config.min_confirmed_hits = 2;
    config.max_lost_frames = 8;
    config.min_iou = 0.1f;
    config.max_center_distance = 0.25f;
    config.acquisition_range_percent = 90.0f;
    config.body_aim_height_ratio = 0.16f;
    config.body_aim_range_percent = 50.0f;
    config.deadzone_pixels = 1.5f;
    config.soft_zone_radius_percent = 30.0f;
    config.soft_zone_min_strength = 0.2f;
    config.smoothing = 0.475f;
    config.counts_per_pixel_x = 0.425f;
    config.counts_per_pixel_y = 0.4f;
    config.max_counts_per_frame = 14.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 44.0f;
    config.max_delay_compensation_percent = 15.0f;
    config.enable_prediction = prediction;
    config.max_prediction_lead_percent = 35.0f;
    config.predicted_gain = 0.5f;
    return config;
}

// 由 2026-09-29 单帧 INCONSISTENT 后持续退零的实际报告指导。
// 这是同世界轨迹的合成软件闭环，不是原始检测候选完整重放。
enum class Scenario { Normal, SingleGap, LongGap, StopAtGap, ReverseAtGap, EpochAtResume, PredictionOff };
int gap_failures = 0;
struct MovingMetrics {
    std::vector<float> base, final, lead;
    float stop_peak = 0.0f;
    float tail_peak = 0.0f;
    int tail_commands = 0, tail_leads = 0, samples = 0;
};
float percentile(std::vector<float> values, float fraction) {
    if (values.empty()) throw std::runtime_error("闭环观察窗为空");
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1,
        static_cast<std::size_t>(values.size() * fraction))];
}
MovingMetrics gap_run(Scenario scenario, int sign) {
    const bool dropout = scenario != Scenario::Normal && scenario != Scenario::PredictionOff;
    const bool prediction = scenario != Scenario::PredictionOff;
    int regenerated_index = 0; float invalid_peak_after_slew=0.0f, stale_direction_peak=0.0f;
    float previous_offset = 0.0f;
    std::uint64_t previous_track = 0, previous_epoch = 0;
    float minimum_restored_lead = 1000.0f, before_lead=0.0f;
    int minimum_index=0, consumed_window=0;
    const auto config = make_config(prediction);
    Aim aim(config);
    std::vector<Command> commands;
    std::size_t applied = 0;
    float camera_x = 0.0f, camera_y = 0.0f, previous_camera = 0.0f;
    MovingMetrics m;
    // 两次运行共享完全相同的世界运动、plant 和采样/完成时序；输出反馈
    // 必然令相机观察不同，因此不是把实际 C 当作开关关闭后的真实录像。
    constexpr double interval = 1.0 / 240.0, age = 0.003;
    for (int index = 0; index < 1920; ++index) {
        const double seconds = index * interval;
        while (applied < commands.size() && commands[applied].apply_at <= seconds) {
            camera_x -= 0.5215f * commands[applied].x;
            camera_y -= 0.5215f * commands[applied].y;
            ++applied;
        }
        const double world_seconds = (scenario == Scenario::StopAtGap || scenario == Scenario::ReverseAtGap) ?
            std::clamp(seconds - 1.0, 0.0, 2.0) - (scenario == Scenario::ReverseAtGap ? std::clamp(seconds - 3.0, 0.0, 2.0) : 0.0) : std::clamp(seconds - 1.0, 0.0, 4.0);
        const float world = sign * static_cast<float>(180.0 * world_seconds);
        AimFrame frame;
        frame.sequence = static_cast<std::uint64_t>(index + 1);
        frame.observation_epoch = 1;
        frame.captured_at = at(seconds);
        frame.control_at = at(seconds + age);
        frame.roi_width = frame.roi_height = 320;
        frame.control_center_x = frame.control_center_y = 160.0f;
        frame.lock_active = true;
        const float x = 160.0f + world + camera_x;
        const float top = 160.0f - 54.0f * config.body_aim_height_ratio + camera_y;
        frame.detections.push_back({x - 10.0f, top, x + 10.0f, top + 54.0f, 0.95f, 0});
        if (index > 0)
            frame.background_motion_x = {AimBackgroundMotionStatus::VALID,
                frame.sequence - 1, frame.sequence, at(seconds - interval),
                frame.captured_at, 1, camera_x - previous_camera, 1.0f, 0.0f, 2};
        if (dropout && (index == 720 || (scenario == Scenario::LongGap && index >= 720 && index <= 840)))
            frame.background_motion_x.status = AimBackgroundMotionStatus::INCONSISTENT;
        if (scenario == Scenario::EpochAtResume && index >= 721) {frame.observation_epoch = 2;frame.background_motion_x.observation_epoch=2;}
        previous_camera = camera_x;
        const auto r = aim.process(frame);
        if (r.status != AimStatus::SUCCESS || (index > 2 && !r.has_target) ||
            !std::isfinite(r.target.aim_x) || !std::isfinite(r.target.base_aim_x) ||
            std::hypot(static_cast<double>(r.command.dx_counts),
                       static_cast<double>(r.command.dy_counts)) > config.max_counts_per_frame + 1e-5)
            throw std::runtime_error("移动合成闭环须保持目标、有限几何和二维上限");
        if (index == 719) before_lead = sign * r.target.lead_x;
        if (index > 720 && index < 780) {
            if (r.control.background_motion_use_x == AimBackgroundMotionUse::CONSUMED) ++consumed_window;
            if (sign * r.target.lead_x < minimum_restored_lead) {minimum_restored_lead=sign*r.target.lead_x;minimum_index=index;}
        }
        if (index > 732 && regenerated_index == 0 && sign*r.target.lead_x > 0.01f) regenerated_index=index;
        if (scenario == Scenario::LongGap && index >= 780 && index <= 840) invalid_peak_after_slew=std::max(invalid_peak_after_slew,std::fabs(r.target.lead_x));
        if ((scenario == Scenario::StopAtGap || scenario == Scenario::ReverseAtGap) && index >= 780 && index <= 1000) stale_direction_peak=std::max(stale_direction_peak,sign*r.target.lead_x);
        if (scenario == Scenario::EpochAtResume && index == 721 && std::fabs(r.target.lead_x) > 0.001f) throw std::runtime_error("新epoch不得沿用旧预测");
        if (r.has_target) {
            ++m.samples;
            const float lead = r.target.aim_x - r.target.base_aim_x;
            if (previous_track == r.target.track_id && previous_epoch == frame.observation_epoch) {
                const float budget = std::hypot(r.target.x2-r.target.x1, r.target.y2-r.target.y1) *
                    1.5f * static_cast<float>(interval);
                if (std::fabs(lead - previous_offset) > budget + 0.001f)
                    throw std::runtime_error("同身份预测仍须遵守既有每秒对角线slew额度");
            }
            previous_offset = lead;
            previous_track = r.target.track_id;
            previous_epoch = frame.observation_epoch;
            const float lead_limit = std::hypot(r.target.x2-r.target.x1,r.target.y2-r.target.y1) *
                config.max_prediction_lead_percent / 100.0f;
            if (std::fabs(lead) > lead_limit + 0.001f ||
                (!prediction && std::fabs(lead) > 0.001f))
                throw std::runtime_error("预测须遵守原几何额度与关闭零偏移合同");
            if (seconds >= 3.0 && seconds < 5.0) {
                m.base.push_back(sign * (r.target.base_aim_x - 160.0f));
                m.final.push_back(sign * (r.target.aim_x - 160.0f));
                m.lead.push_back(sign * lead);
            }
            if (seconds >= 5.0) m.stop_peak = std::max(m.stop_peak,std::fabs(r.target.base_aim_x-160.0f));
            if (seconds >= 7.0) {
                m.tail_peak = std::max(m.tail_peak,std::fabs(r.target.base_aim_x-160.0f));
                m.tail_commands += r.command.dx_counts != 0;
                m.tail_leads += std::fabs(lead) > 0.25f; // 复用已有停止尾部合同。
            }
        }
        if (r.has_command) {
            if (!aim.record_backend_completed_command(frame.sequence, frame.control_at,
                    r.command.dx_counts, r.command.dy_counts))
                throw std::runtime_error("移动闭环完成登记失败");
            commands.push_back({seconds + age + 0.015, r.command.dx_counts, r.command.dy_counts});
        }
    }
    std::cout << "匀速闭环 prediction=" << prediction << " sign=" << sign
        << " base_p50=" << percentile(m.base,0.5f) << " base_p95=" << percentile(m.base,0.95f)
        << " final_p50=" << percentile(m.final,0.5f) << " lead_p50=" << percentile(m.lead,0.5f)
        << " stop_peak=" << m.stop_peak << " tail_peak=" << m.tail_peak
        << " tail_commands=" << m.tail_commands << " tail_leads=" << m.tail_leads << '\n';
    // 只用原死区加一个整数执行步的量化余量，不为预测改善编造通过门槛。
    if (m.tail_peak > config.deadzone_pixels + 0.5215f || m.tail_commands != 0 || m.tail_leads != 0)
        throw std::runtime_error("停止尾部须进入既有死区量化余量，且命令与预测持续归零");
    std::cout << "gap_summary dropout=" << static_cast<int>(scenario) << " sign=" << sign << " before=" << before_lead << " minimum_restored=" << minimum_restored_lead << " at_index=" << minimum_index << " consumed=" << consumed_window << " regenerated_index=" << regenerated_index << " invalid_tail=" << invalid_peak_after_slew << " stale_direction_peak=" << stale_direction_peak << "\n";
    if (invalid_peak_after_slew > 0.001f || stale_direction_peak > 0.001f) throw std::runtime_error("缺源/停止/反向不能保有旧方向预测");
    if (scenario == Scenario::SingleGap) {
        if (before_lead <= 0.001f || consumed_window != 59)
            throw std::runtime_error("单帧缺口夹具必须已有提前且恢复后独立背景连续有效");
        // 不要求坏帧保持幅度或恢复首帧立即满幅；只排除当前同向来源
        // 已连续恢复后仍被旧生命周期清理强迫完全退零。
        if (minimum_restored_lead <= 0.001f) ++gap_failures;
    }
    return m;
}
}
int main() {
    try {
        for (const int sign : {-1, 1}) {
            for (const auto scenario : {Scenario::Normal, Scenario::SingleGap,
                    Scenario::LongGap, Scenario::StopAtGap, Scenario::ReverseAtGap,
                    Scenario::EpochAtResume, Scenario::PredictionOff})
                gap_run(scenario, sign);
        }
        if (gap_failures != 0) {
            std::cerr << "单帧缺口恢复连续同向独立背景后仍退零，反例=" << gap_failures << '\n';
            return 1;
        }
        std::cout << "预测短缺口连续性及失效保护通过\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
