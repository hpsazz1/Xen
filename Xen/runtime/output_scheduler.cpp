#include "output_scheduler.h"

#include <algorithm>
#include <cmath>

void OutputScheduler::configure(const Settings& settings)
{
    const double outputHz = std::isfinite(settings.outputHz)
        ? std::clamp(settings.outputHz, 30.0, 1000.0)
        : 240.0;
    outputPeriodSeconds_ = 1.0 / outputHz;
    reset();
}

void OutputScheduler::reset()
{
    latestRequest_.reset();
    nextTickTime_ = {};
    pendingSkippedTicks_ = 0;
}

void OutputScheduler::submit(const TrajectoryRequest& request)
{
    if (!request.valid)
    {
        reset();
        return;
    }
    latestRequest_ = request;
    if (!frameTimeKnown(nextTickTime_))
    {
        nextTickTime_ = request.requestTime;
        return;
    }

    // 新请求只能从其到达后的首个固定格点开始生效，禁止把新速度回填到过去的tick。
    const auto period = std::chrono::duration_cast<FrameTiming::Clock::duration>(
        std::chrono::duration<double>(outputPeriodSeconds_));
    if (period.count() > 0 && nextTickTime_ < request.requestTime)
    {
        const auto delta = request.requestTime - nextTickTime_;
        const uint64_t advanceTicks = static_cast<uint64_t>(
            (delta.count() + period.count() - 1) / period.count());
        nextTickTime_ += period * advanceTicks;
        pendingSkippedTicks_ += advanceTicks;
    }
}

std::optional<CommandTrajectoryShaper::Result> OutputScheduler::service(
    FrameTiming::Clock::time_point now,
    CommandTrajectoryShaper& shaper)
{
    if (!latestRequest_.has_value() || !frameTimeKnown(now) || now < nextTickTime_)
        return std::nullopt;

    const auto period = std::chrono::duration_cast<FrameTiming::Clock::duration>(
        std::chrono::duration<double>(outputPeriodSeconds_));
    const auto lateness = now - nextTickTime_;
    const uint64_t skippedTicks = period.count() > 0
        ? static_cast<uint64_t>(lateness / period) : 0;
    const auto tickTime = nextTickTime_ + period * skippedTicks;
    nextTickTime_ = tickTime + period;

    CommandTrajectoryShaper::Result result = shaper.update(
        *latestRequest_, outputPeriodSeconds_, tickTime);
    result.output.schedulerSkippedTicks = pendingSkippedTicks_ + skippedTicks;
    pendingSkippedTicks_ = 0;
    result.output.shapingDelayMs = std::max(0.0,
        std::chrono::duration<double, std::milli>(
            now - latestRequest_->requestTime).count());
    result.output.schedulerLatenessMs = std::max(0.0,
        std::chrono::duration<double, std::milli>(now - tickTime).count());
    result.output.scheduledTickTime = tickTime;
    result.output.outputTickTime = now;
    return result;
}
