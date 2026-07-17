#ifndef SHADOW_RESPONSE_REPLAY_H
#define SHADOW_RESPONSE_REPLAY_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "maneuver_los_estimator.h"

namespace ShadowResponseReplay
{
struct AxisResponse
{
    double centerMs = 20.0;
    double widthMs = 20.0;
};

struct Candidate
{
    AxisResponse horizontal{};
    AxisResponse vertical{};
    double maneuverUncertaintyGain = 0.0;
    double maneuverUncertaintyTailMs = 0.0;
};

struct Command
{
    std::int64_t sequence = 0;
    std::int64_t sendTimeNs = 0;
    int countsX = 0;
    int countsY = 0;
    double degreesPerCountX = 0.0;
    double degreesPerCountY = 0.0;
};

struct Sample
{
    std::int64_t observationTimeNs = 0;
    std::int64_t controlTimeNs = 0;
    std::uint64_t resetGeneration = 0;
    int targetId = 0;
    double measuredYawDegrees = 0.0;
    double measuredPitchDownDegrees = 0.0;
    float confidence = 0.0f;
    bool outputPaused = false;
    bool settled = false;
    bool recordedManeuverActive = false;
};

struct Timeline
{
    std::string source;
    std::vector<Sample> samples;
    std::vector<Command> commands;
    double recordedCenterMs = 0.0;
    double recordedWidthMs = 0.0;
};

struct Metrics
{
    std::string source;
    Candidate candidate{};
    std::size_t rows = 0;
    std::size_t runningSamples = 0;
    std::size_t pausedSamples = 0;
    std::size_t runningActiveSamples = 0;
    std::size_t pausedActiveSamples = 0;
    std::size_t runningActiveSegments = 0;
    std::size_t settledActiveSamples = 0;
    std::size_t selectionChanges = 0;
    bool recordedSelectionCompared = false;
    std::size_t recordedSelectionMismatches = 0;
    double maxAbsRateX = 0.0;
    double maxAbsRateY = 0.0;
    double maxManeuverUncertainty = 0.0;
    double maxManeuverEvidence = 0.0;
    std::vector<std::size_t> failedTrials;
};

struct TraceRow
{
    std::size_t row = 0;
    std::size_t trial = 0;
    std::int64_t observationTimeNs = 0;
    bool outputPaused = false;
    bool settled = false;
    bool active = false;
    double cameraRateX = 0.0;
    double cameraRateY = 0.0;
    double uncertaintyX = 0.0;
    double uncertaintyY = 0.0;
    double estimatedRateX = 0.0;
    double estimatedRateY = 0.0;
    double maneuverEvidence = 0.0;
    double holdRemainingSeconds = 0.0;
};

struct PhysicalFitMetrics
{
    std::string source;
    Candidate candidate{};
    std::size_t anchoredSegments = 0;
    std::size_t responseSamples = 0;
    double residualP50XDegrees = 0.0;
    double residualP50YDegrees = 0.0;
    double residualP95XDegrees = 0.0;
    double residualP95YDegrees = 0.0;
    double residualRmseXDegrees = 0.0;
    double residualRmseYDegrees = 0.0;
    double scoreDegrees = 0.0;
};

// 读取流水线CSV中的真实观测和已确认设备命令。解析器按CSV引号规则处理字段，
// 不依赖固定列位置；缺少时间、LOS、置信度或命令诊断时直接拒绝，禁止静默降级。
bool LoadTimelineCsv(const std::filesystem::path& path,
    Timeline& timeline, std::string& error);

// 在完全相同的观测时间线上重放响应核和现有门控CA估计器。degrees-per-count覆盖值
// 大于0时优先使用精确Profile值，避免CSV三位小数显示精度累积成角度漂移。
Metrics Evaluate(const Timeline& timeline, const Candidate& candidate,
    const ManeuverLosEstimator::Settings& settings,
    double degreesPerCountXOverride = 0.0,
    double degreesPerCountYOverride = 0.0,
    bool compareRecordedSelection = false,
    std::vector<TraceRow>* trace = nullptr);

// 固定目标物理拟合以每个运行轮次中“最近命令已超过quietWindow”的观测作为
// 世界角锚点，只评价命令后responseWindow内的稳定化残差。这样拟合目标是实际
// 画面响应，而不是机动模型是否触发；缺少安静锚点的片段不会静默参与评分。
PhysicalFitMetrics FitPhysicalResponse(
    const Timeline& timeline,
    const Candidate& candidate,
    double degreesPerCountXOverride = 0.0,
    double degreesPerCountYOverride = 0.0,
    double quietWindowMs = 150.0,
    double responseWindowMs = 150.0);
PhysicalFitMetrics FitPhysicalResponse(
    const std::vector<Timeline>& timelines,
    const Candidate& candidate,
    double degreesPerCountXOverride = 0.0,
    double degreesPerCountYOverride = 0.0,
    double quietWindowMs = 150.0,
    double responseWindowMs = 150.0);
}

#endif
