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
    relativeLosKalman_.reset();
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

    const bool targetChanged = frame_.observation.valid && observation.valid &&
        frame_.observation.trackId != observation.trackId;
    if (targetChanged)
    {
        ++resetGeneration_;
        observationSequence_ = 0;
        relativeLosKalman_.reset();
        frame_.resetGeneration = resetGeneration_;
    }

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
    frame_.viewMotion = {};
    return frame_;
}

void AimPipelineRuntime::setViewMotionDiagnostics(
    const ViewMotionShadowDiagnostics& diagnostics)
{
    frame_.viewMotion = diagnostics;
    if (!diagnostics.valid || effectiveMode_ == AimPipelineMode::Legacy)
        return;

    relativeLosKalman_.update(
        diagnostics.stabilizedLosYawDegrees,
        diagnostics.stabilizedLosPitchDownDegrees,
        frame_.observation.confidence,
        frame_.observation.timing.observationTime,
        frame_.observation.timing.controlTime);
    const RelativeLosKalmanEstimate& estimate = relativeLosKalman_.estimate();
    frame_.estimate.valid = estimate.x.valid && estimate.y.valid;
    frame_.estimate.angleX = estimate.x.angleDegrees;
    frame_.estimate.angleY = estimate.y.angleDegrees;
    frame_.estimate.rateX = estimate.x.rateDegreesPerSecond;
    frame_.estimate.rateY = estimate.y.rateDegreesPerSecond;
    frame_.estimate.covarianceX = estimate.x.angleVariance;
    frame_.estimate.covarianceY = estimate.y.angleVariance;
    frame_.estimate.innovationVarianceX = estimate.x.innovationVariance;
    frame_.estimate.innovationVarianceY = estimate.y.innovationVariance;
    frame_.estimate.innovationX = estimate.x.innovationDegrees;
    frame_.estimate.innovationY = estimate.y.innovationDegrees;
    frame_.estimate.nisX = estimate.x.nis;
    frame_.estimate.nisY = estimate.y.nis;
    frame_.estimate.measurementConfidence = estimate.measurementConfidence;
    frame_.estimate.feedforwardConfidence = estimate.feedforwardConfidence;
}
