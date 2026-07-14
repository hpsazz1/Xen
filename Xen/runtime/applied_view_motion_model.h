#ifndef APPLIED_VIEW_MOTION_MODEL_H
#define APPLIED_VIEW_MOTION_MODEL_H

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

// 记录设备实际成功发送的相机旋转，并在固定响应延迟后将完整角度步进视为已出现在画面。
class AppliedViewMotionModel
{
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    void configure(double commandToFrameDelayMs);
    void reset();
    void addCommand(
        int countsX,
        int countsY,
        double degreesPerCountX,
        double degreesPerCountY,
        TimePoint sendTime);

    std::pair<double, double> at(TimePoint queryTime) const;
    double commandToFrameDelayMs() const;
    size_t sampleCount() const;

private:
    struct Sample
    {
        double deltaYawDegrees = 0.0;
        double deltaPitchDegrees = 0.0;
        double cumulativeYawDegrees = 0.0;
        double cumulativePitchDegrees = 0.0;
        TimePoint effectiveTime{};
    };

    std::pair<double, double> baselineBeforeFirstLocked() const;
    void pruneLocked(TimePoint newestEffectiveTime);

    mutable std::mutex mutex_;
    std::deque<Sample> samples_;
    double totalYawDegrees_ = 0.0;
    double totalPitchDegrees_ = 0.0;
    double commandToFrameDelayMs_ = 60.0;
};

#endif
