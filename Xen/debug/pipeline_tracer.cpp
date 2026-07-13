#include "pipeline_tracer.h"

#include "runtime/basic_aim_controller.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#ifndef AIMBOT_BUILD_BACKEND
#define AIMBOT_BUILD_BACKEND "unknown"
#endif
#ifndef AIMBOT_BUILD_REVISION
#define AIMBOT_BUILD_REVISION "unknown"
#endif
#ifndef AIMBOT_BUILD_TIMESTAMP_UTC
#define AIMBOT_BUILD_TIMESTAMP_UTC "unknown"
#endif

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
         << "SourceDeclaredFPS,SourceReceiveFPS,SourceReceivedFrames,SourceDroppedFrames,"
         << "NdiDeclaredFPS,NdiReceiveFPS,NdiReceivedFrames,NdiDroppedFrames,TargetDetected,ObservationAgeSec,"
         << "TargetClassID,"
         << "RawPivotX,RawPivotY,"
         << "FilteredX,FilteredY,ObservedVelocityX,ObservedVelocityY,ObservedSpeed,FilterResidual,"
         << "ErrorX,ErrorY,ErrorDistance,"
         << "RequestedPixelX,RequestedPixelY,RequestedCountsX,RequestedCountsY,IntegralCountsX,IntegralCountsY,"
         << "FinalMx,FinalMy,"
         << "ResponseSeconds,IntegralTimeSeconds,MaxCountsPerSecond,FrameCountLimit,"
         << "ErrorMotion,SettleMotionThreshold,MovingInsideSettle,SpeedLimited,Settled,QueuedMoveCount\n";

    for (const auto& f : frames)
    {
        // 时间戳转换为毫秒偏移（相对于 epoch）
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            f.timestamp.time_since_epoch()).count();

        file << f.frameId << ','
             << AIMBOT_BUILD_BACKEND << ','
             << AIMBOT_BUILD_REVISION << ','
             << AIMBOT_BUILD_TIMESTAMP_UTC << ','
             << kBasicAimControllerRevision << ','
             << ms << ','
             << f.resolution << ','
             << f.sourceWidth << ',' << f.sourceHeight << ','
             << std::fixed << std::setprecision(3) << f.fpsValue << ','
             << f.inferenceFps << ','
             << f.sourceDeclaredFps << ',' << f.sourceReceiveFps << ','
             << f.sourceReceivedFrames << ',' << f.sourceDroppedFrames << ','
             << f.ndiDeclaredFps << ','
             << f.ndiReceiveFps << ',' << f.ndiReceivedFrames << ',' << f.ndiDroppedFrames << ','
             << (f.targetDetected ? '1' : '0') << ','
             << f.observationAgeSec << ','
             << f.targetClassId << ','
             << f.rawPivotX << ',' << f.rawPivotY << ','
             << f.filteredX << ',' << f.filteredY << ','
             << f.observedVelocityX << ',' << f.observedVelocityY << ','
             << f.observedSpeed << ',' << f.filterResidual << ','
             << f.errorX << ',' << f.errorY << ',' << f.errorDistance << ','
             << f.requestedPixelX << ',' << f.requestedPixelY << ','
             << f.requestedCountsX << ',' << f.requestedCountsY << ','
             << f.integralCountsX << ',' << f.integralCountsY << ','
             << f.finalMx << ',' << f.finalMy << ','
             << f.responseSeconds << ',' << f.integralTimeSeconds << ','
             << f.maxCountsPerSecond << ','
             << f.frameCountLimit << ','
             << f.errorMotion << ',' << f.settleMotionThreshold << ','
             << (f.movingInsideSettle ? '1' : '0') << ','
             << (f.speedLimited ? '1' : '0') << ','
             << (f.settled ? '1' : '0') << ','
             << f.queuedMoveCount << '\n';
    }

    file.close();
    std::cout << "[PipelineTracer] Exported " << frames.size()
              << " frames to " << path << std::endl;
    return true;
}
