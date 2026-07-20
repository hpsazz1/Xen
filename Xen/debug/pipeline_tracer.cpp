#include "pipeline_tracer.h"

#include "runtime/basic_aim_controller.h"
#include "runtime/build_identity.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

// ============================================================================
// 全局单例
// ============================================================================
PipelineTracer g_pipelineTracer;

namespace
{
void mergeCommandResult(ViewCommandSample& destination, const ViewCommandSample& source)
{
    if (source.sequence == 0 || destination.sequence != source.sequence)
        return;
    if (source.enqueueTime.time_since_epoch().count() != 0)
        destination.enqueueTime = source.enqueueTime;
    if (source.requestedCountsX != 0.0 || source.requestedCountsY != 0.0)
    {
        destination.requestedCountsX = source.requestedCountsX;
        destination.requestedCountsY = source.requestedCountsY;
    }
    destination.enqueueSucceeded = destination.enqueueSucceeded || source.enqueueSucceeded;
    destination.sendAttempted = destination.sendAttempted || source.sendAttempted;
    destination.sendSucceeded = destination.sendSucceeded || source.sendSucceeded;
    destination.droppedBeforeSend = destination.droppedBeforeSend || source.droppedBeforeSend;
    if (source.sendSucceeded)
    {
        destination.appliedCountsX = source.appliedCountsX;
        destination.appliedCountsY = source.appliedCountsY;
        destination.deviceSendTime = source.deviceSendTime;
    }
}

std::string steadyTimeNs(FrameTiming::Clock::time_point time)
{
    if (!frameTimeKnown(time))
        return {};
    return std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
        time.time_since_epoch()).count());
}

std::string signedDurationMs(
    FrameTiming::Clock::time_point start,
    FrameTiming::Clock::time_point end)
{
    const std::optional<double> duration = frameSignedDurationMs(start, end);
    if (!duration)
        return {};
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(3)
           << *duration;
    return stream.str();
}
}

// ============================================================================
// PipelineTracer 实现
// ============================================================================

void PipelineTracer::setMaxFrames(size_t n)
{
    if (n < 10) n = 10;
    if (n > 10000) n = 10000;
    std::lock_guard<std::mutex> lock(mutex);
    maxFrames = n;
    while (ringBuffer.size() > maxFrames)
        ringBuffer.pop_front();
}

size_t PipelineTracer::getMaxFrames() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return maxFrames;
}

PipelineFrame PipelineTracer::beginFrame(int resolution)
{
    PipelineFrame frame;
    frame.frameId = frameCounter.fetch_add(1, std::memory_order_relaxed);
    frame.timestamp = std::chrono::steady_clock::now();
    frame.resolution = resolution;
    return frame;
}

void PipelineTracer::commitFrame(PipelineFrame frame)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (frame.commandSample.sequence != 0)
    {
        const auto pending = pendingCommandResults.find(frame.commandSample.sequence);
        if (pending != pendingCommandResults.end())
        {
            mergeCommandResult(frame.commandSample, pending->second);
            pendingCommandResults.erase(pending);
        }
    }
    if (ringBuffer.size() >= maxFrames)
        ringBuffer.pop_front();
    ringBuffer.push_back(std::move(frame));
}

void PipelineTracer::recordCommandResult(const ViewCommandSample& sample)
{
    if (!isEnabled() || sample.sequence == 0)
        return;
    std::lock_guard<std::mutex> lock(mutex);
    for (auto frame = ringBuffer.rbegin(); frame != ringBuffer.rend(); ++frame)
    {
        if (frame->commandSample.sequence == sample.sequence)
        {
            mergeCommandResult(frame->commandSample, sample);
            return;
        }
    }
    pendingCommandResults[sample.sequence] = sample;
    if (pendingCommandResults.size() > 512)
        pendingCommandResults.erase(pendingCommandResults.begin());
}

void PipelineTracer::recordCommandDropped(uint64_t sequence)
{
    if (sequence == 0)
        return;
    ViewCommandSample sample;
    sample.sequence = sequence;
    sample.droppedBeforeSend = true;
    recordCommandResult(sample);
}

std::vector<PipelineFrame> PipelineTracer::getFrames() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return std::vector<PipelineFrame>(ringBuffer.begin(), ringBuffer.end());
}

void PipelineTracer::clear()
{
    std::lock_guard<std::mutex> lock(mutex);
    ringBuffer.clear();
    pendingCommandResults.clear();
}

size_t PipelineTracer::size() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return ringBuffer.size();
}

bool PipelineTracer::exportCSV(const std::string& path) const
{
    std::vector<PipelineFrame> frames = getFrames();
    if (frames.empty())
        return false;

    std::ofstream file(path);
    if (!file.is_open())
    {
        std::cerr << "[PipelineTracer] Failed to open CSV file: " << path << std::endl;
        return false;
    }

    // 写入 UTF-8 BOM（Excel 兼容）
    file << "\xEF\xBB\xBF";

    // CSV 表头
    file << "FrameID,BuildBackend,BuildRevision,BuildTimestampUtc,ControllerRevision,"
         << "AimPipelineRequestedMode,AimPipelineEffectiveMode,AimPipelineActiveAvailable,AimPipelineShadowProcessed,AimPipelineCommandSuppressed,AimPipelineOutputPaused,AimPipelineResetGeneration,AimPipelineObservationSequence,"
         << "AimPipelineTargetId,AimPipelineClassId,AimPipelineConfidence,AimPipelineRawPivotX,AimPipelineRawPivotY,AimPipelineEstimateValid,"
         << "AimPipelineAngleX,AimPipelineAngleY,AimPipelineRateX,AimPipelineRateY,AimPipelineCovarianceX,AimPipelineCovarianceY,"
         << "AimPipelineInnovationVarianceX,AimPipelineInnovationVarianceY,AimPipelineInnovationX,AimPipelineInnovationY,AimPipelineNisX,AimPipelineNisY,"
         << "AimPipelineMeasurementConfidence,AimPipelineFeedforwardConfidence,"
         << "AimPipelineEstimatorMode,AimPipelineManeuverModelActive,AimPipelineEstimatorSelectionChanged,AimPipelineEstimatorSelectionCount,"
         << "AimPipelineCaJerkStdDps3,AimPipelineManeuverRateThresholdDps,AimPipelineManeuverHoldMs,AimPipelineManeuverHoldRemainingMs,AimPipelineManeuverRateUncertaintyX,AimPipelineManeuverRateUncertaintyY,AimPipelineManeuverRateEvidenceDps,AimPipelineModelAngleDeltaDeg,AimPipelineModelRateDeltaDps,"
         << "AimPipelineBaselineAngleX,AimPipelineBaselineAngleY,AimPipelineBaselineRateX,AimPipelineBaselineRateY,AimPipelineBaselineCovarianceX,AimPipelineBaselineCovarianceY,AimPipelineBaselineNisX,AimPipelineBaselineNisY,"
         << "AimPipelineCaAngleX,AimPipelineCaAngleY,AimPipelineCaRateX,AimPipelineCaRateY,AimPipelineCaCovarianceX,AimPipelineCaCovarianceY,AimPipelineCaNisX,AimPipelineCaNisY,"
         << "AimPipelineControlValid,AimPipelineControlSpeedLimited,AimPipelineIntegralFrozen,AimPipelineSettled,AimPipelineSettleReleased,AimPipelineSettleConfirmationSamples,AimPipelineLowSpeedReverseSuppressed,AimPipelineVerticalCatchUpActive,AimPipelineReverseConfirmationSeconds,AimPipelineEffectiveResponseSecondsY,"
         << "AimPipelineFeedbackX,AimPipelineFeedbackY,AimPipelineTrackingFeedforwardX,AimPipelineTrackingFeedforwardY,AimPipelineLeadReferenceX,AimPipelineLeadReferenceY,"
         << "AimPipelineLeadCountsX,AimPipelineLeadCountsY,AimPipelineIntegralCountsX,AimPipelineIntegralCountsY,AimPipelineUnlimitedCountsX,AimPipelineUnlimitedCountsY,"
         << "AimPipelineRequestedCountsX,AimPipelineRequestedCountsY,AimPipelineFrameCountLimit,"
         << "RecoverySpeedAdviceEligible,RecoverySpeedAdviceActive,RecoverySpeedAdviceExited,RecoverySpeedAdviceLimited,RecoverySpeedBaselineMaxCps,RecoverySpeedAdvisoryMaxCps,RecoverySpeedAdvisoryFrameCountLimit,RecoverySpeedAdvisoryRequestedCountsX,RecoverySpeedAdvisoryRequestedCountsY,RecoverySpeedBaselineStaticBudgetFrames,RecoverySpeedAdvisoryStaticBudgetFrames,RecoverySpeedStaticBudgetFramesSaved,"
         << "TrajectoryRequestValid,TrajectoryRequestSequence,TrajectoryRequestTimeNs,TrajectoryRequestDurationMs,TrajectoryShaperMode,TrajectoryOutputProduced,TrajectoryOutputRequestSequence,TrajectoryScheduledTickNs,TrajectoryOutputTickNs,"
         << "TrajectoryVelocityLimited,TrajectoryAccelerationLimited,TrajectoryJerkLimited,TrajectoryEmergencyReset,"
         << "TrajectoryPositionX,TrajectoryPositionY,TrajectoryTargetVelocityX,TrajectoryTargetVelocityY,TrajectoryVelocityX,TrajectoryVelocityY,TrajectoryAccelerationX,TrajectoryAccelerationY,TrajectoryJerkX,TrajectoryJerkY,"
         << "TrajectoryShapedCountsX,TrajectoryShapedCountsY,TrajectoryQuantizationRemainderX,TrajectoryQuantizationRemainderY,TrajectoryShapingDelayMs,TrajectorySchedulerLatenessMs,TrajectorySchedulerSkippedTicks,"
         << "AimPipelineOutputCountsX,AimPipelineOutputCountsY,AimPipelineTrajectoryCommandSuppressed,"
         << "ViewMotionShadowValid,CommandToFrameDelayMs,CommandResponseMs,ManeuverRateUncertaintyGain,ManeuverRateUncertaintyTailMs,AppliedCameraRateYawDps,AppliedCameraRatePitchDps,ManeuverUncertaintyRateYawDps,ManeuverUncertaintyRatePitchDps,ViewMotionManeuverRateUncertaintyX,ViewMotionManeuverRateUncertaintyY,DegreesPerCountX,DegreesPerCountY,MeasuredLosYawDegrees,MeasuredLosPitchDownDegrees,"
         << "AppliedCameraYawAtObservationDegrees,AppliedCameraPitchAtObservationDegrees,AppliedCameraYawAtControlDegrees,AppliedCameraPitchAtControlDegrees,"
         << "StabilizedLosYawDegrees,StabilizedLosPitchDownDegrees,RelativeErrorYawDegrees,RelativeErrorPitchDownDegrees,"
         << "CaptureFrameSequence,BackendReceiveNs,CaptureSubmitNs,InferenceStartNs,InferencePublishNs,ControlTimeNs,"
         << "SourceTimestampAvailable,SourceTimestamp,SourceTimecodeAvailable,SourceTimecode,SourceTimestampMapped,"
         << "CaptureRoiX,CaptureRoiY,CaptureRoiWidth,CaptureRoiHeight,TimingComplete,TimingOrderValid,TimingAnomaly,"
         << "BackendQueueMs,SubmitToInferenceMs,InferenceToPublishMs,PublishToControlMs,LocalObservationAgeMs,"
         << "Timestamp,Resolution,SourceWidth,SourceHeight,FPS,InferenceFPS,TrackerStaleTimeoutMs,"
         << "DmlModel,DmlInputWidth,DmlInputHeight,DmlPreprocessMs,DmlTensorSetupMs,DmlInferenceMs,DmlCopyMs,DmlPostprocessMs,DmlNmsMs,DmlTotalMs,"
         << "SourceDeclaredFPS,SourceReceiveFPS,SourceReceivedFrames,SourceDroppedFrames,"
         << "NdiDeclaredFPS,NdiReceiveFPS,NdiReceivedFrames,NdiDroppedFrames,TargetDetected,ObservationAgeSec,"
         << "TargetClassID,"
         << "RawPivotX,RawPivotY,TargetBoxX,TargetBoxY,TargetBoxWidth,TargetBoxHeight,"
         << "FilteredX,FilteredY,ObservedVelocityX,ObservedVelocityY,ObservedSpeed,FilterTrendSpeed,FilterTrendActive,FilterResidual,"
         << "PredictionApplied,PredictionEnabled,PredictionAdditionalLeadMs,PredictionVelocityTauMs,PredictionStrength,"
         << "PredictionVelocityX,PredictionVelocityY,PredictionAccelerationX,PredictionAccelerationY,"
         << "PredictionLeadMs,PredictionOffsetX,PredictionOffsetY,"
         << "ViewMotionX,ViewMotionY,ViewMotionCompensationDelayMs,ViewMotionCompensationResponseMs,PredictionDirectionLocked,PredictionSelfMotionSuppressed,PredictionOscillationSuppressed,PredictionHighSpeedSuppressed,PredictionStationarySuppressed,PredictionMotionEvidenceSuppressed,PredictedX,PredictedY,"
         << "ErrorX,ErrorY,ErrorDistance,"
         << "RequestedPixelX,RequestedPixelY,RequestedCountsX,RequestedCountsY,IntegralCountsX,IntegralCountsY,"
         << "FinalMx,FinalMy,"
         << "CommandSequence,CommandEnqueueSucceeded,CommandEnqueueNs,CommandSendAttempted,CommandSendSucceeded,CommandDeviceSendNs,CommandDroppedBeforeSend,"
         << "CommandRequestedCountsX,CommandRequestedCountsY,CommandAppliedCountsX,CommandAppliedCountsY,CommandQueueAgeMs,"
         << "ProfileCalibrationEnabled,ProfileCalibrationValidX,ProfileCalibrationValidY,"
         << "ProfileCalibrationPixelsPerCountX,ProfileCalibrationPixelsPerCountY,"
         << "ProfileCalibrationDegreesPerCountX,ProfileCalibrationDegreesPerCountY,"
         << "ProfileCalibrationDelayMsX,ProfileCalibrationDelayMsY,ProfileCalibrationDriftX,ProfileCalibrationDriftY,"
         << "ProfileCalibrationRmseX,ProfileCalibrationRmseY,ProfileCalibrationCorrelationX,ProfileCalibrationCorrelationY,"
         << "ProfileCalibrationConfidenceX,ProfileCalibrationConfidenceY,ProfileCalibrationSamplesX,ProfileCalibrationSamplesY,"
         << "ProfileCalibrationActiveSamplesX,ProfileCalibrationActiveSamplesY,ProfileCalibrationOverallConfidence,"
         << "MachineProfileLevel,MachineProfileCacheRequested,MachineProfileCacheLoaded,MachineProfileCacheMatched,MachineProfileCalibratedResponseEnabled,MachineProfilePredictionEnabled,MachineProfileIntegralEnabled,MachineProfileFeedforwardScale,MachineProfileDegreesPerCountX,MachineProfileDegreesPerCountY,MachineProfileReason,"
         << "ResponseSeconds,EffectiveResponseSecondsX,EffectiveResponseSecondsY,IntegralTimeSeconds,MaxCountsPerSecond,ConditionalSpeedBudgetActive,FrameCountLimit,ControllerUpdateIntervalMs,"
         << "ErrorMotion,ErrorMotionX,ErrorMotionY,SettleMotionThreshold,MovingInsideSettle,MovingInsideSettleX,MovingInsideSettleY,HorizontalCatchUp,VerticalCatchUp,SpeedLimited,Settled,SettledX,SettledY,QueuedMoveCount\n";

    for (const auto& f : frames)
    {
        // 时间戳转换为毫秒偏移（相对于 epoch）
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            f.timestamp.time_since_epoch()).count();

        const AimPipelineFrameState& ap = f.aimPipeline;
        const FrameTiming& timing = f.frameTiming;
        const ViewCommandSample& command = f.commandSample;
        const bool timingComplete = frameTimingComplete(timing);
        const bool timingOrdered = frameTimingOrdered(timing);
        file << f.frameId << ','
             << BuildIdentity::backend() << ','
             << BuildIdentity::revision() << ','
             << BuildIdentity::timestampUtc() << ','
             << kBasicAimControllerRevision << ','
             << aimPipelineModeName(ap.requestedMode) << ','
             << aimPipelineModeName(ap.effectiveMode) << ','
             << (ap.activeAvailable ? '1' : '0') << ','
             << (ap.shadowProcessed ? '1' : '0') << ','
             << (ap.commandSuppressed ? '1' : '0') << ','
             << (ap.observation.outputPaused ? '1' : '0') << ','
             << ap.resetGeneration << ',' << ap.observationSequence << ','
             << ap.observation.trackId << ',' << ap.observation.classId << ','
             << ap.observation.confidence << ','
             << ap.observation.pivotX << ',' << ap.observation.pivotY << ','
             << (ap.estimate.valid ? '1' : '0') << ','
             << ap.estimate.angleX << ',' << ap.estimate.angleY << ','
             << ap.estimate.rateX << ',' << ap.estimate.rateY << ','
             << ap.estimate.covarianceX << ',' << ap.estimate.covarianceY << ','
             << ap.estimate.innovationVarianceX << ',' << ap.estimate.innovationVarianceY << ','
             << ap.estimate.innovationX << ',' << ap.estimate.innovationY << ','
             << ap.estimate.nisX << ',' << ap.estimate.nisY << ','
             << ap.estimate.measurementConfidence << ','
             << ap.estimate.feedforwardConfidence << ','
             << maneuverLosEstimatorModeName(ap.maneuverEstimator.mode) << ','
             << (ap.maneuverEstimator.maneuverModelActive ? '1' : '0') << ','
             << (ap.maneuverEstimator.selectionChanged ? '1' : '0') << ','
             << ap.maneuverEstimator.selectionCount << ','
             << ap.maneuverEstimator.jerkStdDegreesPerSecond3 << ','
             << ap.maneuverEstimator.maneuverRateThresholdDegreesPerSecond << ','
             << ap.maneuverEstimator.maneuverHoldSeconds * 1000.0 << ','
             << ap.maneuverEstimator.maneuverHoldRemainingSeconds * 1000.0 << ','
             << ap.maneuverEstimator.maneuverRateUncertaintyXDegreesPerSecond << ','
             << ap.maneuverEstimator.maneuverRateUncertaintyYDegreesPerSecond << ','
             << ap.maneuverEstimator.maneuverRateEvidenceDegreesPerSecond << ','
             << ap.maneuverEstimator.modelAngleDeltaDegrees << ','
             << ap.maneuverEstimator.modelRateDeltaDegreesPerSecond << ','
             << ap.baselineEstimate.angleX << ',' << ap.baselineEstimate.angleY << ','
             << ap.baselineEstimate.rateX << ',' << ap.baselineEstimate.rateY << ','
             << ap.baselineEstimate.covarianceX << ',' << ap.baselineEstimate.covarianceY << ','
             << ap.baselineEstimate.nisX << ',' << ap.baselineEstimate.nisY << ','
             << ap.constantAccelerationEstimate.angleX << ','
             << ap.constantAccelerationEstimate.angleY << ','
             << ap.constantAccelerationEstimate.rateX << ','
             << ap.constantAccelerationEstimate.rateY << ','
             << ap.constantAccelerationEstimate.covarianceX << ','
             << ap.constantAccelerationEstimate.covarianceY << ','
             << ap.constantAccelerationEstimate.nisX << ','
             << ap.constantAccelerationEstimate.nisY << ','
             << (ap.control.valid ? '1' : '0') << ','
             << (ap.control.speedLimited ? '1' : '0') << ','
             << (ap.control.integralFrozen ? '1' : '0') << ','
             << (ap.control.settled ? '1' : '0') << ','
             << (ap.control.settleReleased ? '1' : '0') << ','
             << ap.control.settleConfirmationSamples << ','
             << (ap.control.lowSpeedReverseSuppressed ? '1' : '0') << ','
             << (ap.control.verticalCatchUpActive ? '1' : '0') << ','
             << ap.control.reverseConfirmationSeconds << ','
             << ap.control.effectiveResponseSecondsY << ','
             << ap.control.feedbackX << ',' << ap.control.feedbackY << ','
             << ap.control.trackingFeedforwardX << ',' << ap.control.trackingFeedforwardY << ','
             << ap.control.leadReferenceX << ',' << ap.control.leadReferenceY << ','
             << ap.control.leadCountsX << ',' << ap.control.leadCountsY << ','
             << ap.control.integralCountsX << ',' << ap.control.integralCountsY << ','
             << ap.control.unlimitedCountsX << ',' << ap.control.unlimitedCountsY << ','
             << ap.control.requestedCountsX << ',' << ap.control.requestedCountsY << ','
             << ap.control.frameCountLimit << ','
             << (ap.recoverySpeedAdvice.eligible ? '1' : '0') << ','
             << (ap.recoverySpeedAdvice.active ? '1' : '0') << ','
             << (ap.recoverySpeedAdvice.exited ? '1' : '0') << ','
             << (ap.recoverySpeedAdvice.advisorySpeedLimited ? '1' : '0') << ','
             << ap.recoverySpeedAdvice.baselineMaxCountsPerSecond << ','
             << ap.recoverySpeedAdvice.advisoryMaxCountsPerSecond << ','
             << ap.recoverySpeedAdvice.advisoryFrameCountLimit << ','
             << ap.recoverySpeedAdvice.advisoryRequestedCountsX << ','
             << ap.recoverySpeedAdvice.advisoryRequestedCountsY << ','
             << ap.recoverySpeedAdvice.baselineStaticBudgetFrames << ','
             << ap.recoverySpeedAdvice.advisoryStaticBudgetFrames << ','
             << ap.recoverySpeedAdvice.staticBudgetFramesSaved << ','
             << (ap.trajectoryRequest.valid ? '1' : '0') << ','
             << ap.trajectoryRequest.sequence << ','
             << steadyTimeNs(ap.trajectoryRequest.requestTime) << ','
             << ap.trajectoryRequest.requestDurationSeconds * 1000.0 << ','
             << trajectoryShaperModeName(ap.trajectoryOutput.mode) << ','
             << (ap.trajectoryOutput.outputProduced ? '1' : '0') << ','
             << ap.trajectoryOutput.requestSequence << ','
             << steadyTimeNs(ap.trajectoryOutput.scheduledTickTime) << ','
             << steadyTimeNs(ap.trajectoryOutput.outputTickTime) << ','
             << (ap.trajectoryOutput.velocityLimited ? '1' : '0') << ','
             << (ap.trajectoryOutput.accelerationLimited ? '1' : '0') << ','
             << (ap.trajectoryOutput.jerkLimited ? '1' : '0') << ','
             << (ap.trajectoryOutput.emergencyReset ? '1' : '0') << ','
             << ap.trajectoryState.positionCountsX << ','
             << ap.trajectoryState.positionCountsY << ','
             << ap.trajectoryState.targetVelocityCountsPerSecX << ','
             << ap.trajectoryState.targetVelocityCountsPerSecY << ','
             << ap.trajectoryState.velocityCountsPerSecX << ','
             << ap.trajectoryState.velocityCountsPerSecY << ','
             << ap.trajectoryState.accelerationCountsPerSec2X << ','
             << ap.trajectoryState.accelerationCountsPerSec2Y << ','
             << ap.trajectoryState.jerkCountsPerSec3X << ','
             << ap.trajectoryState.jerkCountsPerSec3Y << ','
             << ap.trajectoryOutput.shapedCountsX << ','
             << ap.trajectoryOutput.shapedCountsY << ','
             << ap.trajectoryOutput.quantizationRemainderX << ','
             << ap.trajectoryOutput.quantizationRemainderY << ','
             << ap.trajectoryOutput.shapingDelayMs << ','
             << ap.trajectoryOutput.schedulerLatenessMs << ','
             << ap.trajectoryOutput.schedulerSkippedTicks << ','
             << ap.trajectoryOutput.outputCountsX << ',' << ap.trajectoryOutput.outputCountsY << ','
             << (ap.trajectoryOutput.commandSuppressed ? '1' : '0') << ','
              << (ap.viewMotion.valid ? '1' : '0') << ','
              << ap.viewMotion.commandToFrameDelayMs << ','
              << ap.viewMotion.commandResponseMs << ','
              << ap.viewMotion.maneuverRateUncertaintyGain << ','
              << ap.viewMotion.maneuverRateUncertaintyTailMs << ','
              << ap.viewMotion.appliedCameraRateYawDegreesPerSecond << ','
              << ap.viewMotion.appliedCameraRatePitchDegreesPerSecond << ','
              << ap.viewMotion.maneuverUncertaintyRateYawDegreesPerSecond << ','
              << ap.viewMotion.maneuverUncertaintyRatePitchDegreesPerSecond << ','
              << ap.viewMotion.maneuverRateUncertaintyXDegreesPerSecond << ','
              << ap.viewMotion.maneuverRateUncertaintyYDegreesPerSecond << ','
              << ap.viewMotion.degreesPerCountX << ',' << ap.viewMotion.degreesPerCountY << ','
             << ap.viewMotion.measuredLosYawDegrees << ','
             << ap.viewMotion.measuredLosPitchDownDegrees << ','
             << ap.viewMotion.appliedCameraYawAtObservationDegrees << ','
             << ap.viewMotion.appliedCameraPitchAtObservationDegrees << ','
             << ap.viewMotion.appliedCameraYawAtControlDegrees << ','
             << ap.viewMotion.appliedCameraPitchAtControlDegrees << ','
             << ap.viewMotion.stabilizedLosYawDegrees << ','
             << ap.viewMotion.stabilizedLosPitchDownDegrees << ','
             << ap.viewMotion.relativeErrorYawDegrees << ','
             << ap.viewMotion.relativeErrorPitchDownDegrees << ','
             << timing.frameSequence << ','
             << steadyTimeNs(timing.backendReceiveTime) << ','
             << steadyTimeNs(timing.captureSubmitTime) << ','
             << steadyTimeNs(timing.inferenceStartTime) << ','
             << steadyTimeNs(timing.inferencePublishTime) << ','
             << steadyTimeNs(timing.controlTime) << ','
             << (timing.sourceTimestampAvailable ? '1' : '0') << ','
             << timing.sourceTimestamp << ','
             << (timing.sourceTimecodeAvailable ? '1' : '0') << ','
             << timing.sourceTimecode << ','
             << (timing.sourceTimestampMapped ? '1' : '0') << ','
             << timing.roiX << ',' << timing.roiY << ','
             << timing.roiWidth << ',' << timing.roiHeight << ','
             << (timingComplete ? '1' : '0') << ','
             << (timingOrdered ? '1' : '0') << ','
             << (timingComplete && !timingOrdered ? '1' : '0') << ','
             << signedDurationMs(timing.backendReceiveTime, timing.captureSubmitTime) << ','
             << signedDurationMs(timing.captureSubmitTime, timing.inferenceStartTime) << ','
             << signedDurationMs(timing.inferenceStartTime, timing.inferencePublishTime) << ','
             << signedDurationMs(timing.inferencePublishTime, timing.controlTime) << ','
             << signedDurationMs(timing.backendReceiveTime, timing.controlTime) << ','
             << ms << ','
             << f.resolution << ','
             << f.sourceWidth << ',' << f.sourceHeight << ','
             << std::fixed << std::setprecision(3) << f.fpsValue << ','
             << f.inferenceFps << ','
             << f.trackerStaleTimeoutMs << ','
             << f.dmlModel << ',' << f.dmlInputWidth << ',' << f.dmlInputHeight << ','
             << f.dmlPreprocessMs << ',' << f.dmlTensorSetupMs << ',' << f.dmlInferenceMs << ','
             << f.dmlCopyMs << ',' << f.dmlPostprocessMs << ','
             << f.dmlNmsMs << ',' << f.dmlTotalMs << ','
             << f.sourceDeclaredFps << ',' << f.sourceReceiveFps << ','
             << f.sourceReceivedFrames << ',' << f.sourceDroppedFrames << ','
             << f.ndiDeclaredFps << ','
             << f.ndiReceiveFps << ',' << f.ndiReceivedFrames << ',' << f.ndiDroppedFrames << ','
             << (f.targetDetected ? '1' : '0') << ','
             << f.observationAgeSec << ','
             << f.targetClassId << ','
             << f.rawPivotX << ',' << f.rawPivotY << ','
             << f.targetBoxX << ',' << f.targetBoxY << ','
             << f.targetBoxWidth << ',' << f.targetBoxHeight << ','
             << f.filteredX << ',' << f.filteredY << ','
             << f.observedVelocityX << ',' << f.observedVelocityY << ','
             << f.observedSpeed << ',' << f.filterTrendSpeed << ','
             << (f.filterTrendActive ? '1' : '0') << ',' << f.filterResidual << ','
             << (f.predictionApplied ? '1' : '0') << ','
             << (f.predictionEnabled ? '1' : '0') << ','
             << f.predictionAdditionalLeadMs << ',' << f.predictionVelocityTauMs << ','
             << f.predictionStrength << ','
             << f.predictionVelocityX << ',' << f.predictionVelocityY << ','
             << f.predictionAccelerationX << ',' << f.predictionAccelerationY << ','
             << f.predictionLeadMs << ',' << f.predictionOffsetX << ',' << f.predictionOffsetY << ','
             << f.viewMotionX << ',' << f.viewMotionY << ','
             << f.viewMotionCompensationDelayMs << ','
             << f.viewMotionCompensationResponseMs << ','
             << (f.predictionDirectionLocked ? '1' : '0') << ','
             << (f.predictionSelfMotionSuppressed ? '1' : '0') << ','
             << (f.predictionOscillationSuppressed ? '1' : '0') << ','
             << (f.predictionHighSpeedSuppressed ? '1' : '0') << ','
             << (f.predictionStationarySuppressed ? '1' : '0') << ','
             << (f.predictionMotionEvidenceSuppressed ? '1' : '0') << ','
             << f.predictedX << ',' << f.predictedY << ','
             << f.errorX << ',' << f.errorY << ',' << f.errorDistance << ','
             << f.requestedPixelX << ',' << f.requestedPixelY << ','
             << f.requestedCountsX << ',' << f.requestedCountsY << ','
             << f.integralCountsX << ',' << f.integralCountsY << ','
             << f.finalMx << ',' << f.finalMy << ','
             << command.sequence << ','
             << (command.enqueueSucceeded ? '1' : '0') << ','
             << steadyTimeNs(command.enqueueTime) << ','
             << (command.sendAttempted ? '1' : '0') << ','
             << (command.sendSucceeded ? '1' : '0') << ','
             << steadyTimeNs(command.deviceSendTime) << ','
             << (command.droppedBeforeSend ? '1' : '0') << ','
             << command.requestedCountsX << ',' << command.requestedCountsY << ','
             << command.appliedCountsX << ',' << command.appliedCountsY << ','
             << signedDurationMs(command.enqueueTime, command.deviceSendTime) << ','
             << (f.profileCalibrationEnabled ? '1' : '0') << ','
             << (f.profileCalibrationValidX ? '1' : '0') << ','
             << (f.profileCalibrationValidY ? '1' : '0') << ','
             << f.profileCalibrationPixelsPerCountX << ',' << f.profileCalibrationPixelsPerCountY << ','
             << f.profileCalibrationDegreesPerCountX << ',' << f.profileCalibrationDegreesPerCountY << ','
             << f.profileCalibrationDelayMsX << ',' << f.profileCalibrationDelayMsY << ','
             << f.profileCalibrationDriftX << ',' << f.profileCalibrationDriftY << ','
             << f.profileCalibrationRmseX << ',' << f.profileCalibrationRmseY << ','
             << f.profileCalibrationCorrelationX << ',' << f.profileCalibrationCorrelationY << ','
             << f.profileCalibrationConfidenceX << ',' << f.profileCalibrationConfidenceY << ','
             << f.profileCalibrationSamplesX << ',' << f.profileCalibrationSamplesY << ','
             << f.profileCalibrationActiveSamplesX << ',' << f.profileCalibrationActiveSamplesY << ','
             << f.profileCalibrationOverallConfidence << ','
             << f.machineProfileLevel << ','
             << (f.machineProfileCacheRequested ? '1' : '0') << ','
             << (f.machineProfileCacheLoaded ? '1' : '0') << ','
             << (f.machineProfileCacheMatched ? '1' : '0') << ','
             << (f.machineProfileCalibratedResponseEnabled ? '1' : '0') << ','
             << (f.machineProfilePredictionEnabled ? '1' : '0') << ','
             << (f.machineProfileIntegralEnabled ? '1' : '0') << ','
             << f.machineProfileFeedforwardScale << ','
             << f.machineProfileDegreesPerCountX << ','
             << f.machineProfileDegreesPerCountY << ','
             << f.machineProfileReason << ','
             << f.responseSeconds << ',' << f.effectiveResponseSecondsX << ','
             << f.effectiveResponseSecondsY << ','
             << f.integralTimeSeconds << ','
             << f.maxCountsPerSecond << ','
             << (f.conditionalSpeedBudgetActive ? '1' : '0') << ','
             << f.frameCountLimit << ','
             << f.controllerUpdateIntervalMs << ','
             << f.errorMotion << ',' << f.errorMotionX << ',' << f.errorMotionY << ','
             << f.settleMotionThreshold << ','
             << (f.movingInsideSettle ? '1' : '0') << ','
             << (f.movingInsideSettleX ? '1' : '0') << ','
             << (f.movingInsideSettleY ? '1' : '0') << ','
             << (f.horizontalCatchUp ? '1' : '0') << ','
             << (f.verticalCatchUp ? '1' : '0') << ','
             << (f.speedLimited ? '1' : '0') << ','
             << (f.settled ? '1' : '0') << ','
             << (f.settledX ? '1' : '0') << ','
             << (f.settledY ? '1' : '0') << ','
             << f.queuedMoveCount << '\n';
    }

    file.close();
    std::cout << "[PipelineTracer] Exported " << frames.size()
              << " frames to " << path << std::endl;
    return true;
}
