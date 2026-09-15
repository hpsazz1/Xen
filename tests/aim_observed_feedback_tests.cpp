#include "aim/aim.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <deque>
#include <algorithm>
#include <string_view>

namespace {
int failures = 0;
void expect(bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << "失败：" << message << '\n'; }
}
void observed_feedback(int direction, std::int64_t interval_ns, int width) {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.person_class_ids = {0};
    config.head_class_ids = {1};
    config.high_confidence = .25f;
    config.low_confidence = .1f;
    config.min_iou = .1f;
    config.max_center_distance = .25f;
    config.acquisition_range_percent = 90;
    config.body_aim_height_ratio = 1.0f / 3.0f;
    config.deadzone_pixels = 1.5f;
    config.smoothing = .475f;
    config.counts_per_pixel_x = .425f;
    config.counts_per_pixel_y = .4f;
    config.max_counts_per_frame = 14;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15;
    config.max_delay_compensation_ms = 44;
    config.max_delay_compensation_percent = 15;
    config.enable_prediction = false;
    Aim aim(config);
    const double dt = interval_ns * 1e-9;
    const double plant = .2216375 / .425;
    double error = direction * 8.0;
    double tail_absolute_error = 0.0;
    int previous_command = 0, tail_frames = 0;
    const int frames = static_cast<int>(std::ceil(2.0 / dt));
    // 已知离散相机模型仅用于软件反馈职责，不拟合或宣称真实KMBOX plant。
    // 精确背景来自本分支上一条命令；运动前馈已匹配，初始位置残差应收敛。
    for (int i = 0; i < frames; ++i) {
        const double background = i ? -plant * previous_command : 0.0;
        if (i) error += direction * 120.0 * dt + background;
        AimFrame frame;
        frame.sequence = 100 + i;
        frame.observation_epoch = 17;
        frame.captured_at = std::chrono::steady_clock::time_point{
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::nanoseconds(10000000000LL + interval_ns * i))};
        frame.control_at = frame.captured_at;
        frame.roi_width = frame.roi_height = 320;
        frame.control_center_x = frame.control_center_y = 160;
        frame.lock_active = true;
        const float height = width * 2.5f;
        frame.detections.push_back({static_cast<float>(160 + error - width / 2.0),
            160 - height / 3, static_cast<float>(160 + error + width / 2.0),
            160 + 2 * height / 3, .95f, 0});
        frame.background_motion_x = {AimBackgroundMotionStatus::VALID,
            frame.sequence - 1, frame.sequence,
            frame.captured_at - std::chrono::nanoseconds(interval_ns),
            frame.captured_at, 17, static_cast<float>(background), .9f, 0.0f, 2};
        const auto result = aim.process(frame);
        previous_command = result.has_command ? result.command.dx_counts : 0;
        expect(result.status == AimStatus::SUCCESS && result.command.dy_counts == 0 &&
                   std::abs(previous_command) <= 14 && std::isfinite(result.control.shaped_x_counts),
               "匹配观测闭环须保持有限值、零Y和14上限");
        if (result.has_command)
            expect(aim.record_backend_completed_command(frame.sequence, frame.control_at,
                       previous_command, result.command.dy_counts),
                   "各闭环只能反馈自身生成的命令");
        if (i * dt >= 1.5) { tail_absolute_error += std::fabs(error); ++tail_frames; }
    }
    const double tail_mean = tail_absolute_error / tail_frames;
    std::cout << "direction=" << direction << " interval_ns=" << interval_ns
              << " width=" << width << " tail_mean=" << tail_mean << '\n';
    expect(tail_frames > 0 && tail_mean <= config.deadzone_pixels,
           "已测运动匹配时初始位置残差应回到既有死区，而非持续停在死区外");
}
void current_position_responsibility() {
    for (int direction : {-1, 1}) for (auto dt : {4166667LL, 8000000LL}) {
        AimConfig config;
        config.min_confirmed_hits = 1;
        config.counts_per_pixel_x = .425f;
        config.smoothing = .475f;
        config.deadzone_pixels = 1.5f;
        config.body_aim_height_ratio = 1.0f / 3.0f;
        config.enable_delay_compensation = true;
        config.control_delay_ms = 15;
        config.max_delay_compensation_ms = 44;
        config.enable_prediction = false;
        config.max_counts_per_frame = 14;
        Aim aim(config);
        for (int i = 0; i < 42; ++i) {
            const float error = direction * (i < 40 ? 24.0f : 12.0f);
            AimFrame f;
            f.sequence = 100 + i;
            f.observation_epoch = 17;
            f.captured_at = std::chrono::steady_clock::time_point{
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::nanoseconds(10000000000LL + dt * i))};
            f.control_at = f.captured_at + std::chrono::milliseconds(4);
            f.roi_width = f.roi_height = 320;
            f.control_center_x = f.control_center_y = 160;
            f.lock_active = true;
            f.detections.push_back({140 + error, 140, 180 + error, 200, .95f, 0});
            f.background_motion_x = {AimBackgroundMotionStatus::VALID, f.sequence - 1, f.sequence,
                f.captured_at - std::chrono::nanoseconds(dt), f.captured_at, 17,
                i == 40 ? -direction * 12.0f : 0.0f, .9f, 0.0f, 2};
            const auto r = aim.process(f);
            const auto& c = r.control;
            if (i == 40) {
                const float position_tail = direction * (c.filtered_x_counts - c.filtered_integral_x_counts);
                const float current_position = direction * c.proportional_x_counts;
                std::cout << "direction=" << direction << " dt=" << dt
                          << " position_part=" << position_tail << " current_P=" << current_position
                          << " I=" << c.filtered_integral_x_counts << '\n';
                expect(c.background_motion_use_x == AimBackgroundMotionUse::CONSUMED && !c.filter_reset_x &&
                      std::fabs(c.observer_phase_command_x_counts) < .0001f && current_position > 0 &&
                      std::fabs(c.filtered_integral_x_counts) > .0001f,
                       "收拢回归须有可靠背景、非Reset、零相位及非零积分");
                // 没有开口相位的收拢帧：保留积分记忆，但旧位置份额不能继续高于当前P。
                expect(position_tail <= current_position + .001f,
                       "当前误差缩小后旧比例份额不能继续高于当前P");
            }
            expect(r.status == AimStatus::SUCCESS && r.command.dy_counts == 0 &&
                       std::abs(r.command.dx_counts) <= 14,
                   "当前比例纠偏须保持成功、零Y和14上限");
            if (r.has_command)
                expect(aim.record_backend_completed_command(f.sequence, f.control_at,
                           r.command.dx_counts, r.command.dy_counts),
                       "当前比例纠偏只记录本分支已完成命令");
        }
    }
}

struct StaticApproachSummary {
    double peak_opposite_error = 0.0;
    double tail_mean_error = 0.0;
    double last_outside_deadzone_ms = 0.0;
    int reverse_commands = 0;
    int restored_frame = -1;
};

StaticApproachSummary static_approach_feedback(int direction, bool restore_background,
                                             std::int64_t interval_ns) {
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.body_aim_height_ratio = 1.0f / 3.0f;
    config.deadzone_pixels = 1.5f;
    config.smoothing = .475f;
    config.counts_per_pixel_x = .425f;
    config.counts_per_pixel_y = .4f;
    config.max_counts_per_frame = 14;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15;
    config.max_delay_compensation_ms = 44;
    config.max_delay_compensation_percent = 15;
    config.enable_prediction = false;
    Aim aim(config);
    const auto interval = std::chrono::nanoseconds(interval_ns);
    const int frames = static_cast<int>(std::ceil(2.0e9 / interval_ns));
    const int tail_window_frames = static_cast<int>(std::ceil(0.5e9 / interval_ns));
    // 沿用本文件的已知离散相机比例，仅验证软件状态；不是从人工Run拟合plant。
    // 静态世界目标不移动，只有本分支ACK命令在15ms观察延迟后改变图像。
    constexpr double pixels_per_count = .2216375 / .425;
    struct Effect { Clock::time_point visible_at; int dx; };
    std::deque<Effect> effects;
    double error = direction * 60.0;
    bool background_available = !restore_background;
    StaticApproachSummary summary;
    int tail_frames = 0;
    float previous_integral = 0.0f;
    for (int i = -8; i < frames; ++i) {
        AimFrame frame;
        frame.sequence = static_cast<std::uint64_t>(100 + i);
        frame.observation_epoch = 31;
        frame.captured_at = Clock::time_point{10s} + interval * (i + 8);
        frame.control_at = frame.captured_at + 2ms;
        frame.roi_width = frame.roi_height = 320;
        frame.control_center_x = frame.control_center_y = 160;
        frame.lock_active = i >= 0;
        double background = 0.0;
        while (!effects.empty() && effects.front().visible_at <= frame.captured_at) {
            background -= pixels_per_count * effects.front().dx;
            effects.pop_front();
        }
        error += background;
        // 仅此测试的场景事件：首次进入半框距离时背景纹理恢复，之后不再切换。
        if (restore_background && !background_available && i >= 0 && direction * error <= 24.0) {
            background_available = true;
            summary.restored_frame = i;
        }
        frame.detections.push_back({static_cast<float>(160 + error - 24), 120,
            static_cast<float>(160 + error + 24), 240, .95f, 0});
        frame.background_motion_x = {
            background_available ? AimBackgroundMotionStatus::VALID : AimBackgroundMotionStatus::LOW_TEXTURE,
            frame.sequence - 1, frame.sequence, frame.captured_at - interval, frame.captured_at,
            frame.observation_epoch, static_cast<float>(background), .9f, 0.0f,
            background_available ? 2 : 0};
        const auto result = aim.process(frame);
        const float integral_before_frame = previous_integral;
        previous_integral = result.control.filtered_integral_x_counts;
        const int dx = result.has_command && frame.lock_active ? result.command.dx_counts : 0;
        expect(result.status == AimStatus::SUCCESS && result.command.dy_counts == 0 &&
                   std::abs(dx) <= 14 && std::isfinite(result.control.shaped_x_counts),
               "静态接近诊断须成功、保持零Y和14上限");
        if (result.has_command) {
            const auto ack_at = frame.control_at + 400us;
            expect(aim.record_backend_completed_command(frame.sequence, ack_at, dx, 0),
                   "静态接近仅确认自身生成的命令或未许可零输出");
            if (dx != 0) effects.push_back({ack_at + 15ms, dx});
        }
        if (i < 0) continue;
        summary.peak_opposite_error = std::max(summary.peak_opposite_error, -direction * error);
        if (std::fabs(error) > config.deadzone_pixels)
            summary.last_outside_deadzone_ms = std::chrono::duration<double, std::milli>(interval * i).count();
        if (direction * error < 0 && direction * dx < 0) ++summary.reverse_commands;
        if (i >= frames - tail_window_frames) { summary.tail_mean_error += std::fabs(error); ++tail_frames; }
        if (restore_background && i == summary.restored_frame) {
            const auto& c = result.control;
            std::cout << "static_restore direction=" << direction << " interval_ns=" << interval_ns << " frame=" << i
                      << " error=" << error << " dx=" << dx
                      << " P=" << c.execution_proportional_x_counts
                      << " previous_I=" << integral_before_frame
                      << " I=" << c.filtered_integral_x_counts
                      << " F=" << c.pre_eligibility_filtered_x_counts
                      << " background=" << AimBackgroundMotionUseName(c.background_motion_use_x) << '\n';
        }
    }
    summary.tail_mean_error /= tail_frames;
    expect(!restore_background || summary.restored_frame >= 0,
           "静态接近恢复场景必须实际抵达背景恢复位置");
    return summary;
}

void static_approach_background_diagnostics() {
    for (const auto interval_ns : {4166667LL, 8000000LL}) for (int direction : {-1, 1}) {
        const auto stable = static_approach_feedback(direction, false, interval_ns);
        const auto restored = static_approach_feedback(direction, true, interval_ns);
        {
            // 两周期均来自9fa847a同输入实测，候选不能移动自身的验收线。
            const double original_peak_with_deadzone =
                (interval_ns == 4166667LL ? 3.1015 : 6.2305) + 1.5;
            expect(stable.peak_opposite_error <= original_peak_with_deadzone,
                   "持续有效背景的首次静态接近不能退化");
            expect(restored.peak_opposite_error <= original_peak_with_deadzone,
                   "背景恢复不能把旧位置纠偏延续为额外静态过冲");
        }
        // 两个周期均须检验尾部，不能只压低首次峰值。
        expect(stable.tail_mean_error <= 1.5 && restored.tail_mean_error <= 1.5,
               "静态接近的持续背景和恢复背景尾均误差须回到死区");
        std::cout << "static_compare direction=" << direction << " interval_ns=" << interval_ns
                  << " stable_peak=" << stable.peak_opposite_error
                  << " restored_peak=" << restored.peak_opposite_error
                  << " stable_tail=" << stable.tail_mean_error
                  << " restored_tail=" << restored.tail_mean_error
                  << " stable_last_outside_ms=" << stable.last_outside_deadzone_ms
                  << " restored_last_outside_ms=" << restored.last_outside_deadzone_ms
                  << " stable_pullback_commands=" << stable.reverse_commands
                  << " restored_pullback_commands=" << restored.reverse_commands << '\n';
    }
}

void cross_axis_trace() {
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.body_aim_height_ratio = 1.0f / 3.0f;
    config.deadzone_pixels = 1.5f;
    config.smoothing = .475f;
    config.counts_per_pixel_x = .425f;
    config.counts_per_pixel_y = .4f;
    config.max_counts_per_frame = 14;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15;
    config.max_delay_compensation_ms = 44;
    config.max_delay_compensation_percent = 15;
    config.enable_prediction = false;
    Aim aim(config);
    int nonzero_applied_y = 0;
    // 固定双轴观测及背景序列只用于同源码、不同Aim库的Y负控；不是物理模型。
    for (int i = 0; i < 200; ++i) {
        const float x = static_cast<float>(55.0 * std::cos(i * .055));
        const float y = static_cast<float>(12.0 + 21.0 * std::sin(i * .07));
        const bool background_valid = !(i >= 40 && i < 57) && !(i >= 110 && i < 127);
        AimFrame frame;
        frame.sequence = static_cast<std::uint64_t>(i + 1);
        frame.observation_epoch = 43;
        frame.captured_at = Clock::time_point{20s} + std::chrono::nanoseconds(4166667LL * i);
        frame.control_at = frame.captured_at + 2ms;
        frame.roi_width = frame.roi_height = 320;
        frame.control_center_x = frame.control_center_y = 160;
        frame.lock_active = i >= 8 && !(i >= 80 && i < 89) && i < 185;
        frame.detections.push_back({136 + x, 120 + y, 184 + x, 240 + y, .95f, 0});
        frame.background_motion_x = {
            background_valid ? AimBackgroundMotionStatus::VALID : AimBackgroundMotionStatus::LOW_TEXTURE,
            frame.sequence - 1, frame.sequence, frame.captured_at - std::chrono::nanoseconds(4166667),
            frame.captured_at, frame.observation_epoch, static_cast<float>(.4 * std::sin(i * .09)),
            .9f, 0.0f, background_valid ? 2 : 0};
        const auto result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS &&
                   std::hypot(static_cast<float>(result.command.dx_counts),
                              static_cast<float>(result.command.dy_counts)) <= 14.0001f,
               "双轴对照必须成功且不越过既有向量上限");
        const int dx = result.has_command && frame.lock_active ? result.command.dx_counts : 0;
        const int dy = result.has_command && frame.lock_active ? result.command.dy_counts : 0;
        if (result.has_command)
            expect(aim.record_backend_completed_command(frame.sequence, frame.control_at + 400us, dx, dy),
                   "双轴对照仅确认当前分支自身输出或未许可零命令");
        if (dy != 0) ++nonzero_applied_y;
        std::cout << "cross_axis seq=" << frame.sequence << " y=" << result.command.dy_counts
                  << " applied_y=" << dy << '\n';
    }
    expect(nonzero_applied_y > 0, "双轴对照必须实际覆盖非零Y，不能空覆盖");
}
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--cross-axis-trace") {
        cross_axis_trace();
        return failures ? 1 : 0;
    }
    if (argc != 1) {
        std::cerr << "未知的Aim观测反馈测试参数\n";
        return 1;
    }
    for (const auto interval : {4166667LL, 8000000LL})
        for (const int direction : {-1, 1})
            for (const int width : {24, 48}) observed_feedback(direction, interval, width);
    current_position_responsibility();
    static_approach_background_diagnostics();
    std::cout << "失败数：" << failures << '\n';
    return failures ? 1 : 0;
}
