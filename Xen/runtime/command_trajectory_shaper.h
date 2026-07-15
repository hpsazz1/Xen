#ifndef COMMAND_TRAJECTORY_SHAPER_H
#define COMMAND_TRAJECTORY_SHAPER_H

#include <string>

#include "aim_pipeline_types.h"

// 确定性在线整形器：输入是最新控制请求对应的目标速度，每次固定周期只生成一个二维位移。
// 随机噪声、停顿、Bezier端点路径和输出EMA均不属于工程约束，不在新链路中实现。
class CommandTrajectoryShaper
{
public:
    struct Settings
    {
        TrajectoryShaperMode mode = TrajectoryShaperMode::Off;
        double maxVelocityCountsPerSecond = 1440.0;
        double maxAccelerationCountsPerSecond2 = 60000.0;
        double maxJerkCountsPerSecond3 = 4000000.0;
    };

    struct Result
    {
        TrajectoryState state{};
        TrajectoryOutput output{};
    };

    void configure(const Settings& settings);
    Result update(const TrajectoryRequest& request,
                  double outputDurationSeconds,
                  FrameTiming::Clock::time_point tickTime);
    Result emergencyReset(FrameTiming::Clock::time_point tickTime);
    void reset();

    const Settings& settings() const { return settings_; }
    const TrajectoryState& state() const { return state_; }

private:
    Result buildResult(double shapedCountsX,
                       double shapedCountsY,
                       FrameTiming::Clock::time_point tickTime);

    Settings settings_{};
    TrajectoryState state_{};
    double quantizationRemainderX_ = 0.0;
    double quantizationRemainderY_ = 0.0;
};

TrajectoryShaperMode parseTrajectoryShaperMode(const std::string& value);

#endif
