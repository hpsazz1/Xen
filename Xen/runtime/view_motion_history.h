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

    // 设备命令不会在发送确认的瞬间出现在采集画面。延迟改变时必须清空
    // 累计坐标系，避免同一时间线混用两种生效时基。
    void configure(double commandToFrameDelayMs)
    {
        const double boundedDelay = std::clamp(commandToFrameDelayMs, 0.0, 250.0);
        if (std::abs(commandToFrameDelayMs_ - boundedDelay) <= 1e-9)
            return;
        commandToFrameDelayMs_ = boundedDelay;
        reset();
    }

    void reset()
    {
        samples_.clear();
        totalX_ = 0.0;
        totalY_ = 0.0;
    }

    void add(double x, double y, TimePoint time)
    {
        if (x == 0.0 && y == 0.0)
            return;
        totalX_ += x;
        totalY_ += y;
        const auto delay = std::chrono::duration_cast<TimePoint::duration>(
            std::chrono::duration<double, std::milli>(commandToFrameDelayMs_));
        const TimePoint effectiveTime = time + delay;
        samples_.push_back({ x, y, totalX_, totalY_, effectiveTime });
        prune(effectiveTime);
    }

    std::pair<double, double> at(TimePoint time) const
    {
        if (samples_.empty())
            return { totalX_, totalY_ };

        auto after = std::upper_bound(
            samples_.begin(), samples_.end(), time,
            [](const TimePoint& value, const Sample& sample) {
                return value < sample.time;
            });
        if (after == samples_.begin())
            return baselineBeforeFirst();
        if (after == samples_.end())
            return { totalX_, totalY_ };
        --after;
        return { after->cumulativeX, after->cumulativeY };
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

private:
    struct Sample
    {
        double x = 0.0;
        double y = 0.0;
        double cumulativeX = 0.0;
        double cumulativeY = 0.0;
        TimePoint time{};
    };

    std::pair<double, double> baselineBeforeFirst() const
    {
        return {
            samples_.front().cumulativeX - samples_.front().x,
            samples_.front().cumulativeY - samples_.front().y
        };
    }

    void prune(TimePoint now)
    {
        constexpr auto lifetime = std::chrono::seconds(2);
        while (!samples_.empty() && now - samples_.front().time > lifetime)
            samples_.pop_front();
        constexpr size_t maxSamples = 512;
        while (samples_.size() > maxSamples)
            samples_.pop_front();
    }

    std::deque<Sample> samples_;
    double totalX_ = 0.0;
    double totalY_ = 0.0;
    double commandToFrameDelayMs_ = 0.0;
};
