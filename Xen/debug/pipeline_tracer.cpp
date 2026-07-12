#include "pipeline_tracer.h"

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
    maxFrames = n;

    // 立即裁剪现有数据
    std::lock_guard<std::mutex> lock(mutex);
    while (ringBuffer.size() > maxFrames)
        ringBuffer.pop_front();
}

PipelineFrame& PipelineTracer::beginFrame(int resolution)
{
    std::lock_guard<std::mutex> lock(mutex);

    if (ringBuffer.size() >= maxFrames)
        ringBuffer.pop_front();

    PipelineFrame frame;
    frame.frameId = frameCounter.fetch_add(1, std::memory_order_relaxed);
    frame.timestamp = std::chrono::steady_clock::now();
    frame.resolution = resolution;

    ringBuffer.push_back(std::move(frame));
    return ringBuffer.back();
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
    file << "FrameID,Timestamp,Resolution,FPS,TargetDetected,ObservationAgeSec,"
         << "TargetClassID,"
         << "RawPivotX,RawPivotY,"
         << "McPivotX,McPivotY,McDeltaX,McDeltaY,"
         << "PredX,PredY,VelocityX,VelocityY,HasExternalVel,"
         << "DistToTarget,SpeedMultiplier,"
         << "PpDx,PpDy,"
         << "CountsX,CountsY,"
         << "EmaCountsX,EmaCountsY,"
         << "FinalMx,FinalMy\n";

    for (const auto& f : frames)
    {
        // 时间戳转换为毫秒偏移（相对于 epoch）
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            f.timestamp.time_since_epoch()).count();

        file << f.frameId << ','
             << ms << ','
             << f.resolution << ','
             << std::fixed << std::setprecision(3) << f.fpsValue << ','
             << (f.targetDetected ? '1' : '0') << ','
             << f.observationAgeSec << ','
             << f.targetClassId << ','
             << f.rawPivotX << ',' << f.rawPivotY << ','
             << f.mcPivotX << ',' << f.mcPivotY << ','
             << f.mcDeltaX << ',' << f.mcDeltaY << ','
             << f.predX << ',' << f.predY << ','
             << f.velocityX << ',' << f.velocityY << ','
             << (f.hasExternalVel ? '1' : '0') << ','
             << f.distToTarget << ','
             << f.speedMultiplier << ','
             << f.ppDx << ',' << f.ppDy << ','
             << f.countsX << ',' << f.countsY << ','
             << f.emaCountsX << ',' << f.emaCountsY << ','
             << f.finalMx << ',' << f.finalMy << '\n';
    }

    file.close();
    std::cout << "[PipelineTracer] Exported " << frames.size()
              << " frames to " << path << std::endl;
    return true;
}
