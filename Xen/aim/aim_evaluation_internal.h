#ifndef AIM_EVALUATION_INTERNAL_H
#define AIM_EVALUATION_INTERNAL_H

#include "aim/aim.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aim::detail {

inline constexpr int kAimGroundTruthSchemaVersion = 1;
inline constexpr std::string_view kAimGroundTruthPolicy =
    "aim_ground_truth_v1";

enum class AimGroundTruthState : std::uint8_t {
    NOT_VISIBLE,
    VISIBLE,
    IGNORED,
};

// 真值框永远使用主机完整 FOV 像素坐标，不使用辅机窗口坐标或模型输入坐标。
struct AimGroundTruthTarget {
    std::uint64_t track_id = 0;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float x2 = 0.0f;
    float y2 = 0.0f;
};

struct AimGroundTruthFrame {
    AimGroundTruthState state = AimGroundTruthState::NOT_VISIBLE;
    std::vector<AimGroundTruthTarget> targets;
};

struct AimGroundTruthExpectation {
    std::string video_file;
    std::string video_sha256;
    int source_width = 0;
    int source_height = 0;
    std::size_t frame_count = 0;
    std::string input_mode;
    int roi_x = 0;
    int roi_y = 0;
    int roi_width = 0;
    int roi_height = 0;
};

struct AimGroundTruthAnnotation {
    std::string policy;
    int source_width = 0;
    int source_height = 0;
    int roi_x = 0;
    int roi_y = 0;
    int roi_width = 0;
    int roi_height = 0;
    std::vector<AimGroundTruthFrame> frames;
};

struct AimEvaluationConfig {
    float min_iou = 0.10f;
    float max_center_distance = 0.25f;
};

struct AimEvaluationFrame {
    std::size_t frame_index = 0;
    int source_width = 0;
    int source_height = 0;
    float source_roi_x = 0.0f;
    float source_roi_y = 0.0f;
    int roi_width = 0;
    int roi_height = 0;
    // 一枚 ROI 像素对应多少主机 FOV 像素；辅机显示分辨率不在此结构中。
    float source_pixels_per_roi_pixel_x = 1.0f;
    float source_pixels_per_roi_pixel_y = 1.0f;
    AimStatus aim_status = AimStatus::NOT_RUN;
    bool has_target = false;
    AimTargetSnapshot target;
};

struct AimEvaluationMetrics {
    bool annotations_present = false;
    bool complete = false;
    std::size_t annotated_frames = 0;
    std::size_t visible_frames = 0;
    std::size_t matched_visible_frames = 0;
    std::size_t missed_visible_frames = 0;
    std::size_t not_visible_frames = 0;
    std::size_t ignored_frames = 0;
    std::size_t output_target_frames = 0;
    std::size_t invalid_aim_frames = 0;
    std::size_t id_switches = 0;
    // 每个 GT 目标的连续匹配段总数；fragmentation_events 为超出首段的段数。
    std::size_t track_fragments = 0;
    std::size_t track_fragmentation_events = 0;
    std::size_t unnecessary_switches = 0;

    // 以下字段是评价器的跨帧状态，保持在同一对象中逐帧复用，避免热路径分配。
    bool has_previous_frame = false;
    std::size_t previous_frame_index = 0;
    bool previous_visible = false;
    bool previous_matched = false;
    std::uint64_t previous_ground_truth_id = 0;
    std::uint64_t previous_track_id = 0;
    std::unordered_map<std::uint64_t, std::uint64_t> last_track_by_ground_truth;
    std::unordered_map<std::uint64_t, std::size_t> last_frame_by_ground_truth;
    std::unordered_map<std::uint64_t, std::size_t> fragment_count_by_ground_truth;
};

std::filesystem::path aim_ground_truth_annotation_path(
    const std::filesystem::path& directory,
    const std::filesystem::path& video_path);

bool validate_aim_ground_truth_annotation_set(
    const std::filesystem::path& directory,
    const std::vector<std::filesystem::path>& video_paths,
    std::string& error) noexcept;

bool parse_aim_ground_truth_annotation(
    std::string_view json,
    const AimGroundTruthExpectation& expected,
    AimGroundTruthAnnotation& annotation,
    std::string& error) noexcept;

bool load_aim_ground_truth_annotation(
    const std::filesystem::path& path,
    const AimGroundTruthExpectation& expected,
    AimGroundTruthAnnotation& annotation,
    std::string& error) noexcept;

bool record_aim_evaluation(
    const AimGroundTruthAnnotation& annotation,
    const AimEvaluationConfig& config,
    const AimEvaluationFrame& frame,
    AimEvaluationMetrics& metrics,
    std::string& error) noexcept;

bool finalize_aim_evaluation(
    const AimGroundTruthAnnotation& annotation,
    AimEvaluationMetrics& metrics,
    std::string& error) noexcept;

bool aim_roi_recall_available(const AimEvaluationMetrics& metrics) noexcept;
double aim_roi_recall(const AimEvaluationMetrics& metrics) noexcept;

} // namespace aim::detail

#endif // AIM_EVALUATION_INTERNAL_H
