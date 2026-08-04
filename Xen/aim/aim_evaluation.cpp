#include "aim/aim_evaluation_internal.h"

#include <opencv2/core.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <limits>
#include <numeric>
#include <set>
#include <unordered_set>
#include <utility>

namespace aim::detail {
namespace {

constexpr std::size_t kMaximumAnnotationBytes = 64U * 1024U * 1024U;
constexpr std::size_t kMaximumAnnotatedFrames = 10U * 1000U * 1000U;
constexpr float kGeometryTolerance = 0.01f;

void set_error(std::string& output, const std::string& value) noexcept {
    try {
        output = value;
    } catch (...) {
    }
}

std::string path_to_utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::wstring lowercase_path_component(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t character) {
                       return static_cast<wchar_t>(std::towlower(character));
                   });
    return value;
}

bool has_annotation_suffix(const std::filesystem::path& path) {
    constexpr std::wstring_view kSuffix = L".aim.json";
    const std::wstring name = lowercase_path_component(
        path.filename().wstring());
    return name.size() >= kSuffix.size() &&
           std::wstring_view(name).substr(name.size() - kSuffix.size()) ==
               kSuffix;
}

bool normalize_sha256(std::string_view input, std::string& output) {
    if (input.size() != 64U) return false;
    output.clear();
    output.reserve(input.size());
    for (const unsigned char character : input) {
        if (!std::isxdigit(character)) return false;
        output.push_back(static_cast<char>(std::toupper(character)));
    }
    return true;
}

bool validate_known_fields(
    const cv::FileNode& node,
    const std::unordered_set<std::string>& expected,
    std::string_view context,
    std::string& error) {
    if (!node.isMap()) {
        set_error(error, std::string(context) + " 必须是 JSON 对象");
        return false;
    }
    for (auto iterator = node.begin(); iterator != node.end(); ++iterator) {
        const std::string name = (*iterator).name();
        if (!expected.contains(name)) {
            set_error(error, std::string(context) + " 包含未知字段：" + name);
            return false;
        }
    }
    return true;
}

bool read_required_int(const cv::FileNode& root, const char* name,
                       int& output, std::string& error) {
    const cv::FileNode node = root[name];
    if (node.empty() || !node.isInt()) {
        set_error(error, std::string("标注字段必须是整数：") + name);
        return false;
    }
    output = static_cast<int>(node);
    return true;
}

bool read_required_string(const cv::FileNode& root, const char* name,
                          std::string& output, std::string& error) {
    const cv::FileNode node = root[name];
    if (node.empty() || !node.isString()) {
        set_error(error, std::string("标注字段必须是字符串：") + name);
        return false;
    }
    output = static_cast<std::string>(node);
    if (output.empty()) {
        set_error(error, std::string("标注字段不能为空：") + name);
        return false;
    }
    return true;
}

bool read_required_number(const cv::FileNode& root, const char* name,
                          float& output, std::string& error) {
    const cv::FileNode node = root[name];
    if (node.empty() || (!node.isReal() && !node.isInt())) {
        set_error(error, std::string("标注字段必须是数字：") + name);
        return false;
    }
    output = static_cast<float>(node);
    if (!std::isfinite(output)) {
        set_error(error, std::string("标注字段必须是有限数字：") + name);
        return false;
    }
    return true;
}

bool parse_state(std::string_view value,
                 AimGroundTruthState& output) noexcept {
    if (value == "visible") {
        output = AimGroundTruthState::VISIBLE;
    } else if (value == "not_visible") {
        output = AimGroundTruthState::NOT_VISIBLE;
    } else if (value == "ignore") {
        output = AimGroundTruthState::IGNORED;
    } else {
        return false;
    }
    return true;
}

bool valid_expectation(const AimGroundTruthExpectation& expected) noexcept {
    return !expected.video_file.empty() && expected.frame_count > 0 &&
           expected.frame_count <= kMaximumAnnotatedFrames &&
           expected.source_width > 0 && expected.source_height > 0 &&
           expected.roi_x >= 0 && expected.roi_y >= 0 &&
           expected.roi_width > 0 && expected.roi_height > 0 &&
           static_cast<long long>(expected.roi_x) + expected.roi_width <=
               expected.source_width &&
           static_cast<long long>(expected.roi_y) + expected.roi_height <=
               expected.source_height;
}

bool valid_config(const AimEvaluationConfig& config) noexcept {
    return std::isfinite(config.min_iou) &&
           config.min_iou >= 0.0f && config.min_iou <= 1.0f &&
           std::isfinite(config.max_center_distance) &&
           config.max_center_distance > 0.0f &&
           std::isfinite(config.max_counts_per_frame) &&
           config.max_counts_per_frame > 0.0f &&
           std::isfinite(config.max_prediction_lead_percent) &&
           config.max_prediction_lead_percent >= 1.0f &&
           config.max_prediction_lead_percent <= 50.0f &&
           std::isfinite(config.counts_per_pixel_x) &&
           std::isfinite(config.counts_per_pixel_y) &&
           config.counts_per_pixel_x > 0.0f &&
           config.counts_per_pixel_y > 0.0f;
}

bool valid_track_state(TrackState state) noexcept {
    return state == TrackState::CONFIRMED || state == TrackState::LOST;
}

bool target_is_default(const AimTargetSnapshot& target) noexcept {
    return target.track_id == 0 && target.state == TrackState::TENTATIVE &&
           target.x1 == 0.0f && target.y1 == 0.0f &&
           target.x2 == 0.0f && target.y2 == 0.0f &&
           target.base_aim_x == 0.0f && target.base_aim_y == 0.0f &&
           target.aim_x == 0.0f && target.aim_y == 0.0f &&
           target.velocity_x == 0.0f && target.velocity_y == 0.0f &&
           target.lead_x == 0.0f && target.lead_y == 0.0f &&
           target.observation_age_ms == 0.0f &&
           target.confidence == 0.0f && !target.lead_active &&
           !target.predicted;
}

bool command_is_default(const AimCommand& command) noexcept {
    return command.sequence == 0 &&
           command.captured_at == std::chrono::steady_clock::time_point{} &&
           command.dx_counts == 0 && command.dy_counts == 0;
}

bool inactive_command_is_valid(const AimEvaluationFrame& frame) noexcept {
    if (frame.command.dx_counts != 0 || frame.command.dy_counts != 0) {
        return false;
    }
    if (command_is_default(frame.command)) return true;
    // deadzone 在填写命令元数据前返回；浮点残余量化为零时则保留本帧元数据。
    // 两者都表示 has_command=false，只有“序号和时间戳成对存在”可被接受。
    return frame.has_target && frame.aim_status == AimStatus::SUCCESS &&
           frame.command.sequence != 0 &&
           frame.command.captured_at !=
               std::chrono::steady_clock::time_point{};
}

bool valid_box(float x1, float y1, float x2, float y2) noexcept;

bool valid_aim_output_contract(const AimEvaluationFrame& frame) noexcept {
    return !((frame.aim_status == AimStatus::SUCCESS &&
              (!std::isfinite(frame.acquisition_range_radius) ||
               !std::isfinite(frame.active_range_radius) ||
               frame.acquisition_range_radius <= 0.0f ||
               frame.active_range_radius <= 0.0f ||
               frame.active_range_radius >
                   frame.acquisition_range_radius + kGeometryTolerance ||
               (frame.range_locked && !frame.has_target) ||
               (frame.range_allows_control && !frame.has_target) ||
               (frame.has_command && !frame.range_allows_control))) ||
             (frame.aim_status != AimStatus::SUCCESS &&
              (frame.has_target || frame.has_command)) ||
             (frame.has_command && !frame.has_target) ||
             (frame.has_target &&
              (frame.target.track_id == 0 ||
               !valid_track_state(frame.target.state) ||
               frame.target.predicted !=
                   (frame.target.state == TrackState::LOST) ||
                !valid_box(frame.target.x1, frame.target.y1,
                           frame.target.x2, frame.target.y2) ||
                !std::isfinite(frame.target.base_aim_x) ||
                !std::isfinite(frame.target.base_aim_y) ||
                !std::isfinite(frame.target.aim_x) ||
                !std::isfinite(frame.target.aim_y) ||
                !std::isfinite(frame.target.velocity_x) ||
                !std::isfinite(frame.target.velocity_y) ||
                !std::isfinite(frame.target.lead_x) ||
                !std::isfinite(frame.target.lead_y) ||
                !std::isfinite(frame.target.observation_age_ms) ||
                frame.target.observation_age_ms < 0.0f ||
                frame.target.observation_age_ms > 100.001f ||
                std::fabs(frame.target.aim_x - frame.target.base_aim_x -
                          frame.target.lead_x) > kGeometryTolerance ||
                std::fabs(frame.target.aim_y - frame.target.base_aim_y -
                          frame.target.lead_y) > kGeometryTolerance ||
                (!frame.target.lead_active &&
                 (std::fabs(frame.target.lead_x) > kGeometryTolerance ||
                  std::fabs(frame.target.lead_y) > kGeometryTolerance)) ||
                !std::isfinite(frame.target.confidence) ||
               frame.target.confidence < 0.0f ||
               frame.target.confidence > 1.0f)) ||
             (!frame.has_target && !target_is_default(frame.target)) ||
             (frame.has_command &&
              (frame.command.sequence == 0 ||
               frame.command.captured_at ==
                   std::chrono::steady_clock::time_point{} ||
               (frame.command.dx_counts == 0 &&
                frame.command.dy_counts == 0))) ||
             (!frame.has_command && !inactive_command_is_valid(frame)));
}

void break_command_continuity(AimControlContinuityMetrics& control) noexcept {
    control.has_previous_command = false;
    control.has_previous_delta = false;
}

void accumulate_control_continuity(const AimEvaluationConfig& config,
                                   const AimEvaluationFrame& frame,
                                   AimControlContinuityMetrics& control) {
    if (frame.aim_status != AimStatus::SUCCESS) {
        control.has_previous_target = false;
        break_command_continuity(control);
        return;
    }

    if (!frame.has_target) {
        ++control.no_target_frames;
        control.has_previous_target = false;
        break_command_continuity(control);
        return;
    }

    if (frame.target.predicted) ++control.predicted_target_frames;
    if (frame.range_locked) ++control.range_locked_frames;
    if (!frame.range_allows_control) {
        ++control.range_blocked_target_frames;
    }
    if (frame.target.lead_active) ++control.lead_active_frames;
    const bool base_point_outside =
        frame.target.base_aim_x < frame.target.x1 ||
        frame.target.base_aim_x > frame.target.x2 ||
        frame.target.base_aim_y < frame.target.y1 ||
        frame.target.base_aim_y > frame.target.y2;
    const bool final_point_outside =
        frame.target.aim_x < frame.target.x1 ||
        frame.target.aim_x > frame.target.x2 ||
        frame.target.aim_y < frame.target.y1 ||
        frame.target.aim_y > frame.target.y2;
    if (base_point_outside) {
        ++control.base_aim_point_outside_box_frames;
    }
    if (frame.target.lead_active && final_point_outside) {
        ++control.prediction_point_outside_box_frames;
    }
    const double target_diagonal = std::hypot(
        static_cast<double>(frame.target.x2 - frame.target.x1),
        static_cast<double>(frame.target.y2 - frame.target.y1));
    const double lead_magnitude = std::hypot(
        static_cast<double>(frame.target.lead_x),
        static_cast<double>(frame.target.lead_y));
    const double lead_limit = target_diagonal *
        static_cast<double>(config.max_prediction_lead_percent) / 100.0;
    if (lead_magnitude > lead_limit + kGeometryTolerance) {
        ++control.lead_limit_violation_frames;
    }

    const double scale_x = static_cast<double>(
        frame.source_pixels_per_roi_pixel_x);
    const double scale_y = static_cast<double>(
        frame.source_pixels_per_roi_pixel_y);
    const double base_error_x =
        (static_cast<double>(frame.target.base_aim_x) -
         frame.control_center_x) * scale_x;
    const double base_error_y =
        (static_cast<double>(frame.target.base_aim_y) -
         frame.control_center_y) * scale_y;
    const double final_error_x =
        (static_cast<double>(frame.target.aim_x) -
         frame.control_center_x) * scale_x;
    const double final_error_y =
        (static_cast<double>(frame.target.aim_y) -
         frame.control_center_y) * scale_y;
    control.track_speed_samples.push_back(std::hypot(
        static_cast<double>(frame.target.velocity_x) * scale_x,
        static_cast<double>(frame.target.velocity_y) * scale_y));
    control.base_error_samples.push_back(
        std::hypot(base_error_x, base_error_y));
    control.final_error_samples.push_back(
        std::hypot(final_error_x, final_error_y));
    control.lead_samples.push_back(std::hypot(
        static_cast<double>(frame.target.lead_x) * scale_x,
        static_cast<double>(frame.target.lead_y) * scale_y));
    control.observation_age_samples.push_back(
        frame.target.observation_age_ms);
    control.acquisition_range_samples.push_back(
        frame.acquisition_range_radius);
    control.active_range_samples.push_back(frame.active_range_radius);
    if (control.has_previous_target) {
        if (control.previous_target_track_id != frame.target.track_id) {
            ++control.target_switches;
        } else {
            if (control.previous_target_state != frame.target.state) {
                ++control.target_state_changes;
            }
            if (control.previous_target_predicted != frame.target.predicted) {
                ++control.prediction_state_changes;
            }
        }
    }
    control.has_previous_target = true;
    control.previous_target_track_id = frame.target.track_id;
    control.previous_target_state = frame.target.state;
    control.previous_target_predicted = frame.target.predicted;

    if (!frame.has_command) {
        ++control.target_without_command_frames;
        break_command_continuity(control);
        return;
    }

    ++control.command_frames;
    if (frame.target.predicted) {
        ++control.predicted_command_frames;
    } else {
        ++control.observed_command_frames;
    }

    const double dx = static_cast<double>(frame.command.dx_counts);
    const double dy = static_cast<double>(frame.command.dy_counts);
    const double abs_dx = std::fabs(dx);
    const double abs_dy = std::fabs(dy);
    const double magnitude = std::hypot(dx, dy);
    control.abs_dx_samples.push_back(abs_dx);
    control.abs_dy_samples.push_back(abs_dy);
    control.magnitude_samples.push_back(magnitude);
    const double limit_boundary = std::ceil(
        static_cast<double>(config.max_counts_per_frame));
    if (magnitude >= limit_boundary) {
        ++control.limit_boundary_frames;
    }
    const double desired_x = final_error_x * config.counts_per_pixel_x;
    const double desired_y = final_error_y * config.counts_per_pixel_y;
    if (dx * desired_x + dy * desired_y <= 0.0 ||
        std::fabs(dx) > std::ceil(std::fabs(desired_x)) ||
        std::fabs(dy) > std::ceil(std::fabs(desired_y))) {
        ++control.control_direction_violation_frames;
    }

    const bool continuous =
        control.has_previous_command &&
        control.previous_command_frame_index + 1U == frame.frame_index &&
        control.previous_command_track_id == frame.target.track_id &&
        control.previous_command_state == frame.target.state &&
        control.previous_command_predicted == frame.target.predicted;
    if (!continuous) {
        ++control.continuity_segments;
        control.has_previous_delta = false;
    } else {
        const double delta_x = dx - control.previous_dx_counts;
        const double delta_y = dy - control.previous_dy_counts;
        control.delta_samples.push_back(std::hypot(delta_x, delta_y));
        if (dx * control.previous_dx_counts +
                dy * control.previous_dy_counts < 0.0) {
            ++control.direction_reversals;
        }
        if (control.has_previous_delta) {
            control.acceleration_samples.push_back(std::hypot(
                delta_x - control.previous_delta_x_counts,
                delta_y - control.previous_delta_y_counts));
        }
        control.previous_delta_x_counts = delta_x;
        control.previous_delta_y_counts = delta_y;
        control.has_previous_delta = true;
    }

    control.has_previous_command = true;
    control.previous_command_frame_index = frame.frame_index;
    control.previous_command_track_id = frame.target.track_id;
    control.previous_command_state = frame.target.state;
    control.previous_command_predicted = frame.target.predicted;
    control.previous_dx_counts = dx;
    control.previous_dy_counts = dy;
}

double sample_percentile(const std::vector<double>& sorted,
                         double ratio) noexcept {
    if (sorted.empty()) return 0.0;
    const double position = ratio * static_cast<double>(sorted.size() - 1U);
    const auto lower = static_cast<std::size_t>(position);
    const auto upper = std::min(lower + 1U, sorted.size() - 1U);
    const double fraction = position - static_cast<double>(lower);
    return sorted[lower] * (1.0 - fraction) + sorted[upper] * fraction;
}

void finalize_distribution(std::vector<double>& samples,
                           AimDistributionSummary& summary) noexcept {
    summary.samples = samples.size();
    if (samples.empty()) return;
    summary.mean = std::accumulate(samples.begin(), samples.end(), 0.0) /
                   static_cast<double>(samples.size());
    std::sort(samples.begin(), samples.end());
    summary.p50 = sample_percentile(samples, 0.50);
    summary.p95 = sample_percentile(samples, 0.95);
    summary.p99 = sample_percentile(samples, 0.99);
    summary.maximum = samples.back();
}

bool valid_box(float x1, float y1, float x2, float y2) noexcept {
    return std::isfinite(x1) && std::isfinite(y1) &&
           std::isfinite(x2) && std::isfinite(y2) && x2 > x1 && y2 > y1;
}

float box_iou(float ax1, float ay1, float ax2, float ay2,
              float bx1, float by1, float bx2, float by2) noexcept {
    const float left = std::max(ax1, bx1);
    const float top = std::max(ay1, by1);
    const float right = std::min(ax2, bx2);
    const float bottom = std::min(ay2, by2);
    const float intersection = std::max(0.0f, right - left) *
                               std::max(0.0f, bottom - top);
    const float area_a = (ax2 - ax1) * (ay2 - ay1);
    const float area_b = (bx2 - bx1) * (by2 - by1);
    const float denominator = area_a + area_b - intersection;
    return denominator > 0.0f ? intersection / denominator : 0.0f;
}

bool target_in_source(const AimGroundTruthTarget& target,
                      const AimGroundTruthExpectation& expected) noexcept {
    return valid_box(target.x1, target.y1, target.x2, target.y2) &&
           target.x1 >= 0.0f && target.y1 >= 0.0f &&
           target.x2 <= static_cast<float>(expected.source_width) &&
           target.y2 <= static_cast<float>(expected.source_height);
}

bool target_center_in_roi(const AimGroundTruthTarget& target,
                          const AimGroundTruthExpectation& expected) noexcept {
    const float center_x = (target.x1 + target.x2) * 0.5f;
    const float center_y = (target.y1 + target.y2) * 0.5f;
    return center_x >= static_cast<float>(expected.roi_x) &&
           center_x <= static_cast<float>(expected.roi_x + expected.roi_width) &&
           center_y >= static_cast<float>(expected.roi_y) &&
           center_y <= static_cast<float>(expected.roi_y + expected.roi_height);
}

bool read_target(const cv::FileNode& node,
                 const AimGroundTruthExpectation& expected,
                 AimGroundTruthTarget& target,
                 std::string& error) {
    static const std::unordered_set<std::string> kFields{
        "track_id", "x1", "y1", "x2", "y2"};
    if (!validate_known_fields(node, kFields, "真值目标", error)) return false;

    int track_id = 0;
    if (!read_required_int(node, "track_id", track_id, error) ||
        track_id <= 0) {
        set_error(error, "真值目标 track_id 必须是正整数");
        return false;
    }
    target.track_id = static_cast<std::uint64_t>(track_id);
    if (!read_required_number(node, "x1", target.x1, error) ||
        !read_required_number(node, "y1", target.y1, error) ||
        !read_required_number(node, "x2", target.x2, error) ||
        !read_required_number(node, "y2", target.y2, error)) {
        return false;
    }
    if (!target_in_source(target, expected)) {
        set_error(error, "真值目标框必须位于主机完整 FOV 内且面积为正");
        return false;
    }
    if (!target_center_in_roi(target, expected)) {
        set_error(error, "visible 真值目标中心必须位于主机 ROI 内");
        return false;
    }
    return true;
}

} // namespace

std::filesystem::path aim_ground_truth_annotation_path(
    const std::filesystem::path& directory,
    const std::filesystem::path& video_path) {
    std::filesystem::path name = video_path.filename();
    name += L".aim.json";
    return directory / name;
}

bool validate_aim_ground_truth_annotation_set(
    const std::filesystem::path& directory,
    const std::vector<std::filesystem::path>& video_paths,
    std::string& error) noexcept {
    try {
        error.clear();
        std::error_code filesystem_error;
        if (!std::filesystem::is_directory(directory, filesystem_error) ||
            filesystem_error) {
            set_error(error, "Aim 真值标注目录不存在：" +
                path_to_utf8(directory));
            return false;
        }

        std::set<std::wstring> expected_names;
        for (const auto& video_path : video_paths) {
            const auto annotation_path = aim_ground_truth_annotation_path(
                directory, video_path);
            const std::wstring normalized = lowercase_path_component(
                annotation_path.filename().wstring());
            if (!expected_names.insert(normalized).second) {
                set_error(error, "多个视频映射到同一 Aim 真值标注文件：" +
                    path_to_utf8(annotation_path.filename()));
                return false;
            }
            if (!std::filesystem::is_regular_file(
                    annotation_path, filesystem_error) || filesystem_error) {
                set_error(error, "缺少 Aim 真值标注：" +
                    path_to_utf8(annotation_path));
                return false;
            }
        }

        for (std::filesystem::directory_iterator iterator(
                 directory, filesystem_error), end;
             !filesystem_error && iterator != end;
             iterator.increment(filesystem_error)) {
            if (!iterator->is_regular_file() ||
                !has_annotation_suffix(iterator->path())) {
                continue;
            }
            const std::wstring normalized = lowercase_path_component(
                iterator->path().filename().wstring());
            if (!expected_names.contains(normalized)) {
                set_error(error, "Aim 真值标注目录存在多余文件：" +
                    path_to_utf8(iterator->path()));
                return false;
            }
        }
        if (filesystem_error) {
            set_error(error, "遍历 Aim 真值标注目录失败：" +
                filesystem_error.message());
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        set_error(error, std::string("校验 Aim 真值标注集合异常：") +
            exception.what());
        return false;
    } catch (...) {
        set_error(error, "校验 Aim 真值标注集合发生未知异常");
        return false;
    }
}

bool parse_aim_ground_truth_annotation(
    std::string_view json,
    const AimGroundTruthExpectation& expected,
    AimGroundTruthAnnotation& annotation,
    std::string& error) noexcept {
    try {
        error.clear();
        if (json.empty() || json.size() > kMaximumAnnotationBytes ||
            !valid_expectation(expected)) {
            set_error(error, "Aim 真值标注输入契约无效");
            return false;
        }
        if (json.size() >= 3U &&
            static_cast<unsigned char>(json[0]) == 0xEFU &&
            static_cast<unsigned char>(json[1]) == 0xBBU &&
            static_cast<unsigned char>(json[2]) == 0xBFU) {
            json.remove_prefix(3U);
        }

        cv::FileStorage storage(
            std::string(json), cv::FileStorage::READ |
                cv::FileStorage::MEMORY | cv::FileStorage::FORMAT_JSON);
        if (!storage.isOpened()) {
            set_error(error, "无法解析 Aim 真值 JSON");
            return false;
        }
        const cv::FileNode root = storage.root();
        static const std::unordered_set<std::string> kRootFields{
            "schema_version", "video_file", "video_sha256",
            "source_width", "source_height", "frame_count",
            "input_mode", "roi_x", "roi_y", "roi_width", "roi_height",
            "policy", "frames"};
        if (!validate_known_fields(root, kRootFields, "Aim 真值根节点", error)) {
            return false;
        }

        int schema_version = 0;
        int frame_count = 0;
        std::string video_file;
        std::string annotation_sha256;
        std::string input_mode;
        std::string policy;
        if (!read_required_int(root, "schema_version", schema_version, error) ||
            !read_required_string(root, "video_file", video_file, error) ||
            !read_required_string(root, "video_sha256", annotation_sha256, error) ||
            !read_required_int(root, "frame_count", frame_count, error) ||
            !read_required_string(root, "input_mode", input_mode, error) ||
            !read_required_string(root, "policy", policy, error)) {
            return false;
        }
        if (schema_version != kAimGroundTruthSchemaVersion ||
            video_file != expected.video_file ||
            frame_count != static_cast<int>(expected.frame_count) ||
            input_mode != expected.input_mode || policy != kAimGroundTruthPolicy) {
            set_error(error, "Aim 真值的视频名、帧数、输入模式或策略不一致");
            return false;
        }

        std::string normalized_expected_sha256;
        std::string normalized_annotation_sha256;
        if (!normalize_sha256(expected.video_sha256, normalized_expected_sha256) ||
            !normalize_sha256(annotation_sha256, normalized_annotation_sha256) ||
            normalized_expected_sha256 != normalized_annotation_sha256) {
            set_error(error, "Aim 真值绑定的视频 SHA-256 不一致或格式无效");
            return false;
        }

        int actual = 0;
        const auto match_int = [&](const char* name, int expected_value) {
            return read_required_int(root, name, actual, error) &&
                   actual == expected_value;
        };
        if (!match_int("source_width", expected.source_width) ||
            !match_int("source_height", expected.source_height) ||
            !match_int("roi_x", expected.roi_x) ||
            !match_int("roi_y", expected.roi_y) ||
            !match_int("roi_width", expected.roi_width) ||
            !match_int("roi_height", expected.roi_height)) {
            if (error.empty()) set_error(error, "Aim 真值几何与评价契约不一致");
            return false;
        }

        const cv::FileNode frames = root["frames"];
        if (frames.empty() || !frames.isSeq() ||
            frames.size() != expected.frame_count) {
            set_error(error, "Aim 真值 frames 必须逐帧覆盖整个视频");
            return false;
        }

        AimGroundTruthAnnotation candidate;
        candidate.policy = policy;
        candidate.source_width = expected.source_width;
        candidate.source_height = expected.source_height;
        candidate.roi_x = expected.roi_x;
        candidate.roi_y = expected.roi_y;
        candidate.roi_width = expected.roi_width;
        candidate.roi_height = expected.roi_height;
        candidate.frames.reserve(expected.frame_count);
        static const std::unordered_set<std::string> kFrameFields{
            "frame_index", "state", "targets"};
        for (std::size_t index = 0; index < expected.frame_count; ++index) {
            const cv::FileNode frame = frames[static_cast<int>(index)];
            if (!validate_known_fields(frame, kFrameFields, "Aim 真值帧", error)) {
                return false;
            }
            int frame_index = -1;
            std::string state_text;
            if (!read_required_int(frame, "frame_index", frame_index, error) ||
                frame_index != static_cast<int>(index) ||
                !read_required_string(frame, "state", state_text, error)) {
                set_error(error, "Aim 真值帧序号必须从 0 连续递增");
                return false;
            }
            AimGroundTruthState state = AimGroundTruthState::NOT_VISIBLE;
            if (!parse_state(state_text, state)) {
                set_error(error, "未知 Aim 真值状态：" + state_text);
                return false;
            }
            const cv::FileNode targets = frame["targets"];
            if (!targets.isSeq()) {
                set_error(error, "Aim 真值帧 targets 必须是数组");
                return false;
            }

            AimGroundTruthFrame candidate_frame;
            candidate_frame.state = state;
            std::unordered_set<std::uint64_t> ids;
            for (auto iterator = targets.begin(); iterator != targets.end();
                 ++iterator) {
                AimGroundTruthTarget target;
                if (!read_target(*iterator, expected, target, error)) {
                    return false;
                }
                if (!ids.insert(target.track_id).second) {
                    set_error(error, "同一真值帧存在重复 track_id");
                    return false;
                }
                candidate_frame.targets.push_back(target);
            }
            if (state == AimGroundTruthState::VISIBLE &&
                candidate_frame.targets.empty()) {
                set_error(error, "visible 真值帧必须至少包含一个目标");
                return false;
            }
            if (state != AimGroundTruthState::VISIBLE &&
                !candidate_frame.targets.empty()) {
                set_error(error, "not_visible/ignore 真值帧不得携带目标框");
                return false;
            }
            candidate.frames.push_back(std::move(candidate_frame));
        }
        annotation = std::move(candidate);
        return true;
    } catch (const cv::Exception& exception) {
        set_error(error, std::string("解析 Aim 真值 JSON 失败：") +
            exception.what());
        return false;
    } catch (const std::exception& exception) {
        set_error(error, std::string("解析 Aim 真值标注异常：") +
            exception.what());
        return false;
    } catch (...) {
        set_error(error, "解析 Aim 真值标注发生未知异常");
        return false;
    }
}

bool load_aim_ground_truth_annotation(
    const std::filesystem::path& path,
    const AimGroundTruthExpectation& expected,
    AimGroundTruthAnnotation& annotation,
    std::string& error) noexcept {
    try {
        error.clear();
        std::error_code filesystem_error;
        const std::uintmax_t file_bytes = std::filesystem::file_size(
            path, filesystem_error);
        if (filesystem_error || file_bytes == 0 ||
            file_bytes > kMaximumAnnotationBytes) {
            set_error(error, "Aim 真值文件为空、过大或不可读取：" +
                path_to_utf8(path));
            return false;
        }
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            set_error(error, "无法打开 Aim 真值文件：" + path_to_utf8(path));
            return false;
        }
        std::string json(static_cast<std::size_t>(file_bytes), '\0');
        input.read(json.data(), static_cast<std::streamsize>(json.size()));
        if (input.gcount() != static_cast<std::streamsize>(json.size()) ||
            !input) {
            set_error(error, "无法完整读取 Aim 真值文件：" + path_to_utf8(path));
            return false;
        }
        return parse_aim_ground_truth_annotation(json, expected, annotation,
                                                 error);
    } catch (const std::exception& exception) {
        set_error(error, std::string("加载 Aim 真值标注异常：") +
            exception.what());
        return false;
    } catch (...) {
        set_error(error, "加载 Aim 真值标注发生未知异常");
        return false;
    }
}

bool record_aim_control_continuity(
    const AimEvaluationConfig& config,
    const AimEvaluationFrame& frame,
    AimControlContinuityMetrics& metrics,
    std::string& error) noexcept {
    try {
        error.clear();
        if (!valid_config(config) || metrics.complete ||
            (metrics.has_previous_frame &&
             frame.frame_index != metrics.previous_frame_index + 1U) ||
            (!metrics.has_previous_frame && frame.frame_index != 0U) ||
            frame.roi_width <= 0 || frame.roi_height <= 0 ||
            !std::isfinite(frame.control_center_x) ||
            !std::isfinite(frame.control_center_y) ||
            !std::isfinite(frame.source_pixels_per_roi_pixel_x) ||
            !std::isfinite(frame.source_pixels_per_roi_pixel_y) ||
            frame.source_pixels_per_roi_pixel_x <= 0.0f ||
            frame.source_pixels_per_roi_pixel_y <= 0.0f) {
            set_error(error, "Aim 控制连续性评价帧必须从 0 开始并逐帧连续");
            return false;
        }
        if (!valid_aim_output_contract(frame)) {
            set_error(error, "Aim 控制连续性评价帧的输出契约不一致");
            return false;
        }

        ++metrics.evaluated_frames;
        if (frame.aim_status != AimStatus::SUCCESS) {
            ++metrics.invalid_aim_frames;
        }
        accumulate_control_continuity(config, frame, metrics);
        metrics.has_previous_frame = true;
        metrics.previous_frame_index = frame.frame_index;
        return true;
    } catch (const std::exception& exception) {
        set_error(error, std::string("记录 Aim 控制连续性异常：") +
            exception.what());
        return false;
    } catch (...) {
        set_error(error, "记录 Aim 控制连续性发生未知异常");
        return false;
    }
}

bool finalize_aim_control_continuity(
    std::size_t expected_frames,
    AimControlContinuityMetrics& metrics,
    std::string& error) noexcept {
    error.clear();
    const std::size_t successful_frames =
        metrics.invalid_aim_frames <= metrics.evaluated_frames
            ? metrics.evaluated_frames - metrics.invalid_aim_frames
            : 0U;
    if (metrics.complete || expected_frames == 0 ||
        expected_frames > kMaximumAnnotatedFrames ||
        metrics.evaluated_frames != expected_frames ||
        metrics.invalid_aim_frames > metrics.evaluated_frames ||
        !metrics.has_previous_frame ||
        metrics.previous_frame_index + 1U != expected_frames ||
        metrics.command_frames + metrics.target_without_command_frames +
                metrics.no_target_frames != successful_frames ||
        metrics.observed_command_frames + metrics.predicted_command_frames !=
                metrics.command_frames ||
        metrics.predicted_command_frames > metrics.predicted_target_frames ||
        metrics.predicted_target_frames >
                metrics.command_frames + metrics.target_without_command_frames ||
        metrics.limit_boundary_frames > metrics.command_frames ||
        metrics.control_direction_violation_frames > metrics.command_frames ||
        metrics.lead_active_frames >
                metrics.command_frames + metrics.target_without_command_frames ||
        metrics.base_aim_point_outside_box_frames >
                metrics.command_frames + metrics.target_without_command_frames ||
        metrics.prediction_point_outside_box_frames >
                metrics.lead_active_frames ||
        metrics.lead_limit_violation_frames >
                metrics.command_frames + metrics.target_without_command_frames ||
        metrics.range_locked_frames >
                metrics.command_frames + metrics.target_without_command_frames ||
        metrics.range_blocked_target_frames >
                metrics.target_without_command_frames ||
        metrics.continuity_segments > metrics.command_frames ||
        metrics.delta_samples.size() + metrics.continuity_segments !=
                metrics.command_frames ||
        metrics.acceleration_samples.size() > metrics.delta_samples.size() ||
        metrics.direction_reversals > metrics.delta_samples.size() ||
        metrics.abs_dx_samples.size() != metrics.command_frames ||
        metrics.abs_dy_samples.size() != metrics.command_frames ||
        metrics.magnitude_samples.size() != metrics.command_frames ||
        metrics.track_speed_samples.size() !=
                metrics.command_frames + metrics.target_without_command_frames ||
        metrics.base_error_samples.size() != metrics.track_speed_samples.size() ||
        metrics.final_error_samples.size() != metrics.track_speed_samples.size() ||
        metrics.lead_samples.size() != metrics.track_speed_samples.size() ||
        metrics.observation_age_samples.size() !=
                metrics.track_speed_samples.size() ||
        metrics.acquisition_range_samples.size() !=
                metrics.track_speed_samples.size() ||
        metrics.active_range_samples.size() !=
                metrics.track_speed_samples.size()) {
        set_error(error, "Aim 控制连续性评价没有完整覆盖全部帧");
        return false;
    }
    finalize_distribution(metrics.abs_dx_samples, metrics.abs_dx_counts);
    finalize_distribution(metrics.abs_dy_samples, metrics.abs_dy_counts);
    finalize_distribution(metrics.magnitude_samples, metrics.magnitude_counts);
    finalize_distribution(metrics.delta_samples, metrics.delta_counts);
    finalize_distribution(metrics.acceleration_samples,
                          metrics.acceleration_counts);
    finalize_distribution(metrics.track_speed_samples,
                          metrics.track_speed_pixels_per_second);
    finalize_distribution(metrics.base_error_samples,
                          metrics.base_error_pixels);
    finalize_distribution(metrics.final_error_samples,
                          metrics.final_error_pixels);
    finalize_distribution(metrics.lead_samples, metrics.lead_pixels);
    finalize_distribution(metrics.observation_age_samples,
                          metrics.observation_age_ms);
    finalize_distribution(metrics.acquisition_range_samples,
                          metrics.acquisition_range_pixels);
    finalize_distribution(metrics.active_range_samples,
                          metrics.active_range_pixels);
    metrics.complete = true;
    return true;
}

bool record_aim_evaluation(
    const AimGroundTruthAnnotation& annotation,
    const AimEvaluationConfig& config,
    const AimEvaluationFrame& frame,
    AimEvaluationMetrics& metrics,
    std::string& error) noexcept {
    try {
        error.clear();
        if (!valid_config(config) || frame.frame_index >= annotation.frames.size() ||
            metrics.complete ||
            annotation.policy != kAimGroundTruthPolicy ||
            annotation.source_width <= 0 || annotation.source_height <= 0 ||
            annotation.roi_x < 0 || annotation.roi_y < 0 ||
            annotation.roi_width <= 0 || annotation.roi_height <= 0 ||
            (metrics.has_previous_frame &&
             frame.frame_index != metrics.previous_frame_index + 1U) ||
            (!metrics.has_previous_frame && frame.frame_index != 0U) ||
            frame.source_width != annotation.source_width ||
            frame.source_height != annotation.source_height ||
            !std::isfinite(frame.source_roi_x) ||
            !std::isfinite(frame.source_roi_y) ||
            std::fabs(frame.source_roi_x -
                      static_cast<float>(annotation.roi_x)) >
                kGeometryTolerance ||
            std::fabs(frame.source_roi_y -
                      static_cast<float>(annotation.roi_y)) >
                kGeometryTolerance ||
            frame.roi_width <= 0 || frame.roi_height <= 0 ||
            !std::isfinite(frame.source_pixels_per_roi_pixel_x) ||
            !std::isfinite(frame.source_pixels_per_roi_pixel_y) ||
            frame.source_pixels_per_roi_pixel_x <= 0.0f ||
            frame.source_pixels_per_roi_pixel_y <= 0.0f) {
            set_error(error, "Aim 评价帧序号、ROI 尺寸或像素比例无效");
            return false;
        }

        if (!valid_aim_output_contract(frame)) {
            set_error(error, "Aim 评价帧的状态与目标快照契约不一致");
            return false;
        }

        const float source_roi_width =
            frame.roi_width * frame.source_pixels_per_roi_pixel_x;
        const float source_roi_height =
            frame.roi_height * frame.source_pixels_per_roi_pixel_y;
        if (std::fabs(source_roi_width -
                      static_cast<float>(annotation.roi_width)) >
                kGeometryTolerance ||
            std::fabs(source_roi_height -
                      static_cast<float>(annotation.roi_height)) >
                kGeometryTolerance) {
            set_error(error,
                "Aim 评价帧的 ROI 像素比例与主机真值 ROI 不一致");
            return false;
        }

        const AimGroundTruthFrame& truth = annotation.frames[frame.frame_index];
        metrics.annotations_present = true;
        ++metrics.annotated_frames;
        if (truth.state == AimGroundTruthState::NOT_VISIBLE) {
            ++metrics.not_visible_frames;
        } else if (truth.state == AimGroundTruthState::IGNORED) {
            ++metrics.ignored_frames;
        } else {
            ++metrics.visible_frames;
        }

        const bool output_valid = frame.aim_status == AimStatus::SUCCESS &&
                                  frame.has_target;
        if (frame.aim_status != AimStatus::SUCCESS) {
            ++metrics.invalid_aim_frames;
        }
        if (output_valid) ++metrics.output_target_frames;

        bool matched = false;
        std::uint64_t matched_ground_truth_id = 0;
        float matched_cost = std::numeric_limits<float>::max();
        if (truth.state == AimGroundTruthState::VISIBLE && output_valid) {
            const float diagonal = std::max(
                1.0f, std::hypot(static_cast<float>(frame.roi_width),
                                 static_cast<float>(frame.roi_height)));
            for (const auto& target : truth.targets) {
                const float gt_x1 =
                    (target.x1 - static_cast<float>(annotation.roi_x)) /
                    frame.source_pixels_per_roi_pixel_x;
                const float gt_y1 =
                    (target.y1 - static_cast<float>(annotation.roi_y)) /
                    frame.source_pixels_per_roi_pixel_y;
                const float gt_x2 =
                    (target.x2 - static_cast<float>(annotation.roi_x)) /
                    frame.source_pixels_per_roi_pixel_x;
                const float gt_y2 =
                    (target.y2 - static_cast<float>(annotation.roi_y)) /
                    frame.source_pixels_per_roi_pixel_y;
                const float iou = box_iou(
                    gt_x1, gt_y1, gt_x2, gt_y2,
                    frame.target.x1, frame.target.y1,
                    frame.target.x2, frame.target.y2);
                const float gt_center_x = (gt_x1 + gt_x2) * 0.5f;
                const float gt_center_y = (gt_y1 + gt_y2) * 0.5f;
                const float output_center_x =
                    (frame.target.x1 + frame.target.x2) * 0.5f;
                const float output_center_y =
                    (frame.target.y1 + frame.target.y2) * 0.5f;
                const float distance = std::hypot(
                    gt_center_x - output_center_x,
                    gt_center_y - output_center_y) / diagonal;
                if (iou < config.min_iou &&
                    distance > config.max_center_distance) {
                    continue;
                }
                const float cost = (1.0f - iou) * 0.70f + distance * 0.30f;
                if (cost < matched_cost) {
                    matched = true;
                    matched_cost = cost;
                    matched_ground_truth_id = target.track_id;
                }
            }
        }

        if (truth.state == AimGroundTruthState::VISIBLE) {
            if (matched) {
                ++metrics.matched_visible_frames;
                const auto previous = metrics.last_track_by_ground_truth.find(
                    matched_ground_truth_id);
                const auto previous_frame =
                    metrics.last_frame_by_ground_truth.find(
                        matched_ground_truth_id);
                const bool consecutive =
                    previous_frame != metrics.last_frame_by_ground_truth.end() &&
                    previous_frame->second + 1U == frame.frame_index;
                if (previous == metrics.last_track_by_ground_truth.end() ||
                    !consecutive || previous->second != frame.target.track_id) {
                    ++metrics.track_fragments;
                    const auto fragment_count =
                        ++metrics.fragment_count_by_ground_truth[
                            matched_ground_truth_id];
                    if (fragment_count > 1U) ++metrics.track_fragmentation_events;
                }
                if (previous != metrics.last_track_by_ground_truth.end() &&
                    consecutive && previous->second != frame.target.track_id) {
                    ++metrics.id_switches;
                }
                metrics.last_track_by_ground_truth[matched_ground_truth_id] =
                    frame.target.track_id;
                metrics.last_frame_by_ground_truth[matched_ground_truth_id] =
                    frame.frame_index;

                if (metrics.previous_visible && metrics.previous_matched &&
                    metrics.previous_frame_index + 1U == frame.frame_index &&
                    metrics.previous_ground_truth_id != matched_ground_truth_id) {
                    const bool old_target_still_visible = std::any_of(
                        truth.targets.begin(), truth.targets.end(),
                        [&](const AimGroundTruthTarget& target) {
                            return target.track_id == metrics.previous_ground_truth_id;
                        });
                    if (old_target_still_visible) ++metrics.unnecessary_switches;
                }
            } else {
                ++metrics.missed_visible_frames;
            }
        }

        metrics.has_previous_frame = true;
        metrics.previous_frame_index = frame.frame_index;
        metrics.previous_visible = truth.state == AimGroundTruthState::VISIBLE;
        metrics.previous_matched = matched;
        metrics.previous_ground_truth_id = matched_ground_truth_id;
        metrics.previous_track_id = output_valid ? frame.target.track_id : 0;
        return true;
    } catch (const std::exception& exception) {
        set_error(error, std::string("记录 Aim 评价帧异常：") + exception.what());
        return false;
    } catch (...) {
        set_error(error, "记录 Aim 评价帧发生未知异常");
        return false;
    }
}

bool finalize_aim_evaluation(
    const AimGroundTruthAnnotation& annotation,
    AimEvaluationMetrics& metrics,
    std::string& error) noexcept {
    error.clear();
    if (!metrics.annotations_present ||
        metrics.annotated_frames != annotation.frames.size() ||
        metrics.visible_frames + metrics.not_visible_frames +
                metrics.ignored_frames != metrics.annotated_frames ||
        metrics.matched_visible_frames + metrics.missed_visible_frames !=
                metrics.visible_frames ||
        (!annotation.frames.empty() &&
         (!metrics.has_previous_frame ||
          metrics.previous_frame_index + 1U != annotation.frames.size()))) {
        set_error(error, "Aim 评价没有连续覆盖真值的全部帧");
        return false;
    }
    metrics.complete = true;
    return true;
}

bool aim_roi_recall_available(const AimEvaluationMetrics& metrics) noexcept {
    return metrics.complete && metrics.visible_frames > 0;
}

double aim_roi_recall(const AimEvaluationMetrics& metrics) noexcept {
    if (!aim_roi_recall_available(metrics)) return 0.0;
    return static_cast<double>(metrics.matched_visible_frames) /
           static_cast<double>(metrics.visible_frames);
}

} // namespace aim::detail
