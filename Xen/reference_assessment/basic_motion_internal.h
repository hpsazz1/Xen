#ifndef XEN_BASIC_MOTION_INTERNAL_H
#define XEN_BASIC_MOTION_INTERNAL_H

#include <algorithm>
#include <cmath>
#include <optional>

// 上游 HUD 基础模型：逐轴线性积分，未作对角归一或游戏速度观察。
// 评估层可读取阈值穿越；控制适配层只复用速度积分，不使用评分许可。
namespace xen::reference_assessment::detail {
struct AxisResult { bool counter{}; std::optional<double> cross; };
inline AxisResult advance_axis(double& velocity, int input, double dt, double threshold = 0.34) noexcept {
    const double start = velocity;
    const bool counter = std::abs(start) > 0.001 && input * start < 0;
    const double decel = input == 0 ? 2.5 : 14.0;
    AxisResult result{counter, std::nullopt};
    if ((input == 0 || counter) && std::abs(start) > threshold) {
        const double cross = (std::abs(start) - threshold) / decel;
        if (cross <= dt) result.cross = cross;
    }
    if (input == 0) {
        velocity = std::copysign(std::max(0.0, std::abs(start) - decel * dt), start);
    } else if (!counter) {
        velocity += input * 5.5 * dt;
    } else {
        const double to_zero = std::abs(start) / 14.0;
        velocity = to_zero >= dt ? start + input * 14.0 * dt : input * 5.5 * (dt - to_zero);
    }
    velocity = std::clamp(velocity, -1.0, 1.0);
    return result;
}
}
#endif
