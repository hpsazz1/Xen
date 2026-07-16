#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <utility>

// 保存程序自身已实际发送的视角位移，并按检测观测时间查询累计值。
// 累计值不会随旧样本裁剪而归零，保证相邻观测始终处于同一稳定坐标系。
class ViewMotionHistory
{
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    // delay 表示响应中心，response 表示完整命令在画面中逐步出现的宽度。
    // 参数改变时清空累计坐标系，避免同一时间线混用不同响应核。
    void configure(double commandToFrameDelayMs, double commandResponseMs)
    {
        const double boundedDelay = std::clamp(commandToFrameDelayMs, 0.0, 250.0);
        const double boundedResponse = std::clamp(commandResponseMs, 0.0, 100.0);
        if (std::abs(commandToFrameDelayMs_ - boundedDelay) <= 1e-9 &&
            std::abs(commandResponseMs_ - boundedResponse) <= 1e-9)
            return;
        commandToFrameDelayMs_ = boundedDelay;
        commandResponseMs_ = boundedResponse;
        reset();
    }

    void reset()
    {
        samples_.clear();
        baselineX_ = 0.0;
        baselineY_ = 0.0;
    }

    void add(double x, double y, TimePoint time)
    {
        if (x == 0.0 && y == 0.0)
            return;
        // 默认 12 ms 中心、24 ms 宽度对应从发送时刻开始到 24 ms 完成。
        // 当用户配置的中心小于半宽时从发送时刻开始，禁止响应落到发送之前。
        const double startDelayMs = std::max(
            0.0, commandToFrameDelayMs_ - commandResponseMs_ * 0.5);
        const auto startDelay = std::chrono::duration_cast<TimePoint::duration>(
            std::chrono::duration<double, std::milli>(startDelayMs));
        const auto responseDuration = std::chrono::duration_cast<TimePoint::duration>(
            std::chrono::duration<double, std::milli>(commandResponseMs_));
        const TimePoint startTime = time + startDelay;
        const TimePoint endTime = startTime + responseDuration;
        samples_.push_back({ x, y, startTime, endTime });
        prune(endTime);
    }

    std::pair<double, double> at(TimePoint time) const
    {
        double x = baselineX_;
        double y = baselineY_;
        for (const Sample& sample : samples_)
        {
            if (time < sample.startTime)
                break;
            double fraction = 1.0;
            if (sample.endTime > sample.startTime && time < sample.endTime)
            {
                fraction = std::chrono::duration<double>(
                    time - sample.startTime).count() /
                    std::chrono::duration<double>(
                        sample.endTime - sample.startTime).count();
            }
            x += sample.x * fraction;
            y += sample.y * fraction;
        }
        return { x, y };
    }

    std::pair<double, double> since(TimePoint time) const
    {
        return between(time, std::chrono::steady_clock::now());
    }

    std::pair<double, double> between(TimePoint startTime, TimePoint endTime) const
    {
        const auto start = at(startTime);
        const auto end = at(endTime);
        return { end.first - start.first, end.second - start.second };
    }

    double commandToFrameDelayMs() const
    {
        return commandToFrameDelayMs_;
    }

    double commandResponseMs() const
    {
        return commandResponseMs_;
    }

private:
    struct Sample
    {
        double x = 0.0;
        double y = 0.0;
        TimePoint startTime{};
        TimePoint endTime{};
    };

    void prune(TimePoint now)
    {
        constexpr auto lifetime = std::chrono::seconds(2);
        while (!samples_.empty() && now - samples_.front().endTime > lifetime)
        {
            baselineX_ += samples_.front().x;
            baselineY_ += samples_.front().y;
            samples_.pop_front();
        }
        constexpr size_t maxSamples = 512;
        while (samples_.size() > maxSamples)
        {
            baselineX_ += samples_.front().x;
            baselineY_ += samples_.front().y;
            samples_.pop_front();
        }
    }

    std::deque<Sample> samples_;
    double baselineX_ = 0.0;
    double baselineY_ = 0.0;
    double commandToFrameDelayMs_ = 0.0;
    double commandResponseMs_ = 0.0;
};
