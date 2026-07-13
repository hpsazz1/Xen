#pragma once

#include <algorithm>
#include <chrono>

/**
 * @brief 基于单调时钟的帧率统计器
 *
 * 每次真实帧/结果到达时调用 addFrame。统计窗口至少覆盖一秒，避免用循环轮询次数冒充帧率；
 * value 会在长时间无事件时返回 0，防止断流后界面继续显示旧 FPS。
 */
class FrameRateCounter
{
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    void reset(TimePoint now = Clock::now())
    {
        windowStart_ = now;
        lastFrame_ = {};
        windowFrames_ = 0;
        completedFps_ = 0;
    }

    int addFrame(TimePoint now = Clock::now())
    {
        if (windowStart_.time_since_epoch().count() == 0)
            reset(now);

        lastFrame_ = now;
        ++windowFrames_;
        const double elapsed = std::chrono::duration<double>(now - windowStart_).count();
        if (elapsed >= 1.0)
        {
            completedFps_ = std::max(0, static_cast<int>(windowFrames_ / elapsed));
            windowFrames_ = 0;
            windowStart_ = now;
        }
        return completedFps_;
    }

    int value(TimePoint now = Clock::now(),
              std::chrono::milliseconds staleAfter = std::chrono::milliseconds(2000)) const
    {
        if (lastFrame_.time_since_epoch().count() == 0 || now - lastFrame_ > staleAfter)
            return 0;
        return completedFps_;
    }

private:
    TimePoint windowStart_{};
    TimePoint lastFrame_{};
    int windowFrames_ = 0;
    int completedFps_ = 0;
};
