#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>

// 控制输出周期必须由相邻控制执行时刻计算，不能复用可能成批到达的观测时间差。
// 状态估计继续使用观测时间；本类只负责把连续设备速度换算为本次输出 counts。
class ControlIntervalTracker
{
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    double update(TimePoint controlTime, double fallbackSeconds)
    {
        double interval = std::isfinite(fallbackSeconds) ? fallbackSeconds : 0.010;
        if (controlTime.time_since_epoch().count() != 0)
        {
            if (previousControlTime_.time_since_epoch().count() != 0 &&
                controlTime > previousControlTime_)
            {
                interval = std::chrono::duration<double>(
                    controlTime - previousControlTime_).count();
            }
            previousControlTime_ = controlTime;
        }
        return std::clamp(interval, 0.002, 0.050);
    }

    void reset()
    {
        previousControlTime_ = {};
    }

private:
    TimePoint previousControlTime_{};
};
