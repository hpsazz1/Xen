#include "applied_view_motion_model.h"

#include <algorithm>
#include <cmath>

void AppliedViewMotionModel::configure(double commandToFrameDelayMs)
{
    const double boundedDelay = std::clamp(commandToFrameDelayMs, 0.0, 250.0);
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::abs(commandToFrameDelayMs_ - boundedDelay) <= 1e-9)
        return;
    commandToFrameDelayMs_ = boundedDelay;
    samples_.clear();
    totalYawDegrees_ = 0.0;
    totalPitchDegrees_ = 0.0;
}

void AppliedViewMotionModel::reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    samples_.clear();
    totalYawDegrees_ = 0.0;
    totalPitchDegrees_ = 0.0;
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
    totalYawDegrees_ += deltaYaw;
    totalPitchDegrees_ += deltaPitch;
    const auto delay = std::chrono::duration_cast<TimePoint::duration>(
        std::chrono::duration<double, std::milli>(commandToFrameDelayMs_));
    const TimePoint effectiveTime = sendTime + delay;
    samples_.push_back({
        deltaYaw,
        deltaPitch,
        totalYawDegrees_,
        totalPitchDegrees_,
        effectiveTime
    });
    pruneLocked(effectiveTime);
}

std::pair<double, double> AppliedViewMotionModel::at(TimePoint queryTime) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (samples_.empty())
        return { totalYawDegrees_, totalPitchDegrees_ };

    const auto after = std::upper_bound(
        samples_.begin(), samples_.end(), queryTime,
        [](const TimePoint& value, const Sample& sample) {
            return value < sample.effectiveTime;
        });
    if (after == samples_.begin())
        return baselineBeforeFirstLocked();
    if (after == samples_.end())
        return { totalYawDegrees_, totalPitchDegrees_ };
    const auto applied = std::prev(after);
    return { applied->cumulativeYawDegrees, applied->cumulativePitchDegrees };
}

double AppliedViewMotionModel::commandToFrameDelayMs() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return commandToFrameDelayMs_;
}

size_t AppliedViewMotionModel::sampleCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return samples_.size();
}

std::pair<double, double> AppliedViewMotionModel::baselineBeforeFirstLocked() const
{
    return {
        samples_.front().cumulativeYawDegrees - samples_.front().deltaYawDegrees,
        samples_.front().cumulativePitchDegrees - samples_.front().deltaPitchDegrees
    };
}

void AppliedViewMotionModel::pruneLocked(TimePoint newestEffectiveTime)
{
    constexpr auto lifetime = std::chrono::seconds(2);
    while (!samples_.empty() && newestEffectiveTime - samples_.front().effectiveTime > lifetime)
        samples_.pop_front();
    constexpr size_t maxSamples = 512;
    while (samples_.size() > maxSamples)
        samples_.pop_front();
}
