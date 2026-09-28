#ifndef XEN_TESTS_AIM_CONSUMER_CONTRACT_SUPPORT_H
#define XEN_TESTS_AIM_CONSUMER_CONTRACT_SUPPORT_H

#include "aim/aim.h"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace aim_consumer_contract_support {

// X固定在中心，Y使用自身输出生成下一观测，独立保护跨轴合同迁移后的Y质量。
template<class Expect>
void vertical_closed_loop(int direction, Expect expect) {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.body_aim_height_ratio = 1.0f / 3.0f;
    config.acquisition_range_percent = 100;
    config.enable_prediction = false;
    config.enable_delay_compensation = false;
    config.counts_per_pixel_y = .4f;
    config.deadzone_pixels = 1.5f;
    config.max_counts_per_frame = 14;
    Aim aim(config);
    float error = direction * 8.0f;
    int moved = 0;
    int tail_commands = 0;
    float tail_peak = 0;
    for (int index = 0; index < 400; ++index) {
        AimFrame frame;
        frame.sequence = index + 1;
        frame.observation_epoch = 91;
        frame.lock_active = true;
        frame.captured_at = frame.control_at = std::chrono::steady_clock::time_point{
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::nanoseconds(50000000000LL + 5000000LL * index))};
        frame.roi_width = frame.roi_height = 320;
        frame.control_center_x = frame.control_center_y = 160;
        frame.detections.push_back({136, 120 + error, 184, 240 + error, .95f, 0});
        const auto result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS && result.has_target &&
                   result.command.dx_counts == 0 && std::abs(result.command.dy_counts) <= 14,
               "独立Y闭环固定X中心，不能有跨轴输出或越界");
        if (direction == 0)
            expect(result.command.dy_counts == 0, "双轴精确中心的静止负控不得凭空发Y");
        const int command = result.has_command ? result.command.dy_counts : 0;
        if (command != 0) ++moved;
        if (result.has_command)
            expect(aim.record_backend_completed_command(frame.sequence, frame.control_at, 0, command),
                   "Y闭环仅应用自身命令");
        error -= command * .4f;
        if (index >= 300) {
            tail_peak = std::max(tail_peak, std::fabs(error));
            tail_commands += std::abs(command);
        }
    }
    expect(tail_peak <= config.deadzone_pixels + .001f && tail_commands == 0 &&
               (direction == 0 || moved > 0),
           "独立Y闭环须实际纠正并收敛原几何死区，不能以全零输出假绿");
}

} // namespace aim_consumer_contract_support
#endif
