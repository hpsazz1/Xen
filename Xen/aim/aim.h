#ifndef AIM_H
#define AIM_H

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

#include "detector/detector.h"

enum class AimStatus {
    NOT_RUN,
    SUCCESS,
    INVALID_INPUT,
    TRACKING_FAILED,
    CONTROL_FAILED,
};

const char* AimStatusName(AimStatus status) noexcept;

enum class TrackState {
    TENTATIVE,
    CONFIRMED,
    LOST,
};

struct AimConfig {
    std::vector<int> person_class_ids{0};
    std::vector<int> head_class_ids{1};
    float high_confidence = 0.25f;
    float low_confidence = 0.10f;
    int min_confirmed_hits = 2;
    int max_lost_frames = 8;
    float min_iou = 0.10f;
    float max_center_distance = 0.25f;
    float switch_margin = 0.20f;
    int switch_confirm_frames = 3;
    int switch_cooldown_frames = 5;
    // 相对 ROI 短边半径的百分比；100 表示半个短边。只约束目标获取、
    // 挑战者切换和鼠标命令，不裁剪 Detector 输入或停止轨迹状态更新。
    float acquisition_range_percent = 90.0f;
    float body_aim_height_ratio = 0.35f;
    // 身体框内基础瞄点允许活动的横向有效范围百分比；50 表示身体框中间 50%。
    float body_aim_range_percent = 50.0f;
    float deadzone_pixels = 1.5f;
    float smoothing = 0.35f;
    float counts_per_pixel_x = 0.50f;
    float counts_per_pixel_y = 0.50f;
    float max_counts_per_frame = 50.0f;
    bool enable_prediction = false;
    // 预测提前向量相对当前目标框对角线的最大百分比。该值只限制预测层，
    // 不改变基础移动、观测或轨迹状态更新。
    float max_prediction_lead_percent = 35.0f;
    float predicted_gain = 0.50f;
};

struct AimFrame {
    std::uint64_t sequence = 0;
    std::chrono::steady_clock::time_point captured_at{};
    // 默认零值表示由 Aim 在处理时读取当前时刻。离线回放可显式给出控制
    // 时刻，使轨迹 dt 仍按视频时间推进，而提前量只消费声明的测量延迟。
    std::chrono::steady_clock::time_point control_at{};
    int roi_width = 0;
    int roi_height = 0;
    // 主机准星中心在当前检测 ROI 内的位置；允许位于 ROI 外。
    float control_center_x = 0.0f;
    float control_center_y = 0.0f;
    // 一个检测 ROI 像素对应的主机完整 FOV 像素数。
    float source_pixels_per_roi_pixel_x = 1.0f;
    float source_pixels_per_roi_pixel_y = 1.0f;
    // Runtime 在按住键且安全门允许物理控制时置 true。未按键仍持续完成
    // 观测、跟踪、预选和命令计算，只是不启用锁定后的动态收缩范围。
    bool lock_active = false;
    std::vector<Detection> detections;
};

struct AimCommand {
    std::uint64_t sequence = 0;
    std::chrono::steady_clock::time_point captured_at{};
    int dx_counts = 0;
    int dy_counts = 0;
};

struct AimTargetSnapshot {
    std::uint64_t track_id = 0;
    TrackState state = TrackState::TENTATIVE;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float x2 = 0.0f;
    float y2 = 0.0f;
    // 基础瞄点来自观测/状态估计并始终位于目标框内；aim_* 是应用有界
    // 提前后的最终点，预测开启时允许位于框外。
    float base_aim_x = 0.0f;
    float base_aim_y = 0.0f;
    float aim_x = 0.0f;
    float aim_y = 0.0f;
    // 速度单位为检测 ROI 像素/秒；实际提前向量已经包含帧龄、降权和距离限幅。
    float velocity_x = 0.0f;
    float velocity_y = 0.0f;
    float lead_x = 0.0f;
    float lead_y = 0.0f;
    float observation_age_ms = 0.0f;
    float confidence = 0.0f;
    bool lead_active = false;
    bool predicted = false;
};

struct AimProfile {
    double observation_ms = 0.0;
    double tracking_ms = 0.0;
    double selection_ms = 0.0;
    double control_ms = 0.0;
    double total_ms = 0.0;
};

struct AimResult {
    AimStatus status = AimStatus::NOT_RUN;
    bool has_target = false;
    bool has_command = false;
    float acquisition_range_radius = 0.0f;
    float active_range_radius = 0.0f;
    bool range_locked = false;
    bool range_allows_control = false;
    AimTargetSnapshot target;
    AimCommand command;
    AimProfile profile;
};

class Aim {
public:
    explicit Aim(const AimConfig& config);
    ~Aim();

    Aim(const Aim&) = delete;
    Aim& operator=(const Aim&) = delete;
    Aim(Aim&&) noexcept;
    Aim& operator=(Aim&&) noexcept;

    AimResult process(const AimFrame& frame) noexcept;
    void reset() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif // AIM_H
