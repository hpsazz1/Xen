#include "aim/aim.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>

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
}
int main() {
    for (const auto interval : {4166667LL, 8000000LL})
        for (const int direction : {-1, 1})
            for (const int width : {24, 48}) observed_feedback(direction, interval, width);
    std::cout << "失败数：" << failures << '\n';
    return failures ? 1 : 0;
}
