#ifndef CROSS_DOMAIN_REPLAY_H
#define CROSS_DOMAIN_REPLAY_H

#include <filesystem>
#include <string>
#include <vector>

#include "runtime/video_replay_math.h"
#include "runtime/aim_pipeline_types.h"

namespace CrossDomainReplay
{
enum class CandidateEstimatorMode
{
    Kalman,
    ConstantAcceleration,
    ManeuverGatedConstantAcceleration,
    OracleControlTime
};

const char* candidateEstimatorModeName(CandidateEstimatorMode mode);

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
    double kalmanAccelerationStdDegreesPerSecond2 = 90.0;
    double kalmanMovingAccelerationStdDegreesPerSecond2 = 360.0;
    double kalmanMovingRateThresholdDegreesPerSecond = 8.0;
    // legacy响应保持冻结基线；候选响应为0时沿用基线，非0时只作用于新控制器。
    double responseSeconds = 0.080;
    double candidateResponseSeconds = 0.0;
    // 仅供离线归因：oracle直接使用控制时刻真实LOS状态；0表示候选沿用正式限速。
    CandidateEstimatorMode candidateEstimatorMode = CandidateEstimatorMode::Kalman;
    double candidateJerkStdDegreesPerSecond3 = 8000.0;
    double candidateManeuverRateThresholdDegreesPerSecond = 12.0;
    double candidateManeuverHoldSeconds = 0.120;
    double candidateMaxCountsPerSecond = 0.0;
    double verticalCatchUpErrorDegrees = 0.8;
    double maxCountsPerSecond = 1440.0;
    double legacyPredictionLeadSeconds = 0.050;
    double legacyPredictionWindowSeconds = 0.050;
    double legacyPredictionStrength = 1.0;
    double degreesPerCountX = 0.0308;
    double degreesPerCountY = 0.0308;
    double feedforwardGain = 0.0;
    double leadHorizonSeconds = 0.0;
    double leadStrength = 0.0;
    double reversalFeedforwardBoost = 0.0;
    double reversalFeedforwardSeconds = 0.0;
    double integralTimeSeconds = 0.0;
    double integralZoneDegrees = 1.0;
    double settleErrorDegrees = 0.080;
    double settleRateDegreesPerSecond = 1.200;
    double reverseConfirmationSeconds = 0.080;
    // 离线候选参数；运行时Settings默认1.5，正式配置与active路径均不开放此入口。
    double reverseConfirmationErrorMultiplier = 1.5;
    // 离线结构候选，默认关闭且不接入正式配置或active路径。
    bool confirmLowSpeedReverseSettleRelease = false;
    // 离线评估候选：物理静止场景以整段检测中心中位数作为固定真值，逐帧中心仍作为测量。
    bool staticFixedTruth = false;
    // 离线时轴诊断：按正式shadow链路区分画面中的相对LOS与成功命令形成的累计视角。
    bool candidateViewMotionCompensation = false;
    // 离线Smith型归因：在已发送命令兑现时刻比较目标与累计视角；0保持当前控制时刻语义。
    double candidateCommandCommitHorizonSeconds = 0.0;
    TrajectoryShaperMode trajectoryMode = TrajectoryShaperMode::Off;
    double trajectoryOutputHz = 240.0;
    double trajectoryMaxAccelerationCountsPerSecond2 = 60000.0;
    double trajectoryMaxJerkCountsPerSecond3 = 4000000.0;
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
    double reversalFeedforwardPercent = 0.0;
    double settledPercent = 0.0;
    size_t settleReleases = 0;
    double reverseSuppressedPercent = 0.0;
    double verticalCatchUpPercent = 0.0;
    double maneuverModelPercent = 0.0;
    size_t trajectoryOutputs = 0;
    double trajectoryVelocityLimitedPercent = 0.0;
    double trajectoryAccelerationLimitedPercent = 0.0;
    double trajectoryJerkLimitedPercent = 0.0;
};

struct Comparison
{
    std::string scenario;
    Variant variant{};
    double kalmanAccelerationStdDegreesPerSecond2 = 90.0;
    double kalmanMovingAccelerationStdDegreesPerSecond2 = 360.0;
    double kalmanMovingRateThresholdDegreesPerSecond = 8.0;
    double legacyResponseSeconds = 0.080;
    double candidateResponseSeconds = 0.080;
    CandidateEstimatorMode candidateEstimatorMode = CandidateEstimatorMode::Kalman;
    double candidateJerkStdDegreesPerSecond3 = 8000.0;
    double candidateManeuverRateThresholdDegreesPerSecond = 12.0;
    double candidateManeuverHoldSeconds = 0.120;
    double candidateMaxCountsPerSecond = 1440.0;
    double feedforwardGain = 0.0;
    double leadHorizonSeconds = 0.0;
    double leadStrength = 0.0;
    double reversalFeedforwardBoost = 0.0;
    double reversalFeedforwardSeconds = 0.0;
    double reverseConfirmationErrorMultiplier = 1.5;
    bool confirmLowSpeedReverseSettleRelease = false;
    bool staticFixedTruth = false;
    bool candidateViewMotionCompensation = false;
    double candidateCommandCommitHorizonSeconds = 0.0;
    TrajectoryShaperMode trajectoryMode = TrajectoryShaperMode::Off;
    double trajectoryOutputHz = 0.0;
    Metrics legacy{};
    Metrics candidate{};
    bool passed = false;
    std::string reason;
};

struct Attribution
{
    Comparison baseline{};
    Comparison oracleEstimator{};
    Comparison unlimitedActuator{};
    bool cohortStable = false;
    std::string classification;
};

std::vector<Variant> BuildRequiredVariants();
Comparison RunComparison(const SourceTrajectory& source, const Variant& variant,
                         const ControllerSettings& settings,
                         const std::filesystem::path& frameCsv = {},
                         const std::vector<unsigned char>* frozenDetectionTimeline = nullptr,
                         std::vector<unsigned char>* recordedDetectionTimeline = nullptr);
bool EvaluateGate(const std::string& scenario, const Variant& variant,
                  const Metrics& legacy, const Metrics& candidate,
                  std::string& reason);
void WriteSummary(const std::filesystem::path& path,
                  const std::vector<Comparison>& comparisons);
Attribution RunAttribution(const SourceTrajectory& source, const Variant& variant,
                           const ControllerSettings& settings,
                           const std::filesystem::path& frameCsv = {});
std::string ClassifyAttribution(const Comparison& baseline,
                                const Comparison& oracleEstimator,
                                const Comparison& unlimitedActuator,
                                bool& cohortStable);
void WriteAttributionSummary(const std::filesystem::path& path,
                             const std::vector<Attribution>& attributions);
}

#endif
