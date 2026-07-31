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
    float body_aim_height_ratio = 0.35f;
    float deadzone_pixels = 1.5f;
    float smoothing = 0.35f;
    float counts_per_pixel_x = 0.50f;
    float counts_per_pixel_y = 0.50f;
    float max_counts_per_frame = 50.0f;
    float predicted_gain = 0.50f;
};

struct AimFrame {
    std::uint64_t sequence = 0;
    std::chrono::steady_clock::time_point captured_at{};
    int roi_width = 0;
    int roi_height = 0;
    // 主机准星中心在当前检测 ROI 内的位置；允许位于 ROI 外。
    float control_center_x = 0.0f;
    float control_center_y = 0.0f;
    // 一个检测 ROI 像素对应的主机完整 FOV 像素数。
    float source_pixels_per_roi_pixel_x = 1.0f;
    float source_pixels_per_roi_pixel_y = 1.0f;
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
    float aim_x = 0.0f;
    float aim_y = 0.0f;
    float confidence = 0.0f;
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
