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
    if (ringBuffer.size() >= maxFrames)
        ringBuffer.pop_front();
    ringBuffer.push_back(std::move(frame));
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
         << "Timestamp,Resolution,SourceWidth,SourceHeight,FPS,InferenceFPS,"
         << "DmlModel,DmlInputWidth,DmlInputHeight,DmlPreprocessMs,DmlTensorSetupMs,DmlInferenceMs,DmlCopyMs,DmlPostprocessMs,DmlNmsMs,DmlTotalMs,"
         << "SourceDeclaredFPS,SourceReceiveFPS,SourceReceivedFrames,SourceDroppedFrames,"
         << "NdiDeclaredFPS,NdiReceiveFPS,NdiReceivedFrames,NdiDroppedFrames,TargetDetected,ObservationAgeSec,"
         << "TargetClassID,"
         << "RawPivotX,RawPivotY,TargetBoxX,TargetBoxY,TargetBoxWidth,TargetBoxHeight,"
         << "FilteredX,FilteredY,ObservedVelocityX,ObservedVelocityY,ObservedSpeed,FilterTrendSpeed,FilterTrendActive,FilterResidual,"
         << "PredictionApplied,PredictionEnabled,PredictionAdditionalLeadMs,PredictionVelocityTauMs,PredictionStrength,"
         << "PredictionVelocityX,PredictionVelocityY,PredictionAccelerationX,PredictionAccelerationY,"
         << "PredictionLeadMs,PredictionOffsetX,PredictionOffsetY,"
         << "ViewMotionX,ViewMotionY,PredictionDirectionLocked,PredictionSelfMotionSuppressed,PredictionOscillationSuppressed,PredictionHighSpeedSuppressed,PredictedX,PredictedY,"
         << "ErrorX,ErrorY,ErrorDistance,"
         << "RequestedPixelX,RequestedPixelY,RequestedCountsX,RequestedCountsY,IntegralCountsX,IntegralCountsY,"
         << "FinalMx,FinalMy,"
         << "ResponseSeconds,EffectiveResponseSecondsX,EffectiveResponseSecondsY,IntegralTimeSeconds,MaxCountsPerSecond,FrameCountLimit,"
         << "ErrorMotion,SettleMotionThreshold,MovingInsideSettle,HorizontalCatchUp,VerticalCatchUp,SpeedLimited,Settled,QueuedMoveCount\n";

    for (const auto& f : frames)
    {
        // 时间戳转换为毫秒偏移（相对于 epoch）
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            f.timestamp.time_since_epoch()).count();

        file << f.frameId << ','
             << BuildIdentity::backend() << ','
             << BuildIdentity::revision() << ','
             << BuildIdentity::timestampUtc() << ','
             << kBasicAimControllerRevision << ','
             << ms << ','
             << f.resolution << ','
             << f.sourceWidth << ',' << f.sourceHeight << ','
             << std::fixed << std::setprecision(3) << f.fpsValue << ','
             << f.inferenceFps << ','
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
             << (f.predictionDirectionLocked ? '1' : '0') << ','
             << (f.predictionSelfMotionSuppressed ? '1' : '0') << ','
             << (f.predictionOscillationSuppressed ? '1' : '0') << ','
             << (f.predictionHighSpeedSuppressed ? '1' : '0') << ','
             << f.predictedX << ',' << f.predictedY << ','
             << f.errorX << ',' << f.errorY << ',' << f.errorDistance << ','
             << f.requestedPixelX << ',' << f.requestedPixelY << ','
             << f.requestedCountsX << ',' << f.requestedCountsY << ','
             << f.integralCountsX << ',' << f.integralCountsY << ','
             << f.finalMx << ',' << f.finalMy << ','
             << f.responseSeconds << ',' << f.effectiveResponseSecondsX << ','
             << f.effectiveResponseSecondsY << ','
             << f.integralTimeSeconds << ','
             << f.maxCountsPerSecond << ','
             << f.frameCountLimit << ','
             << f.errorMotion << ',' << f.settleMotionThreshold << ','
             << (f.movingInsideSettle ? '1' : '0') << ','
             << (f.horizontalCatchUp ? '1' : '0') << ','
             << (f.verticalCatchUp ? '1' : '0') << ','
             << (f.speedLimited ? '1' : '0') << ','
             << (f.settled ? '1' : '0') << ','
             << f.queuedMoveCount << '\n';
    }

    file.close();
    std::cout << "[PipelineTracer] Exported " << frames.size()
              << " frames to " << path << std::endl;
    return true;
}
