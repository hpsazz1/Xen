#include "aim/aim.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <vector>

// 真实世界位置与相机作用由夹具独立持有；完成回执不代替物理生效。
int run_aim_width_observation_tests() {
    struct Pending { long long at; int q; };
    int failures = 0;
    int cases = 0;
    for (long long dt : {4166667LL, 8000000LL, 14620000LL}) {
        for (double plant : {0.5215, 0.4325}) {
            for (bool changing_width : {false, true}) {
                for (int direction : {-1, 1}) {
                    ++cases;
                    AimConfig config;
                    config.min_confirmed_hits = 1;
                    config.counts_per_pixel_x = 0.2216375f;
                    config.deadzone_pixels = 1.5f;
                    config.body_aim_height_ratio = 1.0f / 3.0f;
                    config.enable_delay_compensation = true;
                    config.control_delay_ms = 15;
                    config.max_delay_compensation_ms = 44;
                    config.enable_prediction = false;
                    config.max_counts_per_frame = 14;
                    config.acquisition_range_percent = 100;
                    Aim aim(config);
                    const double seconds = dt * 1e-9;
                    const auto base = std::chrono::steady_clock::time_point(std::chrono::seconds(100));
                    std::vector<Pending> pending;
                    std::size_t applied = 0;
                    double camera = 0, previous_camera = 0;
                    double tail_world_error = 0, tail_public_error = 0, tail_observer = 0;
                    double tail_maintenance = 0, tail_anchor = 0;
                    int tail_commands = 0, moving_commands = 0, samples = 0;
                    bool valid = true;
                    std::uint64_t identity = 0;
                    for (int i = 0; i * seconds <= 4; ++i) {
                        const auto elapsed = i * dt;
                        const double time = i * seconds;
                        while (applied < pending.size() && pending[applied].at <= elapsed)
                            camera += plant * pending[applied++].q;
                        const double world = direction * (0.25 + 4.0 / seconds * (std::min)(time, 2.0));
                        const double error = world - camera;
                        const float shape = changing_width && i % 2 ? 1.0f : 0.0f;
                        const float x = 160.0f + static_cast<float>(error);
                        AimFrame frame;
                        frame.sequence = i + 1;
                        frame.observation_epoch = 17;
                        frame.captured_at = base + std::chrono::nanoseconds(elapsed);
                        frame.control_at = frame.captured_at + std::chrono::milliseconds(12);
                        frame.roi_width = frame.roi_height = 320;
                        frame.control_center_x = frame.control_center_y = 160;
                        frame.lock_active = true;
                        frame.detections = {{x - 20 - shape, 140, x + 20 + shape, 200, 0.95f, 0}};
                        if (i) frame.background_motion_x = {
                            AimBackgroundMotionStatus::VALID, frame.sequence - 1, frame.sequence,
                            frame.captured_at - std::chrono::nanoseconds(dt), frame.captured_at,
                            17, static_cast<float>(previous_camera - camera), 0.9f, 0, 2};
                        previous_camera = camera;
                        const auto result = aim.process(frame);
                        valid = valid && result.status == AimStatus::SUCCESS && result.has_target;
                        if (!result.has_target) continue;
                        if (!identity) identity = result.target.track_id;
                        valid = valid && result.target.track_id == identity;
                        const int q = result.has_command ? result.command.dx_counts : 0;
                        valid = valid && std::abs(q) <= config.max_counts_per_frame &&
                            result.command.dy_counts == 0 &&
                            std::isfinite(result.control.observer_target_velocity_x_counts_per_second) &&
                            std::isfinite(result.control.modelled_response_x_counts);
                        if (result.has_command) {
                            valid = aim.record_backend_completed_command(
                                frame.sequence, frame.control_at, q, result.command.dy_counts) && valid;
                            pending.push_back({elapsed + 27000000, q});
                        }
                        if (time >= 1 && time < 2) moving_commands += std::abs(q);
                        if (time >= 3) {
                            ++samples;
                            tail_world_error = (std::max)(tail_world_error, std::fabs(error));
                            tail_public_error = (std::max)(tail_public_error,
                                static_cast<double>(std::fabs(result.target.aim_x - 160)));
                            tail_anchor = (std::max)(tail_anchor,
                                static_cast<double>(std::fabs(result.target.base_aim_x - x)));
                            tail_observer = (std::max)(tail_observer,
                                std::fabs(result.control.observer_target_velocity_x_counts_per_second * 0.5215 * seconds));
                            tail_maintenance = (std::max)(tail_maintenance,
                                static_cast<double>(std::fabs(result.control.modelled_response_x_counts)));
                            tail_commands += std::abs(q);
                        }
                    }
                    // 一秒停止窗仍持续宽度往返：不能以停发掩盖虚假M与反向PI的永久抵消。
                    const bool passed = valid && samples > 0 && moving_commands > 0 &&
                        tail_world_error <= config.deadzone_pixels &&
                        tail_public_error <= config.deadzone_pixels &&
                        tail_anchor <= 20.0 * config.body_aim_range_percent / 100.0 &&
                        tail_observer <= 0.0001 && tail_maintenance <= 0.0001 && tail_commands == 0;
                    if (!passed) {
                        ++failures;
                        std::cerr << "[失败] 对称宽度闭环 dt=" << dt << " plant=" << plant
                            << " width=" << changing_width << " direction=" << direction
                            << " world/public/anchor=" << tail_world_error << '/' << tail_public_error
                            << '/' << tail_anchor << " observer/M/q=" << tail_observer << '/'
                            << tail_maintenance << '/' << tail_commands << '\n';
                    }
                }
            }
        }
    }
    if (cases != 24) ++failures;
    return failures;
}

