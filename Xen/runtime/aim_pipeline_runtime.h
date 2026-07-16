#ifndef AIM_PIPELINE_RUNTIME_H
#define AIM_PIPELINE_RUNTIME_H

#include <string>

#include "aim_pipeline_types.h"
#include "los_aim_controller.h"
#include "command_trajectory_shaper.h"
#include "output_scheduler.h"
#include "relative_los_kalman.h"

// P0-0 影子运行时：只接收同帧观测并生成诊断快照，绝不直接访问设备队列。
class AimPipelineRuntime
{
public:
    AimPipelineRuntime();

    void configure(const std::string& requestedMode);
    void configureController(const LosAimController::Settings& settings);
    void configureTrajectory(const CommandTrajectoryShaper::Settings& shaperSettings,
                             const OutputScheduler::Settings& schedulerSettings);
    void reset();
    /** @brief 正式输出暂停；shadow保留估计状态，legacy维持原空状态重置语义。 */
    void suspendOutput();
    AimPipelineFrameState observe(const AimObservation& observation);
    void setViewMotionDiagnostics(const ViewMotionShadowDiagnostics& diagnostics);

    const AimPipelineFrameState& snapshot() const { return frame_; }
    AimPipelineMode requestedMode() const { return requestedMode_; }
    AimPipelineMode effectiveMode() const { return effectiveMode_; }
    uint64_t resetGeneration() const { return resetGeneration_; }

private:
    AimPipelineMode requestedMode_ = AimPipelineMode::Legacy;
    AimPipelineMode effectiveMode_ = AimPipelineMode::Legacy;
    uint64_t resetGeneration_ = 0;
    uint64_t observationSequence_ = 0;
    AimPipelineFrameState frame_{};
    RelativeLosKalman relativeLosKalman_{};
    LosAimController losAimController_{};
    LosAimController::Settings controllerSettings_{};
    FrameTiming::Clock::time_point lastControllerUpdateTime_{};
    CommandTrajectoryShaper trajectoryShaper_{};
    OutputScheduler outputScheduler_{};
};

AimPipelineMode parseAimPipelineMode(const std::string& value);

#endif
