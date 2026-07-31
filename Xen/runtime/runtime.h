#ifndef RUNTIME_H
#define RUNTIME_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "aim/aim.h"
#include "capture/capture.h"
#include "config/config.h"
#include "detector/detector.h"
#include "mouse/mouse.h"

enum class RuntimeState {
    STOPPED,
    STARTING,
    RUNNING,
    STOPPING,
    FAILED,
};

const char* RuntimeStateName(RuntimeState state) noexcept;

enum class DetectorReloadState {
    IDLE,
    LOADING,
    SUCCEEDED,
    FAILED,
};

const char* DetectorReloadStateName(DetectorReloadState state) noexcept;

enum class RuntimeIntentType {
    START,
    STOP,
    ARM_OUTPUT,
    DISARM_OUTPUT,
    AIM_HOLD_CHANGED,
    EMERGENCY_STOP,
    RESET_EMERGENCY,
};

struct RuntimeIntent {
    RuntimeIntentType type = RuntimeIntentType::DISARM_OUTPUT;
    bool active = false;
};

struct PipelineProfile {
    double capture_ms = 0.0;
    double queue_ms = 0.0;
    InferenceProfile detector;
    AimProfile aim;
    double mouse_ms = 0.0;
    double total_ms = 0.0;
};

// Runtime 每处理一帧发布一个固定大小的诊断样本。该样本只包含数值和枚举，
// 不持有图像、模型或设备资源，便于在主线程锁外写入报告。
struct RuntimePipelineSample {
    std::uint64_t sequence = 0;
    PipelineProfile profile;
    DetectionStatus detection_status = DetectionStatus::NOT_RUN;
    AimStatus aim_status = AimStatus::NOT_RUN;
    MouseStatus mouse_status = MouseStatus::CLOSED;
    bool mouse_sent = false;
};

struct RuntimeSnapshot {
    RuntimeState state = RuntimeState::STOPPED;
    CaptureStatus capture_status = CaptureStatus::CLOSED;
    DetectionStatus detection_status = DetectionStatus::NOT_RUN;
    AimStatus aim_status = AimStatus::NOT_RUN;
    MouseStatus mouse_status = MouseStatus::CLOSED;
    DetectorReloadState detector_reload_state = DetectorReloadState::IDLE;
    std::string provider;
    // 当前实际服务 Pipeline 的模型，不是 Overlay 中尚未提交的编辑值。
    std::string active_model_path;
    // 模型重载错误独立于 Runtime 致命错误；失败时旧模型继续工作。
    std::string detector_reload_error;
    std::string last_error;
    std::uint64_t detector_generation = 0;
    std::uint64_t captured_frames = 0;
    std::uint64_t processed_frames = 0;
    std::uint64_t failed_frames = 0;
    std::uint64_t source_dropped_frames = 0;
    std::uint64_t transport_dropped_frames = 0;
    std::uint64_t transport_invalid_packets = 0;
    std::uint64_t source_received_frames = 0;
    std::uint64_t source_sequence = 0;
    bool source_sequence_valid = false;
    double source_fps = 0.0;
    std::int64_t source_timecode = 0;
    bool source_timecode_valid = false;
    std::int64_t source_timestamp = 0;
    bool source_timestamp_valid = false;
    std::uint64_t overwritten_frames = 0;
    std::uint64_t mouse_commands = 0;
    std::uint64_t debug_samples_dropped = 0;
    std::uint64_t last_sequence = 0;
    int encoded_width = 0;
    int encoded_height = 0;
    int source_width = 0;
    int source_height = 0;
    int capture_roi_width = 0;
    int capture_roi_height = 0;
    double capture_roi_x = 0.0;
    double capture_roi_y = 0.0;
    double source_pixels_per_pixel_x = 1.0;
    double source_pixels_per_pixel_y = 1.0;
    double capture_fps = 0.0;
    double pipeline_p50_ms = 0.0;
    double pipeline_p95_ms = 0.0;
    PipelineProfile last_profile;
    bool output_allowed_by_config = false;
    bool output_armed = false;
    bool aim_hold_active = false;
    bool emergency_stopped = false;
    AimResult last_aim;
};

class Runtime {
public:
    Runtime();
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    bool start(const AppConfig& config) noexcept;
    void stop() noexcept;
    // 仅在 RUNNING 状态接受请求。候选模型异步加载，失败不替换旧模型。
    bool reload_detector(const DetectorConfig& config) noexcept;
    bool post_intent(const RuntimeIntent& intent) noexcept;
    void poll_keyboard() noexcept;
    RuntimeSnapshot snapshot() const noexcept;
    // 取出自上次调用以来的诊断样本；失败时不影响 Runtime 主链。
    bool drain_pipeline_samples(
        std::vector<RuntimePipelineSample>& samples) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif // RUNTIME_H
