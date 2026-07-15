#ifndef AIM_PIPELINE_TYPES_H
#define AIM_PIPELINE_TYPES_H

#include <chrono>
#include <cstdint>
#include <optional>

// P0-0 的运行模式只描述新链路的生命周期，不改变现有 r30 输出。
enum class AimPipelineMode
{
    Legacy,
    Shadow,
    Active
};

inline const char* aimPipelineModeName(AimPipelineMode mode)
{
    switch (mode)
    {
    case AimPipelineMode::Shadow: return "shadow";
    case AimPipelineMode::Active: return "active";
    case AimPipelineMode::Legacy:
    default: return "legacy";
    }
}

// 帧时间契约：所有时间点必须来自同一个 steady_clock 域；未知时间保持空值。
struct FrameTiming
{
    using Clock = std::chrono::steady_clock;
    Clock::time_point backendReceiveTime{};
    Clock::time_point captureSubmitTime{};
    Clock::time_point inferenceStartTime{};
    Clock::time_point inferencePublishTime{};
    Clock::time_point observationTime{};
    Clock::time_point controlTime{};
    uint64_t frameSequence = 0;
    int64_t sourceTimestamp = 0;
    int64_t sourceTimecode = 0;
    int sourceWidth = 0;
    int sourceHeight = 0;
    int roiX = 0;
    int roiY = 0;
    int roiWidth = 0;
    int roiHeight = 0;
    bool sourceTimestampAvailable = false;
    bool sourceTimecodeAvailable = false;
    bool sourceTimestampMapped = false;
};

inline bool frameTimeKnown(FrameTiming::Clock::time_point time)
{
    return time.time_since_epoch().count() != 0;
}

inline bool frameTimingComplete(const FrameTiming& timing)
{
    return frameTimeKnown(timing.backendReceiveTime) &&
        frameTimeKnown(timing.captureSubmitTime) &&
        frameTimeKnown(timing.inferenceStartTime) &&
        frameTimeKnown(timing.inferencePublishTime) &&
        frameTimeKnown(timing.controlTime);
}

inline bool frameTimingOrdered(const FrameTiming& timing)
{
    return frameTimingComplete(timing) &&
        timing.backendReceiveTime <= timing.captureSubmitTime &&
        timing.captureSubmitTime <= timing.inferenceStartTime &&
        timing.inferenceStartTime <= timing.inferencePublishTime &&
        timing.inferencePublishTime <= timing.controlTime;
}

inline std::optional<double> frameSignedDurationMs(
    FrameTiming::Clock::time_point start,
    FrameTiming::Clock::time_point end)
{
    if (!frameTimeKnown(start) || !frameTimeKnown(end))
        return std::nullopt;
    return std::chrono::duration<double, std::milli>(end - start).count();
}

// 统一目标观测契约；P0-0 只传递原始值，不在此处进行角度转换或滤波。
struct AimObservation
{
    FrameTiming timing{};
    int trackId = -1;
    int classId = -1;
    float confidence = 0.0f;
    double pivotX = 0.0;
    double pivotY = 0.0;
    double boxX = 0.0;
    double boxY = 0.0;
    double boxWidth = 0.0;
    double boxHeight = 0.0;
    bool valid = false;
};

// P0-0 预留的设备命令样本；applied 只允许填写设备确认成功后的实际值。
struct ViewCommandSample
{
    uint64_t sequence = 0;
    double requestedCountsX = 0.0;
    double requestedCountsY = 0.0;
    int appliedCountsX = 0;
    int appliedCountsY = 0;
    FrameTiming::Clock::time_point enqueueTime{};
    FrameTiming::Clock::time_point deviceSendTime{};
    bool enqueueSucceeded = false;
    bool sendAttempted = false;
    bool sendSucceeded = false;
    bool droppedBeforeSend = false;
};

// 后续 P0-2/P0-3 填充角度、角速度及统计量；当前阶段保持无效，避免伪造新估计。
struct LosEstimate
{
    bool valid = false;
    double angleX = 0.0;
    double angleY = 0.0;
    double rateX = 0.0;
    double rateY = 0.0;
    double covarianceX = 0.0;
    double covarianceY = 0.0;
    double innovationVarianceX = 0.0;
    double innovationVarianceY = 0.0;
    double innovationX = 0.0;
    double innovationY = 0.0;
    double nisX = 0.0;
    double nisY = 0.0;
    double measurementConfidence = 0.0;
    double feedforwardConfidence = 0.0;
};

// 新控制器的可审计分解；P0-0 默认全部为零且不参与旧控制器输出。
struct AimControlBreakdown
{
    bool valid = false;
    bool speedLimited = false;
    bool integralFrozen = false;
    double feedbackX = 0.0;
    double feedbackY = 0.0;
    double trackingFeedforwardX = 0.0;
    double trackingFeedforwardY = 0.0;
    double leadReferenceX = 0.0;
    double leadReferenceY = 0.0;
    double leadCountsX = 0.0;
    double leadCountsY = 0.0;
    double integralCountsX = 0.0;
    double integralCountsY = 0.0;
    double unlimitedCountsX = 0.0;
    double unlimitedCountsY = 0.0;
    double requestedCountsX = 0.0;
    double requestedCountsY = 0.0;
    double frameCountLimit = 0.0;
};

struct TrajectoryRequest
{
    double requestedCountsX = 0.0;
    double requestedCountsY = 0.0;
    FrameTiming::Clock::time_point requestTime{};
};

struct TrajectoryState
{
    double positionCountsX = 0.0;
    double positionCountsY = 0.0;
    double velocityCountsPerSecX = 0.0;
    double velocityCountsPerSecY = 0.0;
    double accelerationCountsPerSec2X = 0.0;
    double accelerationCountsPerSec2Y = 0.0;
};

struct TrajectoryOutput
{
    int outputCountsX = 0;
    int outputCountsY = 0;
    bool commandSuppressed = true;
    FrameTiming::Clock::time_point outputTickTime{};
};

struct ViewMotionShadowDiagnostics
{
    bool valid = false;
    double commandToFrameDelayMs = 0.0;
    double degreesPerCountX = 0.0;
    double degreesPerCountY = 0.0;
    double measuredLosYawDegrees = 0.0;
    double measuredLosPitchDownDegrees = 0.0;
    double appliedCameraYawAtObservationDegrees = 0.0;
    double appliedCameraPitchAtObservationDegrees = 0.0;
    double appliedCameraYawAtControlDegrees = 0.0;
    double appliedCameraPitchAtControlDegrees = 0.0;
    double stabilizedLosYawDegrees = 0.0;
    double stabilizedLosPitchDownDegrees = 0.0;
    double relativeErrorYawDegrees = 0.0;
    double relativeErrorPitchDownDegrees = 0.0;
};

// 单帧新链路快照，直接嵌入旧 PipelineFrame，保证同帧对照。
struct AimPipelineFrameState
{
    AimPipelineMode requestedMode = AimPipelineMode::Legacy;
    AimPipelineMode effectiveMode = AimPipelineMode::Legacy;
    bool activeAvailable = false;
    bool shadowProcessed = false;
    bool commandSuppressed = true;
    uint64_t resetGeneration = 0;
    uint64_t observationSequence = 0;
    AimObservation observation{};
    LosEstimate estimate{};
    AimControlBreakdown control{};
    TrajectoryRequest trajectoryRequest{};
    TrajectoryState trajectoryState{};
    TrajectoryOutput trajectoryOutput{};
    ViewMotionShadowDiagnostics viewMotion{};
};

#endif
