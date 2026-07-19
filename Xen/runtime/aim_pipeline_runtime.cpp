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

LosEstimate toLosEstimate(const RelativeLosKalmanEstimate& estimate)
{
    LosEstimate result;
    result.valid = estimate.x.valid && estimate.y.valid;
    result.angleX = estimate.x.angleDegrees;
    result.angleY = estimate.y.angleDegrees;
    result.rateX = estimate.x.rateDegreesPerSecond;
    result.rateY = estimate.y.rateDegreesPerSecond;
    result.covarianceX = estimate.x.angleVariance;
    result.covarianceY = estimate.y.angleVariance;
    result.innovationVarianceX = estimate.x.innovationVariance;
    result.innovationVarianceY = estimate.y.innovationVariance;
    result.innovationX = estimate.x.innovationDegrees;
    result.innovationY = estimate.y.innovationDegrees;
    result.nisX = estimate.x.nis;
    result.nisY = estimate.y.nis;
    result.measurementConfidence = estimate.measurementConfidence;
    result.feedforwardConfidence = estimate.feedforwardConfidence;
    return result;
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
    estimator_.reset();
    losAimController_.reset();
    lastControllerUpdateTime_ = {};
    trajectoryShaper_.reset();
    outputScheduler_.reset();
}

void AimPipelineRuntime::suspendOutput()
{
    // 输出暂停不是目标丢失。Shadow必须跨松开区间连续消费观测；Legacy没有
    // 独立估计状态，继续重置空快照可保持旧模式行为和诊断代次语义。
    if (effectiveMode_ == AimPipelineMode::Legacy)
        reset();
}

void AimPipelineRuntime::configureTrajectory(
    const CommandTrajectoryShaper::Settings& shaperSettings,
    const OutputScheduler::Settings& schedulerSettings)
{
    trajectoryShaper_.configure(shaperSettings);
    outputScheduler_.configure(schedulerSettings);
}

void AimPipelineRuntime::configureController(const LosAimController::Settings& settings)
{
    controllerSettings_ = settings;
    losAimController_.reset();
    lastControllerUpdateTime_ = {};
}

void AimPipelineRuntime::configureEstimator(
    const ManeuverLosEstimator::Settings& settings)
{
    estimatorSettings_ = settings;
    estimator_.reset();
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
        estimator_.reset();
        losAimController_.reset();
        lastControllerUpdateTime_ = {};
        trajectoryShaper_.reset();
        outputScheduler_.reset();
        frame_.resetGeneration = resetGeneration_;
    }

    frame_.shadowProcessed = true;
    frame_.observationSequence = ++observationSequence_;
    frame_.observation = observation;
    if (frame_.observation.timing.controlTime.time_since_epoch().count() == 0)
        frame_.observation.timing.controlTime = FrameTiming::Clock::now();
    frame_.estimate = {};
    frame_.baselineEstimate = {};
    frame_.constantAccelerationEstimate = {};
    frame_.maneuverEstimator = {};
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

    ManeuverLosEstimator::Settings sampleSettings = estimatorSettings_;
    sampleSettings.maneuverRateUncertaintyXDegreesPerSecond =
        diagnostics.maneuverRateUncertaintyXDegreesPerSecond;
    sampleSettings.maneuverRateUncertaintyYDegreesPerSecond =
        diagnostics.maneuverRateUncertaintyYDegreesPerSecond;
    estimator_.update(
        diagnostics.stabilizedLosYawDegrees,
        diagnostics.stabilizedLosPitchDownDegrees,
        frame_.observation.confidence,
        frame_.observation.timing.observationTime,
        frame_.observation.timing.controlTime,
        sampleSettings);
    frame_.estimate = toLosEstimate(estimator_.selectedEstimate());
    frame_.baselineEstimate = toLosEstimate(
        estimator_.constantVelocityEstimate());
    frame_.constantAccelerationEstimate = toLosEstimate(
        estimator_.constantAccelerationEstimate());
    const auto& estimatorDiagnostics = estimator_.diagnostics();
    frame_.maneuverEstimator.mode = estimatorSettings_.mode;
    frame_.maneuverEstimator.maneuverModelActive =
        estimatorDiagnostics.maneuverModelActive;
    frame_.maneuverEstimator.selectionChanged =
        estimatorDiagnostics.selectionChanged;
    frame_.maneuverEstimator.selectionCount =
        estimatorDiagnostics.selectionCount;
    frame_.maneuverEstimator.jerkStdDegreesPerSecond3 =
        estimatorSettings_.jerkStdDegreesPerSecond3;
    frame_.maneuverEstimator.maneuverRateThresholdDegreesPerSecond =
        estimatorSettings_.maneuverRateThresholdDegreesPerSecond;
    frame_.maneuverEstimator.maneuverHoldSeconds =
        estimatorSettings_.maneuverHoldSeconds;
    frame_.maneuverEstimator.maneuverHoldRemainingSeconds =
        estimatorDiagnostics.maneuverHoldRemainingSeconds;
    frame_.maneuverEstimator.maneuverRateUncertaintyXDegreesPerSecond =
        sampleSettings.maneuverRateUncertaintyXDegreesPerSecond;
    frame_.maneuverEstimator.maneuverRateUncertaintyYDegreesPerSecond =
        sampleSettings.maneuverRateUncertaintyYDegreesPerSecond;
    frame_.maneuverEstimator.maneuverRateEvidenceDegreesPerSecond =
        estimatorDiagnostics.maneuverRateEvidenceDegreesPerSecond;
    frame_.maneuverEstimator.modelAngleDeltaDegrees =
        estimatorDiagnostics.modelAngleDeltaDegrees;
    frame_.maneuverEstimator.modelRateDeltaDegreesPerSecond =
        estimatorDiagnostics.modelRateDeltaDegreesPerSecond;

    LosAimController::Input controllerInput;
    controllerInput.valid = frame_.estimate.valid;
    controllerInput.errorDegreesX = frame_.estimate.angleX -
        diagnostics.appliedCameraYawAtControlDegrees;
    controllerInput.errorDegreesY = frame_.estimate.angleY -
        diagnostics.appliedCameraPitchAtControlDegrees;
    controllerInput.relativeLosRateDegreesPerSecondX = frame_.estimate.rateX;
    controllerInput.relativeLosRateDegreesPerSecondY = frame_.estimate.rateY;
    controllerInput.feedforwardConfidence = frame_.estimate.feedforwardConfidence *
        std::clamp(diagnostics.machineProfileFeedforwardScale, 0.0, 1.0);
    controllerInput.degreesPerCountX = diagnostics.degreesPerCountX;
    controllerInput.degreesPerCountY = diagnostics.degreesPerCountY;
    controllerInput.dtSeconds = lastControllerUpdateTime_.time_since_epoch().count() == 0
        ? 1.0 / 120.0
        : std::chrono::duration<double>(
            frame_.observation.timing.controlTime - lastControllerUpdateTime_).count();
    LosAimController::Settings effectiveControllerSettings = controllerSettings_;
    if (!diagnostics.machineProfileIntegralEnabled)
        effectiveControllerSettings.integralTimeSeconds = 0.0;
    const LosAimController::Output control =
        losAimController_.update(controllerInput, effectiveControllerSettings);
    lastControllerUpdateTime_ = frame_.observation.timing.controlTime;
    frame_.control.valid = control.valid;
    frame_.control.speedLimited = control.speedLimited;
    frame_.control.integralFrozen = control.integralFrozen;
    frame_.control.settled = control.settled;
    frame_.control.settleReleased = control.settleReleased;
    frame_.control.settleConfirmationSamples = control.settleConfirmationSamples;
    frame_.control.lowSpeedReverseSuppressed = control.lowSpeedReverseSuppressed;
    frame_.control.verticalCatchUpActive = control.verticalCatchUpActive;
    frame_.control.reverseConfirmationSeconds = control.reverseConfirmationSeconds;
    frame_.control.effectiveResponseSecondsY = control.effectiveResponseSecondsY;
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
    frame_.trajectoryRequest.valid = control.valid;
    frame_.trajectoryRequest.sequence = frame_.observationSequence;
    frame_.trajectoryRequest.requestDurationSeconds = std::clamp(
        controllerInput.dtSeconds, 1.0 / 500.0, 0.050);
    frame_.trajectoryRequest.requestTime = frame_.observation.timing.controlTime;

    auto applyTrajectoryResult = [this](const CommandTrajectoryShaper::Result& result)
    {
        frame_.trajectoryState = result.state;
        frame_.trajectoryOutput = result.output;
        // P0-4B仍处于shadow，整形器只能生成可审计候选，不得解除设备命令抑制。
        frame_.trajectoryOutput.commandSuppressed = true;
    };
    if (trajectoryShaper_.settings().mode == TrajectoryShaperMode::Off)
    {
        applyTrajectoryResult(trajectoryShaper_.update(
            frame_.trajectoryRequest,
            frame_.trajectoryRequest.requestDurationSeconds,
            frame_.trajectoryRequest.requestTime));
    }
    else
    {
        outputScheduler_.submit(frame_.trajectoryRequest);
        const auto scheduled = outputScheduler_.service(
            frame_.observation.timing.controlTime, trajectoryShaper_);
        frame_.trajectoryState = trajectoryShaper_.state();
        frame_.trajectoryOutput.mode = trajectoryShaper_.settings().mode;
        frame_.trajectoryOutput.commandSuppressed = true;
        if (scheduled.has_value())
            applyTrajectoryResult(*scheduled);
    }
}
