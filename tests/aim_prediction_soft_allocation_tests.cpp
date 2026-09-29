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

// 合成软件闭环，不是真实录像回放。配置采用本轮 C，plant 是固定已知夹具。
// 只在第一条非零命令之前检查分摊，此时没有相机响应、执行库存、世界
// 运动、换向或饱和可以合理授权负残差。不能仅凭一般闭环中的负 R 判错。
int run(float initial_error, int sign, bool prediction) {
    const auto config = make_config(prediction);
    Aim aim(config);
    std::vector<Command> commands;
    std::size_t applied = 0;
    float camera_x = 0.0f, camera_y = 0.0f, previous_camera = 0.0f;
    bool any_nonzero_command = false;
    bool all_source_p_nonnegative = true;
    bool all_execution_p_nonnegative = true;
    int eligible = 0, violations = 0;
    constexpr double interval = 1.0 / 240.0;
    constexpr double age = 0.003;
    for (int index = 0; index < 120; ++index) {
        const double seconds = index * interval;
        while (applied < commands.size() && commands[applied].apply_at <= seconds) {
            camera_x -= 0.5215f * commands[applied].x;
            camera_y -= 0.5215f * commands[applied].y;
            ++applied;
        }
        AimFrame frame;
        frame.sequence = static_cast<std::uint64_t>(index + 1);
        frame.observation_epoch = 1;
        frame.captured_at = at(seconds);
        frame.control_at = at(seconds + age);
        frame.roi_width = frame.roi_height = 320;
        frame.control_center_x = frame.control_center_y = 160.0f;
        frame.lock_active = true;
        const float x = 160.0f + sign * initial_error + camera_x;
        const float top = 160.0f - 54.0f * config.body_aim_height_ratio + camera_y;
        frame.detections.push_back({x - 10.0f, top, x + 10.0f, top + 54.0f, 0.95f, 0});
        if (index > 0) {
            frame.background_motion_x = {AimBackgroundMotionStatus::VALID,
                frame.sequence - 1, frame.sequence, at(seconds - interval),
                frame.captured_at, 1, camera_x - previous_camera, 1.0f, 0.0f, 2};
        }
        previous_camera = camera_x;
        const auto result = aim.process(frame);
        if (result.status != AimStatus::SUCCESS ||
            std::hypot(static_cast<double>(result.command.dx_counts),
                       static_cast<double>(result.command.dy_counts)) > 14.00001)
            throw std::runtime_error("合成闭环状态或输出上限错误");
        const auto& c = result.control;
        if (result.has_target && c.evaluated) {
            all_source_p_nonnegative &= sign * c.proportional_x_counts >= -1e-5f;
            all_execution_p_nonnegative &= sign * c.execution_proportional_x_counts >= -1e-5f;
            const bool clean_prefix = !any_nonzero_command &&
                all_source_p_nonnegative && all_execution_p_nonnegative &&
                c.residual_role_x && c.residual_background_role_x &&
                std::fabs(c.execution_unseen_command_x_counts) < 1e-6 &&
                std::fabs(c.execution_world_preview_x_counts) < 1e-6 &&
                std::fabs(c.modelled_response_x_counts) < 1e-6 &&
                std::fabs(c.error_derivative_x_source_pixels_per_second) < 1e-4 &&
                sign * c.execution_proportional_x_counts > 1e-4 &&
                std::fabs(c.execution_proportional_x_counts) < 1.0f;
            if (clean_prefix) {
                ++eligible;
                if (sign * c.feedforward_x_counts < -1e-5f) {
                    ++violations;
                    if (violations == 1)
                        std::cout << "红证据 sign=" << sign << " error=" << initial_error
                            << " prediction=" << prediction << " seq=" << frame.sequence
                            << " P=" << c.execution_proportional_x_counts
                            << " R=" << c.feedforward_x_counts
                            << " shaped=" << c.shaped_x_counts << '\n';
                }
            }
        }
        if (result.has_command) {
            if (!aim.record_backend_completed_command(frame.sequence, frame.control_at,
                    result.command.dx_counts, result.command.dy_counts))
                throw std::runtime_error("完成回执登记失败");
            commands.push_back({seconds + age + 0.015, result.command.dx_counts, result.command.dy_counts});
            any_nonzero_command |= result.command.dx_counts != 0 || result.command.dy_counts != 0;
        }
    }
    if (eligible == 0) throw std::runtime_error("分摊检查未覆盖无库存静态前缀");
    return violations;
}

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
MovingMetrics moving(bool prediction, int sign) {
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
        const float world = sign * static_cast<float>(180.0 * std::clamp(seconds - 1.0, 0.0, 4.0));
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
        previous_camera = camera_x;
        const auto r = aim.process(frame);
        if (r.status != AimStatus::SUCCESS || (index > 2 && !r.has_target) ||
            !std::isfinite(r.target.aim_x) || !std::isfinite(r.target.base_aim_x) ||
            std::hypot(static_cast<double>(r.command.dx_counts),
                       static_cast<double>(r.command.dy_counts)) > config.max_counts_per_frame + 1e-5)
            throw std::runtime_error("移动合成闭环须保持目标、有限几何和二维上限");
        if (r.has_target) {
            ++m.samples;
            const float lead = r.target.aim_x - r.target.base_aim_x;
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
    return m;
}
} // namespace

int main() {
    try {
        int violations = 0;
        for (const bool prediction : {false, true})
            for (const int sign : {-1, 1})
                for (const float error : {2.0f, 3.0f})
                    violations += run(error, sign, prediction);
        for (const int sign : {-1, 1}) {
            const auto baseline = moving(false, sign);
            const auto predicted = moving(true, sign);
            std::cout << "同世界输入预测对照 sign=" << sign << " base_p50_change="
                << percentile(predicted.base,0.5f)-percentile(baseline.base,0.5f) << '\n';
        }
        if (violations != 0) {
            std::cerr << "无库存、无运动、无饱和且 P 始终同向时，软区不能把 P 拒收归入反向 R；违规="
                      << violations << '\n';
            return 1;
        }
        std::cout << "静态软区 P/R 分摊通过\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
