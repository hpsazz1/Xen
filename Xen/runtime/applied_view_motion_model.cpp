#include "applied_view_motion_model.h"

#include <algorithm>
#include <cmath>

void AppliedViewMotionModel::configure(
    double commandToFrameDelayMs, double commandResponseMs)
{
    const double boundedDelay = std::clamp(commandToFrameDelayMs, 0.0, 250.0);
    const double boundedResponse = std::clamp(commandResponseMs, 0.0, 100.0);
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::abs(commandToFrameDelayMs_ - boundedDelay) <= 1e-9 &&
        std::abs(commandResponseMs_ - boundedResponse) <= 1e-9)
        return;
    commandToFrameDelayMs_ = boundedDelay;
    commandResponseMs_ = boundedResponse;
    samples_.clear();
    baselineYawDegrees_ = 0.0;
    baselinePitchDegrees_ = 0.0;
}

void AppliedViewMotionModel::reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    samples_.clear();
    baselineYawDegrees_ = 0.0;
    baselinePitchDegrees_ = 0.0;
}

void AppliedViewMotionModel::addCommand(
    int countsX,
    int countsY,
    double degreesPerCountX,
    double degreesPerCountY,
    TimePoint sendTime)
{
    if ((countsX == 0 && countsY == 0) ||
        sendTime.time_since_epoch().count() == 0 ||
        !std::isfinite(degreesPerCountX) || !std::isfinite(degreesPerCountY))
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const double deltaYaw = static_cast<double>(countsX) * degreesPerCountX;
    const double deltaPitch = static_cast<double>(countsY) * degreesPerCountY;
    // delay表示响应中心，response表示完整响应宽度。中心小于半宽时从发送时刻
    // 开始，禁止模型把设备响应外推到命令真正发送之前。
    const double startDelayMs = std::max(
        0.0, commandToFrameDelayMs_ - commandResponseMs_ * 0.5);
    const auto startDelay = std::chrono::duration_cast<TimePoint::duration>(
        std::chrono::duration<double, std::milli>(startDelayMs));
    const auto responseDuration = std::chrono::duration_cast<TimePoint::duration>(
        std::chrono::duration<double, std::milli>(commandResponseMs_));
    const TimePoint startTime = sendTime + startDelay;
    const TimePoint endTime = startTime + responseDuration;
    samples_.push_back({
        deltaYaw,
        deltaPitch,
        startTime,
        endTime
    });
    pruneLocked(endTime);
}

std::pair<double, double> AppliedViewMotionModel::at(TimePoint queryTime) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    double yaw = baselineYawDegrees_;
    double pitch = baselinePitchDegrees_;
    for (const Sample& sample : samples_)
    {
        if (queryTime < sample.startTime)
            break;
        double fraction = 1.0;
        if (sample.endTime > sample.startTime && queryTime < sample.endTime)
        {
            fraction = std::chrono::duration<double>(
                queryTime - sample.startTime).count() /
                std::chrono::duration<double>(
                    sample.endTime - sample.startTime).count();
        }
        yaw += sample.deltaYawDegrees * fraction;
        pitch += sample.deltaPitchDegrees * fraction;
    }
    return { yaw, pitch };
}

double AppliedViewMotionModel::commandToFrameDelayMs() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return commandToFrameDelayMs_;
}

double AppliedViewMotionModel::commandResponseMs() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return commandResponseMs_;
}

size_t AppliedViewMotionModel::sampleCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return samples_.size();
}

void AppliedViewMotionModel::pruneLocked(TimePoint newestEndTime)
{
    constexpr auto lifetime = std::chrono::seconds(2);
    while (!samples_.empty() && newestEndTime - samples_.front().endTime > lifetime)
    {
        baselineYawDegrees_ += samples_.front().deltaYawDegrees;
        baselinePitchDegrees_ += samples_.front().deltaPitchDegrees;
        samples_.pop_front();
    }
    constexpr size_t maxSamples = 512;
    while (samples_.size() > maxSamples)
    {
        baselineYawDegrees_ += samples_.front().deltaYawDegrees;
        baselinePitchDegrees_ += samples_.front().deltaPitchDegrees;
        samples_.pop_front();
    }
}
