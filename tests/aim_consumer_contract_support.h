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

// 历史前缀后冻结世界目标；后继图像只由该Aim自己的输出形成。
// 不再将改变q后的旧录像帧冒充物理闭环。固定测试plant，不作现实标定结论。
template<class Expect>
void settle_static_tail(Aim& aim, AimFrame& f, Expect expect) {
    float camera_x = 0, camera_y = 0, tail_peak = 0;
    int tail_commands = 0;
    for (int i = 0; i < 500; ++i) {
        const auto previous_sequence = f.sequence;
        const auto previous_at = f.captured_at;
        ++f.sequence;
        f.captured_at += std::chrono::milliseconds(5);
        f.control_at = f.captured_at + std::chrono::milliseconds(2);
        f.lock_active = true;
        for (auto& d : f.detections) {
            d.x1 += camera_x;
            d.x2 += camera_x;
            d.y1 += camera_y;
            d.y2 += camera_y;
        }
        f.background_motion_x = {AimBackgroundMotionStatus::VALID, previous_sequence, f.sequence,
            previous_at, f.captured_at, f.observation_epoch, camera_x, .95f, 0, 2};
        const auto r = aim.process(f);
        expect(r.status == AimStatus::SUCCESS && r.has_target, "静止反馈尾必须持续持有目标");
        expect(std::hypot(float(r.command.dx_counts), float(r.command.dy_counts)) <= 14,
               "静止反馈尾守二维上限");
        const int qx = r.has_command ? r.command.dx_counts : 0,
                  qy = r.has_command ? r.command.dy_counts : 0;
        if (r.has_command)
            expect(aim.record_backend_completed_command(f.sequence, f.control_at, qx, qy),
                   "静止反馈尾只应用自身请求");
        camera_x = -qx * .5215f / f.source_pixels_per_roi_pixel_x;
        camera_y = -qy * .4f / f.source_pixels_per_roi_pixel_y;
        if (i >= 400) {
            tail_peak = std::max(tail_peak, std::fabs(
                (r.target.base_aim_x - f.control_center_x) * f.source_pixels_per_roi_pixel_x));
            tail_commands += qx != 0;
            expect(std::fabs(r.control.modelled_response_x_counts) < .001f,
                   "静止反馈尾不得保留运动维护");
        }
    }
    expect(tail_peak <= 1.501f && tail_commands == 0,
           "历史运动尾须收敛原死区且停发，不能以仅方向或cap代替质量门");
}

// 两分支具有相同X输入和各自相同ACK，故Y比较不会误把二维cap改变视为回归。
class RejectedBackgroundPair {
    Aim missing_, invalid_;

public:
    explicit RejectedBackgroundPair(const AimConfig& config)
        : missing_(config), invalid_(config) {}

    void reset() {
        missing_.reset();
        invalid_.reset();
    }

    template<class Expect>
    void process(AimFrame frame, std::chrono::steady_clock::time_point backend, Expect expect) {
        frame.background_motion_x.status = AimBackgroundMotionStatus::MISSING;
        const auto a = missing_.process(frame);
        frame.background_motion_x.status = AimBackgroundMotionStatus::LOW_TEXTURE;
        frame.background_motion_x.dx_roi_pixels = 1000;
        const auto b = invalid_.process(frame);
        expect(a.status == AimStatus::SUCCESS && b.status == AimStatus::SUCCESS &&
                   a.has_target == b.has_target && a.has_command == b.has_command &&
                   a.command.dx_counts == b.command.dx_counts && a.command.dy_counts == b.command.dy_counts &&
                   a.target.base_aim_x == b.target.base_aim_x && a.target.base_aim_y == b.target.base_aim_y &&
                   a.control.observer_target_velocity_x_counts_per_second ==
                       b.control.observer_target_velocity_x_counts_per_second,
               "同X上下文拒绝BG配对须保持X/Y、锚点及observer，不依赖旧Y整数表");
        if (a.has_command)
            expect(missing_.record_backend_completed_command(
                       frame.sequence, backend, a.command.dx_counts, a.command.dy_counts),
                   "缺失分支只回执自身命令");
        if (b.has_command)
            expect(invalid_.record_backend_completed_command(
                       frame.sequence, backend, b.command.dx_counts, b.command.dy_counts),
                   "非法分支只回执自身命令");
    }
};
} // namespace aim_consumer_contract_support
#endif
