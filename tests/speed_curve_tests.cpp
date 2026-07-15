#include "runtime/basic_aim_controller.h"
#include "runtime/aim_pipeline_runtime.h"
#include "runtime/applied_view_motion_model.h"
#include "runtime/relative_los_kalman.h"
#include "runtime/los_aim_controller.h"
#include "runtime/command_trajectory_shaper.h"
#include "runtime/output_scheduler.h"
#include "runtime/aim_coordinate_space.h"
#include "runtime/basic_target_filter.h"
#include "runtime/target_predictor.h"
#include "runtime/video_replay_math.h"
#include "runtime/cross_domain_replay.h"
#include "runtime/view_motion_history.h"
#include "debug/pipeline_tracer.h"
#include "detector/detection_buffer.h"
#include "capture/capture.h"
#include "capture/ndi_frame_geometry.h"
#include "capture/network_frame_geometry.h"
#include "runtime/frame_rate_counter.h"
#include "runtime/latest_frame_queue.h"
#include "runtime/passive_profile_calibrator.h"
#include "runtime/build_identity.h"

#include <cmath>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <queue>
#include <string>

namespace
{
int failures = 0;

class TimedFakeCapture final : public IScreenCapture
{
public:
    explicit TimedFakeCapture(FrameTiming::Clock::time_point receiveTime)
        : receiveTime_(receiveTime)
    {
        SetSourceDimensions(1920, 1080);
    }

    cv::Mat GetNextFrameCpu() override
    {
        RecordSourceFrame(60.0, 1920, 1080, receiveTime_);
        return cv::Mat(2, 3, CV_8UC3, cv::Scalar(0, 0, 0));
    }

private:
    FrameTiming::Clock::time_point receiveTime_{};
};

void expectNear(double actual, double expected, double tolerance, const char* name)
{
    if (std::abs(actual - expected) <= tolerance)
        return;

    std::cerr << name << ": expected " << expected << ", got " << actual << '\n';
    ++failures;
}

void expectTrue(bool condition, const char* name)
{
    if (condition)
        return;

    std::cerr << name << ": condition was false\n";
    ++failures;
}
}

int main()
{
    const auto timingBase = FrameTiming::Clock::time_point(std::chrono::seconds(10));
    TimedFakeCapture fakeCapture(timingBase);
    CapturedFrame captured = fakeCapture.GetNextFrameTimed();
    expectTrue(!captured.image.empty() &&
               captured.timing.backendReceiveTime == timingBase &&
               captured.timing.frameSequence == 1,
               "default capture timing follows the successful backend frame");
    expectTrue(captured.timing.sourceWidth == 1920 &&
               captured.timing.sourceHeight == 1080 &&
               captured.timing.roiWidth == 3 && captured.timing.roiHeight == 2,
               "default capture timing preserves source and roi dimensions");

    std::queue<CapturedFrame> timedFrames;
    CapturedFrame olderFrame;
    olderFrame.timing.frameSequence = 10;
    olderFrame.timing.backendReceiveTime = timingBase;
    CapturedFrame newerFrame;
    newerFrame.timing.frameSequence = 11;
    newerFrame.timing.backendReceiveTime = timingBase + std::chrono::milliseconds(4);
    expectTrue(ReplaceWithLatestFrame(timedFrames, std::move(olderFrame)) == 0,
               "timed latest-frame queue accepts the first frame");
    expectTrue(ReplaceWithLatestFrame(timedFrames, std::move(newerFrame)) == 1,
               "timed latest-frame queue replaces the unconsumed frame");
    CapturedFrame latestTimedFrame;
    uint64_t skippedTimedFrames = 0;
    expectTrue(TakeLatestFrame(timedFrames, latestTimedFrame, skippedTimedFrames) &&
               latestTimedFrame.timing.frameSequence == 11 &&
               latestTimedFrame.timing.backendReceiveTime == timingBase + std::chrono::milliseconds(4),
               "latest-frame replacement moves image timing with the selected frame");

    FrameTiming detectionTiming;
    detectionTiming.backendReceiveTime = timingBase;
    detectionTiming.captureSubmitTime = timingBase + std::chrono::milliseconds(1);
    detectionTiming.inferenceStartTime = timingBase + std::chrono::milliseconds(2);
    detectionTiming.frameSequence = 17;
    DetectionBuffer localDetectionBuffer;
    localDetectionBuffer.set(
        { cv::Rect(1, 2, 3, 4) }, { 5 }, { 0.75f }, detectionTiming);
    std::vector<cv::Rect> timingBoxes;
    std::vector<int> timingClasses;
    std::vector<float> timingConfidences;
    int timingVersion = 0;
    FrameTiming roundTripTiming;
    {
        std::lock_guard<std::mutex> lock(localDetectionBuffer.mutex);
        localDetectionBuffer.swapLocked(
            timingBoxes, timingClasses, timingConfidences, timingVersion, roundTripTiming);
    }
    expectTrue(timingVersion == 1 && roundTripTiming.frameSequence == 17 &&
               roundTripTiming.backendReceiveTime == timingBase &&
               roundTripTiming.captureSubmitTime == timingBase + std::chrono::milliseconds(1) &&
               roundTripTiming.inferenceStartTime == timingBase + std::chrono::milliseconds(2) &&
               roundTripTiming.inferencePublishTime >= roundTripTiming.inferenceStartTime,
               "detection buffer round-trips the complete frame timing contract");
    FrameTiming anomalousTiming = roundTripTiming;
    anomalousTiming.inferencePublishTime = timingBase + std::chrono::milliseconds(5);
    anomalousTiming.controlTime = timingBase + std::chrono::milliseconds(4);
    expectTrue(frameTimingComplete(anomalousTiming) && !frameTimingOrdered(anomalousTiming),
               "complete but out-of-order timing is marked anomalous");
    const auto signedAnomalyMs = frameSignedDurationMs(
        anomalousTiming.inferencePublishTime, anomalousTiming.controlTime);
    expectTrue(signedAnomalyMs.has_value() && *signedAnomalyMs < 0.0,
               "timing anomalies preserve signed negative duration instead of clipping to zero");

    AimObservation defaultObservation;
    expectTrue(!defaultObservation.valid && defaultObservation.trackId == -1,
               "new pipeline observation defaults are invalid and untracked");
    expectNear(defaultObservation.pivotX, 0.0, 0.0,
               "new pipeline observation default pivot is zero");

    AimPipelineRuntime newPipeline;
    expectTrue(newPipeline.requestedMode() == AimPipelineMode::Legacy &&
               newPipeline.effectiveMode() == AimPipelineMode::Legacy,
               "new pipeline defaults to legacy mode");
    const AimPipelineFrameState legacyFrame = newPipeline.observe(AimObservation{});
    expectTrue(!legacyFrame.shadowProcessed && legacyFrame.observationSequence == 0,
               "legacy mode does not advance shadow observations");

    AimObservation shadowObservation;
    shadowObservation.valid = true;
    shadowObservation.trackId = 7;
    shadowObservation.classId = 1;
    shadowObservation.pivotX = 123.0;
    shadowObservation.pivotY = 77.0;
    shadowObservation.confidence = 0.9f;
    shadowObservation.timing.observationTime = timingBase + std::chrono::milliseconds(20);
    shadowObservation.timing.controlTime = timingBase + std::chrono::milliseconds(30);
    newPipeline.configure("shadow");
    const AimPipelineFrameState shadowFrame = newPipeline.observe(shadowObservation);
    expectTrue(shadowFrame.shadowProcessed && shadowFrame.commandSuppressed,
               "shadow pipeline processes diagnostics and suppresses commands");
    expectTrue(shadowFrame.trajectoryOutput.outputCountsX == 0 &&
               shadowFrame.trajectoryOutput.outputCountsY == 0 &&
               shadowFrame.trajectoryOutput.commandSuppressed,
               "shadow pipeline never produces a queued device command");
    ViewMotionShadowDiagnostics shadowDiagnostics;
    shadowDiagnostics.valid = true;
    shadowDiagnostics.stabilizedLosYawDegrees = 2.0;
    shadowDiagnostics.stabilizedLosPitchDownDegrees = -1.0;
    shadowDiagnostics.degreesPerCountX = 0.02;
    shadowDiagnostics.degreesPerCountY = 0.02;
    newPipeline.setViewMotionDiagnostics(shadowDiagnostics);
    const AimPipelineFrameState kalmanFrame = newPipeline.snapshot();
    expectTrue(kalmanFrame.estimate.valid &&
               kalmanFrame.estimate.measurementConfidence > 0.89 &&
               kalmanFrame.estimate.feedforwardConfidence > 0.0,
               "shadow pipeline updates relative LOS Kalman with detection confidence");
    expectNear(kalmanFrame.estimate.angleX, 2.0, 1e-9,
               "first relative LOS Kalman observation anchors the yaw angle");
    expectNear(kalmanFrame.estimate.angleY, -1.0, 1e-9,
               "first relative LOS Kalman observation anchors the pitch angle");
    expectTrue(kalmanFrame.control.valid &&
               kalmanFrame.control.feedbackX > 0.0 &&
               kalmanFrame.trajectoryRequest.requestedCountsX > 0.0 &&
               kalmanFrame.commandSuppressed,
               "P0-4A produces an auditable P-only request without enabling device output");
    expectTrue(kalmanFrame.trajectoryOutput.outputProduced &&
               kalmanFrame.trajectoryOutput.commandSuppressed,
               "P0-4B pass-through produces diagnostics without enabling device output");
    expectNear(kalmanFrame.trajectoryOutput.shapedCountsX,
               kalmanFrame.trajectoryRequest.requestedCountsX, 1e-12,
               "P0-4B off mode is exactly equal to the controller request");
    expectTrue(shadowFrame.observation.trackId == 7 &&
               shadowFrame.observationSequence == 1,
               "shadow pipeline preserves target identity and observation sequence");

    const uint64_t generationBeforeSwitch = newPipeline.resetGeneration();
    newPipeline.reset();
    const AimPipelineFrameState resetFrame = newPipeline.snapshot();
    expectTrue(resetFrame.resetGeneration == generationBeforeSwitch + 1 &&
               resetFrame.observationSequence == 0 && !resetFrame.observation.valid,
               "target loss reset clears the independent shadow state");
    const AimPipelineFrameState switchedTargetFrame = newPipeline.observe(shadowObservation);
    expectTrue(switchedTargetFrame.observationSequence == 1,
               "target id switch restarts the shadow observation sequence");

    newPipeline.configure("active");
    const AimPipelineFrameState deferredActiveFrame = newPipeline.observe(shadowObservation);
    expectTrue(deferredActiveFrame.requestedMode == AimPipelineMode::Active &&
               deferredActiveFrame.effectiveMode == AimPipelineMode::Shadow &&
               !deferredActiveFrame.activeAvailable && deferredActiveFrame.commandSuppressed,
               "P0 active request safely degrades to command-suppressed shadow mode");
    newPipeline.configure("invalid-mode");
    expectTrue(newPipeline.effectiveMode() == AimPipelineMode::Legacy,
               "invalid new pipeline mode falls back to legacy");

    RelativeLosKalman constantVelocityKalman;
    const auto kalmanStart = timingBase + std::chrono::seconds(2);
    for (int sample = 0; sample <= 30; ++sample)
    {
        const auto sampleTime = kalmanStart + std::chrono::milliseconds(sample * 10);
        constantVelocityKalman.update(
            sample * 0.05, sample * -0.02, 1.0f, sampleTime, sampleTime);
    }
    const RelativeLosKalmanEstimate constantVelocityEstimate =
        constantVelocityKalman.estimate();
    expectNear(constantVelocityEstimate.x.rateDegreesPerSecond, 5.0, 0.35,
               "relative LOS Kalman converges to horizontal constant angular rate");
    expectNear(constantVelocityEstimate.y.rateDegreesPerSecond, -2.0, 0.35,
               "relative LOS Kalman converges to vertical constant angular rate");
    expectTrue(constantVelocityEstimate.x.angleVariance > 0.0 &&
               constantVelocityEstimate.x.innovationVariance > 0.0,
               "relative LOS Kalman exposes positive covariance diagnostics");

    RelativeLosKalman highConfidenceKalman;
    RelativeLosKalman lowConfidenceKalman;
    highConfidenceKalman.update(0.0, 0.0, 1.0f, kalmanStart, kalmanStart);
    lowConfidenceKalman.update(0.0, 0.0, 1.0f, kalmanStart, kalmanStart);
    const auto confidenceTime = kalmanStart + std::chrono::milliseconds(16);
    highConfidenceKalman.update(1.0, 0.0, 1.0f, confidenceTime, confidenceTime);
    lowConfidenceKalman.update(1.0, 0.0, 0.2f, confidenceTime, confidenceTime);
    expectTrue(lowConfidenceKalman.estimate().x.innovationVariance >
                   highConfidenceKalman.estimate().x.innovationVariance &&
               lowConfidenceKalman.estimate().measurementConfidence <
                   highConfidenceKalman.estimate().measurementConfidence,
               "lower detection confidence increases Kalman measurement uncertainty");
    expectTrue(lowConfidenceKalman.estimate().feedforwardConfidence <
                   highConfidenceKalman.estimate().feedforwardConfidence,
               "lower detection confidence reduces feedforward confidence");

    LosAimController losController;
    LosAimController::Input losInput;
    losInput.valid = true;
    losInput.errorDegreesX = 2.0;
    losInput.errorDegreesY = -1.0;
    losInput.relativeLosRateDegreesPerSecondX = 2.0;
    losInput.relativeLosRateDegreesPerSecondY = -1.0;
    losInput.feedforwardConfidence = 1.0;
    losInput.degreesPerCountX = 0.02;
    losInput.degreesPerCountY = 0.02;
    losInput.dtSeconds = 0.010;
    LosAimController::Settings losSettings;
    losSettings.responseSeconds = 0.100;
    losSettings.maxCountsPerSecond = 10000.0;
    const auto pOnlyOutput = losController.update(losInput, losSettings);
    expectNear(pOnlyOutput.feedbackCountsX,
               100.0 * (1.0 - std::exp(-0.1)), 1e-12,
               "angle controller P feedback uses frame-rate-independent response");
    expectNear(pOnlyOutput.trackingFeedforwardCountsX, 0.0, 0.0,
               "P-only baseline does not hide velocity feedforward");
    expectNear(pOnlyOutput.leadReferenceDegreesX, 0.0, 0.0,
               "zero lead horizon does not create artificial lead");

    LosAimController settleController;
    LosAimController::Settings settleSettings;
    settleSettings.responseSeconds = 0.100;
    settleSettings.maxCountsPerSecond = 10000.0;
    settleSettings.settleErrorDegrees = 0.080;
    settleSettings.settleRateDegreesPerSecond = 0.800;
    LosAimController::Input quietInput = losInput;
    quietInput.errorDegreesX = 0.050;
    quietInput.errorDegreesY = 0.0;
    quietInput.relativeLosRateDegreesPerSecondX = 0.40;
    quietInput.relativeLosRateDegreesPerSecondY = 0.0;
    const auto firstQuiet = settleController.update(quietInput, settleSettings);
    const auto confirmedQuiet = settleController.update(quietInput, settleSettings);
    expectTrue(!firstQuiet.settled && firstQuiet.settleConfirmationSamples == 1 &&
               confirmedQuiet.settled && confirmedQuiet.settleConfirmationSamples == 2,
               "angle settle requires two consecutive quiet observations");
    expectNear(confirmedQuiet.limitedCountsX, 0.0, 0.0,
               "settled angle controller suppresses sub-count static pulses");
    quietInput.errorDegreesX = 0.10;
    quietInput.relativeLosRateDegreesPerSecondX = 1.0;
    const auto insideHysteresis = settleController.update(quietInput, settleSettings);
    expectTrue(insideHysteresis.settled && !insideHysteresis.settleReleased,
               "angle settle remains latched inside the fixed 1.5x release band");
    quietInput.relativeLosRateDegreesPerSecondX = 1.21;
    const auto movingRelease = settleController.update(quietInput, settleSettings);
    expectTrue(!movingRelease.settled && movingRelease.settleReleased &&
               movingRelease.limitedCountsX > 0.0,
               "real LOS motion beyond the release rate resumes feedback in the same frame");
    settleController.reset();
    const auto quietAfterReset = settleController.update(quietInput, settleSettings);
    expectTrue(!quietAfterReset.settled && quietAfterReset.settleConfirmationSamples == 0,
               "controller reset clears the settle latch and confirmation history");

    LosAimController reverseConfirmController;
    LosAimController::Settings reverseConfirmSettings = settleSettings;
    reverseConfirmSettings.reverseConfirmationSeconds = 0.080;
    LosAimController::Input reverseConfirmInput = losInput;
    reverseConfirmInput.errorDegreesX = 0.10;
    reverseConfirmInput.errorDegreesY = 0.0;
    reverseConfirmInput.relativeLosRateDegreesPerSecondX = 0.0;
    reverseConfirmInput.relativeLosRateDegreesPerSecondY = 0.0;
    reverseConfirmController.update(reverseConfirmInput, reverseConfirmSettings);
    reverseConfirmInput.errorDegreesX = -0.10;
    for (int sample = 0; sample < 7; ++sample)
    {
        const auto suppressedReverse = reverseConfirmController.update(
            reverseConfirmInput, reverseConfirmSettings);
        expectTrue(suppressedReverse.lowSpeedReverseSuppressed &&
                   suppressedReverse.limitedCountsX == 0.0,
                   "low-speed opposite request remains suppressed before eighty milliseconds");
    }
    const auto confirmedReverse = reverseConfirmController.update(
        reverseConfirmInput, reverseConfirmSettings);
    expectTrue(!confirmedReverse.lowSpeedReverseSuppressed &&
               confirmedReverse.limitedCountsX < 0.0,
               "persistent low-speed reverse is released after eighty milliseconds");

    LosAimController outsideSettleBandController;
    reverseConfirmInput.errorDegreesX = 0.20;
    outsideSettleBandController.update(reverseConfirmInput, reverseConfirmSettings);
    reverseConfirmInput.errorDegreesX = -0.20;
    const auto outsideSettleBandReverse = outsideSettleBandController.update(
        reverseConfirmInput, reverseConfirmSettings);
    expectTrue(!outsideSettleBandReverse.lowSpeedReverseSuppressed &&
               outsideSettleBandReverse.limitedCountsX < 0.0,
               "reverse outside the settle hysteresis band bypasses confirmation immediately");

    LosAimController fastReverseController;
    fastReverseController.update(reverseConfirmInput, reverseConfirmSettings);
    reverseConfirmInput.errorDegreesX = 0.20;
    reverseConfirmInput.relativeLosRateDegreesPerSecondX = 3.0;
    const auto fastReverse = fastReverseController.update(
        reverseConfirmInput, reverseConfirmSettings);
    expectTrue(!fastReverse.lowSpeedReverseSuppressed && fastReverse.limitedCountsX > 0.0,
               "reliable fast LOS reversal bypasses low-speed confirmation immediately");

    LosAimController angularVerticalCatchUpController;
    LosAimController::Settings angularVerticalCatchUpSettings = settleSettings;
    angularVerticalCatchUpSettings.verticalCatchUpErrorDegrees = 0.8;
    LosAimController::Input angularVerticalCatchUpInput = losInput;
    angularVerticalCatchUpInput.errorDegreesX = 4.0;
    angularVerticalCatchUpInput.errorDegreesY = 1.0;
    angularVerticalCatchUpInput.relativeLosRateDegreesPerSecondX = 6.0;
    const auto angularVerticalCatchUp = angularVerticalCatchUpController.update(
        angularVerticalCatchUpInput, angularVerticalCatchUpSettings);
    expectTrue(angularVerticalCatchUp.verticalCatchUpActive &&
               angularVerticalCatchUp.effectiveResponseSecondsY <
                   angularVerticalCatchUpSettings.responseSeconds,
               "sustained cross-axis error receives continuous vertical catch-up");
    angularVerticalCatchUpController.reset();
    angularVerticalCatchUpInput.relativeLosRateDegreesPerSecondX = 4.0;
    const auto lowHorizontalRate = angularVerticalCatchUpController.update(
        angularVerticalCatchUpInput, angularVerticalCatchUpSettings);
    expectTrue(!lowHorizontalRate.verticalCatchUpActive &&
               lowHorizontalRate.effectiveResponseSecondsY ==
                   angularVerticalCatchUpSettings.responseSeconds,
               "low horizontal LOS rate cannot activate vertical catch-up");
    angularVerticalCatchUpController.reset();
    angularVerticalCatchUpInput.relativeLosRateDegreesPerSecondX = 6.0;
    angularVerticalCatchUpInput.errorDegreesX = 10.0;
    const auto insignificantVerticalError = angularVerticalCatchUpController.update(
        angularVerticalCatchUpInput, angularVerticalCatchUpSettings);
    expectTrue(!insignificantVerticalError.verticalCatchUpActive,
               "minor vertical residual cannot steal budget from unidirectional tracking");

    losSettings.feedforwardGain = 1.0;
    losSettings.leadHorizonSeconds = 0.050;
    losSettings.leadStrength = 1.0;
    const auto separatedOutput = losController.update(losInput, losSettings);
    expectNear(separatedOutput.trackingFeedforwardCountsX, 1.0, 1e-12,
               "constant angular velocity feedforward requests one-period camera motion");
    expectNear(separatedOutput.leadReferenceDegreesX, 0.1, 1e-12,
               "experience lead remains an independent angular reference");
    LosAimController::Input lowTrustInput = losInput;
    lowTrustInput.feedforwardConfidence = 0.0;
    const auto lowTrustOutput = losController.update(lowTrustInput, losSettings);
    expectNear(lowTrustOutput.feedbackCountsX, separatedOutput.feedbackCountsX, 1e-12,
               "low trust preserves corrective feedback");
    expectNear(lowTrustOutput.trackingFeedforwardCountsX, 0.0, 0.0,
               "low trust withdraws velocity feedforward immediately");
    expectNear(lowTrustOutput.leadReferenceDegreesX, 0.0, 0.0,
               "low trust withdraws experience lead immediately");

    LosAimController limitedLosController;
    LosAimController::Settings limitedLosSettings = losSettings;
    limitedLosSettings.feedforwardGain = 0.0;
    limitedLosSettings.leadHorizonSeconds = 0.0;
    limitedLosSettings.leadStrength = 0.0;
    limitedLosSettings.maxCountsPerSecond = 100.0;
    LosAimController::Input diagonalInput = losInput;
    diagonalInput.errorDegreesX = 0.6;
    diagonalInput.errorDegreesY = 0.8;
    const auto diagonalLimited = limitedLosController.update(
        diagonalInput, limitedLosSettings);
    expectTrue(diagonalLimited.speedLimited,
               "angle controller reports two-axis saturation");
    expectNear(std::hypot(diagonalLimited.limitedCountsX,
                          diagonalLimited.limitedCountsY),
               1.0, 1e-12,
               "two-axis limiter preserves direction within the period budget");

    LosAimController integralLosController;
    LosAimController::Settings integralLosSettings;
    integralLosSettings.responseSeconds = 0.100;
    integralLosSettings.maxCountsPerSecond = 10000.0;
    integralLosSettings.integralTimeSeconds = 0.200;
    integralLosSettings.integralZoneDegrees = 1.0;
    LosAimController::Input integralInput = losInput;
    integralInput.errorDegreesX = 0.1;
    integralInput.errorDegreesY = 0.0;
    integralInput.relativeLosRateDegreesPerSecondX = 0.0;
    integralInput.relativeLosRateDegreesPerSecondY = 0.0;
    integralInput.degreesPerCountX = 0.01;
    integralInput.degreesPerCountY = 0.01;
    double previousIntegralCounts = 0.0;
    for (int sample = 0; sample < 10; ++sample)
    {
        const auto integralOutput = integralLosController.update(
            integralInput, integralLosSettings);
        expectTrue(integralOutput.integralCountsX >= previousIntegralCounts,
                   "in-zone angle integral grows monotonically on a constant error");
        previousIntegralCounts = integralOutput.integralCountsX;
    }
    integralInput.errorDegreesX = -0.1;
    const auto reversedIntegral = integralLosController.update(
        integralInput, integralLosSettings);
    expectNear(reversedIntegral.integralCountsX, 0.0, 1e-12,
               "opposite error unwinds the previous integral in one period");
    integralInput.errorDegreesX = 2.0;
    const auto outsideZoneIntegral = integralLosController.update(
        integralInput, integralLosSettings);
    expectNear(outsideZoneIntegral.integralCountsX, 0.0, 1e-12,
               "I-zone excludes large step errors from integration");

    LosAimController saturatedIntegralController;
    LosAimController::Settings saturatedIntegralSettings = integralLosSettings;
    saturatedIntegralSettings.maxCountsPerSecond = 1.0;
    integralInput.errorDegreesX = 0.1;
    const auto saturatedIntegral = saturatedIntegralController.update(
        integralInput, saturatedIntegralSettings);
    expectTrue(saturatedIntegral.speedLimited && saturatedIntegral.integralFrozen,
               "saturation freezes the angle integral candidate");

    expectTrue(parseTrajectoryShaperMode("trapezoid") == TrajectoryShaperMode::Trapezoid &&
               parseTrajectoryShaperMode("ruckig") == TrajectoryShaperMode::Off,
               "unsupported trajectory modes fail safely to pass-through off mode");

    CommandTrajectoryShaper passThroughShaper;
    CommandTrajectoryShaper::Settings passThroughSettings;
    passThroughSettings.mode = TrajectoryShaperMode::Off;
    passThroughShaper.configure(passThroughSettings);
    TrajectoryRequest fractionalRequest;
    fractionalRequest.valid = true;
    fractionalRequest.sequence = 1;
    fractionalRequest.requestedCountsX = 0.25;
    fractionalRequest.requestedCountsY = -0.25;
    fractionalRequest.requestDurationSeconds = 0.010;
    fractionalRequest.requestTime = timingBase;
    int quantizedTotalX = 0;
    int quantizedTotalY = 0;
    CommandTrajectoryShaper::Result fractionalResult;
    for (int tick = 0; tick < 4; ++tick)
    {
        fractionalRequest.sequence = static_cast<uint64_t>(tick + 1);
        fractionalResult = passThroughShaper.update(
            fractionalRequest, 0.010,
            timingBase + std::chrono::milliseconds(tick * 10));
        expectNear(fractionalResult.output.shapedCountsX, 0.25, 0.0,
                   "pass-through preserves every floating-point request");
        quantizedTotalX += fractionalResult.output.outputCountsX;
        quantizedTotalY += fractionalResult.output.outputCountsY;
    }
    expectNear(quantizedTotalX, 1.0, 0.0,
               "positive integer quantization remainder is conserved long term");
    expectNear(quantizedTotalY, -1.0, 0.0,
               "negative integer quantization remainder is conserved long term");
    expectNear(fractionalResult.output.quantizationRemainderX, 0.0, 1e-12,
               "pass-through returns to zero remainder after an exact count");

    CommandTrajectoryShaper constrainedShaper;
    CommandTrajectoryShaper::Settings constrainedSettings;
    constrainedSettings.mode = TrajectoryShaperMode::Trapezoid;
    constrainedSettings.maxVelocityCountsPerSecond = 100.0;
    constrainedSettings.maxAccelerationCountsPerSecond2 = 200.0;
    constrainedSettings.maxJerkCountsPerSecond3 = 1000.0;
    constrainedShaper.configure(constrainedSettings);
    TrajectoryRequest positiveVelocityRequest;
    positiveVelocityRequest.valid = true;
    positiveVelocityRequest.sequence = 10;
    positiveVelocityRequest.requestedCountsX = 10.0;
    positiveVelocityRequest.requestDurationSeconds = 0.010;
    positiveVelocityRequest.requestTime = timingBase;
    CommandTrajectoryShaper::Result constrainedResult;
    double previousConstrainedVelocityX = 0.0;
    double previousConstrainedAccelerationX = 0.0;
    for (int tick = 0; tick < 200; ++tick)
    {
        constrainedResult = constrainedShaper.update(
            positiveVelocityRequest, 0.010,
            timingBase + std::chrono::milliseconds(tick * 10));
        expectTrue(std::hypot(constrainedResult.state.velocityCountsPerSecX,
                              constrainedResult.state.velocityCountsPerSecY) <= 100.0 + 1e-9,
                   "trapezoid speed stays inside the two-axis vector limit");
        expectTrue(std::hypot(constrainedResult.state.accelerationCountsPerSec2X,
                              constrainedResult.state.accelerationCountsPerSec2Y) <= 200.0 + 1e-9,
                   "trapezoid acceleration stays inside the two-axis vector limit");
        expectTrue(std::hypot(constrainedResult.state.jerkCountsPerSec3X,
                              constrainedResult.state.jerkCountsPerSec3Y) <= 1000.0 + 1e-9,
                   "trapezoid jerk stays inside the two-axis vector limit");
        expectNear(constrainedResult.state.accelerationCountsPerSec2X,
                   (constrainedResult.state.velocityCountsPerSecX -
                    previousConstrainedVelocityX) / 0.010,
                   1e-9,
                   "stored acceleration matches the actual shaped velocity derivative");
        expectNear(constrainedResult.state.jerkCountsPerSec3X,
                   (constrainedResult.state.accelerationCountsPerSec2X -
                    previousConstrainedAccelerationX) / 0.010,
                   1e-9,
                   "stored jerk matches the actual shaped acceleration derivative");
        previousConstrainedVelocityX = constrainedResult.state.velocityCountsPerSecX;
        previousConstrainedAccelerationX =
            constrainedResult.state.accelerationCountsPerSec2X;
    }
    expectTrue(constrainedResult.output.velocityLimited &&
               constrainedResult.output.requestSequence == 10,
               "trapezoid exposes target speed limiting and source request identity");
    TrajectoryRequest reverseVelocityRequest = positiveVelocityRequest;
    reverseVelocityRequest.sequence = 11;
    reverseVelocityRequest.requestedCountsX = -1.0;
    for (int tick = 200; tick < 500; ++tick)
    {
        constrainedResult = constrainedShaper.update(
            reverseVelocityRequest, 0.010,
            timingBase + std::chrono::milliseconds(tick * 10));
    }
    expectTrue(constrainedResult.state.velocityCountsPerSecX < 0.0,
               "online replanning reaches a reversed target without replaying an old path");
    const auto emergencyTrajectory = constrainedShaper.emergencyReset(
        timingBase + std::chrono::seconds(6));
    expectTrue(emergencyTrajectory.output.emergencyReset &&
               emergencyTrajectory.output.outputCountsX == 0 &&
               emergencyTrajectory.state.velocityCountsPerSecX == 0.0 &&
               emergencyTrajectory.state.accelerationCountsPerSec2X == 0.0,
               "target loss emergency reset clears velocity and acceleration in one cycle");

    CommandTrajectoryShaper scheduledShaper;
    scheduledShaper.configure(constrainedSettings);
    OutputScheduler scheduler;
    OutputScheduler::Settings schedulerSettings;
    schedulerSettings.outputHz = 100.0;
    scheduler.configure(schedulerSettings);
    TrajectoryRequest scheduledRequest = positiveVelocityRequest;
    scheduledRequest.sequence = 20;
    scheduledRequest.requestTime = timingBase;
    scheduler.submit(scheduledRequest);
    const auto firstScheduled = scheduler.service(timingBase, scheduledShaper);
    expectTrue(firstScheduled.has_value() &&
               firstScheduled->output.requestSequence == 20,
               "fixed scheduler emits the first latest request at its initial lattice point");
    expectTrue(!scheduler.service(
                   timingBase + std::chrono::milliseconds(5), scheduledShaper).has_value(),
               "fixed scheduler emits at most one command per output period");
    TrajectoryRequest newerScheduledRequest = scheduledRequest;
    newerScheduledRequest.sequence = 21;
    newerScheduledRequest.requestedCountsX = -1.0;
    newerScheduledRequest.requestTime = timingBase + std::chrono::milliseconds(6);
    scheduler.submit(newerScheduledRequest);
    const auto lateScheduled = scheduler.service(
        timingBase + std::chrono::milliseconds(25), scheduledShaper);
    expectTrue(lateScheduled.has_value() &&
               lateScheduled->output.requestSequence == 21 &&
               lateScheduled->output.schedulerSkippedTicks == 1,
               "late scheduler service skips stale ticks and uses only the latest request");
    expectNear(lateScheduled->output.schedulerLatenessMs, 5.0, 1e-6,
               "scheduler reports actual service lateness from the selected lattice tick");
    expectNear(lateScheduled->output.shapingDelayMs, 19.0, 1e-6,
               "shaping delay includes scheduler lateness instead of using planned tick time");
    OutputScheduler arrivalOrderedScheduler;
    arrivalOrderedScheduler.configure(schedulerSettings);
    CommandTrajectoryShaper arrivalOrderedShaper;
    arrivalOrderedShaper.configure(constrainedSettings);
    arrivalOrderedScheduler.submit(scheduledRequest);
    expectTrue(arrivalOrderedScheduler.service(
                   timingBase, arrivalOrderedShaper).has_value(),
               "arrival-ordered scheduler establishes its initial lattice");
    TrajectoryRequest afterTickRequest = newerScheduledRequest;
    afterTickRequest.sequence = 22;
    afterTickRequest.requestTime = timingBase + std::chrono::milliseconds(16);
    arrivalOrderedScheduler.submit(afterTickRequest);
    expectTrue(!arrivalOrderedScheduler.service(
                   timingBase + std::chrono::milliseconds(17),
                   arrivalOrderedShaper).has_value(),
               "a new request is never backfilled into a tick before its arrival");
    const auto firstTickAfterArrival = arrivalOrderedScheduler.service(
        timingBase + std::chrono::milliseconds(20), arrivalOrderedShaper);
    expectTrue(firstTickAfterArrival.has_value() &&
               firstTickAfterArrival->output.requestSequence == 22 &&
               firstTickAfterArrival->output.schedulerSkippedTicks == 1,
               "new request starts at the first fixed lattice point after arrival");

    FrameRateCounter rateCounter;
    const auto rateStart = FrameRateCounter::TimePoint(std::chrono::milliseconds(1000));
    rateCounter.reset(rateStart);
    for (int frame = 1; frame <= 240; ++frame)
    {
        rateCounter.addFrame(rateStart + std::chrono::microseconds(frame * 1000000 / 240));
    }
    expectNear(rateCounter.value(rateStart + std::chrono::seconds(1)), 240.0, 0.0,
               "frame rate counter measures real event cadence");
    expectNear(rateCounter.value(rateStart + std::chrono::seconds(4)), 0.0, 0.0,
               "frame rate counter expires stale rate");

    std::queue<int> latestFrames;
    expectTrue(ReplaceWithLatestFrame(latestFrames, 1) == 0,
               "latest frame queue accepts first frame");
    expectTrue(ReplaceWithLatestFrame(latestFrames, 2) == 1,
               "latest frame queue counts superseded frame");
    int latestValue = 0;
    uint64_t skippedFrames = 0;
    expectTrue(TakeLatestFrame(latestFrames, latestValue, skippedFrames),
               "latest frame queue returns pending frame");
    expectNear(latestValue, 2.0, 0.0, "latest frame queue returns newest value");
    expectTrue(skippedFrames == 0, "latest frame queue has no hidden backlog");
    latestFrames.push(3);
    latestFrames.push(4);
    latestFrames.push(5);
    expectTrue(TakeLatestFrame(latestFrames, latestValue, skippedFrames),
               "latest frame queue drains defensive backlog");
    expectNear(latestValue, 5.0, 0.0, "latest frame queue selects newest backlog value");
    expectTrue(skippedFrames == 2, "latest frame queue reports defensive backlog");

    const auto ndiMetadataGeometry = ResolveNdiFrameGeometry(
        320, 320, "<xen source_width=\"2560\" source_height=\"1440\" roi_x=\"1120\" roi_y=\"560\"/>",
        1920, 1080);
    expectTrue(ndiMetadataGeometry.fromMetadata, "ndi metadata has priority");
    expectNear(ndiMetadataGeometry.sourceWidth, 2560.0, 0.0, "ndi metadata source width");
    expectNear(ndiMetadataGeometry.sourceHeight, 1440.0, 0.0, "ndi metadata source height");

    const auto ndiConfigGeometry = ResolveNdiFrameGeometry(
        320, 320, "<ndi source_width=\"9999\" source_height=\"9999\"/>", 2560, 1440);
    expectTrue(ndiConfigGeometry.fromConfig, "ndi config fallback for obs roi");
    expectNear(ndiConfigGeometry.sourceWidth, 2560.0, 0.0, "ndi config source width");

    const auto ndiSafeGeometry = ResolveNdiFrameGeometry(
        320, 320, "<xen source_width=\"100\" source_height=\"100\"/>", 0, 0);
    expectTrue(!ndiSafeGeometry.fromMetadata && !ndiSafeGeometry.fromConfig,
               "ndi rejects invalid source geometry");
    expectNear(ndiSafeGeometry.sourceWidth, 320.0, 0.0, "ndi falls back to encoded width");

    const auto udpConfiguredGeometry = ResolveConfiguredNetworkFrameGeometry(
        320, 320, 2560, 1440);
    expectTrue(udpConfiguredGeometry.fromConfig, "udp roi uses configured full fov geometry");
    expectNear(udpConfiguredGeometry.sourceWidth, 2560.0, 0.0,
               "udp roi reports full fov width");
    expectNear(udpConfiguredGeometry.sourceHeight, 1440.0, 0.0,
               "udp roi reports full fov height");
    const auto udpInvalidGeometry = ResolveConfiguredNetworkFrameGeometry(
        320, 320, 2560, 100);
    expectTrue(!udpInvalidGeometry.fromConfig, "udp rejects undersized source geometry");
    expectNear(udpInvalidGeometry.sourceWidth, 320.0, 0.0,
               "udp invalid geometry falls back to encoded width");

    const double sourceSpan = AimCoordinateSpace::resolveFovPixelSpan(2560, 320.0);
    const double fallbackSpan = AimCoordinateSpace::resolveFovPixelSpan(0, 320.0);
    expectNear(sourceSpan, 2560.0, 0.0, "aim conversion uses capture source width");
    expectNear(fallbackSpan, 320.0, 0.0, "aim conversion falls back to detection width");
    const double sourceCountsPerPixel = AimCoordinateSpace::countsPerSourcePixel(
        106.0, sourceSpan, 1.4 * 0.022);
    const double wrongCropCountsPerPixel = AimCoordinateSpace::countsPerSourcePixel(
        106.0, 320.0, 1.4 * 0.022);
    expectNear(sourceCountsPerPixel, 1.92862318033702, 1e-12,
               "aim conversion uses perspective projection at the source center");
    expectTrue(wrongCropCountsPerPixel / sourceCountsPerPixel > 7.99,
               "center crop must not multiply counts per pixel");
    const double projectedAngle = AimCoordinateSpace::angleDegreesForSourcePixelDelta(
        160.0, 106.0, sourceSpan);
    expectNear(AimCoordinateSpace::sourcePixelDeltaForAngleDegrees(
        projectedAngle, 106.0, sourceSpan), 160.0, 1e-9,
        "perspective pixel and angle conversions round trip exactly");
    const auto losAngles = AimCoordinateSpace::pixelOffsetToLosAngles(
        160.0, -90.0, 106.0, 74.0, 2560.0, 1440.0);
    const auto losPixels = AimCoordinateSpace::losAnglesToPixelOffset(
        losAngles.yawDegrees, losAngles.pitchDownDegrees,
        106.0, 74.0, 2560.0, 1440.0);
    expectNear(losPixels.x, 160.0, 1e-9,
               "yaw angle conversion round trips source pixel offset");
    expectNear(losPixels.y, -90.0, 1e-9,
               "pitch angle conversion round trips source pixel offset");

    AppliedViewMotionModel appliedViewModel;
    appliedViewModel.configure(50.0);
    const auto commandStart = timingBase + std::chrono::seconds(1);
    appliedViewModel.addCommand(10, -4, 0.03, 0.02, commandStart);
    expectNear(appliedViewModel.at(commandStart + std::chrono::milliseconds(49)).first,
               0.0, 1e-12, "delayed view command is not applied before response delay");
    expectNear(appliedViewModel.at(commandStart + std::chrono::milliseconds(50)).first,
               0.30, 1e-12, "delayed view command applies horizontal angle at delay");
    expectNear(appliedViewModel.at(commandStart + std::chrono::milliseconds(50)).second,
               -0.08, 1e-12, "delayed view command applies vertical angle at delay");
    appliedViewModel.addCommand(-2, 1, 0.03, 0.02,
        commandStart + std::chrono::milliseconds(10));
    expectNear(appliedViewModel.at(commandStart + std::chrono::milliseconds(60)).first,
               0.24, 1e-12, "view model accumulates signed delayed commands");
    for (int sample = 0; sample < 520; ++sample)
    {
        appliedViewModel.addCommand(1, 0, 0.01, 0.02,
            commandStart + std::chrono::milliseconds(100 + sample));
    }
    expectTrue(appliedViewModel.sampleCount() <= 512,
               "view model prunes history to a bounded sample count");
    expectNear(appliedViewModel.at(commandStart + std::chrono::milliseconds(2000)).first,
               5.44, 1e-9,
               "view model pruning preserves the cumulative angle baseline");

    AppliedViewMotionModel trueCameraModel;
    AppliedViewMotionModel alignedCameraModel;
    AppliedViewMotionModel wrongDelayCameraModel;
    trueCameraModel.configure(60.0);
    alignedCameraModel.configure(60.0);
    wrongDelayCameraModel.configure(0.0);
    for (int command = 0; command < 4; ++command)
    {
        const int signedCounts = (command % 2 == 0) ? 8 : -5;
        const auto sendTime = commandStart + std::chrono::milliseconds(80 + command * 90);
        trueCameraModel.addCommand(signedCounts, 0, 0.03, 0.02, sendTime);
        alignedCameraModel.addCommand(signedCounts, 0, 0.03, 0.02, sendTime);
        wrongDelayCameraModel.addCommand(signedCounts, 0, 0.03, 0.02, sendTime);
    }
    constexpr double staticWorldYaw = 1.25;
    double alignedSquaredError = 0.0;
    double wrongSquaredError = 0.0;
    double maximumAlignedRate = 0.0;
    double previousAlignedYaw = staticWorldYaw;
    int syntheticSamples = 0;
    for (int milliseconds = 0; milliseconds <= 500; milliseconds += 10)
    {
        const auto observationTime = commandStart + std::chrono::milliseconds(milliseconds);
        const double measuredRelativeYaw =
            staticWorldYaw - trueCameraModel.at(observationTime).first;
        const double alignedYaw =
            measuredRelativeYaw + alignedCameraModel.at(observationTime).first;
        const double wrongYaw =
            measuredRelativeYaw + wrongDelayCameraModel.at(observationTime).first;
        const double alignedError = alignedYaw - staticWorldYaw;
        const double wrongError = wrongYaw - staticWorldYaw;
        alignedSquaredError += alignedError * alignedError;
        wrongSquaredError += wrongError * wrongError;
        if (syntheticSamples > 0)
        {
            maximumAlignedRate = std::max(
                maximumAlignedRate,
                std::abs((alignedYaw - previousAlignedYaw) / 0.010));
        }
        previousAlignedYaw = alignedYaw;
        ++syntheticSamples;
    }
    const double alignedRmse = std::sqrt(alignedSquaredError / syntheticSamples);
    const double wrongDelayRmse = std::sqrt(wrongSquaredError / syntheticSamples);
    expectNear(alignedRmse, 0.0, 1e-12,
               "delay-aligned static camera motion has zero stabilized angle rmse");
    expectNear(maximumAlignedRate, 0.0, 1e-9,
               "delay-aligned static camera motion has zero stabilized angular rate");
    expectTrue(wrongDelayRmse > alignedRmse + 0.05,
               "deliberately wrong camera delay increases stabilized angle rmse");

    PassiveProfileCalibrator calibrator;
    const auto calibrationStart = PassiveProfileCalibrator::TimePoint(
        std::chrono::seconds(10));
    calibrator.setEnabled(true);
    double cumulativeX = 0.0;
    double cumulativeY = 0.0;
    std::vector<std::pair<double, double>> cumulativeCommands;
    cumulativeCommands.reserve(260);
    cumulativeCommands.push_back({ 0.0, 0.0 });
    for (int frame = 1; frame <= 240; ++frame)
    {
        const auto frameTime = calibrationStart + std::chrono::milliseconds(frame * 4);
        const int commandX = ((frame * 7) % 9) - 4;
        const int commandY = ((frame * 5 + 2) % 7) - 3;
        calibrator.recordCommand(commandX, commandY, frameTime);
        cumulativeX += commandX;
        cumulativeY += commandY;
        cumulativeCommands.push_back({ cumulativeX, cumulativeY });

        const int delayedFrame = std::max(0, frame - 15);
        const double elapsedSeconds = frame * 0.004;
        const double noiseX = ((frame % 5) - 2) * 0.01;
        const double noiseY = ((frame % 7) - 3) * 0.008;
        const double pivotX = 160.0 + 15.0 * elapsedSeconds -
            0.52 * cumulativeCommands[delayedFrame].first + noiseX;
        const double pivotY = 160.0 - 8.0 * elapsedSeconds -
            0.51 * cumulativeCommands[delayedFrame].second + noiseY;
        calibrator.addObservation(
            pivotX, pivotY, frameTime, 2560.0, 1440.0, 106.0, 74.0);
    }
    const auto calibrationResult = calibrator.snapshot();
    expectTrue(calibrationResult.x.valid && calibrationResult.y.valid,
               "passive profile calibration recovers both axes");
    expectNear(calibrationResult.x.pixelsPerCount, 0.52, 0.03,
               "passive calibration recovers horizontal pixels per count");
    expectNear(calibrationResult.y.pixelsPerCount, 0.51, 0.03,
               "passive calibration recovers vertical pixels per count");
    expectNear(calibrationResult.x.delayMs, 60.0, 6.0,
               "passive calibration recovers horizontal command delay");
    expectNear(calibrationResult.y.delayMs, 60.0, 6.0,
               "passive calibration recovers vertical command delay");
    expectTrue(calibrationResult.x.correlation < -0.8 &&
               calibrationResult.y.correlation < -0.8,
               "passive calibration reports inverse view correlation");
    calibrator.reset();
    expectTrue(calibrator.snapshot().observationCount == 0 &&
               calibrator.snapshot().commandCount == 0,
               "passive profile calibration reset clears samples");
    calibrator.setEnabled(false);
    calibrator.recordCommand(5, 5, calibrationStart + std::chrono::seconds(4));
    calibrator.addObservation(
        160.0, 160.0, calibrationStart + std::chrono::seconds(4),
        2560.0, 1440.0, 106.0, 74.0);
    expectTrue(calibrator.snapshot().observationCount == 0 &&
               calibrator.snapshot().commandCount == 0,
               "disabled passive calibration ignores samples");

    PipelineTracer tracer;
    tracer.setEnabled(true);
    tracer.setMaxFrames(10);
    PipelineFrame pending = tracer.beginFrame(320);
    pending.rawPivotX = 123.0;
    pending.finalMx = 17;
    pending.finalMy = -3;
    pending.aimPipeline = shadowFrame;
    pending.aimPipeline.viewMotion.valid = true;
    pending.aimPipeline.viewMotion.commandToFrameDelayMs = 50.0;
    pending.frameTiming.backendReceiveTime = timingBase;
    pending.frameTiming.captureSubmitTime = timingBase + std::chrono::milliseconds(1);
    pending.frameTiming.inferenceStartTime = timingBase + std::chrono::milliseconds(2);
    pending.frameTiming.inferencePublishTime = timingBase + std::chrono::milliseconds(5);
    pending.frameTiming.controlTime = timingBase + std::chrono::milliseconds(7);
    pending.frameTiming.frameSequence = 21;
    pending.commandSample.sequence = 42;
    pending.commandSample.requestedCountsX = 17;
    pending.commandSample.requestedCountsY = -3;
    pending.commandSample.enqueueTime = timingBase + std::chrono::milliseconds(8);
    pending.commandSample.enqueueSucceeded = true;
    expectTrue(pending.finalMx == 17 && pending.finalMy == -3,
               "shadow diagnostics do not alter legacy final output");
    expectTrue(tracer.getFrames().empty(), "trace frame is invisible before commit");
    tracer.commitFrame(std::move(pending));
    ViewCommandSample successfulCommand;
    successfulCommand.sequence = 42;
    successfulCommand.sendAttempted = true;
    successfulCommand.sendSucceeded = true;
    successfulCommand.appliedCountsX = 17;
    successfulCommand.appliedCountsY = -3;
    successfulCommand.deviceSendTime = timingBase + std::chrono::milliseconds(9);
    tracer.recordCommandResult(successfulCommand);
    const auto committed = tracer.getFrames();
    expectTrue(committed.size() == 1, "trace frame commit count");
    expectNear(committed.front().rawPivotX, 123.0, 0.0, "trace frame committed value");
    expectTrue(committed.front().commandSample.sendSucceeded &&
               committed.front().commandSample.appliedCountsX == 17 &&
               committed.front().commandSample.deviceSendTime ==
                   timingBase + std::chrono::milliseconds(9),
               "successful device command updates the matching trace frame");

    PipelineFrame failedCommandFrame = tracer.beginFrame(320);
    failedCommandFrame.commandSample.sequence = 43;
    failedCommandFrame.commandSample.enqueueSucceeded = true;
    tracer.commitFrame(std::move(failedCommandFrame));
    ViewCommandSample failedCommand;
    failedCommand.sequence = 43;
    failedCommand.sendAttempted = true;
    tracer.recordCommandResult(failedCommand);
    auto commandFrames = tracer.getFrames();
    expectTrue(commandFrames.back().commandSample.sendAttempted &&
               !commandFrames.back().commandSample.sendSucceeded &&
               commandFrames.back().commandSample.deviceSendTime.time_since_epoch().count() == 0,
               "failed device command keeps send time and applied counts empty");

    PipelineFrame droppedCommandFrame = tracer.beginFrame(320);
    droppedCommandFrame.commandSample.sequence = 44;
    droppedCommandFrame.commandSample.enqueueSucceeded = true;
    tracer.commitFrame(std::move(droppedCommandFrame));
    tracer.recordCommandDropped(44);
    commandFrames = tracer.getFrames();
    expectTrue(commandFrames.back().commandSample.droppedBeforeSend &&
               !commandFrames.back().commandSample.sendAttempted,
               "cleared or superseded queue command is marked dropped before send");

    const char* tracePath = "xen_basic_pipeline_test.csv";
    expectTrue(tracer.exportCSV(tracePath), "basic pipeline csv export");
    std::ifstream traceFile(tracePath, std::ios::binary);
    std::string traceHeader;
    std::getline(traceFile, traceHeader);
    if (traceHeader.size() >= 3 &&
        static_cast<unsigned char>(traceHeader[0]) == 0xEF)
        traceHeader.erase(0, 3);
    expectTrue(traceHeader.find("FilteredX") != std::string::npos,
               "basic pipeline contains filtered observation");
    expectTrue(traceHeader.find(
        "ObservedVelocityX,ObservedVelocityY,ObservedSpeed,FilterTrendSpeed,FilterTrendActive") != std::string::npos,
               "basic pipeline contains signed target velocity diagnostics");
    expectTrue(traceHeader.find("SourceWidth,SourceHeight") != std::string::npos,
               "basic pipeline contains capture source dimensions");
    expectTrue(traceHeader.find("InferenceFPS") != std::string::npos && traceHeader.find(
        "SourceDeclaredFPS,SourceReceiveFPS,SourceReceivedFrames,SourceDroppedFrames") != std::string::npos,
        "basic pipeline contains generic source fps diagnostics");
    expectTrue(traceHeader.find(
        "DmlModel,DmlInputWidth,DmlInputHeight,DmlPreprocessMs,DmlTensorSetupMs,DmlInferenceMs,DmlCopyMs,DmlPostprocessMs,DmlNmsMs,DmlTotalMs") != std::string::npos,
        "basic pipeline contains dml stage timing diagnostics");
    expectTrue(traceHeader.find("NdiDeclaredFPS,NdiReceiveFPS,NdiReceivedFrames,NdiDroppedFrames") != std::string::npos,
               "basic pipeline keeps ndi compatibility diagnostics");
    expectTrue(traceHeader.find("FrameCountLimit,ErrorMotion") != std::string::npos &&
               traceHeader.find("MovingInsideSettle,HorizontalCatchUp,VerticalCatchUp,SpeedLimited,Settled") != std::string::npos,
               "basic pipeline reports controller speed limiting");
    expectTrue(traceHeader.find(
        "ResponseSeconds,EffectiveResponseSecondsX,EffectiveResponseSecondsY") != std::string::npos,
        "basic pipeline reports effective per-axis catch-up response");
    expectTrue(traceHeader.find(
        "FrameID,BuildBackend,BuildRevision,BuildTimestampUtc,ControllerRevision") != std::string::npos,
        "basic pipeline identifies the executable and controller revision");
    expectTrue(traceHeader.find(
        "AimPipelineRequestedMode,AimPipelineEffectiveMode,AimPipelineActiveAvailable,AimPipelineShadowProcessed,AimPipelineCommandSuppressed") != std::string::npos,
        "basic pipeline records same-frame new pipeline mode diagnostics");
    expectTrue(traceHeader.find(
        "ViewMotionShadowValid,CommandToFrameDelayMs,DegreesPerCountX,DegreesPerCountY,MeasuredLosYawDegrees") != std::string::npos,
        "basic pipeline records delayed applied-view angle diagnostics");
    expectTrue(traceHeader.find(
        "AimPipelineAngleX,AimPipelineAngleY,AimPipelineRateX,AimPipelineRateY,AimPipelineCovarianceX,AimPipelineCovarianceY") != std::string::npos &&
        traceHeader.find("AimPipelineInnovationVarianceX,AimPipelineInnovationVarianceY,AimPipelineInnovationX,AimPipelineInnovationY,AimPipelineNisX,AimPipelineNisY") != std::string::npos &&
        traceHeader.find("AimPipelineMeasurementConfidence,AimPipelineFeedforwardConfidence") != std::string::npos,
        "basic pipeline records relative LOS Kalman diagnostics");
    expectTrue(traceHeader.find(
        "AimPipelineControlValid,AimPipelineControlSpeedLimited,AimPipelineIntegralFrozen,AimPipelineSettled,AimPipelineSettleReleased,AimPipelineSettleConfirmationSamples,AimPipelineLowSpeedReverseSuppressed,AimPipelineVerticalCatchUpActive,AimPipelineReverseConfirmationSeconds,AimPipelineEffectiveResponseSecondsY") != std::string::npos &&
        traceHeader.find("AimPipelineFeedbackX,AimPipelineFeedbackY,AimPipelineTrackingFeedforwardX,AimPipelineTrackingFeedforwardY") != std::string::npos &&
        traceHeader.find("AimPipelineLeadCountsX,AimPipelineLeadCountsY,AimPipelineIntegralCountsX,AimPipelineIntegralCountsY,AimPipelineUnlimitedCountsX,AimPipelineUnlimitedCountsY") != std::string::npos &&
        traceHeader.find("AimPipelineRequestedCountsX,AimPipelineRequestedCountsY,AimPipelineFrameCountLimit") != std::string::npos,
        "basic pipeline records the complete P0-4A controller decomposition");
    expectTrue(traceHeader.find(
        "TrajectoryRequestValid,TrajectoryRequestSequence,TrajectoryRequestTimeNs,TrajectoryRequestDurationMs,TrajectoryShaperMode,TrajectoryOutputProduced,TrajectoryOutputRequestSequence,TrajectoryScheduledTickNs,TrajectoryOutputTickNs") != std::string::npos &&
        traceHeader.find("TrajectoryPositionX,TrajectoryPositionY,TrajectoryTargetVelocityX,TrajectoryTargetVelocityY,TrajectoryVelocityX,TrajectoryVelocityY,TrajectoryAccelerationX,TrajectoryAccelerationY,TrajectoryJerkX,TrajectoryJerkY") != std::string::npos &&
        traceHeader.find("TrajectoryShapedCountsX,TrajectoryShapedCountsY,TrajectoryQuantizationRemainderX,TrajectoryQuantizationRemainderY,TrajectoryShapingDelayMs,TrajectorySchedulerLatenessMs,TrajectorySchedulerSkippedTicks") != std::string::npos,
        "basic pipeline records the complete P0-4B trajectory timeline");
    expectTrue(traceHeader.find(
        "CaptureFrameSequence,BackendReceiveNs,CaptureSubmitNs,InferenceStartNs,InferencePublishNs,ControlTimeNs") != std::string::npos &&
        traceHeader.find("BackendQueueMs,SubmitToInferenceMs,InferenceToPublishMs,PublishToControlMs,LocalObservationAgeMs") != std::string::npos,
        "basic pipeline records complete local timing stages and signed segment ages");
    expectTrue(traceHeader.find(
        "CommandSequence,CommandEnqueueSucceeded,CommandEnqueueNs,CommandSendAttempted,CommandSendSucceeded,CommandDeviceSendNs,CommandDroppedBeforeSend") != std::string::npos,
        "basic pipeline records command enqueue and device acknowledgement lifecycle");
    expectTrue(traceHeader.find(
        "ErrorMotion,SettleMotionThreshold,MovingInsideSettle") != std::string::npos,
        "basic pipeline reports settle motion release diagnostics");
    std::string traceRow;
    std::getline(traceFile, traceRow);
    expectTrue(std::count(traceHeader.begin(), traceHeader.end(), ',') ==
                   std::count(traceRow.begin(), traceRow.end(), ','),
               "basic pipeline csv header and data row keep the same column count");
    expectTrue(traceRow.find(",DML,") != std::string::npos ||
               traceRow.find(",CUDA,") != std::string::npos,
               "basic pipeline writes the configured build backend");
    expectTrue(traceRow.find(",unknown,") == std::string::npos,
               "basic pipeline writes concrete build revision and timestamp");
    expectTrue(traceRow.find(",shadow,shadow,0,1,1,") != std::string::npos,
               "basic pipeline writes command-suppressed shadow state in the legacy frame");
    expectTrue(BuildIdentity::displayLabel().find(" r30") != std::string::npos,
               "ui build label includes controller revision");
    expectTrue(traceHeader.find("IntegralCountsX,IntegralCountsY") != std::string::npos &&
               traceHeader.find("ResponseSeconds,EffectiveResponseSecondsX,EffectiveResponseSecondsY,IntegralTimeSeconds") != std::string::npos,
               "basic pipeline reports moving-target integral diagnostics");
    expectTrue(traceHeader.find(
        "ProfileCalibrationEnabled,ProfileCalibrationValidX,ProfileCalibrationValidY") != std::string::npos &&
        traceHeader.find("ProfileCalibrationOverallConfidence") != std::string::npos,
        "basic pipeline reports passive profile calibration diagnostics");
    expectTrue(traceHeader.find(
        "PredictionApplied,PredictionEnabled,PredictionAdditionalLeadMs,PredictionVelocityTauMs,PredictionStrength,PredictionVelocityX,PredictionVelocityY,PredictionAccelerationX,PredictionAccelerationY,PredictionLeadMs,PredictionOffsetX,PredictionOffsetY,ViewMotionX,ViewMotionY,PredictionDirectionLocked,PredictionSelfMotionSuppressed,PredictionOscillationSuppressed,PredictionHighSpeedSuppressed,PredictedX,PredictedY") != std::string::npos,
        "basic pipeline reports prediction diagnostics");
    traceFile.close();
    std::remove(tracePath);

    for (int i = 0; i < 15; ++i)
        tracer.commitFrame(tracer.beginFrame(320));
    expectTrue(tracer.size() == 10, "trace ring capacity");

    BasicTargetFilter filter;
    const auto t0 = std::chrono::steady_clock::time_point(std::chrono::milliseconds(1000));

    ViewMotionHistory viewHistory;
    viewHistory.add(4.0, -2.0, t0 + std::chrono::milliseconds(10));
    viewHistory.add(6.0, 3.0, t0 + std::chrono::milliseconds(20));
    const auto viewBefore = viewHistory.at(t0);
    const auto viewBetween = viewHistory.at(t0 + std::chrono::milliseconds(15));
    const auto viewAfter = viewHistory.at(t0 + std::chrono::milliseconds(25));
    expectNear(viewBefore.first, 0.0, 0.0, "view history keeps pre-movement baseline");
    expectNear(viewBetween.first, 4.0, 0.0, "view history queries observation-time cumulative motion");
    expectNear(viewAfter.first, 10.0, 0.0, "view history keeps current cumulative motion");
    const auto viewSince = viewHistory.since(t0 + std::chrono::milliseconds(15));
    expectNear(viewSince.first, 6.0, 0.0, "view history isolates self motion between observations");
    viewHistory.add(5.0, 0.0, t0 + std::chrono::seconds(3));
    const auto viewAfterPrune = viewHistory.at(t0 + std::chrono::seconds(4));
    expectNear(viewAfterPrune.first, 15.0, 0.0,
               "view history pruning never resets the stable coordinate system");
    filter.update(160.0, 160.0, t0, 1.0 / 120.0, 320.0);
    const auto filteredNoise = filter.update(
        160.3, 159.8, t0 + std::chrono::milliseconds(8), 1.0 / 120.0, 320.0);
    expectTrue(std::hypot(filteredNoise.x - 160.0, filteredNoise.y - 160.0) < 0.2,
               "basic filter suppresses detector quantization");
    const auto filteredMove = filter.update(
        168.0, 160.0, t0 + std::chrono::milliseconds(16), 1.0 / 120.0, 320.0);
    expectTrue(filteredMove.x > filteredNoise.x, "basic filter follows movement");
    expectTrue(filteredMove.x <= 168.0, "basic filter does not predict future position");
    expectNear(filteredMove.observedVelocityX, 962.5, 1e-9,
               "basic filter reports signed horizontal observation velocity");
    expectNear(filteredMove.observedVelocityY, 25.0, 1e-9,
               "basic filter reports signed vertical observation velocity");
    expectNear(filteredMove.observedSpeed,
               std::hypot(filteredMove.observedVelocityX, filteredMove.observedVelocityY),
               1e-9, "basic filter speed matches signed velocity magnitude");

    BasicTargetFilter alternatingJitterFilter;
    alternatingJitterFilter.update(160.0, 160.0, t0, 0.010, 320.0);
    double jitterMinimum = 160.0;
    double jitterMaximum = 160.0;
    BasicTargetFilter::Result alternatingJitter{};
    for (int sample = 1; sample <= 20; ++sample)
    {
        const auto time = t0 + std::chrono::milliseconds(sample * 10);
        alternatingJitter = alternatingJitterFilter.update(
            sample % 2 == 0 ? 163.0 : 157.0, 160.0,
            time, 0.010, 320.0, true);
        if (sample > 10)
        {
            jitterMinimum = std::min(jitterMinimum, alternatingJitter.x);
            jitterMaximum = std::max(jitterMaximum, alternatingJitter.x);
        }
    }
    expectTrue(jitterMaximum - jitterMinimum < 2.0,
               "alternating high-speed detector jitter keeps a narrow filtered range");
    expectTrue(alternatingJitter.motionTrendSpeed < 120.0,
               "alternating detector jitter does not become sustained motion trend");
    expectTrue(alternatingJitter.motionTrendActive,
               "confirmed prediction enables motion-trend filtering");

    BasicTargetFilter sustainedMotionFilter;
    sustainedMotionFilter.update(160.0, 160.0, t0, 0.010, 320.0);
    BasicTargetFilter::Result sustainedFiltered{};
    for (int sample = 1; sample <= 20; ++sample)
    {
        const auto time = t0 + std::chrono::milliseconds(sample * 10);
        sustainedFiltered = sustainedMotionFilter.update(
            160.0 + sample * 3.0, 160.0, time, 0.010, 320.0, true);
    }
    expectTrue(220.0 - sustainedFiltered.x < 8.0,
               "sustained horizontal motion retains bounded filter lag");
    expectTrue(sustainedFiltered.motionTrendSpeed > 250.0,
               "sustained horizontal motion opens the adaptive filter");

    BasicTargetFilter convergenceFilter;
    convergenceFilter.update(100.0, 100.0, t0, 0.010, 320.0);
    const auto convergenceResponse = convergenceFilter.update(
        108.0, 100.0, t0 + std::chrono::milliseconds(10), 0.010, 320.0, false);
    expectTrue(!convergenceResponse.motionTrendActive && convergenceResponse.x > 105.0,
               "pre-prediction convergence keeps the instantaneous responsive filter");

    TargetPredictor::Settings predictionSettings;
    predictionSettings.additionalLeadSeconds = 0.020;
    predictionSettings.velocityTimeConstantSeconds = 0.050;
    predictionSettings.predictionStrength = 2.0;

    TargetPredictor movingPredictor;
    const auto predictionFirst = movingPredictor.update(
        100.0, 100.0, t0, t0, 320.0, predictionSettings);
    expectTrue(!predictionFirst.applied && predictionFirst.x == 100.0,
               "prediction waits for a velocity observation");
    for (int sample = 1; sample <= 8; ++sample)
    {
        const auto time = t0 + std::chrono::milliseconds(sample * 8);
        movingPredictor.update(
            100.0 + sample * 8.0, 100.0, time, time + std::chrono::milliseconds(10),
            320.0, predictionSettings);
    }
    const auto predictionMoving = movingPredictor.update(
        172.0, 100.0, t0 + std::chrono::milliseconds(72),
        t0 + std::chrono::milliseconds(82), 320.0, predictionSettings);
    expectTrue(predictionMoving.applied && predictionMoving.directionLocked &&
               predictionMoving.x > 172.0,
               "confirmed movement progressively releases a forward constant-velocity lead");
    expectNear(predictionMoving.velocityX, 1000.0, 1e-9,
               "prediction velocity uses observation timestamps");
    expectNear(predictionMoving.leadSeconds, 0.030, 1e-9,
               "prediction adds observation age to fixed lead");

    TargetPredictor staticPredictor;
    staticPredictor.update(
        160.0, 160.0, t0, t0, 320.0, predictionSettings);
    const auto predictionStatic = staticPredictor.update(
        160.0, 160.0, t0 + std::chrono::milliseconds(17),
        t0 + std::chrono::milliseconds(27), 320.0, predictionSettings);
    expectTrue(predictionStatic.applied, "static target keeps prediction stage active");
    expectTrue(!predictionStatic.directionLocked,
               "static target never activates motion lead");
    expectNear(predictionStatic.x, 160.0, 0.0, "static prediction does not drift horizontally");
    expectNear(predictionStatic.y, 160.0, 0.0, "static prediction does not drift vertically");

    TargetPredictor detectorNoisePredictor;
    for (int sample = 0; sample < 10; ++sample)
    {
        const auto time = t0 + std::chrono::milliseconds(sample * 8);
        const double noiseX = sample % 2 == 0 ? 161.2 : 158.8;
        const double noiseY = sample % 3 == 0 ? 161.0 : 159.0;
        const auto noisyPrediction = detectorNoisePredictor.update(
            noiseX, noiseY, time, time, 320.0, predictionSettings);
        expectTrue(!noisyPrediction.directionLocked &&
                   noisyPrediction.offsetX == 0.0 && noisyPrediction.offsetY == 0.0,
                   "oscillating detector noise never activates prediction");
    }

    TargetPredictor horizontalPredictor;
    TargetPredictor::Result horizontalPrediction;
    for (int sample = 0; sample < 10; ++sample)
    {
        const auto time = t0 + std::chrono::milliseconds(sample * 8);
        horizontalPrediction = horizontalPredictor.update(
            100.0 + sample * 8.0, 100.0 + (sample % 2 == 0 ? 1.0 : -1.0),
            time, time, 320.0, predictionSettings);
    }
    expectTrue(horizontalPrediction.directionLocked && horizontalPrediction.offsetX > 0.0,
               "consistent horizontal movement activates prediction");
    expectNear(horizontalPrediction.velocityY, 0.0, 0.0,
               "minor non-dominant vertical noise is suppressed");
    expectNear(horizontalPrediction.offsetY, 0.0, 0.0,
               "horizontal movement never creates vertical prediction lead");

    expectTrue(TargetPredictor::isSelfMotionArtifact(
                   50.0, 0.0, -8.0, 0.0, 10.0, 0.0, 200.0, 0.0),
               "self-driven screen convergence suppresses outward prediction residual");
    expectTrue(!TargetPredictor::isSelfMotionArtifact(
                   50.0, 0.0, 5.0, 0.0, 10.0, 0.0, 300.0, 0.0),
               "real outward target motion remains eligible for prediction");

    TargetPredictor selfMotionHoldPredictor;
    TargetPredictor::Result artifactResult{
        170.0, 160.0, 200.0, 0.0, 0.0, 0.0,
        0.05, 10.0, 0.0, true, true
    };
    selfMotionHoldPredictor.applySelfMotionSuppression(artifactResult, true);
    expectTrue(artifactResult.selfMotionSuppressed && artifactResult.x == 160.0 &&
               artifactResult.offsetX == 0.0,
               "detected self-motion artifact withdraws the current lead");
    for (int frame = 0; frame < 4; ++frame)
    {
        TargetPredictor::Result heldResult{
            170.0, 160.0, 200.0, 0.0, 0.0, 0.0,
            0.05, 10.0, 0.0, true, true
        };
        selfMotionHoldPredictor.applySelfMotionSuppression(heldResult, false);
        expectTrue(heldResult.selfMotionSuppressed && heldResult.x == 160.0,
                   "self-motion suppression covers the four-frame response tail");
    }
    TargetPredictor::Result recoveredResult{
        170.0, 160.0, 200.0, 0.0, 0.0, 0.0,
        0.05, 10.0, 0.0, true, true
    };
    selfMotionHoldPredictor.applySelfMotionSuppression(recoveredResult, false);
    expectTrue(!recoveredResult.selfMotionSuppressed && recoveredResult.offsetX == 10.0,
               "prediction resumes after the bounded self-motion hold");
    selfMotionHoldPredictor.applySelfMotionSuppression(recoveredResult, true);
    selfMotionHoldPredictor.reset();
    TargetPredictor::Result resetResult{
        170.0, 160.0, 200.0, 0.0, 0.0, 0.0,
        0.05, 10.0, 0.0, true, true
    };
    selfMotionHoldPredictor.applySelfMotionSuppression(resetResult, false);
    expectTrue(!resetResult.selfMotionSuppressed,
               "tracking reset clears the self-motion suppression hold");

    TargetPredictor selfMotionRearmPredictor;
    TargetPredictor::Result rearmPrediction{};
    for (int sample = 0; sample < 10; ++sample)
    {
        const auto time = t0 + std::chrono::milliseconds(sample * 8);
        rearmPrediction = selfMotionRearmPredictor.update(
            100.0 + sample * 8.0, 100.0, time, time, 320.0, predictionSettings);
    }
    expectTrue(rearmPrediction.directionLocked && rearmPrediction.offsetX > 0.0,
               "self-motion rearm test starts from an established prediction");
    selfMotionRearmPredictor.applySelfMotionSuppression(rearmPrediction, true);
    for (int frame = 1; frame <= 4; ++frame)
    {
        const auto time = t0 + std::chrono::milliseconds((9 + frame) * 8);
        rearmPrediction = selfMotionRearmPredictor.update(
            172.0 - frame * 4.0, 100.0, time, time, 320.0, predictionSettings);
        selfMotionRearmPredictor.applySelfMotionSuppression(rearmPrediction, false);
    }
    const double reboundPositions[] = { 154.5, 153.0, 151.5, 150.5 };
    TargetPredictor::Result reboundPrediction{};
    for (int sample = 0; sample < 4; ++sample)
    {
        const auto time = t0 + std::chrono::milliseconds(112 + sample * 8);
        reboundPrediction = selfMotionRearmPredictor.update(
            reboundPositions[sample], 100.0, time, time, 320.0, predictionSettings);
        expectTrue(!reboundPrediction.directionLocked && reboundPrediction.offsetX == 0.0,
                   "sub-eight-pixel self-motion rebound cannot rearm prediction");
    }
    const double postHoldMovement[] = { 147.0, 143.0, 139.0, 135.0, 131.0, 127.0 };
    TargetPredictor::Result confirmedPostHoldMovement{};
    for (int sample = 0; sample < 6; ++sample)
    {
        const auto time = t0 + std::chrono::milliseconds(144 + sample * 8);
        confirmedPostHoldMovement = selfMotionRearmPredictor.update(
            postHoldMovement[sample], 100.0, time, time, 320.0, predictionSettings);
    }
    expectTrue(confirmedPostHoldMovement.directionLocked &&
               confirmedPostHoldMovement.offsetX < 0.0,
               "post-hold movement beyond eight pixels can rearm prediction");

    TargetPredictor sustainedMotionPredictor;
    TargetPredictor::Result sustainedPrediction{};
    int activePredictionFrames = 0;
    for (int sample = 0; sample < 16 && activePredictionFrames < 5; ++sample)
    {
        const auto time = t0 + std::chrono::milliseconds(sample * 8);
        sustainedPrediction = sustainedMotionPredictor.update(
            100.0 + sample * 6.0, 100.0, time, time, 320.0, predictionSettings);
        if (sustainedPrediction.offsetX > 0.0)
            ++activePredictionFrames;
    }
    expectTrue(activePredictionFrames == 5 && sustainedPrediction.offsetX > 0.0,
               "five continuous prediction frames establish sustained target motion");
    sustainedMotionPredictor.applySelfMotionSuppression(sustainedPrediction, true);
    expectTrue(!sustainedPrediction.selfMotionSuppressed && sustainedPrediction.offsetX > 0.0,
               "sustained target motion is exempt from self-motion artifact suppression");
    for (int sample = 16; sample < 20; ++sample)
    {
        const auto time = t0 + std::chrono::milliseconds(sample * 8);
        sustainedPrediction = sustainedMotionPredictor.update(
            190.0, 100.0, time, time, 320.0, predictionSettings);
    }
    TargetPredictor::Result stoppedArtifactResult{
        170.0, 160.0, 200.0, 0.0, 0.0, 0.0,
        0.05, 10.0, 0.0, true, true
    };
    sustainedMotionPredictor.applySelfMotionSuppression(stoppedArtifactResult, true);
    expectTrue(stoppedArtifactResult.selfMotionSuppressed && stoppedArtifactResult.offsetX == 0.0,
               "stopping clears the sustained-motion suppression exemption");

    TargetPredictor irregularPredictor;
    irregularPredictor.update(
        50.0, 50.0, t0, t0, 320.0, predictionSettings);
    irregularPredictor.update(55.0, 50.0, t0 + std::chrono::milliseconds(10),
        t0 + std::chrono::milliseconds(15), 320.0, predictionSettings);
    irregularPredictor.update(60.0, 50.0, t0 + std::chrono::milliseconds(20),
        t0 + std::chrono::milliseconds(25), 320.0, predictionSettings);
    irregularPredictor.update(65.0, 50.0, t0 + std::chrono::milliseconds(30),
        t0 + std::chrono::milliseconds(35), 320.0, predictionSettings);
    const auto predictionIrregular = irregularPredictor.update(
        70.0, 50.0, t0 + std::chrono::milliseconds(40),
        t0 + std::chrono::milliseconds(45), 320.0, predictionSettings);
    expectNear(predictionIrregular.velocityX, 500.0, 1e-9,
               "prediction remains frame-rate independent under irregular dml cadence");

    TargetPredictor youngObservationPredictor;
    TargetPredictor oldObservationPredictor;
    for (int sample = 0; sample < 9; ++sample)
    {
        const double x = 100.0 + sample * 8.0;
        const auto time = t0 + std::chrono::milliseconds(sample * 8);
        youngObservationPredictor.update(
            x, 100.0, time, time, 320.0, predictionSettings);
        oldObservationPredictor.update(
            x, 100.0, time, time, 320.0, predictionSettings);
    }
    const auto youngObservation = youngObservationPredictor.update(
        172.0, 100.0, t0 + std::chrono::milliseconds(72),
        t0 + std::chrono::milliseconds(72), 320.0, predictionSettings);
    const auto oldObservation = oldObservationPredictor.update(
        172.0, 100.0, t0 + std::chrono::milliseconds(72),
        t0 + std::chrono::milliseconds(92), 320.0, predictionSettings);
    expectTrue(oldObservation.x > youngObservation.x,
               "older observation receives more automatic latency compensation");

    movingPredictor.reset();
    const auto predictionAfterLoss = movingPredictor.update(
        200.0, 100.0, t0 + std::chrono::milliseconds(30),
        t0 + std::chrono::milliseconds(30), 320.0, predictionSettings);
    expectTrue(!predictionAfterLoss.applied && predictionAfterLoss.velocityX == 0.0,
               "target loss reset prevents stale prediction output");

    TargetPredictor jumpPredictor;
    TargetPredictor::Result boundedJump{};
    for (int sample = 0; sample < 18; ++sample)
    {
        const auto time = t0 + std::chrono::milliseconds(sample * 8);
        boundedJump = jumpPredictor.update(
            10.0 + sample * 16.0, 10.0, time, time,
            320.0, predictionSettings);
    }
    expectNear(std::hypot(boundedJump.offsetX, boundedJump.offsetY), 24.0, 1e-9,
               "mature abnormal high-speed prediction is capped at seven point five percent span");

    TargetPredictor oscillatingPredictor;
    TargetPredictor::Result oscillatingPrediction{};
    double oscillatingX = 100.0;
    int oscillatingSample = 0;
    for (int segment = 0; segment < 5; ++segment)
    {
        const double step = segment % 2 == 0 ? 2.0 : -2.0;
        for (int frame = 0; frame < 14; ++frame)
        {
            oscillatingX += step;
            const auto time = t0 + std::chrono::milliseconds(oscillatingSample++ * 8);
            oscillatingPrediction = oscillatingPredictor.update(
                oscillatingX, 100.0, time, time, 320.0, predictionSettings);
        }
        if (segment < 4)
        {
            for (int frame = 0; frame < 3; ++frame)
            {
                const auto time = t0 + std::chrono::milliseconds(oscillatingSample++ * 8);
                oscillatingPrediction = oscillatingPredictor.update(
                    oscillatingX, 100.0, time, time, 320.0, predictionSettings);
            }
        }
    }
    expectTrue(oscillatingPrediction.oscillationSuppressed &&
               oscillatingPrediction.offsetX == 0.0,
               "three reliable reversals inside one point five seconds keep aim on the observed target");

    TargetPredictor shortReboundPredictor;
    TargetPredictor::Result shortReboundPrediction{};
    double shortReboundX = 100.0;
    int shortReboundSample = 0;
    for (const double step : { 2.0, -2.0, 2.0, -2.0 })
    {
        const int frames = step > 0.0 && shortReboundSample == 0 ? 20 : 8;
        for (int frame = 0; frame < frames; ++frame)
        {
            shortReboundX += step;
            const auto time = t0 + std::chrono::milliseconds(shortReboundSample++ * 8);
            shortReboundPrediction = shortReboundPredictor.update(
                shortReboundX, 100.0, time, time, 320.0, predictionSettings);
        }
    }
    expectTrue(!shortReboundPrediction.oscillationSuppressed,
               "sub-eighty-millisecond rebound and recovery do not form high-frequency oscillation");
    expectNear(TargetPredictor::boxHoldCoordinate(160.0, 140.0, 50.0), 160.0, 0.0,
               "oscillation box hold stops moving while the crosshair is inside");
    expectNear(TargetPredictor::boxHoldCoordinate(160.0, 170.0, 50.0), 172.0, 0.0,
               "oscillation box hold returns only to the nearest inset edge");
    expectNear(TargetPredictor::boxHoldCoordinate(160.0, 80.0, 50.0), 128.0, 0.0,
               "oscillation box hold handles the opposite outside edge");
    expectNear(TargetPredictor::boxHoldCoordinate(160.0, 170.0, 50.0, 20.0), 190.0, 0.0,
               "oscillation horizontal corridor follows the central fifth of the box");

    TargetPredictor highSpeedPredictor;
    TargetPredictor::Result highSpeedPrediction{};
    bool observedModerateSpeedAcceleration = false;
    bool observedHighSpeedTransient = false;
    double highSpeedX = 100.0;
    for (int sample = 0; sample < 18; ++sample)
    {
        const auto time = t0 + std::chrono::milliseconds(sample * 8);
        highSpeedX += 4.0;
        highSpeedPrediction = highSpeedPredictor.update(
            highSpeedX, 100.0, time, time,
            320.0, predictionSettings);
    }
    expectTrue(highSpeedPrediction.offsetX > 0.0,
               "stable high-speed motion establishes constant-velocity lead");
    for (int sample = 0; sample < 10; ++sample)
    {
        const auto time = t0 + std::chrono::milliseconds((18 + sample) * 8);
        const double sampleDelta = 6.0 + sample * 2.0;
        highSpeedX += sampleDelta;
        highSpeedPrediction = highSpeedPredictor.update(
            highSpeedX, 100.0, time, time,
            320.0, predictionSettings);
        const double fittedSpeed = std::hypot(
            highSpeedPrediction.velocityX, highSpeedPrediction.velocityY);
        const double fittedAcceleration = std::hypot(
            highSpeedPrediction.accelerationX, highSpeedPrediction.accelerationY);
        const bool currentModerateSpeedAcceleration =
            highSpeedPrediction.directionLocked &&
            fittedSpeed > 400.0 && fittedSpeed <= 640.0 &&
            fittedAcceleration > 1600.0;
        if (currentModerateSpeedAcceleration)
        {
            observedModerateSpeedAcceleration = true;
            expectTrue(!highSpeedPrediction.highSpeedSuppressed,
                       "moderate stable tracking speed ignores regression acceleration noise");
        }
        const bool currentHighSpeedTransient =
            highSpeedPrediction.directionLocked &&
            fittedSpeed > 640.0 && fittedAcceleration > 1600.0;
        if (currentHighSpeedTransient)
        {
            observedHighSpeedTransient = true;
            expectTrue(highSpeedPrediction.highSpeedSuppressed &&
                       highSpeedPrediction.offsetX == 0.0,
                       "first aligned acceleration after mature prediction withdraws constant-velocity lead");
        }
    }
    expectTrue(observedModerateSpeedAcceleration && observedHighSpeedTransient,
               "high-speed acceleration sequence enters the guarded transient envelope");

    TargetPredictor reversalPredictor;
    for (int sample = 0; sample < 10; ++sample)
    {
        const double x = 100.0 + sample * 8.0;
        const auto time = t0 + std::chrono::milliseconds(sample * 8);
        reversalPredictor.update(
            x, 100.0, time, time, 320.0, predictionSettings);
    }
    const auto reversalPending = reversalPredictor.update(
        164.0, 100.0, t0 + std::chrono::milliseconds(80), t0 + std::chrono::milliseconds(80),
        320.0, predictionSettings);
    expectTrue(reversalPending.directionLocked && reversalPending.x > 164.0,
               "single reverse coordinate outlier keeps the robust forward lead");

    bool withdrewOldLead = false;
    bool acquiredReverseLead = false;
    TargetPredictor::Result leftPrediction{};
    for (int sample = 1; sample <= 12; ++sample)
    {
        const auto time = t0 + std::chrono::milliseconds(80 + sample * 8);
        leftPrediction = reversalPredictor.update(
            164.0 - sample * 8.0, 100.0, time, time, 320.0, predictionSettings);
        withdrewOldLead = withdrewOldLead || leftPrediction.offsetX == 0.0;
        acquiredReverseLead = acquiredReverseLead || leftPrediction.offsetX < 0.0;
    }
    expectTrue(withdrewOldLead,
               "robustly confirmed reversal withdraws the old prediction side");
    expectTrue(acquiredReverseLead && leftPrediction.directionLocked,
               "stable reverse movement reacquires a continuous lead on the new side");

    const auto firstStationaryPrediction = reversalPredictor.update(
        68.0, 100.0, t0 + std::chrono::milliseconds(184), t0 + std::chrono::milliseconds(184),
        320.0, predictionSettings);
    expectTrue(firstStationaryPrediction.offsetX < 0.0,
               "single stationary observation does not interrupt an established lead");
    const auto confirmedStopPrediction = reversalPredictor.update(
        68.0, 100.0, t0 + std::chrono::milliseconds(192), t0 + std::chrono::milliseconds(192),
        320.0, predictionSettings);
    expectTrue(confirmedStopPrediction.x == 68.0,
               "two stationary observations withdraw the prediction lead");

    TargetPredictor matureLeadPredictor;
    TargetPredictor::Result matureLeadPrediction{};
    TargetPredictor::Result firstReleasedLead{};
    for (int sample = 0; sample < 18; ++sample)
    {
        const auto time = t0 + std::chrono::milliseconds(sample * 8);
        matureLeadPrediction = matureLeadPredictor.update(
            100.0 + sample * 4.0, 100.0, time, time,
            320.0, predictionSettings);
        if (firstReleasedLead.offsetX == 0.0 && matureLeadPrediction.offsetX > 0.0)
            firstReleasedLead = matureLeadPrediction;
    }
    expectTrue(firstReleasedLead.offsetX > 0.0 &&
               firstReleasedLead.offsetX < matureLeadPrediction.offsetX,
               "newly confirmed direction ramps lead instead of emitting a full pulse");
    expectNear(matureLeadPrediction.offsetX,
               matureLeadPrediction.velocityX * matureLeadPrediction.leadSeconds *
                   predictionSettings.predictionStrength,
               1e-9, "seven stable prediction frames restore the full constant-velocity lead");

    TargetPredictor disabledPredictor;
    TargetPredictor::Settings disabledPredictionSettings = predictionSettings;
    disabledPredictionSettings.enabled = false;
    const auto disabledPrediction = disabledPredictor.update(
        123.0, 87.0, t0, t0, 320.0, disabledPredictionSettings);
    expectTrue(!disabledPrediction.applied &&
               disabledPrediction.x == 123.0 && disabledPrediction.y == 87.0,
               "disabled prediction preserves base target position exactly");

    expectNear(VideoReplay::ObservedCoordinate(1400.0, 24.0, 1120.0), 256.0, 0.0,
               "video replay applies virtual camera offset before fixed crop");
    std::vector<VideoReplay::TrajectoryPoint> replayPoints{
        { 0.0, 1280.0, 720.0, true },
        { 0.010, 1290.0, 720.0, true }
    };
    expectNear(VideoReplay::InterpolateCoordinate(replayPoints, 0, 0.005, true), 1285.0, 1e-9,
               "video replay interpolates target position during observation age");
    expectTrue(!VideoReplay::IsNewTrajectorySegment(replayPoints[0], replayPoints[1]),
               "video replay keeps continuous motion in one trial");
    const VideoReplay::TrajectoryPoint replayReset{ 0.020, 1500.0, 720.0, true };
    expectTrue(VideoReplay::IsNewTrajectorySegment(replayPoints[1], replayReset),
               "video replay splits target teleport into an independent trial");

    const auto crossDomainVariants = CrossDomainReplay::BuildRequiredVariants();
    expectTrue(crossDomainVariants.size() == 169,
               "cross-domain matrix covers 162 physical variants and seven role-relative variants");
    expectTrue(std::any_of(crossDomainVariants.begin(), crossDomainVariants.end(), [](const auto& variant) {
        return variant.name == "role_same_self_faster" && variant.relativeMotionScale < 0.0;
    }), "role-faster transform reverses relative LOS naturally instead of using WASD direction");

    CrossDomainReplay::Metrics gateBaseline;
    gateBaseline.samples = 100;
    gateBaseline.errorP95Degrees = 1.0;
    gateBaseline.verticalP95Degrees = 1.0;
    gateBaseline.insideBoxPercent = 70.0;
    gateBaseline.lateHalfErrorP95Degrees = 1.0;
    CrossDomainReplay::Metrics gateCandidate = gateBaseline;
    std::string gateReason;
    CrossDomainReplay::Variant gateVariant;
    gateVariant.name = "unit";
    gateCandidate.errorP95Degrees = 0.89;
    gateCandidate.insideBoxPercent = 75.0;
    gateCandidate.lateHalfErrorP95Degrees = 0.95;
    expectTrue(CrossDomainReplay::EvaluateGate(
        "reverse", gateVariant, gateBaseline, gateCandidate, gateReason),
        "reverse gate accepts a measured ten-percent angular improvement");
    gateCandidate = gateBaseline;
    gateCandidate.verticalP95Degrees = 0.95;
    expectTrue(!CrossDomainReplay::EvaluateGate(
        "jump", gateVariant, gateBaseline, gateCandidate, gateReason) &&
        gateReason.find("10%") != std::string::npos,
        "jump gate rejects vertical improvement below ten percent");

    CrossDomainReplay::SourceTrajectory syntheticReplay;
    syntheticReplay.scenario = "horizontal_left";
    syntheticReplay.sourceWidth = 2560;
    syntheticReplay.sourceHeight = 1440;
    syntheticReplay.centerX = 1280.0;
    syntheticReplay.centerY = 720.0;
    for (int sample = 0; sample < 40; ++sample)
    {
        VideoReplay::TrajectoryPoint point;
        point.timeSeconds = sample * 0.01;
        point.globalX = 1280.0 - sample * 1.5;
        point.globalY = 720.0;
        point.detected = true;
        point.boxWidth = 32.0;
        point.boxHeight = 64.0;
        point.confidence = 0.9f;
        syntheticReplay.points.push_back(point);
    }
    CrossDomainReplay::Variant syntheticVariant;
    syntheticVariant.name = "unit_94fps";
    syntheticVariant.replayFps = 94.0;
    CrossDomainReplay::ControllerSettings syntheticSettings;
    const auto syntheticComparison = CrossDomainReplay::RunComparison(
        syntheticReplay, syntheticVariant, syntheticSettings);
    expectTrue(syntheticComparison.legacy.samples > 20 &&
               syntheticComparison.candidate.samples == syntheticComparison.legacy.samples,
               "same-frame replay gives legacy and candidate identical valid sample coverage");
    expectTrue(std::isfinite(syntheticComparison.candidate.meanNis) &&
               syntheticComparison.candidate.requestedCounts > 0.0 &&
               syntheticComparison.candidate.shapedCounts > 0.0 &&
               syntheticComparison.candidate.sentCounts > 0.0,
               "cross-domain replay records estimator, requested, shaped and sent diagnostics");

    BasicAimController controller;
    BasicAimController::Settings controllerSettings;
    controllerSettings.responseSeconds = 0.08;
    controllerSettings.maxCountsPerSecond = 240.0;
    controllerSettings.settleRadiusPixels = 5.0;
    controllerSettings.releaseRadiusPixels = 8.0;
    const auto farOutput = controller.update(
        -100.0, 0.0, 1.0 / 120.0, 15.0, 15.0, controllerSettings);
    expectTrue(farOutput.countsX < 0.0, "basic controller direction");
    expectTrue(std::hypot(farOutput.countsX, farOutput.countsY) <= 2.0 + 1e-9,
               "basic controller frame-rate speed limit");
    expectTrue(farOutput.speedLimited, "basic controller reports speed limiting");
    const auto settledOutput = controller.update(
        3.0, -2.0, 1.0 / 120.0, 15.0, 15.0, controllerSettings);
    expectTrue(settledOutput.settled, "basic controller settle state");
    expectNear(settledOutput.countsX, 0.0, 0.0, "basic controller settled x");

    BasicAimController verticalCatchUpController;
    BasicAimController::Settings verticalCatchUpSettings;
    verticalCatchUpSettings.responseSeconds = 0.080;
    verticalCatchUpSettings.maxCountsPerSecond = 3200.0;
    verticalCatchUpSettings.settleRadiusPixels = 5.0;
    verticalCatchUpSettings.releaseRadiusPixels = 8.0;
    const auto normalVerticalOutput = verticalCatchUpController.update(
        0.0, 15.0, 0.010, 1.0, 1.0, verticalCatchUpSettings);
    verticalCatchUpController.reset();
    const auto catchUpVerticalOutput = verticalCatchUpController.update(
        0.0, 20.0, 0.010, 1.0, 1.0, verticalCatchUpSettings);
    expectTrue(!normalVerticalOutput.verticalCatchUp && catchUpVerticalOutput.verticalCatchUp,
               "vertical catch-up activates only outside the sixteen-pixel threshold");
    expectNear(catchUpVerticalOutput.effectiveResponseSecondsY, 0.0375, 1e-12,
               "vertical catch-up follows the continuous response curve");
    expectTrue(catchUpVerticalOutput.countsY / 20.0 > normalVerticalOutput.countsY / 15.0,
               "vertical catch-up increases per-pixel response without changing max speed");
    verticalCatchUpController.reset();
    const auto catchUpHorizontalOutput = verticalCatchUpController.update(
        20.0, 0.0, 0.010, 1.0, 1.0, verticalCatchUpSettings);
    expectTrue(catchUpHorizontalOutput.horizontalCatchUp &&
               !catchUpHorizontalOutput.verticalCatchUp,
               "horizontal catch-up activates independently for jump lateral error");
    expectNear(catchUpHorizontalOutput.effectiveResponseSecondsX, 0.0375, 1e-12,
               "horizontal catch-up follows the continuous response independently");
    verticalCatchUpController.reset();
    const auto transitionalHorizontalOutput = verticalCatchUpController.update(
        24.0, 0.0, 0.010, 1.0, 1.0, verticalCatchUpSettings);
    expectNear(transitionalHorizontalOutput.effectiveResponseSecondsX, 0.035, 1e-12,
               "jump catch-up changes continuously between sixteen and thirty-two pixels");
    verticalCatchUpController.reset();
    const auto largeHorizontalOutput = verticalCatchUpController.update(
        40.0, 0.0, 0.010, 1.0, 1.0, verticalCatchUpSettings);
    expectNear(largeHorizontalOutput.effectiveResponseSecondsX, 0.030, 1e-12,
               "very large jump error keeps the thirty-millisecond response floor");
    expectTrue(largeHorizontalOutput.countsX / 40.0 >
                   catchUpHorizontalOutput.countsX / 20.0,
               "continuous catch-up still increases per-pixel response for extreme jumps");

    // 生产速率必须使用捕获窗统计出的实际 FPS 换算单帧预算，而不是绑定某个设备或固定帧率。
    // 选取本轮 CSV 的约 127/143 FPS 和未来可能出现的 240 FPS，验证每秒总预算始终一致。
    BasicAimController::Settings productionSettings;
    productionSettings.settleRadiusPixels = 0.0;
    productionSettings.releaseRadiusPixels = 0.0;
    expectNear(productionSettings.maxCountsPerSecond, 1440.0, 0.0,
               "production max speed uses four-chain nine-grid result");
    for (const double actualFps : { 127.0, 143.0, 240.0 })
    {
        BasicAimController actualFpsController;
        const auto actualFpsOutput = actualFpsController.update(
            1000.0, 0.0, 1.0 / actualFps, 15.0, 15.0, productionSettings);
        expectNear(actualFpsOutput.frameCountLimit * actualFps,
                   productionSettings.maxCountsPerSecond, 1e-9,
                   "production speed follows capture-window fps");
    }

    // 本轮只放宽远距离设备速率上限。近中心未触发限速时，两档速度必须产生完全相同的
    // 一阶响应，证明优化不会改变已经通过 36 段实测验证的稳定半径与近中心收敛形态。
    BasicAimController baselineNearController;
    BasicAimController optimizedNearController;
    BasicAimController::Settings baselineSettings = productionSettings;
    baselineSettings.maxCountsPerSecond = 1200.0;
    const auto baselineNearOutput = baselineNearController.update(
        12.0, 0.0, 1.0 / 120.0, 1.344, 1.668, baselineSettings);
    const auto optimizedNearOutput = optimizedNearController.update(
        12.0, 0.0, 1.0 / 120.0, 1.344, 1.668, productionSettings);
    expectTrue(!baselineNearOutput.speedLimited && !optimizedNearOutput.speedLimited,
               "near-center response remains outside speed limiting");
    expectNear(optimizedNearOutput.countsX, baselineNearOutput.countsX, 1e-12,
               "max speed optimization preserves near-center response");

    // 远距离请求在两档速度下均受限，1440 cps 的单帧预算应严格比 1200 cps 高 20%，
    // 从而只缩短大误差阶段，不通过修改响应时间或稳定回差掩盖问题。
    BasicAimController baselineFarController;
    BasicAimController optimizedFarController;
    const auto baselineFarOutput = baselineFarController.update(
        160.0, 0.0, 1.0 / 120.0, 1.344, 1.668, baselineSettings);
    const auto optimizedFarOutput = optimizedFarController.update(
        160.0, 0.0, 1.0 / 120.0, 1.344, 1.668, productionSettings);
    expectTrue(baselineFarOutput.speedLimited && optimizedFarOutput.speedLimited,
               "far response remains explicitly speed limited");
    expectNear(optimizedFarOutput.frameCountLimit / baselineFarOutput.frameCountLimit,
               1.2, 1e-12, "far frame budget increases by twenty percent");

    // 匀速目标是斜坡输入，纯比例控制必然保留“目标速度 × 响应时间”的固定滞后。
    // 现场六轮数据折算闭环滞后约 84~85 ms，即使把 UI 响应压到 20 ms 也不能归零。
    // 使用标准 PI 积分后，设备需求仍低于 1440 cps 时应消除该结构性稳态误差。
    auto simulateMovingTarget = [](double integralTimeSeconds) {
        BasicAimController movingController;
        BasicAimController::Settings movingSettings;
        movingSettings.responseSeconds = 0.080;
        movingSettings.maxCountsPerSecond = 1440.0;
        movingSettings.integralTimeSeconds = integralTimeSeconds;
        movingSettings.settleRadiusPixels = 0.0;
        movingSettings.releaseRadiusPixels = 0.0;
        constexpr double dt = 1.0 / 120.0;
        constexpr double countsPerPixel = 1.344;
        constexpr double targetCountsPerSecond = 800.0;
        double targetPixels = 0.0;
        double cameraPixels = 0.0;
        for (int frame = 0; frame < 480; ++frame)
        {
            targetPixels += targetCountsPerSecond / countsPerPixel * dt;
            const auto output = movingController.update(
                targetPixels - cameraPixels, 0.0, dt,
                countsPerPixel, countsPerPixel, movingSettings);
            cameraPixels += output.countsX / countsPerPixel;
        }
        return targetPixels - cameraPixels;
    };
    const double proportionalRampError = simulateMovingTarget(0.0);
    const double integralRampError = simulateMovingTarget(0.320);
    expectTrue(std::abs(proportionalRampError) > 10.0 &&
               std::abs(proportionalRampError) > std::abs(integralRampError) * 2.0,
               "catch-up reduces but does not eliminate proportional ramp steady-state error");
    expectTrue(std::abs(integralRampError) < 5.0,
               "pi controller removes moving-target steady-state error");

    // DML 推理帧率较低时单帧积分输出更大，PI 追进 5 px 中心区后必须继续保留
    // 匀速目标所需的整数 count 输出，不能被静止目标稳定分支反复清零。
    BasicAimController dmlRateController;
    BasicAimController::Settings dmlRateSettings;
    dmlRateSettings.responseSeconds = 0.080;
    dmlRateSettings.maxCountsPerSecond = 1440.0;
    dmlRateSettings.integralTimeSeconds = 0.320;
    constexpr double dmlDt = 1.0 / 60.0;
    constexpr double dmlCountsPerPixel = 1.344;
    constexpr double dmlTargetCountsPerSecond = 800.0;
    double dmlTargetPixels = 0.0;
    double dmlCameraPixels = 0.0;
    int dmlSettledMovingFrames = 0;
    for (int frame = 0; frame < 360; ++frame)
    {
        dmlTargetPixels += dmlTargetCountsPerSecond / dmlCountsPerPixel * dmlDt;
        const auto output = dmlRateController.update(
            dmlTargetPixels - dmlCameraPixels, 0.0, dmlDt,
            dmlCountsPerPixel, dmlCountsPerPixel, dmlRateSettings);
        dmlCameraPixels += output.countsX / dmlCountsPerPixel;
        if (frame >= 120 && output.settled)
            ++dmlSettledMovingFrames;
    }
    expectTrue(dmlSettledMovingFrames == 0,
               "pi controller does not settle and clear integral on a moving target");
    expectTrue(std::abs(dmlTargetPixels - dmlCameraPixels) < 5.0,
               "pi controller tracks a moving target at dml inference rate");

    // 同侧进入中心和亚像素级越心都应保留有效积分；只有反向误差扩大到稳定半径，
    // 才能判定旧积分不再代表持续目标速度并开始解卷绕。
    BasicAimController centerHoldController;
    BasicAimController::Settings centerHoldSettings = dmlRateSettings;
    for (int frame = 0; frame < 20; ++frame)
        centerHoldController.update(20.0, 0.0, dmlDt, 1.344, 1.344, centerHoldSettings);
    const auto sameSideCenter = centerHoldController.update(
        4.0, 0.0, dmlDt, 1.344, 1.344, centerHoldSettings);
    expectTrue(!sameSideCenter.settled && std::abs(sameSideCenter.integralCountsX) >= 0.5,
               "actionable integral survives same-side center entry");
    const auto crossedCenter = centerHoldController.update(
        -1.0, 0.0, dmlDt, 1.344, 1.344, centerHoldSettings);
    expectTrue(!crossedCenter.settled && crossedCenter.integralCountsX > 0.5,
               "subpixel center crossing preserves moving integral");
    const auto oppositeOutsideSettle = centerHoldController.update(
        -6.0, 0.0, dmlDt, 1.344, 1.344, centerHoldSettings);
    expectTrue(oppositeOutsideSettle.integralCountsX <= 0.0,
               "opposite error outside settle radius unwinds old integral");

    BasicAimController predictedMotionController;
    BasicAimController::Settings predictedMotionSettings = dmlRateSettings;
    for (int frame = 0; frame < 20; ++frame)
        predictedMotionController.update(
            20.0, 0.0, dmlDt, 1.344, 1.344, predictedMotionSettings);
    predictedMotionSettings.preserveMovingIntegral = true;
    const auto predictedPointCrossing = predictedMotionController.update(
        -6.0, 0.0, dmlDt, 1.344, 1.344, predictedMotionSettings);
    expectTrue(predictedPointCrossing.integralCountsX > 0.5,
               "active prediction preserves moving integral across aim-point crossing");
    predictedMotionSettings.preserveMovingIntegral = false;
    const auto predictionWithdrawn = predictedMotionController.update(
        -6.0, 0.0, dmlDt, 1.344, 1.344, predictedMotionSettings);
    expectTrue(predictionWithdrawn.integralCountsX <= 0.0,
               "withdrawing prediction restores opposite-error integral unwind");
    const auto returnedToCenter = centerHoldController.update(
        -4.0, 0.0, dmlDt, 1.344, 1.344, centerHoldSettings);
    expectTrue(!returnedToCenter.settled,
               "rapid static return remains active for the current frame");
    const auto quietAfterReturn = centerHoldController.update(
        -4.0, 0.0, dmlDt, 1.344, 1.344, centerHoldSettings);
    expectTrue(quietAfterReturn.settled,
               "unwound static overshoot settles after error stops changing");

    // 高频换向目标可能在稳定回差内部快速移动。PI 模式下误差单帧变化超过默认
    // 1.25 px 时必须立即解除锁存；静止检测的小幅量化变化仍应保持停止。
    BasicAimController fastSettleReleaseController;
    const auto initialSettle = fastSettleReleaseController.update(
        1.0, 0.0, dmlDt, 1.344, 1.344, centerHoldSettings);
    expectTrue(initialSettle.settled, "pi controller initially settles a quiet center target");
    const auto movingInsideRelease = fastSettleReleaseController.update(
        3.0, 0.0, dmlDt, 1.344, 1.344, centerHoldSettings);
    expectTrue(!movingInsideRelease.settled && movingInsideRelease.countsX > 0.0,
               "fast error motion releases pi settle latch inside hysteresis");
    expectTrue(movingInsideRelease.movingInsideSettle &&
               movingInsideRelease.errorMotion > movingInsideRelease.settleMotionThreshold,
               "fast settle release exposes auditable motion diagnostics");

    BasicAimController quietSettleController;
    quietSettleController.update(1.0, 0.0, dmlDt, 1.344, 1.344, centerHoldSettings);
    const auto detectorJitter = quietSettleController.update(
        1.5, 0.0, dmlDt, 1.344, 1.344, centerHoldSettings);
    expectTrue(detectorJitter.settled,
               "subpixel detector jitter keeps pi controller settled");

    // 积分候选必须保持静止闭环安全：大误差阶段受设备限速时禁止 wind-up，
    // 接近中心后进入既有 5 px 稳定半径并清空积分，不得跨越目标持续反向输出。
    BasicAimController staticIntegralController;
    BasicAimController::Settings staticIntegralSettings;
    staticIntegralSettings.responseSeconds = 0.080;
    staticIntegralSettings.maxCountsPerSecond = 1440.0;
    staticIntegralSettings.integralTimeSeconds = 0.320;
    double staticCameraPixels = 0.0;
    bool staticSettled = false;
    for (int frame = 0; frame < 480; ++frame)
    {
        const auto output = staticIntegralController.update(
            140.0 - staticCameraPixels, 0.0, 1.0 / 120.0,
            1.344, 1.344, staticIntegralSettings);
        staticCameraPixels += output.countsX / 1.344;
        staticSettled = staticSettled || output.settled;
    }
    expectTrue(staticSettled, "pi controller settles a static step");
    expectTrue(std::abs(140.0 - staticCameraPixels) <= 5.0,
               "pi controller keeps static step inside settle radius");

    if (failures != 0)
    {
        std::cerr << failures << " basic algorithm test(s) failed\n";
        return 1;
    }

    std::cout << "basic algorithm tests passed\n";
    return 0;
}
