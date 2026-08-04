#ifndef RUNTIME_H
#define RUNTIME_H

#include <array>
#include <cstddef>
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

// `total_ms` 必须继续在 Aim/Mouse 完成时封口。该结构只描述随后发生的
// Pipeline 收尾；valid=false 时默认零值不是实测零耗时。snapshot_ms 包含
// debug_ring_ms 与 profile_window_ms，三者是包含关系，禁止直接相加。
struct RuntimeServiceProfile {
    bool valid = false;
    bool preview_attempted = false;
    bool preview_published = false;
    double preview_ms = 0.0;
    double snapshot_ms = 0.0;
    double snapshot_lock_wait_ms = 0.0;
    double debug_ring_ms = 0.0;
    double profile_window_ms = 0.0;
    // 从旧 total_ms 的终点到 Snapshot 解锁，包含 Preview 和原有观测收尾；
    // 刻意排除探针自身的 debug ring finalize，避免自我计时递归。
    double service_tail_ms = 0.0;
    // 从 Pipeline 取到帧到收尾结束，不包含 queue_ms。
    double pipeline_service_ms = 0.0;
    // 从 captured_at 到原有收尾结束；同样不含探针自身 finalize。探针完整
    // 扰动只能由 off/on A/B 的旧 total/queue 对照量化。
    double pipeline_complete_ms = 0.0;
};

// 每个 Pipeline 样本都固化对应输入帧的几何，避免只看最终快照时漏掉
// 网络重连、OBS 场景切换或显示模式变化造成的瞬时坐标漂移。
struct RuntimeFrameGeometry {
    int encoded_width = 0;
    int encoded_height = 0;
    int source_width = 0;
    int source_height = 0;
    int roi_width = 0;
    int roi_height = 0;
    double roi_x = 0.0;
    double roi_y = 0.0;
    double source_pixels_per_pixel_x = 1.0;
    double source_pixels_per_pixel_y = 1.0;
};

inline constexpr std::size_t kRuntimeReportedClassCount = 16;

// Runtime 每处理一帧发布一个固定大小的诊断样本。该样本只包含数值和枚举，
// 不持有图像、模型或设备资源，便于在主线程锁外写入报告。
struct RuntimePipelineSample {
    std::uint64_t sequence = 0;
    RuntimeFrameGeometry geometry;
    PipelineProfile profile;
    CaptureStageTiming capture_stages;
    RuntimeServiceProfile service;
    // 固化本帧完成时的累计计数，使 startup/warmup/formal 能以样本边界
    // 做单调差分，不再把最终快照累计值直接除以正式样本数。
    std::uint64_t source_dropped_frames = 0;
    std::uint64_t transport_dropped_frames = 0;
    std::uint64_t transport_invalid_packets = 0;
    std::uint64_t runtime_overwritten_frames = 0;
    DetectionStatus detection_status = DetectionStatus::NOT_RUN;
    AimStatus aim_status = AimStatus::NOT_RUN;
    MouseStatus mouse_status = MouseStatus::CLOSED;
    bool mouse_sent = false;
    // Aim 诊断只固化固定大小快照，不复制检测容器。control_center 与几何比例
    // 可把 ROI 点、速度和命令统一换算到主机 FOV 口径做双机 A/B。
    float aim_control_center_x = 0.0f;
    float aim_control_center_y = 0.0f;
    float aim_acquisition_range_radius = 0.0f;
    float aim_active_range_radius = 0.0f;
    bool aim_has_target = false;
    bool aim_has_command = false;
    bool aim_range_locked = false;
    bool aim_range_allows_control = false;
    bool aim_base_point_inside_box = false;
    bool aim_prediction_point_outside_box = false;
    bool aim_command_toward_target = false;
    AimTargetSnapshot aim_target;
    AimCommand aim_command;
    // 分类诊断只保留固定标量；count=0 时对应 confidence=0，避免把无检测
    // 与真实 0 置信度混为一谈，也不把动态检测集合复制进报告环。
    std::uint32_t person_detection_count = 0;
    std::uint32_t head_detection_count = 0;
    float max_person_confidence = 0.0f;
    float max_head_confidence = 0.0f;
    // 固定 0..15 类别槽保留模型原始类别维度，避免身体/头部角色聚合再次
    // 折叠 CT/T 等业务类别；更大 class_id 仍参与角色统计，但不进入此诊断表。
    std::array<std::uint32_t, kRuntimeReportedClassCount>
        detection_count_by_class{};
    std::array<float, kRuntimeReportedClassCount> max_confidence_by_class{};
};

inline constexpr int kRuntimePreviewMaxDimension = 512;
inline constexpr std::size_t kRuntimePreviewMaxDetections = 128;

// 可选诊断预览使用独立固定容量通道，不进入 RuntimeSnapshot 的高频复制。
// bgra 在首次启用时按最大尺寸一次分配，实际有效字节由 width/height 决定。
struct RuntimePreviewFrame {
    // 与 Pipeline 输入帧一致的单调序号，用于保证图像与下列标注同帧。
    std::uint64_t sequence = 0;
    // BGRA 有效区域尺寸，单位为像素；只缩小、不放大，最长边不超过 512。
    int width = 0;
    int height = 0;
    // Capture 交付的原始 BGR ROI 尺寸，Detection 和 Aim 坐标均以此为基准。
    int roi_width = 0;
    int roi_height = 0;
    // 原始 ROI 坐标到 BGRA 预览像素的独立横纵比例。
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    // 主机准星中心在原始 ROI 内的位置，允许落在 ROI 边界之外。
    float control_center_x = 0.0f;
    float control_center_y = 0.0f;
    float aim_acquisition_range_radius = 0.0f;
    float aim_active_range_radius = 0.0f;
    bool aim_range_locked = false;
    bool aim_range_allows_control = false;
    DetectionStatus detection_status = DetectionStatus::NOT_RUN;
    AimStatus aim_status = AimStatus::NOT_RUN;
    std::vector<std::uint8_t> bgra;
    // 检测框仍使用原始 ROI 像素坐标；超出固定容量时仅截断诊断显示。
    std::array<Detection, kRuntimePreviewMaxDetections> detections{};
    std::size_t detection_count = 0;
    bool has_target = false;
    AimTargetSnapshot target;
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
    std::uint64_t duplication_recoveries = 0;
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
    bool preview_enabled = false;
    bool d3d11_cuda_interop = false;
    bool d3d11_directml_interop = false;
    std::uint64_t preview_sampled_frames = 0;
    std::uint64_t preview_dropped_frames = 0;
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
    RuntimeSnapshot snapshot() const noexcept;
    // 诊断预览默认关闭；启用时最多 10 FPS，最长边 512，且只保留最新同帧图像与标注。
    bool set_preview_enabled(bool enabled) noexcept;
    std::shared_ptr<const RuntimePreviewFrame> preview_frame() const noexcept;
    // 取出自上次调用以来的诊断样本；失败时不影响 Runtime 主链。
    bool drain_pipeline_samples(
        std::vector<RuntimePipelineSample>& samples) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif // RUNTIME_H
