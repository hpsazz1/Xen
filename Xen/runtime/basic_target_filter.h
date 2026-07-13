#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>

// 基础跟踪阶段的自适应观测滤波器。
// 静止时抑制检测量化噪声，真实位移增大时自动提高响应速度。
// 本模块只处理当前观测，不进行未来外推或目标速度前馈。
class BasicTargetFilter
{
public:
    struct Result
    {
        double x = 0.0;
        double y = 0.0;
        double observedSpeed = 0.0; // 仅用于诊断的相邻观测速度，px/sec
        double residual = 0.0;      // 原始观测到滤波位置的距离，px
    };

    Result update(double x, double y,
                  std::chrono::steady_clock::time_point observationTime,
                  double fallbackFrameSeconds,
                  double screenWidth)
    {
        if (!std::isfinite(x) || !std::isfinite(y))
            return { filteredX_, filteredY_, 0.0, 0.0 };

        if (observationTime.time_since_epoch().count() == 0)
            observationTime = std::chrono::steady_clock::now();

        if (!initialized_)
        {
            initialize(x, y, observationTime);
            return { x, y, 0.0, 0.0 };
        }

        double dt = std::chrono::duration<double>(observationTime - previousTime_).count();
        if (!std::isfinite(dt) || dt <= 0.0)
            dt = fallbackFrameSeconds;
        dt = std::clamp(dt, 1.0 / 500.0, 0.10);

        const double rawDx = x - previousRawX_;
        const double rawDy = y - previousRawY_;
        const double rawDistance = std::hypot(rawDx, rawDy);
        const double observedSpeed = rawDistance / dt;

        // 大幅跳变代表新目标或新轨迹，旧滤波状态不能跨目标延续；
        // 目标身份由本模块上游的锁定器负责。
        if (rawDistance > std::max(24.0, screenWidth * 0.20))
        {
            initialize(x, y, observationTime);
            return { x, y, observedSpeed, 0.0 };
        }

        const double residualX = x - filteredX_;
        const double residualY = y - filteredY_;
        const double residual = std::hypot(residualX, residualY);

        // 静止时约 30 ms 时间常数用于抑制检测量化；观测速度升高时
        // 连续降低到约 6 ms，减少移动目标的相位滞后。
        const double motionRatio = std::clamp(
            observedSpeed / std::max(120.0, screenWidth * 1.25), 0.0, 1.0);
        const double tau = 0.030 + (0.006 - 0.030) * motionRatio;
        double alpha = 1.0 - std::exp(-dt / tau);

        // 亚像素残差主要来自检测噪声。采用连续衰减，避免硬死区边界跳变。
        const double jitterRadius = std::max(0.35, screenWidth / 900.0);
        if (residual < jitterRadius)
            alpha *= residual / jitterRadius;

        filteredX_ += residualX * alpha;
        filteredY_ += residualY * alpha;
        previousRawX_ = x;
        previousRawY_ = y;
        previousTime_ = observationTime;
        return { filteredX_, filteredY_, observedSpeed, residual };
    }

    void reset()
    {
        initialized_ = false;
        filteredX_ = filteredY_ = 0.0;
        previousRawX_ = previousRawY_ = 0.0;
        previousTime_ = {};
    }

private:
    void initialize(double x, double y, std::chrono::steady_clock::time_point time)
    {
        initialized_ = true;
        filteredX_ = previousRawX_ = x;
        filteredY_ = previousRawY_ = y;
        previousTime_ = time;
    }

    bool initialized_ = false;
    double filteredX_ = 0.0;
    double filteredY_ = 0.0;
    double previousRawX_ = 0.0;
    double previousRawY_ = 0.0;
    std::chrono::steady_clock::time_point previousTime_{};
};
