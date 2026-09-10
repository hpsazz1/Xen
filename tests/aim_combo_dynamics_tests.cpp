#include "aim/aim.h"
#include "fixtures/aim_bg_control_intervals.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

Clock::time_point at(double seconds) {
    return Clock::time_point(std::chrono::duration_cast<Clock::duration>(
        std::chrono::nanoseconds(10000000000LL + std::llround(seconds * 1e9))));
}

double world_x(double seconds, double velocity = 180.0) {
    return velocity * (std::clamp(seconds - 1.0, 0.0, 1.0) -
                    std::clamp(seconds - 3.0, 0.0, 1.0));
}

enum class Scenario { Standard, BackgroundLossNoise, BackgroundLossDiagonal };

struct Observation {
    double seconds;
    double camera_x;
    double camera_y;
    double error_x;
    double error_y;
};

struct AppliedCommand {
    double seconds;
    int x;
    int y;
};

struct Metrics {
    double following_error_sum = 0.0;
    int following_samples = 0;
    double stop_peak_x = 0.0;
    double stop_peak_y = 0.0;
    double stop_lead_peak_x = 0.0;
    int stop_samples = 0;
    double final_min[2] = {std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::infinity()};
    double final_max[2] = {-std::numeric_limits<double>::infinity(),
                           -std::numeric_limits<double>::infinity()};
    int final_reversals[2] = {};
    int final_previous_nonzero[2] = {};
    int final_samples = 0;
};

AimConfig make_config(bool delay, bool prediction) {
    AimConfig config;
    config.person_class_ids = {0, 2};
    config.head_class_ids = {1, 3};
    config.high_confidence = 0.25f;
    config.low_confidence = 0.10f;
    config.min_confirmed_hits = 2;
    config.max_lost_frames = 8;
    config.min_iou = 0.10f;
    config.max_center_distance = 0.25f;
    config.switch_margin = 0.20f;
    config.switch_confirm_frames = 3;
    config.switch_cooldown_frames = 5;
    config.acquisition_range_percent = 90.0f;
    config.body_aim_height_ratio = 0.35f;
    config.body_aim_range_percent = 50.0f;
    config.deadzone_pixels = 1.5f;
    config.smoothing = 0.475f;
    config.counts_per_pixel_x = 0.425f;
    config.counts_per_pixel_y = 0.40f;
    config.max_counts_per_frame = 14.0f;
    config.enable_delay_compensation = delay;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 44.0f;
    config.max_delay_compensation_percent = 15.0f;
    config.enable_prediction = prediction;
    config.max_prediction_lead_percent = 35.0f;
    config.predicted_gain = 0.5f;
    return config;
}

Metrics run(bool delay, bool prediction, int sign,
            Scenario scenario = Scenario::Standard, double actuation_delay = 0.015,
            double background_loss_at = 2.2) {
    auto config = make_config(delay, prediction);
    config.control_delay_ms = static_cast<float>(actuation_delay * 1000.0);
    Aim aim(config);
    // 独立离散模型：关闭软件补偿不删除真实采样/执行延迟。
    // 本模型用于回归反例，不声明此响应值是所有设备的实际标定。
    const double response = scenario == Scenario::Standard ? 0.5215 : 0.78225;
    const double velocity_x = scenario == Scenario::Standard ? 180.0 : 90.0;
    const auto world_y = [scenario](double seconds) {
        if (scenario == Scenario::Standard) return 6.0;
        return scenario == Scenario::BackgroundLossDiagonal
            ? world_x(seconds, 35.0) : 0.0;
    };
    constexpr double interval = 1.0 / 240.0;
    constexpr double observation_age = 0.009;
    constexpr int source_frames = 1128;
    std::vector<Observation> observations;
    std::vector<AppliedCommand> commands;
    observations.reserve(source_frames);
    commands.reserve(source_frames);
    std::size_t control_index = 0;
    std::size_t application_index = 0;
    int source_index = 0;
    double camera_x = 0.0;
    double camera_y = 0.0;
    Metrics metrics;
    while (source_index < source_frames || control_index < observations.size() ||
           application_index < commands.size()) {
        const auto infinity = std::numeric_limits<double>::infinity();
        const double source_at = source_index < source_frames
            ? source_index * interval : infinity;
        const double control_at = control_index < observations.size()
            ? observations[control_index].seconds + observation_age : infinity;
        const double apply_at = application_index < commands.size()
            ? commands[application_index].seconds : infinity;
        if (apply_at <= source_at && apply_at <= control_at) {
            camera_x -= response * commands[application_index].x;
            camera_y -= response * commands[application_index].y;
            ++application_index;
            continue;
        }
        if (source_at <= control_at) {
            const double noise = scenario == Scenario::BackgroundLossDiagonal
                ? 0.0 : (source_index % 2 == 0 ? -0.2 : 0.2);
            observations.push_back({source_at, camera_x, camera_y,
                sign * world_x(source_at, velocity_x) + camera_x + noise,
                world_y(source_at) + camera_y - noise});
            ++source_index;
            continue;
        }
        const auto& observation = observations[control_index];
        AimFrame frame;
        frame.sequence = static_cast<std::uint64_t>(control_index + 1);
        frame.observation_epoch = 17;
        frame.captured_at = at(observation.seconds);
        frame.control_at = at(control_at);
        frame.roi_width = frame.roi_height = 320;
        frame.control_center_x = frame.control_center_y = 160.0f;
        frame.lock_active = true;
        frame.detections.push_back({
            static_cast<float>(148.0 + observation.error_x),
            static_cast<float>(139.0 + observation.error_y),
            static_cast<float>(172.0 + observation.error_x),
            static_cast<float>(199.0 + observation.error_y), 0.95f, 0});
        if (control_index > 0) {
            const auto& previous = observations[control_index - 1];
            const auto background_status = scenario != Scenario::Standard &&
                    observation.seconds >= background_loss_at
                ? AimBackgroundMotionStatus::MISSING : AimBackgroundMotionStatus::VALID;
            frame.background_motion_x = {
                background_status, frame.sequence - 1,
                frame.sequence, at(previous.seconds), frame.captured_at, 17,
                static_cast<float>(observation.camera_x - previous.camera_x),
                0.95f, 0.0f, 4};
        }
        const AimResult result = aim.process(frame);
        const bool finite_points = std::isfinite(result.target.base_aim_x) &&
            std::isfinite(result.target.base_aim_y) &&
            std::isfinite(result.target.aim_x) && std::isfinite(result.target.aim_y);
        if (result.status != AimStatus::SUCCESS ||
            !finite_points ||
            (control_index > 2 && !result.has_target) ||
            std::hypot(static_cast<double>(result.command.dx_counts),
                       static_cast<double>(result.command.dy_counts)) >
                config.max_counts_per_frame + 1e-5) {
            throw std::runtime_error("闭环模型必须保持目标、成功状态与二维上限");
        }
        if (result.has_command) {
            if (!aim.record_backend_completed_command(frame.sequence, frame.control_at,
                    result.command.dx_counts, result.command.dy_counts)) {
                throw std::runtime_error("模型命令完成记录失败");
            }
            commands.push_back({control_at + actuation_delay,
                result.command.dx_counts, result.command.dy_counts});
        }
        const double live_x = sign * world_x(control_at, velocity_x) + camera_x;
        const double live_y = world_y(control_at) + camera_y;
        if (observation.seconds >= 1.4 && observation.seconds < 1.9) {
            // 预测允许提前；持续落后计入误差，提前不伪装成跟随滞后。
            metrics.following_error_sum += std::max(0.0, sign * live_x);
            ++metrics.following_samples;
        }
        if ((observation.seconds >= 2.3 && observation.seconds < 2.8) ||
            observation.seconds >= 4.3) {
            metrics.stop_peak_x = std::max(metrics.stop_peak_x, std::fabs(live_x));
            metrics.stop_peak_y = std::max(metrics.stop_peak_y, std::fabs(live_y));
            metrics.stop_lead_peak_x = std::max(metrics.stop_lead_peak_x,
                std::fabs(static_cast<double>(result.target.aim_x - result.target.base_aim_x)));
            ++metrics.stop_samples;
        }
        if (observation.seconds >= 4.3) {
            const double errors[2] = {live_x, live_y};
            const int counts[2] = {result.command.dx_counts, result.command.dy_counts};
            for (int axis = 0; axis < 2; ++axis) {
                metrics.final_min[axis] = std::min(metrics.final_min[axis], errors[axis]);
                metrics.final_max[axis] = std::max(metrics.final_max[axis], errors[axis]);
                if (counts[axis] != 0) {
                    if (counts[axis] * metrics.final_previous_nonzero[axis] < 0) {
                        ++metrics.final_reversals[axis];
                    }
                    metrics.final_previous_nonzero[axis] = counts[axis];
                }
            }
            ++metrics.final_samples;
        }
        ++control_index;
    }
    return metrics;
}

// 严格配对且输入有限，仍可能在速度除法或 FOV 换算时溢出。
// 来源派生失效必须回相同delay配置的基础反馈，且坏帧不能污染后续预测状态。
bool prediction_numeric_fallback(int kind, int sign) {
    const auto config = make_config(false, true);
    Aim candidate(config);
    auto basic_config = config;
    basic_config.enable_prediction = false;
    Aim fallback(basic_config);
    auto captured_at = at(0.0);
    int basic_commands = 0;
    int forecast_frames = 0;
    int covered = 0;
    int mismatches = 0;
    bool finite = true;
    for (int index = 1; index <= 60; ++index) {
        const auto previous_at = captured_at;
        captured_at += index == 30 ? std::chrono::nanoseconds(1)
            : std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::milliseconds(4));
        AimFrame frame;
        frame.sequence = static_cast<std::uint64_t>(index);
        frame.observation_epoch = 7;
        frame.captured_at = captured_at;
        frame.control_at = captured_at + std::chrono::milliseconds(9);
        frame.roi_width = frame.roi_height = 320;
        frame.control_center_x = frame.control_center_y = 160.0f;
        frame.lock_active = true;
        frame.source_pixels_per_roi_pixel_x = kind == 1 ? 4.0f : 1.0f;
        const float x = 160.0f + sign * static_cast<float>(20 + index);
        frame.detections.push_back({x - 12.0f, 139.0f, x + 12.0f, 199.0f, 0.95f, 0});
        const auto missing = frame;
        if (index == 30) {
            frame.background_motion_x = {
                AimBackgroundMotionStatus::VALID, frame.sequence - 1, frame.sequence,
                previous_at, captured_at, frame.observation_epoch,
                sign * (kind == 0 ? 1e30f : 1e29f), 0.9f, 0.0f, 4};
        }
        const auto actual = candidate.process(frame);
        const auto reference = fallback.process(missing);
        basic_commands += reference.has_command;
        forecast_frames += actual.target.lead_active || actual.target.lead_x != 0.0f ||
            actual.target.lead_y != 0.0f;
        if (index > 2) {
            covered += actual.has_target && reference.has_target &&
                actual.status == AimStatus::SUCCESS && reference.status == AimStatus::SUCCESS;
        }
        finite = finite && std::isfinite(actual.target.aim_x) &&
            std::isfinite(actual.target.aim_y) && std::isfinite(actual.target.lead_x) &&
            std::isfinite(actual.control.observer_target_velocity_x_counts_per_second) &&
            std::hypot(actual.command.dx_counts, actual.command.dy_counts) <= config.max_counts_per_frame;
        if (actual.has_command != reference.has_command ||
            actual.command.dx_counts != reference.command.dx_counts ||
            actual.command.dy_counts != reference.command.dy_counts ||
            actual.target.aim_x != reference.target.aim_x ||
            actual.target.aim_y != reference.target.aim_y ||
            actual.target.lead_x != reference.target.lead_x) {
            std::cout << "numeric_mismatch kind=" << kind << " sign=" << sign
                << " frame=" << index << " aim=" << actual.target.aim_x
                << "/" << reference.target.aim_x << " lead=" << actual.target.lead_x
                << " q=" << actual.command.dx_counts << "/" << reference.command.dx_counts
                << " observer=" << actual.control.observer_target_velocity_x_counts_per_second
                << '\n';
            ++mismatches;
        }
    }
    const bool passed = finite && basic_commands > 0 && covered == 58 &&
        forecast_frames == 0 && mismatches == 0;
    std::cout << "prediction_numeric kind=" << kind << " sign=" << sign
              << " basic_commands=" << basic_commands << " forecast_frames=" << forecast_frames
              << " covered=" << covered << " mismatches=" << mismatches
              << " pass=" << passed << '\n';
    return passed;
}

// 同一源观测历史下，调度间隔不能改变按源时钟更新的观察器速度。
bool background_feedforward_rate_timing(int scenario, int sign) {
    auto config = make_config(true, true);
    config.max_prediction_lead_percent = 1.0f;
    Aim fixed(config);
    Aim jitter(config);
    auto source_at = at(0.0);
    auto control_at = source_at + std::chrono::milliseconds(9);
    double max_rate_difference = 0.0;
    double max_rate = 0.0;
    int samples = 0;
    bool finite = true;
    const int count = scenario == 0 ? 180 :
        static_cast<int>(sizeof(kAimBgControlActualIntervals) / sizeof(kAimBgControlActualIntervals[0]));
    for (int index = 0; index < count; ++index) {
        const auto previous_at = source_at;
        const auto source_ns = scenario == 0 ? 4000000LL : kAimBgControlActualIntervals[index][0];
        const auto control_ns = scenario == 0 ? (index % 2 ? 6000000LL : 2000000LL)
            : kAimBgControlActualIntervals[index][1];
        // 两种序列均不跨既有8ms直接FF分支，避免混入另一个时序政策。
        if (source_ns <= 0 || source_ns >= 8000000 || control_ns <= 0 || control_ns >= 8000000) {
            throw std::runtime_error("请求率测试时间间隔越过既有分支");
        }
        source_at += std::chrono::nanoseconds(source_ns);
        control_at += std::chrono::nanoseconds(control_ns);
        AimFrame frame;
        frame.sequence = static_cast<std::uint64_t>(index + 1);
        frame.observation_epoch = 7;
        frame.captured_at = source_at;
        frame.control_at = source_at + std::chrono::milliseconds(9);
        frame.roi_width = frame.roi_height = 320;
        frame.control_center_x = frame.control_center_y = 160.0f;
        frame.lock_active = true;
        const float x = 160.0f + sign * (20.0f + 0.1f * index);
        frame.detections.push_back({x - 12.0f, 139.0f, x + 12.0f, 199.0f, 0.95f, 0});
        if (index > 0) {
            frame.background_motion_x = {AimBackgroundMotionStatus::VALID,
                frame.sequence - 1, frame.sequence, previous_at, source_at,
                frame.observation_epoch, 0.0f, 0.9f, 0.0f, 4};
        }
        auto varied = frame;
        varied.control_at = control_at;
        const auto a = fixed.process(frame);
        const auto b = jitter.process(varied);
        if (index > 2) {
            const double rate_a = a.control.observer_target_velocity_x_counts_per_second;
            const double rate_b = b.control.observer_target_velocity_x_counts_per_second;
            finite = finite && a.status == AimStatus::SUCCESS && b.status == AimStatus::SUCCESS &&
                a.has_target && b.has_target &&
                a.control.background_motion_use_x == AimBackgroundMotionUse::CONSUMED &&
                b.control.background_motion_use_x == AimBackgroundMotionUse::CONSUMED &&
                std::isfinite(rate_a) && std::isfinite(rate_b);
            max_rate_difference = std::max(max_rate_difference, std::fabs(rate_a - rate_b));
            max_rate = std::max(max_rate, std::fabs(rate_a));
            ++samples;
        }
    }
    // 数值等价容差，不是控制阈值；非空非零检查防止静默绕过测量路径。
    const bool passed = finite && samples > 0 && max_rate > 0.01 && max_rate_difference < 0.001;
    std::cout << "background_rate scenario=" << scenario << " sign=" << sign
              << " max_rate=" << max_rate << " difference=" << max_rate_difference
              << " pass=" << passed << '\n';
    return passed;
}

int run_long_delay_tests() {
    int failures = 0;
    const auto check_scenario = [&](Scenario scenario, double loss_at) {
        for (const bool delay : {false, true}) {
            for (const bool prediction : {false, true}) {
                for (const int sign : {-1, 1}) {
                    const auto metrics = run(delay, prediction, sign, scenario, 0.040, loss_at);
                    if (!metrics.following_samples || !metrics.stop_samples || !metrics.final_samples) {
                        throw std::runtime_error("40ms观察窗为空");
                    }
                    const double lag = metrics.following_error_sum / metrics.following_samples;
                    const double tolerance = 2.0 * make_config(delay, prediction).deadzone_pixels;
                    const bool passed = lag <= tolerance && metrics.stop_peak_x <= tolerance &&
                        metrics.stop_peak_y <= tolerance && metrics.stop_lead_peak_x <= tolerance;
                    std::cout << "scenario=" << static_cast<int>(scenario) << " loss_at=" << loss_at
                              << " delay=" << delay << " prediction=" << prediction << " sign=" << sign
                              << " lag=" << lag << " stopX=" << metrics.stop_peak_x
                              << " stopY=" << metrics.stop_peak_y
                              << " spanX=" << metrics.final_max[0] - metrics.final_min[0]
                              << " spanY=" << metrics.final_max[1] - metrics.final_min[1]
                              << " pass=" << passed << '\n';
                    if (!passed) ++failures;
                }
            }
        }
    };
    for (const auto scenario : {Scenario::Standard, Scenario::BackgroundLossNoise,
                                Scenario::BackgroundLossDiagonal}) {
        check_scenario(scenario, 2.2);
    }
    // 额外覆盖仍在移动时失源；不只在停止、维护量接近零时检查交接。
    for (const auto scenario : {Scenario::BackgroundLossNoise, Scenario::BackgroundLossDiagonal}) {
        check_scenario(scenario, 1.6);
    }
    return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    int failures = 0;
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--long-delay") {
            return run_long_delay_tests();
        }
        if (argc != 1) throw std::runtime_error("未知的组合测试参数");
        for (int scenario : {0, 1}) {
            for (int sign : {-1, 1}) {
                if (!background_feedforward_rate_timing(scenario, sign)) ++failures;
            }
        }
        for (int kind : {0, 1}) {
            for (int sign : {-1, 1}) {
                if (!prediction_numeric_fallback(kind, sign)) ++failures;
            }
        }
        for (bool delay : {false, true}) {
            for (bool prediction : {false, true}) {
                for (int sign : {-1, 1}) {
                    const auto metrics = run(delay, prediction, sign);
                    if (metrics.following_samples == 0 || metrics.stop_samples == 0) {
                        throw std::runtime_error("闭环模型观察窗为空");
                    }
                    const double lag = metrics.following_error_sum / metrics.following_samples;
                    // 以当前配置的两倍死区作为稳定回位范围，不按候选结果移动界限。
                    const double tolerance = 2.0 * make_config(delay, prediction).deadzone_pixels;
                    const bool passed = lag <= tolerance &&
                        metrics.stop_peak_x <= tolerance && metrics.stop_peak_y <= tolerance &&
                        metrics.stop_lead_peak_x <= tolerance;
                    std::cout << "delay=" << delay << " prediction=" << prediction
                              << " sign=" << sign << " lag=" << lag
                              << " stopX=" << metrics.stop_peak_x
                              << " stopY=" << metrics.stop_peak_y
                              << " stopOffsetX=" << metrics.stop_lead_peak_x
                              << " pass=" << passed << '\n';
                    if (!passed) ++failures;
                }
            }
        }
        // 已有反例：先有同源背景，2.2s 后缺测，3s 反向，4s 停止。
        // 同场景第四配置作为参照；比较波动而非把旧双关失稳当容许基线。
        for (const auto scenario : {Scenario::BackgroundLossNoise,
                                    Scenario::BackgroundLossDiagonal}) {
            for (int sign : {-1, 1}) {
                const auto reference = run(true, false, sign, scenario);
                const auto candidate = run(false, false, sign, scenario);
                if (reference.final_samples == 0 ||
                    candidate.final_samples != reference.final_samples) {
                    throw std::runtime_error("来源缺测后的对照观察窗为空或不一致");
                }
                // 一个完整 ±deadzone 带宽允许亚像素回位波动；不按候选调界限。
                const double deadzone_band = 2.0 * make_config(false, false).deadzone_pixels;
                bool passed = true;
                std::cout << "background_loss=" << static_cast<int>(scenario)
                          << " sign=" << sign;
                for (int axis = 0; axis < 2; ++axis) {
                    const double reference_span = reference.final_max[axis] - reference.final_min[axis];
                    const double candidate_span = candidate.final_max[axis] - candidate.final_min[axis];
                    passed = passed && candidate_span <= reference_span + deadzone_band &&
                        candidate.final_reversals[axis] <= reference.final_reversals[axis];
                    std::cout << (axis == 0 ? " Xspan=" : " Yspan=")
                              << candidate_span << "/" << reference_span
                              << " reversals=" << candidate.final_reversals[axis]
                              << "/" << reference.final_reversals[axis];
                }
                std::cout << " pass=" << passed << '\n';
                if (!passed) ++failures;
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
