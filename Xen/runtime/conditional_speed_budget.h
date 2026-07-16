#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>

// jump的极高速尾部需要比普通移动更高的设备预算，但全局4000 counts/s A/B
// 只改善了极高速区间并放大普通输出步进。该状态机把额外预算限制在“已经明显落后、
// 目标仍沿误差方向高速远离”的短窗口内，并通过误差/速度滞回、最长持续时间和冷却
// 防止检测尖峰或往返目标反复打开高预算。
class ConditionalSpeedBudget
{
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    struct Settings
    {
        double baseMaxCountsPerSecond = 3200.0;
        double catchUpMaxCountsPerSecond = 4000.0;
        double entryErrorPixels = 60.0;
        double exitErrorPixels = 32.0;
        double entryVelocityPixelsPerSecond = 1280.0;
        double exitVelocityPixelsPerSecond = 640.0;
        double maximumActiveSeconds = 0.120;
        double cooldownSeconds = 0.200;
    };

    struct Output
    {
        double maxCountsPerSecond = 3200.0;
        bool active = false;
        bool entered = false;
        bool exited = false;
    };

    Output update(double errorX, double velocityX,
                  bool highSpeedTransientSuppressed,
                  bool directionLocked, bool safetySuppressed,
                  TimePoint now, const Settings& settings)
    {
        const double baseMax = std::clamp(
            settings.baseMaxCountsPerSecond, 30.0, 4000.0);
        const double catchUpMax = std::clamp(
            settings.catchUpMaxCountsPerSecond, baseMax, 4000.0);
        const double entryError = std::max(1.0, settings.entryErrorPixels);
        const double exitError = std::clamp(
            settings.exitErrorPixels, 0.0, entryError);
        const double entryVelocity = std::max(
            1.0, settings.entryVelocityPixelsPerSecond);
        const double exitVelocity = std::clamp(
            settings.exitVelocityPixelsPerSecond, 0.0, entryVelocity);
        const double maximumActiveSeconds = std::clamp(
            settings.maximumActiveSeconds, 0.010, 0.500);
        const double cooldownSeconds = std::clamp(
            settings.cooldownSeconds, 0.0, 1.0);

        Output output;
        output.maxCountsPerSecond = baseMax;
        const double absError = std::abs(errorX);
        const double absVelocity = std::abs(velocityX);
        const bool movingAwayFromAim = errorX * velocityX > 0.0;

        if (active_)
        {
            const bool timedOut = std::chrono::duration<double>(
                now - activeSince_).count() >= maximumActiveSeconds;
            const bool keepActive = !timedOut && directionLocked &&
                !safetySuppressed && movingAwayFromAim &&
                absError > exitError && absVelocity >= exitVelocity;
            if (!keepActive)
            {
                active_ = false;
                cooldownUntil_ = now + std::chrono::duration_cast<TimePoint::duration>(
                    std::chrono::duration<double>(cooldownSeconds));
                output.exited = true;
            }
        }

        const bool cooldownComplete = cooldownUntil_ == TimePoint{} ||
            now >= cooldownUntil_;
        const bool canIncreaseBudget = catchUpMax > baseMax + 1e-9;
        if (!active_ && cooldownComplete && canIncreaseBudget &&
            highSpeedTransientSuppressed && directionLocked &&
            !safetySuppressed && movingAwayFromAim &&
            absError >= entryError && absVelocity >= entryVelocity)
        {
            active_ = true;
            activeSince_ = now;
            output.entered = true;
        }

        output.active = active_;
        output.maxCountsPerSecond = active_ ? catchUpMax : baseMax;
        return output;
    }

    void reset()
    {
        active_ = false;
        activeSince_ = {};
        cooldownUntil_ = {};
    }

    bool active() const { return active_; }

private:
    bool active_ = false;
    TimePoint activeSince_{};
    TimePoint cooldownUntil_{};
};
