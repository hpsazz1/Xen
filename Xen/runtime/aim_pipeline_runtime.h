#ifndef AIM_PIPELINE_RUNTIME_H
#define AIM_PIPELINE_RUNTIME_H

#include <string>

#include "aim_pipeline_types.h"

// P0-0 影子运行时：只接收同帧观测并生成诊断快照，绝不直接访问设备队列。
class AimPipelineRuntime
{
public:
    AimPipelineRuntime();

    void configure(const std::string& requestedMode);
    void reset();
    AimPipelineFrameState observe(const AimObservation& observation);

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
};

AimPipelineMode parseAimPipelineMode(const std::string& value);

#endif
