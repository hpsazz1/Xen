#ifndef DETECTOR_VIDEO_VISIBILITY_INTERNAL_H
#define DETECTOR_VIDEO_VISIBILITY_INTERNAL_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace detector::detail {

inline constexpr int kVideoVisibilitySchemaVersion = 1;
inline constexpr std::string_view kVideoVisibilityPolicy =
    "target_frame_visibility_v1";

enum class FrameVisibility : std::uint8_t {
    NOT_VISIBLE,
    VISIBLE,
    IGNORED,
};

struct VideoVisibilityExpectation {
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

struct VideoVisibilityAnnotation {
    std::string policy;
    std::vector<FrameVisibility> frames;
};

struct VideoVisibilityMetrics {
    bool annotations_present = false;
    std::size_t annotated_frames = 0;
    std::size_t visible_frames = 0;
    std::size_t visible_detected_frames = 0;
    std::size_t visible_missed_frames = 0;
    std::size_t longest_visible_miss_sequence = 0;
    std::size_t not_visible_frames = 0;
    std::size_t not_visible_detected_frames = 0;
    std::size_t ignored_frames = 0;
    std::size_t ignored_detected_frames = 0;
    std::size_t current_visible_miss_sequence = 0;
};

// 标注文件名保留视频扩展名，避免同目录下同名 MP4/AVI 映射到同一份真值。
std::filesystem::path video_visibility_annotation_path(
    const std::filesystem::path& directory,
    const std::filesystem::path& video_path);

// 标注目录启用后必须与视频集合一一对应；多余或缺失文件都表示本次评价集不完整。
bool validate_video_visibility_annotation_set(
    const std::filesystem::path& directory,
    std::span<const std::filesystem::path> video_paths,
    std::string& error) noexcept;

bool compute_file_sha256(
    const std::filesystem::path& path,
    std::string& sha256,
    std::string& error) noexcept;

bool parse_video_visibility_annotation(
    std::string_view json,
    const VideoVisibilityExpectation& expected,
    VideoVisibilityAnnotation& annotation,
    std::string& error) noexcept;

bool load_video_visibility_annotation(
    const std::filesystem::path& path,
    const VideoVisibilityExpectation& expected,
    VideoVisibilityAnnotation& annotation,
    std::string& error) noexcept;

void record_video_visibility(
    FrameVisibility visibility,
    bool detected,
    VideoVisibilityMetrics& metrics) noexcept;

bool video_visibility_recall_available(
    const VideoVisibilityMetrics& metrics) noexcept;

double video_visibility_recall(
    const VideoVisibilityMetrics& metrics) noexcept;

double video_visibility_evaluable_rate(
    const VideoVisibilityMetrics& metrics) noexcept;

} // namespace detector::detail

#endif // DETECTOR_VIDEO_VISIBILITY_INTERNAL_H
