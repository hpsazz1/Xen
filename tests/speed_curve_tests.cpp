#include "runtime/basic_aim_controller.h"
#include "runtime/conditional_speed_budget.h"
#include "runtime/control_interval_tracker.h"
#include "runtime/command_cancellation_epoch.h"
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
#include "runtime/tracker_timing.h"
#include "runtime/passive_profile_calibrator.h"
#include "runtime/build_identity.h"
#include "runtime/shadow_response_replay.h"
#include "runtime/physical_response_probe.h"

#include <cmath>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <queue>
#include <string>
#include <thread>

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
    expectTrue(trackerStaleTimeoutMs(240) == 100 &&
               trackerStaleTimeoutMs(68) == 100 &&
               trackerStaleTimeoutMs(20) == 200 &&
               trackerStaleTimeoutMs(0) == 100,
               "tracker stale timeout follows inference publications and preserves prediction gap boundary");

    // r57 条件预算只服务于 jump 的高速严重落后尾部：普通移动、反向、
    // 自运动伪影或尚未锁定方向时都必须维持基础 3200 counts/s。
    ConditionalSpeedBudget speedBudget;
    ConditionalSpeedBudget::Settings speedBudgetSettings;
    const auto speedBudgetBase = ConditionalSpeedBudget::TimePoint(
        std::chrono::seconds(1));
    auto budgetOutput = speedBudget.update(
        59.0, 2000.0, true, true, false,
        speedBudgetBase, speedBudgetSettings);
    expectTrue(!budgetOutput.active && budgetOutput.maxCountsPerSecond == 3200.0,
               "conditional speed budget rejects error below entry threshold");
    budgetOutput = speedBudget.update(
        70.0, -2000.0, true, true, false,
        speedBudgetBase + std::chrono::milliseconds(1), speedBudgetSettings);
    expectTrue(!budgetOutput.active,
               "conditional speed budget rejects velocity moving toward the aim point");
    budgetOutput = speedBudget.update(
        70.0, 2000.0, true, true, true,
        speedBudgetBase + std::chrono::milliseconds(2), speedBudgetSettings);
    expectTrue(!budgetOutput.active,
               "conditional speed budget rejects safety-suppressed prediction");
    budgetOutput = speedBudget.update(
        70.0, 2000.0, true, true, false,
        speedBudgetBase + std::chrono::milliseconds(3), speedBudgetSettings);
    expectTrue(budgetOutput.active && budgetOutput.entered &&
               budgetOutput.maxCountsPerSecond == 4000.0,
               "conditional speed budget enters on reliable high-speed severe lag");
    budgetOutput = speedBudget.update(
        40.0, 900.0, false, true, false,
        speedBudgetBase + std::chrono::milliseconds(60), speedBudgetSettings);
    expectTrue(budgetOutput.active && !budgetOutput.entered,
               "conditional speed budget uses exit hysteresis after transient evidence clears");
    budgetOutput = speedBudget.update(
        32.0, 900.0, false, true, false,
        speedBudgetBase + std::chrono::milliseconds(70), speedBudgetSettings);
    expectTrue(!budgetOutput.active && budgetOutput.exited &&
               budgetOutput.maxCountsPerSecond == 3200.0,
               "conditional speed budget exits when lag reaches the recovery boundary");
    budgetOutput = speedBudget.update(
        80.0, 2200.0, true, true, false,
        speedBudgetBase + std::chrono::milliseconds(100), speedBudgetSettings);
    expectTrue(!budgetOutput.active,
               "conditional speed budget blocks immediate re-entry during cooldown");
    budgetOutput = speedBudget.update(
        80.0, 2200.0, true, true, false,
        speedBudgetBase + std::chrono::milliseconds(271), speedBudgetSettings);
    expectTrue(budgetOutput.active,
               "conditional speed budget can re-enter after cooldown");
    budgetOutput = speedBudget.update(
        80.0, 2200.0, false, true, false,
        speedBudgetBase + std::chrono::milliseconds(391), speedBudgetSettings);
    expectTrue(!budgetOutput.active && budgetOutput.exited,
               "conditional speed budget enforces the maximum active window");
    speedBudget.reset();
    speedBudgetSettings.catchUpMaxCountsPerSecond = 3200.0;
    budgetOutput = speedBudget.update(
        90.0, 2500.0, true, true, false,
        speedBudgetBase + std::chrono::seconds(1), speedBudgetSettings);
    expectTrue(!budgetOutput.active && budgetOutput.maxCountsPerSecond == 3200.0,
               "conditional speed budget is disabled when catch-up cap equals base cap");

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
    // 该组测试验证估计器与控制器本身；显式使用L3倍率，避免机器Profile降级策略
    // 把前馈关闭后掩盖原有控制器契约。
    shadowDiagnostics.machineProfileLevel = 3;
    shadowDiagnostics.machineProfileCacheMatched = true;
    shadowDiagnostics.machineProfileFeedforwardScale = 1.0;
    shadowDiagnostics.machineProfileIntegralEnabled = true;
    shadowDiagnostics.machineProfileReason = "unit_test_exact_match";
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
    expectNear(kalmanFrame.estimate.angleX,
               kalmanFrame.baselineEstimate.angleX, 0.0,
               "default shadow estimator remains exactly equal to the frozen Kalman");
    expectTrue(kalmanFrame.control.valid &&
               kalmanFrame.control.feedbackX > 0.0 &&
               kalmanFrame.trajectoryRequest.requestedCountsX > 0.0 &&
               kalmanFrame.commandSuppressed,
               "P0-4A produces an auditable P-only request without enabling device output");

    AimPipelineRuntime degradedPolicyPipeline;
    degradedPolicyPipeline.configure("shadow");
    LosAimController::Settings degradedControllerSettings;
    degradedControllerSettings.responseSeconds = 0.100;
    degradedControllerSettings.maxCountsPerSecond = 100000.0;
    degradedControllerSettings.feedforwardGain = 1.0;
    degradedControllerSettings.integralTimeSeconds = 0.200;
    degradedControllerSettings.integralZoneDegrees = 5.0;
    degradedPolicyPipeline.configureController(degradedControllerSettings);
    AimObservation degradedObservation = shadowObservation;
    ViewMotionShadowDiagnostics degradedDiagnostics = shadowDiagnostics;
    degradedDiagnostics.machineProfileLevel = 2;
    degradedDiagnostics.machineProfileCacheMatched = false;
    degradedDiagnostics.machineProfileFeedforwardScale = 0.25;
    degradedDiagnostics.machineProfileIntegralEnabled = false;
    degradedDiagnostics.machineProfileReason = "unit_test_cache_miss";
    degradedDiagnostics.stabilizedLosYawDegrees = 0.10;
    degradedObservation.timing.observationTime = timingBase + std::chrono::milliseconds(40);
    degradedObservation.timing.controlTime = timingBase + std::chrono::milliseconds(50);
    degradedPolicyPipeline.observe(degradedObservation);
    degradedPolicyPipeline.setViewMotionDiagnostics(degradedDiagnostics);
    degradedDiagnostics.stabilizedLosYawDegrees = 0.20;
    degradedObservation.timing.observationTime += std::chrono::milliseconds(10);
    degradedObservation.timing.controlTime += std::chrono::milliseconds(10);
    degradedPolicyPipeline.observe(degradedObservation);
    degradedPolicyPipeline.setViewMotionDiagnostics(degradedDiagnostics);
    const AimPipelineFrameState degradedPolicyFrame = degradedPolicyPipeline.snapshot();
    const double expectedDegradedFeedforward =
        degradedPolicyFrame.estimate.rateX * 0.010 *
        degradedPolicyFrame.estimate.feedforwardConfidence * 0.25 /
        degradedDiagnostics.degreesPerCountX;
    expectNear(degradedPolicyFrame.control.trackingFeedforwardX,
               expectedDegradedFeedforward, 1e-9,
               "Level 2 applies the machine-profile feedforward confidence ceiling");
    expectNear(degradedPolicyFrame.control.integralCountsX, 0.0, 1e-12,
               "Level 2 disables controller integral even when configured globally");
    expectTrue(kalmanFrame.trajectoryOutput.outputProduced &&
               kalmanFrame.trajectoryOutput.commandSuppressed,
               "P0-4B pass-through produces diagnostics without enabling device output");
    expectNear(kalmanFrame.trajectoryOutput.shapedCountsX,
               kalmanFrame.trajectoryRequest.requestedCountsX, 1e-12,
               "P0-4B off mode is exactly equal to the controller request");
    expectTrue(shadowFrame.observation.trackId == 7 &&
               shadowFrame.observationSequence == 1,
               "shadow pipeline preserves target identity and observation sequence");

    AimObservation pausedShadowObservation = shadowObservation;
    pausedShadowObservation.outputPaused = true;
    pausedShadowObservation.timing.observationTime += std::chrono::milliseconds(10);
    pausedShadowObservation.timing.controlTime += std::chrono::milliseconds(10);
    const uint64_t generationBeforePause = newPipeline.resetGeneration();
    newPipeline.suspendOutput();
    expectTrue(newPipeline.snapshot().resetGeneration == generationBeforePause &&
                   newPipeline.snapshot().observationSequence == 1,
               "suspending formal output preserves the independent shadow generation");
    const AimPipelineFrameState pausedShadowFrame =
        newPipeline.observe(pausedShadowObservation);
    expectTrue(pausedShadowFrame.shadowProcessed &&
                   pausedShadowFrame.observation.outputPaused &&
                   pausedShadowFrame.observationSequence == 2 &&
                   pausedShadowFrame.resetGeneration == generationBeforePause &&
                   pausedShadowFrame.commandSuppressed &&
                   pausedShadowFrame.trajectoryOutput.commandSuppressed,
               "paused output keeps shadow observations continuous and doubly suppressed");
    AimObservation resumedShadowObservation = pausedShadowObservation;
    resumedShadowObservation.outputPaused = false;
    resumedShadowObservation.timing.observationTime += std::chrono::milliseconds(10);
    resumedShadowObservation.timing.controlTime += std::chrono::milliseconds(10);
    const AimPipelineFrameState resumedShadowFrame =
        newPipeline.observe(resumedShadowObservation);
    expectTrue(!resumedShadowFrame.observation.outputPaused &&
                   resumedShadowFrame.observationSequence == 3 &&
                   resumedShadowFrame.resetGeneration == generationBeforePause,
               "resuming output continues the same shadow target generation");

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

    AimPipelineRuntime maneuverPipeline;
    maneuverPipeline.configure("shadow");
    ManeuverLosEstimator::Settings maneuverSettings;
    maneuverSettings.mode =
        ManeuverLosEstimatorMode::ManeuverGatedConstantAcceleration;
    maneuverSettings.jerkStdDegreesPerSecond3 = 8000.0;
    maneuverSettings.maneuverRateThresholdDegreesPerSecond = 1.0;
    maneuverSettings.maneuverHoldSeconds = 0.120;
    maneuverPipeline.configureEstimator(maneuverSettings);
    AimObservation maneuverObservation = shadowObservation;
    ViewMotionShadowDiagnostics maneuverDiagnostics = shadowDiagnostics;
    for (int sample = 0; sample <= 20; ++sample)
    {
        maneuverObservation.timing.observationTime =
            timingBase + std::chrono::seconds(1) +
            std::chrono::milliseconds(sample * 10);
        maneuverObservation.timing.controlTime =
            maneuverObservation.timing.observationTime +
            std::chrono::milliseconds(10);
        maneuverDiagnostics.stabilizedLosYawDegrees = sample * 0.05;
        maneuverDiagnostics.stabilizedLosPitchDownDegrees = 0.0;
        maneuverPipeline.observe(maneuverObservation);
        maneuverPipeline.setViewMotionDiagnostics(maneuverDiagnostics);
    }
    const auto activeManeuverFrame = maneuverPipeline.snapshot();
    expectTrue(activeManeuverFrame.maneuverEstimator.maneuverModelActive &&
                   activeManeuverFrame.maneuverEstimator.selectionCount == 1 &&
                   activeManeuverFrame.baselineEstimate.valid &&
                   activeManeuverFrame.constantAccelerationEstimate.valid &&
                   activeManeuverFrame.commandSuppressed &&
                   activeManeuverFrame.trajectoryOutput.commandSuppressed,
               "DML maneuver candidate selects an auditable model without enabling output");
    maneuverPipeline.suspendOutput();
    maneuverObservation.outputPaused = true;
    maneuverObservation.timing.observationTime += std::chrono::milliseconds(10);
    maneuverObservation.timing.controlTime += std::chrono::milliseconds(10);
    maneuverDiagnostics.stabilizedLosYawDegrees += 0.05;
    maneuverPipeline.observe(maneuverObservation);
    maneuverPipeline.setViewMotionDiagnostics(maneuverDiagnostics);
    const auto pausedManeuverFrame = maneuverPipeline.snapshot();
    expectTrue(pausedManeuverFrame.observation.outputPaused &&
                   pausedManeuverFrame.maneuverEstimator.maneuverModelActive &&
                   pausedManeuverFrame.maneuverEstimator.selectionCount == 1,
               "output pause preserves maneuver estimator state and model selection");

    AimPipelineRuntime staticManeuverPipeline;
    staticManeuverPipeline.configure("shadow");
    maneuverSettings.maneuverRateThresholdDegreesPerSecond = 12.0;
    staticManeuverPipeline.configureEstimator(maneuverSettings);
    for (int sample = 0; sample <= 20; ++sample)
    {
        maneuverObservation.outputPaused = false;
        maneuverObservation.timing.observationTime =
            timingBase + std::chrono::seconds(2) +
            std::chrono::milliseconds(sample * 10);
        maneuverObservation.timing.controlTime =
            maneuverObservation.timing.observationTime +
            std::chrono::milliseconds(10);
        maneuverDiagnostics.stabilizedLosYawDegrees = 0.25;
        staticManeuverPipeline.observe(maneuverObservation);
        staticManeuverPipeline.setViewMotionDiagnostics(maneuverDiagnostics);
    }
    expectTrue(!staticManeuverPipeline.snapshot().maneuverEstimator.maneuverModelActive &&
                   staticManeuverPipeline.snapshot().maneuverEstimator.selectionCount == 0,
               "static shadow observations never enter the maneuver model");

    // 同一观测时间线必须能够区分“响应核正确”和“模型相位错误”，并允许X/Y使用
    // 不同响应而不复制估计器实现。静止世界角固定为0，屏幕LOS等于真实相机角的反号。
    ShadowResponseReplay::Timeline responseTimeline;
    responseTimeline.source = "synthetic_static.csv";
    responseTimeline.recordedCenterMs = 20.0;
    responseTimeline.recordedWidthMs = 0.0;
    responseTimeline.commands.push_back({
        1, 1'000'000'000, 100, 100, 0.03, 0.02 });
    for (int sampleIndex = 0; sampleIndex < 30; ++sampleIndex)
    {
        const std::int64_t observationNs =
            1'000'000'000 + sampleIndex * 10'000'000LL;
        ShadowResponseReplay::Sample sample;
        sample.observationTimeNs = observationNs;
        sample.controlTimeNs = observationNs + 1'000'000;
        sample.resetGeneration = 1;
        sample.targetId = 7;
        sample.confidence = 1.0f;
        sample.measuredYawDegrees = sampleIndex >= 2 ? -3.0 : 0.0;
        sample.measuredPitchDownDegrees = sampleIndex >= 3 ? -2.0 : 0.0;
        responseTimeline.samples.push_back(sample);
    }
    ManeuverLosEstimator::Settings replaySettings;
    replaySettings.mode =
        ManeuverLosEstimatorMode::ManeuverGatedConstantAcceleration;
    replaySettings.jerkStdDegreesPerSecond3 = 8000.0;
    replaySettings.maneuverRateThresholdDegreesPerSecond = 12.0;
    replaySettings.maneuverHoldSeconds = 0.120;
    const ShadowResponseReplay::Candidate matchedSplitResponse{
        { 20.0, 0.0 }, { 30.0, 0.0 } };
    const auto matchedResponseMetrics = ShadowResponseReplay::Evaluate(
        responseTimeline, matchedSplitResponse, replaySettings, 0.03, 0.02, true);
    expectTrue(matchedResponseMetrics.runningActiveSamples == 0 &&
                   matchedResponseMetrics.recordedSelectionMismatches == 0,
               "shadow response replay preserves stationary LOS for the matching split-axis kernel");
    const ShadowResponseReplay::Candidate wrongSharedResponse{
        { 60.0, 0.0 }, { 60.0, 0.0 } };
    const auto wrongResponseMetrics = ShadowResponseReplay::Evaluate(
        responseTimeline, wrongSharedResponse, replaySettings, 0.03, 0.02, false);
    expectTrue(wrongResponseMetrics.runningActiveSamples > 0 &&
                   wrongResponseMetrics.runningActiveSegments > 0,
               "shadow response replay exposes a phase-mismatched static response");
    const auto matchedPhysicalFit = ShadowResponseReplay::FitPhysicalResponse(
        responseTimeline, matchedSplitResponse, 0.03, 0.02, 150.0, 150.0);
    const auto wrongPhysicalFit = ShadowResponseReplay::FitPhysicalResponse(
        responseTimeline, wrongSharedResponse, 0.03, 0.02, 150.0, 150.0);
    expectTrue(matchedPhysicalFit.anchoredSegments == 1 &&
                   matchedPhysicalFit.responseSamples >= 10,
               "physical response fit requires a quiet anchor and resolved response samples");
    expectNear(matchedPhysicalFit.scoreDegrees, 0.0, 1e-9,
               "matching split-axis physical response stabilizes a fixed world target");
    expectTrue(wrongPhysicalFit.scoreDegrees > 1.0 &&
                   wrongPhysicalFit.scoreDegrees > matchedPhysicalFit.scoreDegrees,
               "physical response fit rejects a phase-mismatched shared step response");
    ShadowResponseReplay::Timeline segmentedPhysicalTimeline = responseTimeline;
    for (int sampleIndex = 30; sampleIndex < 50; ++sampleIndex)
    {
        const std::int64_t observationNs =
            1'000'000'000 + sampleIndex * 10'000'000LL;
        ShadowResponseReplay::Sample sample;
        sample.observationTimeNs = observationNs;
        sample.controlTimeNs = observationNs + 1'000'000;
        sample.resetGeneration = 1;
        sample.targetId = 7;
        sample.confidence = 1.0f;
        sample.outputPaused = true;
        // 暂停时的人工换点不是设备命令响应，必须完全排除。
        sample.measuredYawDegrees = 20.0 + sampleIndex;
        sample.measuredPitchDownDegrees = -20.0 - sampleIndex;
        segmentedPhysicalTimeline.samples.push_back(sample);
    }
    segmentedPhysicalTimeline.commands.push_back({
        2, 1'500'000'000, -100, -100, 0.03, 0.02 });
    for (int sampleIndex = 50; sampleIndex < 80; ++sampleIndex)
    {
        const std::int64_t observationNs =
            1'000'000'000 + sampleIndex * 10'000'000LL;
        ShadowResponseReplay::Sample sample;
        sample.observationTimeNs = observationNs;
        sample.controlTimeNs = observationNs + 1'000'000;
        sample.resetGeneration = 1;
        sample.targetId = 7;
        sample.confidence = 1.0f;
        const bool xApplied = sampleIndex >= 52;
        const bool yApplied = sampleIndex >= 53;
        sample.measuredYawDegrees = 5.0 - (xApplied ? 0.0 : 3.0);
        sample.measuredPitchDownDegrees = -4.0 - (yApplied ? 0.0 : 2.0);
        segmentedPhysicalTimeline.samples.push_back(sample);
    }
    const auto segmentedPhysicalFit = ShadowResponseReplay::FitPhysicalResponse(
        segmentedPhysicalTimeline, matchedSplitResponse, 0.03, 0.02, 150.0, 150.0);
    expectTrue(segmentedPhysicalFit.anchoredSegments == 2 &&
                   segmentedPhysicalFit.responseSamples >= 20,
               "physical response fit splits repeated running trials within one target identity");
    expectNear(segmentedPhysicalFit.scoreDegrees, 0.0, 1e-9,
               "physical response fit excludes paused manual reposition observations");

    ManeuverLosEstimator unguardedEvidenceEstimator;
    ManeuverLosEstimator guardedEvidenceEstimator;
    ManeuverLosEstimator::Settings unguardedEvidenceSettings = replaySettings;
    ManeuverLosEstimator::Settings guardedEvidenceSettings = replaySettings;
    guardedEvidenceSettings.maneuverRateUncertaintyXDegreesPerSecond = 60.0;
    for (int sampleIndex = 0; sampleIndex < 20; ++sampleIndex)
    {
        const auto observationTime = ManeuverLosEstimator::TimePoint(
            std::chrono::seconds(5) + std::chrono::milliseconds(sampleIndex * 10));
        const auto controlTime = observationTime + std::chrono::milliseconds(1);
        const double angle = sampleIndex * 0.5;
        unguardedEvidenceEstimator.update(
            angle, 0.0, 1.0f, observationTime, controlTime,
            unguardedEvidenceSettings);
        guardedEvidenceEstimator.update(
            angle, 0.0, 1.0f, observationTime, controlTime,
            guardedEvidenceSettings);
    }
    expectTrue(unguardedEvidenceEstimator.diagnostics().maneuverModelActive &&
                   !guardedEvidenceEstimator.diagnostics().maneuverModelActive &&
                   guardedEvidenceEstimator.diagnostics().
                       maneuverRateEvidenceDegreesPerSecond < 12.0,
               "command-response uncertainty discounts explainable rate without changing the physical threshold");

    const char* responseCsvPath = "xen_shadow_response_replay_test.csv";
    {
        std::ofstream responseCsv(responseCsvPath);
        responseCsv << "BackendReceiveNs,ControlTimeNs,AimPipelineResetGeneration,"
            "AimPipelineTargetId,MeasuredLosYawDegrees,MeasuredLosPitchDownDegrees,"
            "AimPipelineConfidence,AimPipelineOutputPaused,Settled,"
            "AimPipelineManeuverModelActive,CommandToFrameDelayMs,CommandResponseMs,"
            "CommandSendSucceeded,CommandSequence,CommandDeviceSendNs,"
            "CommandAppliedCountsX,CommandAppliedCountsY,DegreesPerCountX,DegreesPerCountY\n"
            "1000000000,1001000000,1,7,0,0,1,0,0,0,20,0,1,1,1000000000,"
            "100,100,0.03,0.02\n";
    }
    ShadowResponseReplay::Timeline parsedResponseTimeline;
    std::string responseCsvError;
    expectTrue(ShadowResponseReplay::LoadTimelineCsv(
                   responseCsvPath, parsedResponseTimeline, responseCsvError) &&
                   parsedResponseTimeline.samples.size() == 1 &&
                   parsedResponseTimeline.commands.size() == 1,
               "shadow response replay parses the named CSV timing and command contract");
    std::remove(responseCsvPath);

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

    RelativeLosKalman delayedConstantVelocityKalman;
    for (int sample = 0; sample <= 30; ++sample)
    {
        const auto observationTime = kalmanStart + std::chrono::milliseconds(sample * 10);
        const auto delayedControlTime = observationTime + std::chrono::milliseconds(40);
        delayedConstantVelocityKalman.update(
            sample * 0.05, sample * -0.02, 1.0f,
            observationTime, delayedControlTime);
    }
    const RelativeLosKalmanEstimate delayedConstantVelocityEstimate =
        delayedConstantVelocityKalman.estimate();
    expectNear(delayedConstantVelocityEstimate.x.rateDegreesPerSecond, 5.0, 0.35,
               "control projection does not feed delay back into persistent Kalman rate state");
    expectNear(delayedConstantVelocityEstimate.x.angleDegrees, 1.70, 0.04,
               "forty-millisecond control projection advances only the published angle");
    expectNear(delayedConstantVelocityEstimate.y.angleDegrees, -0.68, 0.04,
               "delayed vertical projection preserves the observation-time posterior");

    RelativeLosKalman stationaryNoiseKalman;
    RelativeLosKalman movingNoiseKalman;
    RelativeLosKalman::Settings adaptiveKalmanSettings;
    for (int sample = 0; sample <= 30; ++sample)
    {
        const auto sampleTime = kalmanStart + std::chrono::milliseconds(sample * 10);
        stationaryNoiseKalman.update(
            0.0, 0.0, 1.0f, sampleTime, sampleTime, adaptiveKalmanSettings);
        movingNoiseKalman.update(
            sample * 0.10, 0.0, 1.0f,
            sampleTime, sampleTime, adaptiveKalmanSettings);
    }
    expectTrue(movingNoiseKalman.estimate().x.angleVariance >
                   stationaryNoiseKalman.estimate().x.angleVariance,
               "adaptive Kalman process noise preserves more uncertainty for moving targets");
    expectNear(movingNoiseKalman.estimate().x.rateDegreesPerSecond, 10.0, 0.45,
               "adaptive Kalman process noise tracks moving-target angular rate");

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
    LosAimController guardedSettleController;
    LosAimController::Input guardedSettleInput = quietInput;
    guardedSettleInput.settleEntryErrorValid = true;
    guardedSettleInput.settleEntryErrorDegreesX = 0.20;
    guardedSettleInput.settleEntryErrorDegreesY = 0.0;
    const auto guardedFirst = guardedSettleController.update(
        guardedSettleInput, settleSettings);
    const auto guardedSecond = guardedSettleController.update(
        guardedSettleInput, settleSettings);
    expectTrue(!guardedFirst.settled && !guardedSecond.settled &&
                   guardedSecond.settleConfirmationSamples == 0 &&
                   guardedSecond.limitedCountsX != 0.0,
               "committed command endpoint blocks settle entry without replacing feedback error");
    guardedSettleInput.settleEntryErrorDegreesX = quietInput.errorDegreesX;
    guardedSettleController.update(guardedSettleInput, settleSettings);
    const auto guardedConfirmed = guardedSettleController.update(
        guardedSettleInput, settleSettings);
    expectTrue(guardedConfirmed.settled,
               "settle entry guard admits two quiet committed endpoints");
    LosAimController currentErrorGuardController;
    guardedSettleInput.errorDegreesX = 0.20;
    guardedSettleInput.settleEntryErrorDegreesX = 0.05;
    const auto currentErrorGuardFirst = currentErrorGuardController.update(
        guardedSettleInput, settleSettings);
    const auto currentErrorGuardSecond = currentErrorGuardController.update(
        guardedSettleInput, settleSettings);
    expectTrue(!currentErrorGuardFirst.settled &&
                   !currentErrorGuardSecond.settled &&
                   currentErrorGuardSecond.settleConfirmationSamples == 0,
               "quiet committed endpoint cannot replace an out-of-band current error");
    guardedSettleInput.errorDegreesX = quietInput.errorDegreesX;
    LosAimController heldSettleController;
    guardedSettleInput.settleEntryErrorDegreesX = 0.20;
    guardedSettleInput.holdOutputWhileSettleEntryBlocked = true;
    const auto heldSettleEntry = heldSettleController.update(
        guardedSettleInput, settleSettings);
    expectTrue(heldSettleEntry.settleEntryCommandHeld &&
                   !heldSettleEntry.settled &&
                   heldSettleEntry.settleConfirmationSamples == 0 &&
                   heldSettleEntry.limitedCountsX == 0.0,
               "settle entry command hold waits for committed motion without latching settle");
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

    LosAimController widenedReverseBandController;
    LosAimController::Settings widenedReverseBandSettings = reverseConfirmSettings;
    widenedReverseBandSettings.reverseConfirmationErrorMultiplier = 1.75;
    reverseConfirmInput.errorDegreesX = 0.10;
    widenedReverseBandController.update(reverseConfirmInput, widenedReverseBandSettings);
    reverseConfirmInput.errorDegreesX = -0.13;
    const auto widenedBandReverse = widenedReverseBandController.update(
        reverseConfirmInput, widenedReverseBandSettings);
    expectTrue(widenedBandReverse.lowSpeedReverseSuppressed &&
               widenedBandReverse.limitedCountsX == 0.0,
               "independent 1.75x error band confirms low-speed reverse beyond settle release");

    LosAimController independentSettleReleaseController;
    reverseConfirmInput.errorDegreesX = 0.05;
    independentSettleReleaseController.update(
        reverseConfirmInput, widenedReverseBandSettings);
    independentSettleReleaseController.update(
        reverseConfirmInput, widenedReverseBandSettings);
    reverseConfirmInput.errorDegreesX = 0.13;
    const auto independentSettleRelease = independentSettleReleaseController.update(
        reverseConfirmInput, widenedReverseBandSettings);
    expectTrue(independentSettleRelease.settleReleased &&
               !independentSettleRelease.settled &&
               independentSettleRelease.limitedCountsX > 0.0,
               "wider reverse confirmation band does not delay fixed 1.5x settle release");

    LosAimController confirmedSettleReleaseController;
    LosAimController::Settings confirmedSettleReleaseSettings = reverseConfirmSettings;
    confirmedSettleReleaseSettings.confirmLowSpeedReverseSettleRelease = true;
    reverseConfirmInput.errorDegreesX = 0.10;
    reverseConfirmInput.relativeLosRateDegreesPerSecondX = 0.0;
    confirmedSettleReleaseController.update(
        reverseConfirmInput, confirmedSettleReleaseSettings);
    reverseConfirmInput.errorDegreesX = 0.05;
    confirmedSettleReleaseController.update(
        reverseConfirmInput, confirmedSettleReleaseSettings);
    confirmedSettleReleaseController.update(
        reverseConfirmInput, confirmedSettleReleaseSettings);
    reverseConfirmInput.errorDegreesX = -0.13;
    for (int sample = 0; sample < 7; ++sample)
    {
        const auto heldRelease = confirmedSettleReleaseController.update(
            reverseConfirmInput, confirmedSettleReleaseSettings);
        expectTrue(heldRelease.settled && !heldRelease.settleReleased &&
                   heldRelease.lowSpeedReverseSuppressed &&
                   heldRelease.limitedCountsX == 0.0,
                   "low-speed reverse settle release waits for persistent error");
    }
    const auto confirmedSettleRelease = confirmedSettleReleaseController.update(
        reverseConfirmInput, confirmedSettleReleaseSettings);
    expectTrue(!confirmedSettleRelease.settled &&
               confirmedSettleRelease.settleReleased &&
               confirmedSettleRelease.limitedCountsX < 0.0,
               "persistent low-speed reverse error releases settle after confirmation");

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

    LosAimController reversalFeedforwardController;
    LosAimController::Settings reversalFeedforwardSettings = settleSettings;
    reversalFeedforwardSettings.feedforwardGain = 0.16;
    reversalFeedforwardSettings.reversalFeedforwardBoost = 0.08;
    reversalFeedforwardSettings.reversalFeedforwardSeconds = 0.080;
    LosAimController::Input reversalFeedforwardInput = losInput;
    reversalFeedforwardInput.relativeLosRateDegreesPerSecondX = 2.0;
    const auto initialMovingDirection = reversalFeedforwardController.update(
        reversalFeedforwardInput, reversalFeedforwardSettings);
    expectTrue(!initialMovingDirection.reversalDetected &&
               !initialMovingDirection.reversalFeedforwardActive,
               "initial reliable movement establishes direction without a false reversal");
    reversalFeedforwardInput.relativeLosRateDegreesPerSecondX = -2.0;
    const auto reliableReversal = reversalFeedforwardController.update(
        reversalFeedforwardInput, reversalFeedforwardSettings);
    expectTrue(reliableReversal.reversalDetected &&
               reliableReversal.reversalFeedforwardActive,
               "reliable horizontal rate sign change activates reversal feedforward");
    expectNear(reliableReversal.effectiveFeedforwardGainX, 0.24, 1e-12,
               "reversal feedforward adds its bounded gain only on the horizontal axis");
    for (int sample = 0; sample < 7; ++sample)
    {
        const auto activeWindow = reversalFeedforwardController.update(
            reversalFeedforwardInput, reversalFeedforwardSettings);
        expectTrue(activeWindow.reversalFeedforwardActive,
                   "reversal feedforward remains active for the configured real-time window");
    }
    const auto afterReversalWindow = reversalFeedforwardController.update(
        reversalFeedforwardInput, reversalFeedforwardSettings);
    expectTrue(!afterReversalWindow.reversalFeedforwardActive &&
               afterReversalWindow.effectiveFeedforwardGainX == 0.16,
               "reversal feedforward returns to the baseline gain after the window");

    LosAimController lowRateReversalController;
    lowRateReversalController.update(reversalFeedforwardInput, reversalFeedforwardSettings);
    reversalFeedforwardInput.relativeLosRateDegreesPerSecondX = 0.79;
    const auto lowRateSignNoise = lowRateReversalController.update(
        reversalFeedforwardInput, reversalFeedforwardSettings);
    expectTrue(!lowRateSignNoise.reversalDetected &&
               !lowRateSignNoise.reversalFeedforwardActive,
               "sub-settle-rate sign noise cannot activate reversal feedforward");
    lowRateReversalController.reset();
    reversalFeedforwardInput.relativeLosRateDegreesPerSecondX = 2.0;
    const auto firstDirectionAfterReset = lowRateReversalController.update(
        reversalFeedforwardInput, reversalFeedforwardSettings);
    expectTrue(!firstDirectionAfterReset.reversalDetected,
               "controller reset clears the accepted moving direction");

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

    std::queue<int> preservedFrames;
    expectTrue(AppendBoundedFrame(preservedFrames, 1, 3) == 0 &&
               AppendBoundedFrame(preservedFrames, 2, 3) == 0 &&
               AppendBoundedFrame(preservedFrames, 3, 3) == 0 &&
               AppendBoundedFrame(preservedFrames, 4, 3) == 1,
               "bounded diagnostic queue reports capacity loss");
    int oldestValue = 0;
    expectTrue(TakeOldestFrame(preservedFrames, oldestValue) && oldestValue == 2,
               "bounded diagnostic queue preserves receive order");

    std::vector<PhysicalResponseSample> physicalSamples;
    constexpr int64_t commandNs = 1'000'000'000LL;
    for (int frame = -72; frame <= 120; ++frame)
    {
        const double timeMs = frame * (1000.0 / 240.0);
        const double normalized = timeMs <= 20.0 ? 0.0
            : std::clamp((timeMs - 20.0) / 40.0, 0.0, 1.0);
        PhysicalResponseSample sample;
        sample.receiveNs = commandNs + static_cast<int64_t>(timeMs * 1e6);
        sample.displacementX = normalized * 8.0;
        sample.displacementY = normalized * 0.4;
        sample.valid = true;
        sample.trackingQuality = 1.0;
        physicalSamples.push_back(sample);
    }
    const auto physicalSummary = AnalyzePhysicalResponse(
        physicalSamples, commandNs, true, 300, 500);
    expectTrue(physicalSummary.valid, "physical response quantiles accept complete 240hz pulse");
    expectNear(physicalSummary.t10Ms, 24.0, 1.0, "physical response t10 interpolation");
    expectNear(physicalSummary.t50Ms, 40.0, 1.0, "physical response t50 interpolation");
    expectNear(physicalSummary.t90Ms, 56.0, 1.0, "physical response t90 interpolation");
    expectNear(physicalSummary.t99Ms, 61.5, 1.0, "physical response t99 interpolation");
    expectNear(physicalSummary.orthogonalFinalDisplacement, 0.4, 1e-9,
               "physical response preserves orthogonal-axis leakage diagnostics");

    cv::Mat trackerFrame = cv::Mat::zeros(160, 160, CV_8UC3);
    cv::RNG trackerRandom(63);
    trackerRandom.fill(trackerFrame(cv::Rect(35, 35, 85, 85)), cv::RNG::UNIFORM, 0, 256);
    PhysicalResponseTracker physicalTracker;
    std::string trackerReason;
    expectTrue(physicalTracker.initialize(trackerFrame, cv::Rect(35, 35, 85, 85), trackerReason),
               "physical response tracker initializes high contrast roi");
    cv::Mat shiftedTrackerFrame;
    const cv::Mat trackerTransform =
        (cv::Mat_<double>(2, 3) << 1.0, 0.0, 4.0, 0.0, 1.0, -3.0);
    cv::warpAffine(trackerFrame, shiftedTrackerFrame, trackerTransform, trackerFrame.size());
    const auto trackedShift = physicalTracker.update(shiftedTrackerFrame, commandNs);
    expectTrue(trackedShift.valid && trackedShift.trackingQuality >= 0.5,
               "physical response tracker keeps normalized template correlation");
    expectNear(trackedShift.displacementX, 4.0, 0.25, "physical response tracker x displacement");
    expectNear(trackedShift.displacementY, -3.0, 0.25, "physical response tracker y displacement");

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

    AppliedViewMotionModel rampedAppliedViewModel;
    rampedAppliedViewModel.configure(20.0, 20.0);
    rampedAppliedViewModel.addCommand(10, -4, 0.03, 0.02, commandStart);
    expectNear(rampedAppliedViewModel.at(
                   commandStart + std::chrono::milliseconds(9)).first,
               0.0, 1e-12,
               "finite view response does not start before the measured lower boundary");
    expectNear(rampedAppliedViewModel.at(
                   commandStart + std::chrono::milliseconds(20)).first,
               0.15, 1e-12,
               "finite view response applies half the command at the response center");
    expectNear(rampedAppliedViewModel.at(
                   commandStart + std::chrono::milliseconds(30)).first,
               0.30, 1e-12,
               "finite view response completes at the measured upper boundary");
    expectNear(rampedAppliedViewModel.commandResponseMs(), 20.0, 0.0,
               "finite view model reports its configured response width");
    expectNear(rampedAppliedViewModel.rateAt(
                   commandStart + std::chrono::milliseconds(20)).first,
               15.0, 1e-12,
               "finite view model exposes horizontal response rate inside the ramp");
    expectNear(rampedAppliedViewModel.rateAt(
                   commandStart + std::chrono::milliseconds(30)).first,
               0.0, 1e-12,
               "finite view model ends response rate at the upper boundary");
    expectNear(rampedAppliedViewModel.uncertaintyRateAt(
                   commandStart + std::chrono::milliseconds(40), 20.0).first,
               15.0, 1e-12,
               "response uncertainty keeps the modeled rate through the bounded tail");
    expectNear(rampedAppliedViewModel.uncertaintyRateAt(
                   commandStart + std::chrono::milliseconds(50), 20.0).first,
               0.0, 1e-12,
               "response uncertainty ends at the exclusive tail boundary");
    rampedAppliedViewModel.configure(20.0, 0.0);
    expectNear(rampedAppliedViewModel.at(
                   commandStart + std::chrono::milliseconds(100)).first,
               0.0, 0.0,
               "changing finite response settings clears the incompatible timeline");

    AppliedViewMotionModel concurrentAppliedViewModel;
    concurrentAppliedViewModel.configure(20.0, 20.0);
    std::atomic<bool> concurrentViewQueriesFinite{ true };
    std::thread viewWriter([&]() {
        for (int index = 0; index < 600; ++index)
        {
            concurrentAppliedViewModel.addCommand(
                index % 3 - 1, index % 5 - 2, 0.03, 0.02,
                commandStart + std::chrono::milliseconds(index));
        }
    });
    std::thread viewReader([&]() {
        for (int index = 0; index < 600; ++index)
        {
            const auto value = concurrentAppliedViewModel.at(
                commandStart + std::chrono::milliseconds(index));
            if (!std::isfinite(value.first) || !std::isfinite(value.second))
                concurrentViewQueriesFinite.store(false);
        }
    });
    viewWriter.join();
    viewReader.join();
    expectTrue(concurrentViewQueriesFinite.load(),
               "finite applied-view model keeps concurrent queries finite");
    expectTrue(concurrentAppliedViewModel.sampleCount() <= 512,
               "finite applied-view model keeps concurrent history bounded");

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
    expectTrue(traceHeader.find("InferenceFPS,TrackerStaleTimeoutMs") != std::string::npos && traceHeader.find(
        "SourceDeclaredFPS,SourceReceiveFPS,SourceReceivedFrames,SourceDroppedFrames") != std::string::npos,
        "basic pipeline contains generic source fps diagnostics");
    expectTrue(traceHeader.find(
        "DmlModel,DmlInputWidth,DmlInputHeight,DmlPreprocessMs,DmlTensorSetupMs,DmlInferenceMs,DmlCopyMs,DmlPostprocessMs,DmlNmsMs,DmlTotalMs") != std::string::npos,
        "basic pipeline contains dml stage timing diagnostics");
    expectTrue(traceHeader.find("NdiDeclaredFPS,NdiReceiveFPS,NdiReceivedFrames,NdiDroppedFrames") != std::string::npos,
               "basic pipeline keeps ndi compatibility diagnostics");
    expectTrue(traceHeader.find(
                   "FrameCountLimit,ControllerUpdateIntervalMs,ErrorMotion,ErrorMotionX,ErrorMotionY") != std::string::npos &&
               traceHeader.find(
                   "MovingInsideSettle,MovingInsideSettleX,MovingInsideSettleY,HorizontalCatchUp,VerticalCatchUp,SpeedLimited,Settled,SettledX,SettledY") != std::string::npos,
               "basic pipeline reports per-axis settle and speed limiting diagnostics");
    expectTrue(traceHeader.find(
        "ResponseSeconds,EffectiveResponseSecondsX,EffectiveResponseSecondsY") != std::string::npos,
        "basic pipeline reports effective per-axis catch-up response");
    expectTrue(traceHeader.find(
        "FrameID,BuildBackend,BuildRevision,BuildTimestampUtc,ControllerRevision") != std::string::npos,
        "basic pipeline identifies the executable and controller revision");
    expectTrue(traceHeader.find(
        "AimPipelineRequestedMode,AimPipelineEffectiveMode,AimPipelineActiveAvailable,AimPipelineShadowProcessed,AimPipelineCommandSuppressed,AimPipelineOutputPaused") != std::string::npos,
        "basic pipeline records same-frame mode and output-pause diagnostics");
    expectTrue(traceHeader.find(
        "ViewMotionShadowValid,CommandToFrameDelayMs,CommandResponseMs,ManeuverRateUncertaintyGain,ManeuverRateUncertaintyTailMs,AppliedCameraRateYawDps") != std::string::npos,
        "basic pipeline records finite applied-view response diagnostics");
    expectTrue(traceHeader.find(
        "AimPipelineAngleX,AimPipelineAngleY,AimPipelineRateX,AimPipelineRateY,AimPipelineCovarianceX,AimPipelineCovarianceY") != std::string::npos &&
        traceHeader.find("AimPipelineInnovationVarianceX,AimPipelineInnovationVarianceY,AimPipelineInnovationX,AimPipelineInnovationY,AimPipelineNisX,AimPipelineNisY") != std::string::npos &&
        traceHeader.find("AimPipelineMeasurementConfidence,AimPipelineFeedforwardConfidence") != std::string::npos,
        "basic pipeline records relative LOS Kalman diagnostics");
    expectTrue(traceHeader.find(
        "AimPipelineEstimatorMode,AimPipelineManeuverModelActive,AimPipelineEstimatorSelectionChanged,AimPipelineEstimatorSelectionCount") != std::string::npos &&
        traceHeader.find("AimPipelineBaselineAngleX,AimPipelineBaselineAngleY,AimPipelineBaselineRateX,AimPipelineBaselineRateY") != std::string::npos &&
        traceHeader.find("AimPipelineCaAngleX,AimPipelineCaAngleY,AimPipelineCaRateX,AimPipelineCaRateY") != std::string::npos,
        "basic pipeline records same-frame baseline, acceleration and model-selection diagnostics");
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
        "ErrorMotion,ErrorMotionX,ErrorMotionY,SettleMotionThreshold,MovingInsideSettle") != std::string::npos,
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
    std::istringstream traceRowStream(traceRow);
    std::vector<std::string> identityColumns;
    for (std::string column; identityColumns.size() < 5 && std::getline(traceRowStream, column, ',');) {
        identityColumns.push_back(column);
    }
    expectTrue(identityColumns.size() == 5 &&
                   identityColumns[4] == std::to_string(kBasicAimControllerRevision),
               "pipeline row carries the compiled controller revision");
    expectTrue(traceRow.find(",shadow,shadow,0,1,1,0,") != std::string::npos,
               "basic pipeline writes command-suppressed shadow state in the legacy frame");
    expectTrue(BuildIdentity::displayLabel().find(
                   " r" + std::to_string(kBasicAimControllerRevision)) != std::string::npos,
               "ui build label includes controller revision");

    CommandCancellationEpoch cancellationEpoch;
    const auto firstCommandToken = cancellationEpoch.capture();
    expectTrue(cancellationEpoch.isCurrent(firstCommandToken),
               "newly captured device command token is current");
    const auto secondEpoch = cancellationEpoch.cancel();
    expectTrue(!cancellationEpoch.isCurrent(firstCommandToken) &&
                   cancellationEpoch.isCurrent(secondEpoch),
               "cancelling device commands invalidates popped old-generation work");
    expectTrue(traceHeader.find("IntegralCountsX,IntegralCountsY") != std::string::npos &&
               traceHeader.find("ResponseSeconds,EffectiveResponseSecondsX,EffectiveResponseSecondsY,IntegralTimeSeconds,MaxCountsPerSecond,ConditionalSpeedBudgetActive,FrameCountLimit,ControllerUpdateIntervalMs") != std::string::npos,
               "basic pipeline reports moving-target integral diagnostics");
    expectTrue(traceHeader.find(
        "ProfileCalibrationEnabled,ProfileCalibrationValidX,ProfileCalibrationValidY") != std::string::npos &&
        traceHeader.find("ProfileCalibrationOverallConfidence") != std::string::npos,
        "basic pipeline reports passive profile calibration diagnostics");
    expectTrue(traceHeader.find(
        "MachineProfileLevel,MachineProfileCacheRequested,MachineProfileCacheLoaded,MachineProfileCacheMatched") != std::string::npos &&
        traceHeader.find("MachineProfilePredictionEnabled,MachineProfileIntegralEnabled") != std::string::npos &&
        traceHeader.find("MachineProfileReason") != std::string::npos,
        "basic pipeline reports machine profile cache and degradation diagnostics");
    expectTrue(traceHeader.find(
        "PredictionApplied,PredictionEnabled,PredictionAdditionalLeadMs,PredictionVelocityTauMs,PredictionStrength,PredictionVelocityX,PredictionVelocityY,PredictionAccelerationX,PredictionAccelerationY,PredictionLeadMs,PredictionOffsetX,PredictionOffsetY,ViewMotionX,ViewMotionY,ViewMotionCompensationDelayMs,ViewMotionCompensationResponseMs,PredictionDirectionLocked,PredictionSelfMotionSuppressed,PredictionOscillationSuppressed,PredictionHighSpeedSuppressed,PredictionStationarySuppressed,PredictionMotionEvidenceSuppressed,PredictedX,PredictedY") != std::string::npos,
        "basic pipeline reports prediction diagnostics");
    traceFile.close();
    std::remove(tracePath);

    for (int i = 0; i < 15; ++i)
        tracer.commitFrame(tracer.beginFrame(320));
    expectTrue(tracer.size() == 10, "trace ring capacity");

    BasicTargetFilter filter;
    const auto t0 = std::chrono::steady_clock::time_point(std::chrono::milliseconds(1000));

    ControlIntervalTracker controlIntervalTracker;
    expectNear(controlIntervalTracker.update(t0, 0.010), 0.010, 0.0,
               "control interval tracker uses the first-frame fallback");
    expectNear(controlIntervalTracker.update(t0 + std::chrono::milliseconds(12), 0.020),
               0.012, 1e-12,
               "control interval tracker follows control execution time instead of fallback cadence");
    expectNear(controlIntervalTracker.update(t0 + std::chrono::milliseconds(13), 0.010),
               0.002, 0.0,
               "control interval tracker clamps an implausibly short execution interval");
    controlIntervalTracker.reset();
    expectNear(controlIntervalTracker.update(t0 + std::chrono::seconds(1), 0.015),
               0.015, 0.0,
               "control interval reset restores first-frame fallback semantics");

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
    ViewMotionHistory delayedViewHistory;
    delayedViewHistory.configure(12.0, 0.0);
    delayedViewHistory.add(8.0, -4.0, t0 + std::chrono::milliseconds(10));
    const auto delayedBefore = delayedViewHistory.at(t0 + std::chrono::milliseconds(21));
    const auto delayedAtEffective = delayedViewHistory.at(t0 + std::chrono::milliseconds(22));
    expectNear(delayedBefore.first, 0.0, 0.0,
               "delayed view history excludes commands before frame-effective time");
    expectNear(delayedAtEffective.first, 8.0, 0.0,
               "delayed view history applies the complete command at frame-effective time");
    expectNear(delayedViewHistory.between(
                   t0 + std::chrono::milliseconds(10),
                   t0 + std::chrono::milliseconds(21)).first,
               0.0, 0.0,
               "delayed view interval excludes commands still in the future");
    delayedViewHistory.add(2.0, 0.0, t0 + std::chrono::seconds(3));
    expectNear(delayedViewHistory.at(t0 + std::chrono::seconds(4)).first, 10.0, 0.0,
               "delayed view history pruning preserves the cumulative coordinate system");
    delayedViewHistory.configure(20.0, 0.0);
    expectNear(delayedViewHistory.at(t0 + std::chrono::seconds(5)).first, 0.0, 0.0,
               "changing compensation delay clears the incompatible cumulative timeline");
    expectNear(delayedViewHistory.commandToFrameDelayMs(), 20.0, 0.0,
               "view history reports the configured command-to-frame delay");
    ViewMotionHistory rampedViewHistory;
    rampedViewHistory.configure(12.0, 24.0);
    rampedViewHistory.add(24.0, -12.0, t0 + std::chrono::milliseconds(10));
    expectNear(rampedViewHistory.at(t0 + std::chrono::milliseconds(9)).first,
               0.0, 0.0, "ramped view history never responds before command send time");
    expectNear(rampedViewHistory.at(t0 + std::chrono::milliseconds(22)).first,
               12.0, 1e-9, "ramped view history applies half the command at response center");
    expectNear(rampedViewHistory.at(t0 + std::chrono::milliseconds(34)).first,
               24.0, 1e-9, "ramped view history reaches the complete command after response width");
    expectNear(rampedViewHistory.between(
                   t0 + std::chrono::milliseconds(10),
                   t0 + std::chrono::milliseconds(22)).first,
               12.0, 1e-9, "ramped interval reports only the response fraction inside the window");
    rampedViewHistory.add(2.0, 0.0, t0 + std::chrono::seconds(3));
    expectNear(rampedViewHistory.at(t0 + std::chrono::seconds(4)).first,
               26.0, 1e-9, "ramped view pruning folds completed commands into the baseline");
    expectNear(rampedViewHistory.commandResponseMs(), 24.0, 0.0,
               "view history reports the configured response width");
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

    TargetPredictor smoothLeadPredictor;
    TargetPredictor::Settings smoothLeadSettings = predictionSettings;
    smoothLeadSettings.additionalLeadSeconds = 0.020;
    smoothLeadSettings.predictionStrength = 1.0;
    double previousSmoothLead = 0.0;
    double maximumMatureLeadDelta = 0.0;
    double matureLeadTotal = 0.0;
    int matureLeadSamples = 0;
    for (int sample = 0; sample < 50; ++sample)
    {
        const auto observationTime = t0 + std::chrono::milliseconds(sample * 10);
        const auto controlTime = observationTime +
            std::chrono::milliseconds(sample % 2 == 0 ? 0 : 20);
        const auto smoothLead = smoothLeadPredictor.update(
            100.0 + sample * 4.0, 100.0,
            observationTime, controlTime, 320.0, smoothLeadSettings);
        if (sample >= 20)
        {
            if (matureLeadSamples > 0)
            {
                maximumMatureLeadDelta = std::max(
                    maximumMatureLeadDelta,
                    std::abs(smoothLead.offsetX - previousSmoothLead));
            }
            previousSmoothLead = smoothLead.offsetX;
            matureLeadTotal += smoothLead.offsetX;
            ++matureLeadSamples;
        }
    }
    expectTrue(maximumMatureLeadDelta < 3.0,
               "alternating observation age cannot make mature prediction lead pulse per frame");
    expectTrue(matureLeadSamples > 0 && matureLeadTotal / matureLeadSamples > 8.0,
               "lead smoothing preserves useful latency compensation instead of suppressing prediction");

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

    // 实机r47中，早期自运动保持尚未结束时，内部连续预测计数先达到持续运动门槛。
    // 旧逻辑会因此跳过已有保持并留下未消费计数，数十帧后在成熟预测中突然归零。
    TargetPredictor uninterruptedHoldPredictor;
    TargetPredictor::Result uninterruptedHoldResult{};
    int uninterruptedActiveFrames = 0;
    int uninterruptedLastSample = 0;
    for (int sample = 0; sample < 16 && uninterruptedActiveFrames < 1; ++sample)
    {
        const auto time = t0 + std::chrono::milliseconds(sample * 8);
        uninterruptedHoldResult = uninterruptedHoldPredictor.update(
            100.0 + sample * 6.0, 100.0, time, time, 320.0, predictionSettings);
        uninterruptedLastSample = sample;
        if (uninterruptedHoldResult.offsetX > 0.0)
            ++uninterruptedActiveFrames;
    }
    expectTrue(uninterruptedActiveFrames == 1,
               "pending-hold regression starts before sustained motion is confirmed");
    uninterruptedHoldPredictor.applySelfMotionSuppression(
        uninterruptedHoldResult, true);
    expectTrue(uninterruptedHoldResult.selfMotionSuppressed,
               "an early artifact starts a self-motion hold");
    int sustainedHoldSuppressedFrames = 0;
    int sustainedHoldReleaseSample = 0;
    for (int sample = 1; sample <= 8; ++sample)
    {
        const auto time = t0 + std::chrono::milliseconds(
            (uninterruptedLastSample + sample) * 8);
        uninterruptedHoldResult = uninterruptedHoldPredictor.update(
            100.0 + (uninterruptedLastSample + sample) * 6.0,
            100.0, time, time, 320.0, predictionSettings);
        uninterruptedHoldPredictor.applySelfMotionSuppression(
            uninterruptedHoldResult, true);
        if (uninterruptedHoldResult.selfMotionSuppressed)
        {
            ++sustainedHoldSuppressedFrames;
            expectTrue(uninterruptedHoldResult.offsetX == 0.0,
                       "an active hold cannot leak lead before its response tail is consumed");
        }
        else if (sustainedHoldReleaseSample == 0)
        {
            sustainedHoldReleaseSample = sample;
        }
    }
    expectTrue(sustainedHoldSuppressedFrames >= 4 &&
                   sustainedHoldReleaseSample > 0 &&
                   sustainedHoldReleaseSample <= 5 &&
                   uninterruptedHoldResult.offsetX > 0.0,
               "mature motion counts already-suppressed tail frames and resumes without discarding its direction");

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
    int lastSustainedSample = 0;
    for (int sample = 0; sample < 16 && activePredictionFrames < 3; ++sample)
    {
        const auto time = t0 + std::chrono::milliseconds(sample * 8);
        sustainedPrediction = sustainedMotionPredictor.update(
            100.0 + sample * 6.0, 100.0, time, time, 320.0, predictionSettings);
        lastSustainedSample = sample;
        if (sustainedPrediction.offsetX > 0.0)
            ++activePredictionFrames;
    }
    expectTrue(activePredictionFrames == 3 && sustainedPrediction.offsetX > 0.0,
               "three continuous prediction frames establish sustained target motion");
    sustainedMotionPredictor.applySelfMotionSuppression(sustainedPrediction, true);
    expectTrue(!sustainedPrediction.selfMotionSuppressed && sustainedPrediction.offsetX > 0.0,
               "sustained target motion is exempt from self-motion artifact suppression");
    const double sustainedBaseX = sustainedPrediction.x - sustainedPrediction.offsetX;
    TargetPredictor::Result transientRegression{};
    const double regressionJitterX[] = { 20.0, -20.0, -20.0, 20.0 };
    const double regressionJitterY[] = { 20.0, 20.0, -20.0, -20.0 };
    for (int sample = 1; sample <= 4; ++sample)
    {
        const auto time = t0 + std::chrono::milliseconds(
            (lastSustainedSample + sample) * 8);
        transientRegression = sustainedMotionPredictor.update(
            sustainedBaseX + regressionJitterX[sample - 1],
            100.0 + regressionJitterY[sample - 1],
            time, time, 320.0, predictionSettings);
    }
    expectTrue(transientRegression.offsetX == 0.0,
               "four unreliable regression frames withdraw the current prediction lead");
    sustainedMotionPredictor.applySelfMotionSuppression(transientRegression, true);
    expectTrue(!transientRegression.selfMotionSuppressed,
               "brief regression degradation cannot reopen self-motion gating after sustained motion");
    for (int sample = 16; sample < 24; ++sample)
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
    for (int sample = 0; sample < 60; ++sample)
    {
        const auto time = t0 + std::chrono::milliseconds(sample * 8);
        boundedJump = jumpPredictor.update(
            10.0 + sample * 16.0, 10.0, time, time,
            320.0, predictionSettings);
    }
    expectNear(std::hypot(boundedJump.offsetX, boundedJump.offsetY), 24.0, 0.05,
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
    TargetPredictor protectedHighSpeedPredictor;
    TargetPredictor::Result highSpeedPrediction{};
    TargetPredictor::Result protectedHighSpeedPrediction{};
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
        protectedHighSpeedPrediction = protectedHighSpeedPredictor.update(
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
        protectedHighSpeedPrediction = protectedHighSpeedPredictor.update(
            highSpeedX, 100.0, time, time,
            320.0, predictionSettings, 0.10, true);
        expectTrue(!protectedHighSpeedPrediction.highSpeedSuppressed &&
                       protectedHighSpeedPrediction.offsetX > 0.0,
                   "bounded catch-up tail preserves mature lead through aligned acceleration");
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

    TargetPredictor unprotectedCatchUpArtifactPredictor;
    TargetPredictor protectedCatchUpArtifactPredictor;
    for (int sample = 0; sample < 40; ++sample)
    {
        const auto time = t0 + std::chrono::milliseconds(sample * 8);
        const double x = 100.0 + sample * 4.0;
        unprotectedCatchUpArtifactPredictor.update(
            x, 100.0, time, time, 320.0, predictionSettings);
        protectedCatchUpArtifactPredictor.update(
            x, 100.0, time, time, 320.0, predictionSettings);
    }
    TargetPredictor::Result unprotectedCatchUpArtifact{};
    TargetPredictor::Result protectedCatchUpArtifact{};
    for (int sample = 0; sample < 3; ++sample)
    {
        const auto artifactTime =
            t0 + std::chrono::milliseconds(320 + sample * 8);
        const double artifactX = 220.0 - sample * 20.0;
        unprotectedCatchUpArtifact = unprotectedCatchUpArtifactPredictor.update(
            artifactX, 100.0, artifactTime, artifactTime,
            320.0, predictionSettings);
        protectedCatchUpArtifact = protectedCatchUpArtifactPredictor.update(
            artifactX, 100.0, artifactTime, artifactTime,
            320.0, predictionSettings, 0.10, true);
    }
    expectTrue(unprotectedCatchUpArtifact.offsetX == 0.0,
               "unprotected contradictory regression withdraws the mature direction");
    expectTrue(protectedCatchUpArtifact.offsetX > 0.0 &&
                   protectedCatchUpArtifact.velocityX > 0.0,
               "bounded catch-up tail rejects contradictory view-motion regression");

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
    const auto shortStationaryPrediction = reversalPredictor.update(
        68.0, 100.0, t0 + std::chrono::milliseconds(192), t0 + std::chrono::milliseconds(192),
        320.0, predictionSettings);
    expectTrue(shortStationaryPrediction.offsetX < 0.0 &&
               !shortStationaryPrediction.stationarySuppressed,
               "sub-thirty-millisecond detection plateaus preserve an established lead");
    const auto confirmedStopPrediction = reversalPredictor.update(
        68.0, 100.0, t0 + std::chrono::milliseconds(216), t0 + std::chrono::milliseconds(216),
        320.0, predictionSettings);
    expectTrue(confirmedStopPrediction.x == 68.0 &&
               confirmedStopPrediction.stationarySuppressed,
               "thirty milliseconds of continuous low speed withdraws prediction lead");

    TargetPredictor quantizedPlateauPredictor;
    TargetPredictor::Result plateauPrediction{};
    for (int sample = 0; sample < 20; ++sample)
    {
        const auto time = t0 + std::chrono::milliseconds(sample * 8);
        plateauPrediction = quantizedPlateauPredictor.update(
            100.0 + sample * 4.0, 100.0, time, time,
            320.0, predictionSettings);
    }
    const double plateauX = 100.0 + 19.0 * 4.0;
    for (int sample = 1; sample <= 3; ++sample)
    {
        const auto time = t0 + std::chrono::milliseconds((19 + sample) * 8);
        plateauPrediction = quantizedPlateauPredictor.update(
            plateauX, 100.0, time, time, 320.0, predictionSettings);
        expectTrue(plateauPrediction.offsetX > 0.0 &&
                   !plateauPrediction.stationarySuppressed,
                   "short quantized plateaus cannot pulse a mature prediction lead");
    }
    plateauPrediction = quantizedPlateauPredictor.update(
        plateauX + 4.0, 100.0, t0 + std::chrono::milliseconds(184),
        t0 + std::chrono::milliseconds(184), 320.0, predictionSettings);
    expectTrue(plateauPrediction.offsetX > 0.0 &&
               !plateauPrediction.stationarySuppressed,
               "motion resumption clears partial stationary evidence without rearming lead");

    TargetPredictor matureLeadPredictor;
    TargetPredictor::Result matureLeadPrediction{};
    TargetPredictor::Result firstReleasedLead{};
    for (int sample = 0; sample < 50; ++sample)
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
               0.01, "mature stable motion converges to the full constant-velocity lead");

    const auto quickResumePrediction = matureLeadPredictor.update(
        396.0, 100.0, t0 + std::chrono::milliseconds(592),
        t0 + std::chrono::milliseconds(602), 320.0, predictionSettings, 0.35);
    expectTrue(quickResumePrediction.directionLocked &&
                   quickResumePrediction.offsetX > 0.0,
               "short same-target aiming pause preserves mature forward prediction");
    const auto quantizedQuickResumePrediction = matureLeadPredictor.update(
        396.0, 100.0, t0 + std::chrono::milliseconds(600),
        t0 + std::chrono::milliseconds(610), 320.0, predictionSettings);
    expectTrue(quantizedQuickResumePrediction.directionLocked &&
                   quantizedQuickResumePrediction.offsetX > 0.0 &&
                   quantizedQuickResumePrediction.velocityX > 0.0,
               "quick resume latch preserves lead through a quantized second frame");
    const auto regressionQuickResumePrediction = matureLeadPredictor.update(
        400.0, 100.0, t0 + std::chrono::milliseconds(608),
        t0 + std::chrono::milliseconds(618), 320.0, predictionSettings);
    expectTrue(regressionQuickResumePrediction.directionLocked &&
                   regressionQuickResumePrediction.offsetX > 0.0,
               "four-sample regression takes over without a quick-resume prediction hole");

    TargetPredictor reversedResumePredictor;
    for (int sample = 0; sample < 50; ++sample)
    {
        const auto time = t0 + std::chrono::milliseconds(sample * 8);
        reversedResumePredictor.update(
            100.0 + sample * 4.0, 100.0, time, time,
            320.0, predictionSettings);
    }
    const auto reversedResumePrediction = reversedResumePredictor.update(
        240.0, 100.0, t0 + std::chrono::milliseconds(592),
        t0 + std::chrono::milliseconds(602), 320.0, predictionSettings, 0.35);
    expectTrue(reversedResumePrediction.offsetX == 0.0 &&
                   reversedResumePrediction.velocityX == 0.0,
               "opposite quick-resume motion cannot reuse the previous prediction direction");

    TargetPredictor expiredResumePredictor;
    for (int sample = 0; sample < 50; ++sample)
    {
        const auto time = t0 + std::chrono::milliseconds(sample * 8);
        expiredResumePredictor.update(
            100.0 + sample * 4.0, 100.0, time, time,
            320.0, predictionSettings);
    }
    const auto expiredResumePrediction = expiredResumePredictor.update(
        500.0, 100.0, t0 + std::chrono::milliseconds(1200),
        t0 + std::chrono::milliseconds(1210), 320.0, predictionSettings, 0.35);
    expectTrue(!expiredResumePrediction.directionLocked &&
                   expiredResumePrediction.offsetX == 0.0,
               "long aiming pause expires old prediction state");

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
    expectTrue(crossDomainVariants.size() == 162 &&
               std::none_of(crossDomainVariants.begin(), crossDomainVariants.end(), [](const auto& variant) {
                   return variant.name.rfind("role_", 0) == 0;
               }),
               "required cross-domain matrix covers only target-motion physical variants");

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
    const auto phaseFramePath = std::filesystem::temp_directory_path() /
        "xen_cross_domain_phase_frames.csv";
    std::error_code phaseFrameError;
    std::filesystem::remove(phaseFramePath, phaseFrameError);
    CrossDomainReplay::ControllerSettings phaseFrameSettings = syntheticSettings;
    phaseFrameSettings.candidateEstimatorMode =
        CrossDomainReplay::CandidateEstimatorMode::ManeuverGatedConstantAcceleration;
    CrossDomainReplay::RunComparison(
        syntheticReplay, syntheticVariant, phaseFrameSettings, phaseFramePath);
    std::ifstream phaseFrameFile(phaseFramePath);
    std::string phaseFrameHeader;
    std::string phaseFrameRow;
    std::getline(phaseFrameFile, phaseFrameHeader);
    std::getline(phaseFrameFile, phaseFrameRow);
    expectTrue(phaseFrameHeader.find(
                   "TruthRateX,TruthRateY,PhysicalCameraRateX,PhysicalCameraRateY") !=
                   std::string::npos &&
               phaseFrameHeader.find(
                   "BaselineAngleX,BaselineAngleY,BaselineRateX,BaselineRateY,CaAngleX,CaAngleY,CaRateX,CaRateY,ModelAngleDeltaDeg,ModelRateDeltaDps,ManeuverRateEvidenceDps,ManeuverModelActive") !=
                   std::string::npos &&
               phaseFrameHeader.find(
                   "UnlimitedX,UnlimitedY,FrameCountLimit,UnlimitedToFrameLimitRatio,LimitedToUnlimitedRatio") !=
                   std::string::npos,
               "cross-domain frame csv exposes phase-separation evidence");
    expectTrue(std::count(phaseFrameHeader.begin(), phaseFrameHeader.end(), ',') ==
                   std::count(phaseFrameRow.begin(), phaseFrameRow.end(), ','),
               "cross-domain phase frame header and row keep the same column count");
    std::filesystem::remove(phaseFramePath, phaseFrameError);
    CrossDomainReplay::ControllerSettings integralReplaySettings = syntheticSettings;
    integralReplaySettings.integralTimeSeconds = 0.500;
    integralReplaySettings.integralZoneDegrees = 10.0;
    const auto integralReplayComparison = CrossDomainReplay::RunComparison(
        syntheticReplay, syntheticVariant, integralReplaySettings);
    expectNear(integralReplayComparison.candidateIntegralTimeSeconds, 0.500, 0.0,
               "cross-domain replay records candidate integral time");
    expectNear(integralReplayComparison.candidateIntegralZoneDegrees, 10.0, 0.0,
               "cross-domain replay records candidate integral zone");
    expectNear(integralReplayComparison.legacy.errorP95Degrees,
               syntheticComparison.legacy.errorP95Degrees, 0.0,
               "candidate integral leaves the frozen legacy comparator unchanged");
    expectTrue(std::abs(integralReplayComparison.candidate.requestedCounts -
                   syntheticComparison.candidate.requestedCounts) > 1e-6,
               "candidate integral changes only the candidate control request");
    const auto integralSummaryPath = std::filesystem::temp_directory_path() /
        "xen_cross_domain_integral_summary.csv";
    CrossDomainReplay::WriteSummary(
        integralSummaryPath, { integralReplayComparison });
    std::ifstream integralSummary(integralSummaryPath);
    std::string integralSummaryHeader;
    std::string integralSummaryRow;
    std::getline(integralSummary, integralSummaryHeader);
    std::getline(integralSummary, integralSummaryRow);
    expectTrue(integralSummaryHeader.find(
                   "CandidateIntegralTimeMs,CandidateIntegralZoneDeg") !=
                   std::string::npos &&
               integralSummaryRow.find(",500.000000,10.000000,") !=
                   std::string::npos,
               "cross-domain summary audits candidate integral parameters");
    std::error_code integralSummaryError;
    std::filesystem::remove(integralSummaryPath, integralSummaryError);
    CrossDomainReplay::Variant finiteResponseVariant = syntheticVariant;
    finiteResponseVariant.commandToFrameDelayMs = 20.0;
    finiteResponseVariant.commandResponseMs = 20.0;
    const auto finiteResponseComparison = CrossDomainReplay::RunComparison(
        syntheticReplay, finiteResponseVariant, syntheticSettings);
    expectNear(finiteResponseComparison.variant.commandToFrameDelayMs, 20.0, 0.0,
               "cross-domain replay records the shared physical response center");
    expectNear(finiteResponseComparison.variant.commandResponseMs, 20.0, 0.0,
               "cross-domain replay records the shared physical response width");
    expectTrue(std::abs(finiteResponseComparison.legacy.errorP95Degrees -
                   syntheticComparison.legacy.errorP95Degrees) > 1e-6,
               "finite physical camera response changes the closed-loop legacy trajectory");
    expectTrue(std::abs(finiteResponseComparison.candidate.errorP95Degrees -
                   syntheticComparison.candidate.errorP95Degrees) > 1e-6,
               "finite physical camera response changes the closed-loop candidate trajectory");
    CrossDomainReplay::ControllerSettings widenedBandReplaySettings = syntheticSettings;
    widenedBandReplaySettings.reverseConfirmationErrorMultiplier = 1.75;
    widenedBandReplaySettings.confirmLowSpeedReverseSettleRelease = true;
    const auto widenedBandComparison = CrossDomainReplay::RunComparison(
        syntheticReplay, syntheticVariant, widenedBandReplaySettings);
    expectNear(widenedBandComparison.reverseConfirmationErrorMultiplier, 1.75, 0.0,
               "cross-domain replay records the independent reverse confirmation error band");
    expectTrue(widenedBandComparison.confirmLowSpeedReverseSettleRelease,
               "cross-domain replay records low-speed reverse settle release confirmation");
    const auto detailTracePath = std::filesystem::temp_directory_path() /
        "xen_cross_domain_detail_trace.csv";
    std::error_code detailTraceError;
    std::filesystem::remove(detailTracePath, detailTraceError);
    CrossDomainReplay::RunComparison(
        syntheticReplay, syntheticVariant, syntheticSettings, detailTracePath);
    CrossDomainReplay::RunComparison(
        syntheticReplay, syntheticVariant, syntheticSettings, detailTracePath);
    std::ifstream detailTrace(detailTracePath);
    size_t detailTraceHeaders = 0;
    bool detailTraceHasEstimatorBias = false;
    for (std::string line; std::getline(detailTrace, line);)
    {
        if (line.rfind("Scenario,Variant,", 0) == 0)
        {
            ++detailTraceHeaders;
            detailTraceHasEstimatorBias =
                line.find("MeasurementYaw,MeasurementPitch,MeasuredRelativeYaw,") !=
                    std::string::npos &&
                line.find("MeasuredRelativePitch,ViewAtObservationYaw,") !=
                    std::string::npos &&
                line.find("ViewAtObservationPitch,ViewAtControlYaw,ViewAtControlPitch,") !=
                    std::string::npos &&
                line.find("ObservationTruthYaw,") != std::string::npos &&
                line.find("ObservationTruthPitch,TruthYaw,TruthPitch,") !=
                    std::string::npos &&
                line.find("EstimateAngleX,EstimateAngleY,InputErrorX,InputErrorY,") !=
                    std::string::npos &&
                line.find("SettleEntryErrorX,SettleEntryErrorY,") != std::string::npos &&
                line.find("EstimateTruthBiasX,EstimateTruthBiasY,") != std::string::npos &&
                line.find("SettleConfirmationSamples,SettleEntryCommandHeld,") !=
                    std::string::npos;
        }
    }
    expectTrue(detailTraceHeaders == 1,
               "cross-domain detail trace writes one header across appended scenarios");
    expectTrue(detailTraceHasEstimatorBias,
               "cross-domain detail trace exposes estimator truth bias during settle");
    detailTrace.close();
    std::filesystem::remove(detailTracePath, detailTraceError);
    CrossDomainReplay::SourceTrajectory staticTruthReplay = syntheticReplay;
    staticTruthReplay.scenario = "static";
    CrossDomainReplay::ControllerSettings staticTruthSettings = syntheticSettings;
    staticTruthSettings.staticFixedTruth = true;
    const auto fixedTruthComparison = CrossDomainReplay::RunComparison(
        staticTruthReplay, syntheticVariant, staticTruthSettings);
    expectTrue(fixedTruthComparison.staticFixedTruth &&
               fixedTruthComparison.legacy.errorP95Degrees !=
                   syntheticComparison.legacy.errorP95Degrees,
               "static replay can separate fixed physical truth from noisy measurements");
    CrossDomainReplay::ControllerSettings viewMotionSettings = syntheticSettings;
    viewMotionSettings.candidateViewMotionCompensation = true;
    const auto viewMotionComparison = CrossDomainReplay::RunComparison(
        syntheticReplay, syntheticVariant, viewMotionSettings);
    expectTrue(viewMotionComparison.candidateViewMotionCompensation,
               "cross-domain replay identifies the formal view-motion timeline candidate");
    expectTrue(std::isfinite(viewMotionComparison.candidate.errorP95Degrees) &&
                   std::isfinite(viewMotionComparison.candidate.meanNis) &&
                   viewMotionComparison.candidate.sentCounts > 0.0,
               "formal view-motion timeline candidate keeps finite delayed-command diagnostics");
    expectTrue(std::abs(viewMotionComparison.candidate.errorP95Degrees -
                   syntheticComparison.candidate.errorP95Degrees) < 0.1,
               "formal view-motion timeline candidate stays within the continuous-detection error band");
    CrossDomainReplay::ControllerSettings commitHorizonSettings = viewMotionSettings;
    commitHorizonSettings.candidateCommandCommitHorizonSeconds = 0.060;
    const auto commitHorizonComparison = CrossDomainReplay::RunComparison(
        syntheticReplay, syntheticVariant, commitHorizonSettings);
    expectNear(commitHorizonComparison.candidateCommandCommitHorizonSeconds,
               0.060, 0.0,
               "cross-domain replay records the bounded command commit horizon");
    expectTrue(std::abs(commitHorizonComparison.candidate.requestedCounts -
                   viewMotionComparison.candidate.requestedCounts) > 1e-6,
               "command commit horizon changes the candidate control request");
    CrossDomainReplay::ControllerSettings settleGuardSettings = syntheticSettings;
    settleGuardSettings.candidateCommandCommitHorizonSeconds = 0.060;
    settleGuardSettings.candidateSettleEntryCommandGuard = true;
    const auto settleGuardComparison = CrossDomainReplay::RunComparison(
        syntheticReplay, syntheticVariant, settleGuardSettings);
    expectTrue(settleGuardComparison.candidateSettleEntryCommandGuard,
               "cross-domain replay records the settle-only committed command guard");
    settleGuardSettings.candidateSettleEntryCommandHold = true;
    const auto settleHoldComparison = CrossDomainReplay::RunComparison(
        syntheticReplay, syntheticVariant, settleGuardSettings);
    expectTrue(settleHoldComparison.candidateSettleEntryCommandHold,
               "cross-domain replay records the settle-entry committed command hold");
    CrossDomainReplay::ControllerSettings responseReplaySettings = syntheticSettings;
    responseReplaySettings.responseSeconds = 0.080;
    responseReplaySettings.candidateResponseSeconds = 0.060;
    const auto responseComparison = CrossDomainReplay::RunComparison(
        syntheticReplay, syntheticVariant, responseReplaySettings);
    expectNear(responseComparison.legacyResponseSeconds, 0.080, 0.0,
               "cross-domain replay records the frozen legacy response");
    expectNear(responseComparison.candidateResponseSeconds, 0.060, 0.0,
               "cross-domain replay records the independent candidate response");
    expectNear(responseComparison.legacy.errorP95Degrees,
               syntheticComparison.legacy.errorP95Degrees, 0.0,
               "candidate response experiments cannot mutate the legacy comparator");
    CrossDomainReplay::ControllerSettings responseCounterfactualSettings =
        syntheticSettings;
    responseCounterfactualSettings.responseSeconds = 0.080;
    responseCounterfactualSettings.candidateResponseSeconds = 0.080;
    const auto responseCounterfactual =
        CrossDomainReplay::RunResponseCounterfactual(
            syntheticReplay, finiteResponseVariant,
            responseCounterfactualSettings, 0.100);
    expectTrue(responseCounterfactual.cohortStable &&
                   responseCounterfactual.timelineFrames > 0 &&
                   responseCounterfactual.detectedFrames > 0 &&
                   responseCounterfactual.detectedFrames ==
                       responseCounterfactual.baseline.candidate.samples,
               "response counterfactual records and reuses one detection timeline");
    expectNear(responseCounterfactual.baseline.candidateResponseSeconds,
               0.080, 0.0,
               "response counterfactual preserves the 80 ms baseline");
    expectNear(responseCounterfactual.counterfactual.candidateResponseSeconds,
               0.100, 0.0,
               "response counterfactual applies the independent 100 ms response");
    expectTrue(responseCounterfactual.baseline.candidate.samples ==
                   responseCounterfactual.counterfactual.candidate.samples &&
                   responseCounterfactual.baseline.legacy.errorP95Degrees ==
                   responseCounterfactual.counterfactual.legacy.errorP95Degrees,
               "response counterfactual keeps candidate samples and legacy metrics exact");
    const auto responseCounterfactualSummaryPath =
        std::filesystem::temp_directory_path() /
        "xen_response_counterfactual_summary.csv";
    CrossDomainReplay::WriteResponseCounterfactualSummary(
        responseCounterfactualSummaryPath, { responseCounterfactual });
    std::ifstream responseCounterfactualSummary(
        responseCounterfactualSummaryPath);
    std::string responseCounterfactualHeader;
    std::string responseCounterfactualRow;
    std::getline(responseCounterfactualSummary, responseCounterfactualHeader);
    std::getline(responseCounterfactualSummary, responseCounterfactualRow);
    expectTrue(responseCounterfactualHeader.find(
                   "CohortStable,TimelineFrames,DetectedFrames") !=
                   std::string::npos &&
                   responseCounterfactualRow.find(",1,") != std::string::npos,
               "response counterfactual summary exposes the queue contract");
    responseCounterfactualSummary.close();
    std::error_code responseCounterfactualSummaryError;
    std::filesystem::remove(responseCounterfactualSummaryPath,
        responseCounterfactualSummaryError);
    expectTrue(std::abs(responseComparison.candidate.requestedCounts -
                   syntheticComparison.candidate.requestedCounts) > 1e-6,
               "independent candidate response changes only the candidate control path");
    const auto attribution = CrossDomainReplay::RunAttribution(
        syntheticReplay, syntheticVariant, syntheticSettings);
    expectTrue(attribution.oracleEstimator.candidateEstimatorMode ==
                   CrossDomainReplay::CandidateEstimatorMode::OracleControlTime &&
               attribution.unlimitedActuator.candidateMaxCountsPerSecond == 100000.0 &&
               attribution.cohortStable,
               "cross-domain attribution isolates oracle state and actuator-limit counterfactuals");
    expectNear(attribution.oracleEstimator.legacy.errorP95Degrees,
               attribution.baseline.legacy.errorP95Degrees, 0.0,
               "oracle attribution cannot mutate the frozen legacy comparator");
    expectNear(attribution.unlimitedActuator.legacy.errorP95Degrees,
               attribution.baseline.legacy.errorP95Degrees, 0.0,
               "actuator attribution cannot mutate the frozen legacy comparator");

    CrossDomainReplay::Comparison failedAttribution = syntheticComparison;
    failedAttribution.passed = false;
    CrossDomainReplay::Comparison oraclePassAttribution = failedAttribution;
    oraclePassAttribution.passed = true;
    CrossDomainReplay::Comparison unlimitedFailAttribution = failedAttribution;
    bool attributionCohortStable = false;
    expectTrue(CrossDomainReplay::ClassifyAttribution(
                   failedAttribution, oraclePassAttribution,
                   unlimitedFailAttribution, attributionCohortStable) ==
                   "ESTIMATOR_LIMITED" && attributionCohortStable,
               "attribution labels an oracle-only rescue as estimator limited");
    CrossDomainReplay::Comparison changedCohortAttribution = oraclePassAttribution;
    changedCohortAttribution.legacy.errorP95Degrees += 0.001;
    expectTrue(CrossDomainReplay::ClassifyAttribution(
                   failedAttribution, changedCohortAttribution,
                   unlimitedFailAttribution, attributionCohortStable) ==
                   "COHORT_CHANGED" && !attributionCohortStable,
               "attribution refuses conclusions when the frozen cohort changes");
    CrossDomainReplay::ControllerSettings accelerationReplaySettings = syntheticSettings;
    accelerationReplaySettings.candidateEstimatorMode =
        CrossDomainReplay::CandidateEstimatorMode::ConstantAcceleration;
    accelerationReplaySettings.candidateJerkStdDegreesPerSecond3 = 2000.0;
    const auto accelerationComparison = CrossDomainReplay::RunComparison(
        syntheticReplay, syntheticVariant, accelerationReplaySettings);
    expectTrue(accelerationComparison.candidateEstimatorMode ==
                   CrossDomainReplay::CandidateEstimatorMode::ConstantAcceleration &&
               accelerationComparison.candidateJerkStdDegreesPerSecond3 == 2000.0 &&
               accelerationComparison.candidate.samples ==
                   syntheticComparison.candidate.samples &&
               std::isfinite(accelerationComparison.candidate.meanCovariance),
               "constant-acceleration replay records a finite same-cohort offline estimate");
    expectNear(accelerationComparison.legacy.errorP95Degrees,
               syntheticComparison.legacy.errorP95Degrees, 0.0,
               "constant-acceleration replay cannot mutate the frozen legacy comparator");
    CrossDomainReplay::ControllerSettings gatedAccelerationSettings = syntheticSettings;
    gatedAccelerationSettings.candidateEstimatorMode = CrossDomainReplay::
        CandidateEstimatorMode::ManeuverGatedConstantAcceleration;
    gatedAccelerationSettings.candidateJerkStdDegreesPerSecond3 = 8000.0;
    gatedAccelerationSettings.candidateManeuverRateThresholdDegreesPerSecond = 1.0;
    gatedAccelerationSettings.candidateManeuverHoldSeconds = 0.120;
    const auto gatedAccelerationComparison = CrossDomainReplay::RunComparison(
        syntheticReplay, syntheticVariant, gatedAccelerationSettings);
    expectTrue(gatedAccelerationComparison.candidate.maneuverModelPercent > 0.0 &&
               gatedAccelerationComparison.candidate.maneuverModelPercent <= 100.0,
               "maneuver-gated acceleration model activates on reliable fast motion");
    gatedAccelerationSettings.candidateManeuverRateThresholdDegreesPerSecond = 1000.0;
    const auto inactiveAccelerationComparison = CrossDomainReplay::RunComparison(
        syntheticReplay, syntheticVariant, gatedAccelerationSettings);
    expectNear(inactiveAccelerationComparison.candidate.maneuverModelPercent,
               0.0, 0.0,
               "maneuver-gated acceleration model stays off below its physical rate threshold");
    expectNear(inactiveAccelerationComparison.candidate.errorP95Degrees,
               syntheticComparison.candidate.errorP95Degrees, 0.0,
               "inactive maneuver model is exactly equivalent to the frozen Kalman candidate");
    CrossDomainReplay::ControllerSettings feedforwardReplaySettings = syntheticSettings;
    feedforwardReplaySettings.feedforwardGain = 0.5;
    const auto feedforwardComparison = CrossDomainReplay::RunComparison(
        syntheticReplay, syntheticVariant, feedforwardReplaySettings);
    expectTrue(feedforwardComparison.feedforwardGain == 0.5 &&
               feedforwardComparison.candidate.feedforwardCounts > 0.0,
               "cross-domain replay applies and records relative LOS feedforward gain");
    CrossDomainReplay::ControllerSettings trapezoidReplaySettings = syntheticSettings;
    trapezoidReplaySettings.trajectoryMode = TrajectoryShaperMode::Trapezoid;
    trapezoidReplaySettings.trajectoryOutputHz = 240.0;
    trapezoidReplaySettings.trajectoryMaxAccelerationCountsPerSecond2 = 1000.0;
    trapezoidReplaySettings.trajectoryMaxJerkCountsPerSecond3 = 10000.0;
    const auto trapezoidComparison = CrossDomainReplay::RunComparison(
        syntheticReplay, syntheticVariant, trapezoidReplaySettings);
    expectTrue(trapezoidComparison.trajectoryMode == TrajectoryShaperMode::Trapezoid &&
               trapezoidComparison.trajectoryOutputHz == 240.0 &&
               trapezoidComparison.candidate.trajectoryOutputs >
                   trapezoidComparison.candidate.samples * 2,
               "trapezoid replay services every fixed 240 Hz tick between 94 Hz observations");
    expectTrue(trapezoidComparison.candidate.trajectoryAccelerationLimitedPercent > 0.0 &&
               trapezoidComparison.candidate.shapedCounts > 0.0,
               "trapezoid replay records constrained fixed-tick output diagnostics");

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

    BasicAimController normalResumeController;
    BasicAimController boostedResumeController;
    BasicAimController::Settings normalResumeSettings;
    normalResumeSettings.responseSeconds = 0.120;
    normalResumeSettings.maxCountsPerSecond = 3200.0;
    normalResumeSettings.integralTimeSeconds = 0.0;
    normalResumeSettings.settleRadiusPixels = 10.0;
    normalResumeSettings.releaseRadiusPixels = 16.0;
    BasicAimController::Settings boostedResumeSettings = normalResumeSettings;
    boostedResumeSettings.responseSeconds = 0.100;
    const auto normalResumeOutput = normalResumeController.update(
        48.0, 0.0, 0.010, 1.0, 1.0, normalResumeSettings);
    const auto boostedResumeOutput = boostedResumeController.update(
        48.0, 0.0, 0.010, 1.0, 1.0, boostedResumeSettings);
    expectTrue(!normalResumeOutput.speedLimited && !boostedResumeOutput.speedLimited,
               "medium quick-resume backlog remains below the device speed limit");
    expectNear(normalResumeOutput.effectiveResponseSecondsX, 0.045, 1e-12,
               "normal 120 ms response converges continuously toward the validated floor");
    expectNear(boostedResumeOutput.effectiveResponseSecondsX, 0.040, 1e-12,
               "quick-resume 100 ms response still shortens the bounded backlog phase");
    expectTrue(boostedResumeOutput.countsX > normalResumeOutput.countsX,
               "bounded quick-resume response consumes available medium-error headroom");
    normalResumeController.reset();
    const auto extremeNormalResumeOutput = normalResumeController.update(
        80.0, 0.0, 0.010, 1.0, 1.0, normalResumeSettings);
    expectNear(extremeNormalResumeOutput.effectiveResponseSecondsX, 0.030, 1e-12,
               "120 ms center response cannot stretch extreme jump catch-up beyond 30 ms");
    BasicAimController alreadyFastController;
    BasicAimController::Settings alreadyFastSettings = normalResumeSettings;
    alreadyFastSettings.responseSeconds = 0.040;
    const auto alreadyFastOutput = alreadyFastController.update(
        80.0, 0.0, 0.010, 1.0, 1.0, alreadyFastSettings);
    expectNear(alreadyFastOutput.effectiveResponseSecondsX, 0.020, 1e-12,
               "large-error floor never slows an explicitly faster controller response");

    // 生产速率必须使用捕获窗统计出的实际 FPS 换算单帧预算，而不是绑定某个设备或固定帧率。
    // 选取本轮 CSV 的约 127/143 FPS 和未来可能出现的 240 FPS，验证每秒总预算始终一致。
    BasicAimController::Settings productionSettings;
    productionSettings.settleRadiusPixels = 0.0;
    productionSettings.releaseRadiusPixels = 0.0;
    expectNear(productionSettings.maxCountsPerSecond, 1440.0, 0.0,
               "controller unit-test default remains conservative");
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
    predictedMotionSettings.preserveMovingIntegralX = true;
    const auto predictedPointCrossing = predictedMotionController.update(
        -6.0, 0.0, dmlDt, 1.344, 1.344, predictedMotionSettings);
    expectTrue(predictedPointCrossing.integralCountsX > 0.5,
               "active prediction preserves moving integral across aim-point crossing");
    predictedMotionSettings.preserveMovingIntegralX = false;
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

    // 水平持续运动时，Y轴没有可靠运动证据。旧二维锁存会因X误差始终较大而
    // 让Y比例输出追逐检测噪声；按轴锁存后X继续输出，Y必须独立保持为零。
    BasicAimController perAxisSettleController;
    BasicAimController::Settings perAxisSettleSettings = dmlRateSettings;
    perAxisSettleSettings.preserveMovingIntegralX = true;
    perAxisSettleSettings.allowMovingInsideSettleX = true;
    perAxisSettleSettings.preserveMovingIntegralY = false;
    perAxisSettleSettings.allowMovingInsideSettleY = false;
    bool horizontalOutputObserved = false;
    bool verticalNoiseOutputObserved = false;
    for (int frame = 0; frame < 120; ++frame)
    {
        const double verticalNoise = frame % 2 == 0 ? 2.0 : -2.0;
        const auto output = perAxisSettleController.update(
            12.0, verticalNoise, dmlDt,
            dmlCountsPerPixel, dmlCountsPerPixel, perAxisSettleSettings);
        horizontalOutputObserved = horizontalOutputObserved || output.countsX > 0.0;
        verticalNoiseOutputObserved = verticalNoiseOutputObserved || output.countsY != 0.0;
        expectTrue(output.settledY && !output.settledX,
                   "stationary vertical axis settles independently during horizontal tracking");
    }
    expectTrue(horizontalOutputObserved && !verticalNoiseOutputObserved,
               "per-axis settle keeps horizontal tracking and rejects vertical detector noise");

    // 可靠垂直或斜向运动必须按该轴误差变化立即释放，不能因上一段水平运动
    // 留下的Y锁存而等待完整二维误差离开回差区。
    perAxisSettleSettings.preserveMovingIntegralY = true;
    perAxisSettleSettings.allowMovingInsideSettleY = true;
    const auto verticalMotionRelease = perAxisSettleController.update(
        12.0, 8.0, dmlDt,
        dmlCountsPerPixel, dmlCountsPerPixel, perAxisSettleSettings);
    expectTrue(!verticalMotionRelease.settledY &&
               verticalMotionRelease.movingInsideSettleY &&
               verticalMotionRelease.countsY > 0.0,
               "reliable vertical motion releases only the vertical settle latch");

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
