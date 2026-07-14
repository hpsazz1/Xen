#include "aim_pipeline_runtime.h"

#include <algorithm>
#include <cctype>

namespace
{
std::string lowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}
}

AimPipelineRuntime::AimPipelineRuntime()
{
    configure("legacy");
}

AimPipelineMode parseAimPipelineMode(const std::string& value)
{
    const std::string normalized = lowerAscii(value);
    if (normalized == "shadow")
        return AimPipelineMode::Shadow;
    if (normalized == "active")
        return AimPipelineMode::Active;
    return AimPipelineMode::Legacy;
}

void AimPipelineRuntime::configure(const std::string& requestedMode)
{
    requestedMode_ = parseAimPipelineMode(requestedMode);
    // P0-0 至 P0-5 不允许新链路接管设备；active 只保留请求状态并安全降级为 shadow。
    effectiveMode_ = requestedMode_ == AimPipelineMode::Legacy
        ? AimPipelineMode::Legacy
        : AimPipelineMode::Shadow;
    reset();
}

void AimPipelineRuntime::reset()
{
    ++resetGeneration_;
    observationSequence_ = 0;
    frame_ = {};
    frame_.requestedMode = requestedMode_;
    frame_.effectiveMode = effectiveMode_;
    frame_.activeAvailable = false;
    frame_.commandSuppressed = true;
    frame_.resetGeneration = resetGeneration_;
}

AimPipelineFrameState AimPipelineRuntime::observe(const AimObservation& observation)
{
    frame_.requestedMode = requestedMode_;
    frame_.effectiveMode = effectiveMode_;
    frame_.activeAvailable = false;
    frame_.shadowProcessed = false;
    frame_.commandSuppressed = true;
    frame_.resetGeneration = resetGeneration_;
    if (effectiveMode_ == AimPipelineMode::Legacy)
        return frame_;

    frame_.shadowProcessed = true;
    frame_.observationSequence = ++observationSequence_;
    frame_.observation = observation;
    if (frame_.observation.timing.controlTime.time_since_epoch().count() == 0)
        frame_.observation.timing.controlTime = FrameTiming::Clock::now();
    frame_.estimate = {};
    frame_.control = {};
    frame_.trajectoryRequest = {};
    frame_.trajectoryState = {};
    frame_.trajectoryOutput = {};
    frame_.trajectoryOutput.commandSuppressed = true;
    return frame_;
}
