#ifndef CROSS_DOMAIN_REPLAY_H
#define CROSS_DOMAIN_REPLAY_H

#include <filesystem>
#include <string>
#include <vector>

#include "runtime/video_replay_math.h"

namespace CrossDomainReplay
{
struct SourceTrajectory
{
    std::string scenario;
    int sourceWidth = 0;
    int sourceHeight = 0;
    double sourceFovXDegrees = 106.0;
    double sourceFovYDegrees = 74.0;
    double centerX = 0.0;
    double centerY = 0.0;
    double detectionWidth = 320.0;
    double detectionHeight = 320.0;
    std::vector<VideoReplay::TrajectoryPoint> points;
};

struct Variant
{
    std::string name;
    int sourceWidth = 2560;
    int sourceHeight = 1440;
    double fovXDegrees = 106.0;
    double fovYDegrees = 74.0;
    double replayFps = 94.0;
    double observationDelayMs = 15.0;
    double commandToFrameDelayMs = 60.0;
    double speedScale = 1.0;
    // 相对运动缩放直接作用于视线轨迹：同向等速接近0，角色更快时允许自然反号。
    double relativeMotionScale = 1.0;
};

struct ControllerSettings
{
    double responseSeconds = 0.080;
    double verticalCatchUpErrorDegrees = 0.8;
    double maxCountsPerSecond = 1440.0;
    double legacyPredictionLeadSeconds = 0.050;
    double legacyPredictionWindowSeconds = 0.050;
    double legacyPredictionStrength = 1.0;
    double degreesPerCountX = 0.0308;
    double degreesPerCountY = 0.0308;
    double feedforwardGain = 0.0;
    double integralTimeSeconds = 0.0;
    double integralZoneDegrees = 1.0;
    double settleErrorDegrees = 0.080;
    double settleRateDegreesPerSecond = 1.200;
    double reverseConfirmationSeconds = 0.080;
};

struct Metrics
{
    size_t samples = 0;
    double errorP50Degrees = 0.0;
    double errorP95Degrees = 0.0;
    double errorP99Degrees = 0.0;
    double verticalP95Degrees = 0.0;
    double insideBoxPercent = 0.0;
    double edgeMarginP05Degrees = 0.0;
    double interruptionPercent = 0.0;
    size_t outputDirectionFlips = 0;
    size_t estimatedDirectionErrors = 0;
    size_t estimatedRateSignFlips = 0;
    size_t nonZeroOutputsAfterLoss = 0;
    double lateHalfErrorP95Degrees = 0.0;
    double meanNis = 0.0;
    double meanCovariance = 0.0;
    double meanFeedforwardConfidence = 0.0;
    double requestedCounts = 0.0;
    double shapedCounts = 0.0;
    double sentCounts = 0.0;
    double feedforwardCounts = 0.0;
    double settledPercent = 0.0;
    size_t settleReleases = 0;
    double reverseSuppressedPercent = 0.0;
    double verticalCatchUpPercent = 0.0;
};

struct Comparison
{
    std::string scenario;
    Variant variant{};
    Metrics legacy{};
    Metrics candidate{};
    bool passed = false;
    std::string reason;
};

std::vector<Variant> BuildRequiredVariants();
Comparison RunComparison(const SourceTrajectory& source, const Variant& variant,
                         const ControllerSettings& settings,
                         const std::filesystem::path& frameCsv = {});
bool EvaluateGate(const std::string& scenario, const Variant& variant,
                  const Metrics& legacy, const Metrics& candidate,
                  std::string& reason);
void WriteSummary(const std::filesystem::path& path,
                  const std::vector<Comparison>& comparisons);
}

#endif
