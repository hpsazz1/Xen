#ifndef APPLIED_VIEW_MOTION_MODEL_H
#define APPLIED_VIEW_MOTION_MODEL_H

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

// 记录设备实际成功发送的相机旋转，并按响应中心和宽度查询画面中的累计角度。
class AppliedViewMotionModel
{
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    void configure(double commandToFrameDelayMs, double commandResponseMs = 0.0);
    void reset();
    void addCommand(
        int countsX,
        int countsY,
        double degreesPerCountX,
        double degreesPerCountY,
        TimePoint sendTime);

    std::pair<double, double> at(TimePoint queryTime) const;
    std::pair<double, double> rateAt(TimePoint queryTime) const;
    double commandToFrameDelayMs() const;
    double commandResponseMs() const;
    size_t sampleCount() const;

private:
    struct Sample
    {
        double deltaYawDegrees = 0.0;
        double deltaPitchDegrees = 0.0;
        TimePoint startTime{};
        TimePoint endTime{};
    };

    void pruneLocked(TimePoint newestEndTime);

    mutable std::mutex mutex_;
    std::deque<Sample> samples_;
    double baselineYawDegrees_ = 0.0;
    double baselinePitchDegrees_ = 0.0;
    double commandToFrameDelayMs_ = 60.0;
    double commandResponseMs_ = 0.0;
};

#endif
