#ifndef OUTPUT_SCHEDULER_H
#define OUTPUT_SCHEDULER_H

#include <optional>

#include "aim_pipeline_types.h"
#include "command_trajectory_shaper.h"

// Latest-only固定周期调度器。一次service最多生成一个二维输出；若调用线程迟到，
// 直接跳到最近的周期格点并记录跳过数，不补发一串过期子步。
class OutputScheduler
{
public:
    struct Settings
    {
        double outputHz = 240.0;
    };

    void configure(const Settings& settings);
    void reset();
    void submit(const TrajectoryRequest& request);
    std::optional<CommandTrajectoryShaper::Result> service(
        FrameTiming::Clock::time_point now,
        CommandTrajectoryShaper& shaper);

    double outputPeriodSeconds() const { return outputPeriodSeconds_; }

private:
    double outputPeriodSeconds_ = 1.0 / 240.0;
    std::optional<TrajectoryRequest> latestRequest_{};
    FrameTiming::Clock::time_point nextTickTime_{};
    uint64_t pendingSkippedTicks_ = 0;
};

#endif
