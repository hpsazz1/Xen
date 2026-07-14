#pragma once

#include <algorithm>
#include <chrono>
#include <deque>
#include <utility>

// 保存程序自身已实际发送的视角位移，并按检测观测时间查询累计值。
// 累计值不会随旧样本裁剪而归零，保证相邻观测始终处于同一稳定坐标系。
class ViewMotionHistory
{
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    void add(double x, double y, TimePoint time)
    {
        if (x == 0.0 && y == 0.0)
            return;
        totalX_ += x;
        totalY_ += y;
        samples_.push_back({ x, y, totalX_, totalY_, time });
        prune(time);
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
        const auto start = at(time);
        return { totalX_ - start.first, totalY_ - start.second };
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
};
