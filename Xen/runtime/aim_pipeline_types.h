#ifndef AIM_PIPELINE_TYPES_H
#define AIM_PIPELINE_TYPES_H

#include <chrono>
#include <cstdint>

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
    int sourceWidth = 0;
    int sourceHeight = 0;
    bool sourceTimestampMapped = false;
};

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
    bool sendSucceeded = false;
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
    double innovationX = 0.0;
    double innovationY = 0.0;
    double nisX = 0.0;
    double nisY = 0.0;
};

// 新控制器的可审计分解；P0-0 默认全部为零且不参与旧控制器输出。
struct AimControlBreakdown
{
    double feedbackX = 0.0;
    double feedbackY = 0.0;
    double trackingFeedforwardX = 0.0;
    double trackingFeedforwardY = 0.0;
    double leadReferenceX = 0.0;
    double leadReferenceY = 0.0;
    double requestedCountsX = 0.0;
    double requestedCountsY = 0.0;
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
};

#endif
