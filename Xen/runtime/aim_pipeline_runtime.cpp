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
    losAimController_.reset();
    lastControllerUpdateTime_ = {};
}

void AimPipelineRuntime::configureController(const LosAimController::Settings& settings)
{
    controllerSettings_ = settings;
    losAimController_.reset();
    lastControllerUpdateTime_ = {};
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
        losAimController_.reset();
        lastControllerUpdateTime_ = {};
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

    LosAimController::Input controllerInput;
    controllerInput.valid = frame_.estimate.valid;
    controllerInput.errorDegreesX = frame_.estimate.angleX -
        diagnostics.appliedCameraYawAtControlDegrees;
    controllerInput.errorDegreesY = frame_.estimate.angleY -
        diagnostics.appliedCameraPitchAtControlDegrees;
    controllerInput.relativeLosRateDegreesPerSecondX = frame_.estimate.rateX;
    controllerInput.relativeLosRateDegreesPerSecondY = frame_.estimate.rateY;
    controllerInput.feedforwardConfidence = frame_.estimate.feedforwardConfidence;
    controllerInput.degreesPerCountX = diagnostics.degreesPerCountX;
    controllerInput.degreesPerCountY = diagnostics.degreesPerCountY;
    controllerInput.dtSeconds = lastControllerUpdateTime_.time_since_epoch().count() == 0
        ? 1.0 / 120.0
        : std::chrono::duration<double>(
            frame_.observation.timing.controlTime - lastControllerUpdateTime_).count();
    const LosAimController::Output control =
        losAimController_.update(controllerInput, controllerSettings_);
    lastControllerUpdateTime_ = frame_.observation.timing.controlTime;
    frame_.control.valid = control.valid;
    frame_.control.speedLimited = control.speedLimited;
    frame_.control.integralFrozen = control.integralFrozen;
    frame_.control.feedbackX = control.feedbackCountsX;
    frame_.control.feedbackY = control.feedbackCountsY;
    frame_.control.trackingFeedforwardX = control.trackingFeedforwardCountsX;
    frame_.control.trackingFeedforwardY = control.trackingFeedforwardCountsY;
    frame_.control.leadReferenceX = control.leadReferenceDegreesX;
    frame_.control.leadReferenceY = control.leadReferenceDegreesY;
    frame_.control.leadCountsX = control.leadCountsX;
    frame_.control.leadCountsY = control.leadCountsY;
    frame_.control.integralCountsX = control.integralCountsX;
    frame_.control.integralCountsY = control.integralCountsY;
    frame_.control.unlimitedCountsX = control.unlimitedCountsX;
    frame_.control.unlimitedCountsY = control.unlimitedCountsY;
    frame_.control.requestedCountsX = control.limitedCountsX;
    frame_.control.requestedCountsY = control.limitedCountsY;
    frame_.control.frameCountLimit = control.frameCountLimit;
    frame_.trajectoryRequest.requestedCountsX = control.limitedCountsX;
    frame_.trajectoryRequest.requestedCountsY = control.limitedCountsY;
    frame_.trajectoryRequest.requestTime = frame_.observation.timing.controlTime;
}
