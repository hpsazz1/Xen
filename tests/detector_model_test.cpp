#include "detector/detector.h"
#include "detector/video_visibility_internal.h"
#include "aim/aim.h"
#include "aim/aim_evaluation_internal.h"
#include "log/log.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct LogScope {
    LogScope() {
        LogConfig config;
        config.enable_file = false;
        config.enable_debug_file = false;
        config.enable_ringbuf = false;
        Log::init(config);
        Log::register_module("detector", LogLevel::INFO);
    }

    ~LogScope() { Log::shutdown(); }
};

struct CommandLineOptions {
    std::string model_path;
    std::string backend = "cpu";
    std::string trt_cache_path;
    std::string video_directory;
    std::string report_path = "cache/benchmarks/detector-videos.csv";
    std::string input_mode = "center";
    std::string comparison_image_path;
    std::string visibility_directory;
    std::string aim_annotation_directory;
    OutputFormat output_format = OutputFormat::AUTO;
    bool has_video_directory = false;
    bool enable_gpu_preprocess = true;
    bool enable_aim_continuity = false;
};

const char* output_format_name(OutputFormat format) noexcept {
    switch (format) {
        case OutputFormat::AUTO: return "auto";
        case OutputFormat::CHANNEL_FIRST: return "channel_first";
        case OutputFormat::ANCHOR_FIRST_OBJECTNESS: return "objectness";
        case OutputFormat::END_TO_END: return "end_to_end";
    }
    return "unknown";
}

bool parse_output_format(const std::string& value,
                         OutputFormat& format) noexcept {
    if (value == "auto") {
        format = OutputFormat::AUTO;
    } else if (value == "channel_first") {
        format = OutputFormat::CHANNEL_FIRST;
    } else if (value == "objectness" ||
               value == "anchor_first_objectness") {
        format = OutputFormat::ANCHOR_FIRST_OBJECTNESS;
    } else if (value == "end_to_end") {
        format = OutputFormat::END_TO_END;
    } else {
        return false;
    }
    return true;
}

bool parse_command_line(int argc, char* argv[],
                        CommandLineOptions& options) {
    constexpr const char* kOutputFormatOption = "--output-format";
    constexpr const char* kOutputFormatPrefix = "--output-format=";
    constexpr const char* kComparisonImageOption = "--comparison-image";
    constexpr const char* kComparisonImagePrefix = "--comparison-image=";
    constexpr const char* kGpuPreprocessOption = "--gpu-preprocess";
    constexpr const char* kGpuPreprocessPrefix = "--gpu-preprocess=";
    constexpr const char* kVisibilityDirectoryOption =
        "--visibility-directory";
    constexpr const char* kVisibilityDirectoryPrefix =
        "--visibility-directory=";
    constexpr const char* kAimAnnotationDirectoryOption =
        "--aim-annotation-directory";
    constexpr const char* kAimAnnotationDirectoryPrefix =
        "--aim-annotation-directory=";
    constexpr const char* kAimContinuityOption = "--aim-continuity";
    std::vector<std::string> positional;
    positional.reserve(6);

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index] ? argv[index] : "";
        std::string format_value;
        if (argument == kOutputFormatOption) {
            if (++index >= argc || !argv[index]) {
                std::cerr << "--output-format 缺少参数\n";
                return false;
            }
            format_value = argv[index];
        } else if (argument.starts_with(kOutputFormatPrefix)) {
            format_value = argument.substr(
                std::char_traits<char>::length(kOutputFormatPrefix));
        } else if (argument == kComparisonImageOption) {
            if (++index >= argc || !argv[index] || argv[index][0] == '\0') {
                std::cerr << "--comparison-image 缺少参数\n";
                return false;
            }
            options.comparison_image_path = argv[index];
            continue;
        } else if (argument.starts_with(kComparisonImagePrefix)) {
            options.comparison_image_path = argument.substr(
                std::char_traits<char>::length(kComparisonImagePrefix));
            if (options.comparison_image_path.empty()) {
                std::cerr << "--comparison-image 缺少参数\n";
                return false;
            }
            continue;
        } else if (argument == kGpuPreprocessOption ||
                   argument.starts_with(kGpuPreprocessPrefix)) {
            std::string value;
            if (argument == kGpuPreprocessOption) {
                if (++index >= argc || !argv[index]) {
                    std::cerr << "--gpu-preprocess 缺少参数\n";
                    return false;
                }
                value = argv[index];
            } else {
                value = argument.substr(
                    std::char_traits<char>::length(kGpuPreprocessPrefix));
            }
            if (value == "on" || value == "true" || value == "1") {
                options.enable_gpu_preprocess = true;
            } else if (value == "off" || value == "false" || value == "0") {
                options.enable_gpu_preprocess = false;
            } else {
                std::cerr << "--gpu-preprocess 只接受 on 或 off\n";
                return false;
            }
            continue;
        } else if (argument == kVisibilityDirectoryOption) {
            if (++index >= argc || !argv[index] || argv[index][0] == '\0') {
                std::cerr << "--visibility-directory 缺少参数\n";
                return false;
            }
            options.visibility_directory = argv[index];
            continue;
        } else if (argument.starts_with(kVisibilityDirectoryPrefix)) {
            options.visibility_directory = argument.substr(
                std::char_traits<char>::length(
                    kVisibilityDirectoryPrefix));
            if (options.visibility_directory.empty()) {
                std::cerr << "--visibility-directory 缺少参数\n";
                return false;
            }
            continue;
        } else if (argument == kAimAnnotationDirectoryOption) {
            if (++index >= argc || !argv[index] || argv[index][0] == '\0') {
                std::cerr << "--aim-annotation-directory 缺少参数\n";
                return false;
            }
            options.aim_annotation_directory = argv[index];
            continue;
        } else if (argument.starts_with(kAimAnnotationDirectoryPrefix)) {
            options.aim_annotation_directory = argument.substr(
                std::char_traits<char>::length(
                    kAimAnnotationDirectoryPrefix));
            if (options.aim_annotation_directory.empty()) {
                std::cerr << "--aim-annotation-directory 缺少参数\n";
                return false;
            }
            continue;
        } else if (argument == kAimContinuityOption) {
            options.enable_aim_continuity = true;
            continue;
        } else if (argument.starts_with("--")) {
            std::cerr << "未知选项：" << argument << '\n';
            return false;
        } else {
            positional.push_back(argument);
            continue;
        }

        if (!parse_output_format(format_value, options.output_format)) {
            std::cerr << "未知输出契约：" << format_value << '\n';
            return false;
        }
    }

    if (positional.empty() || positional.size() > 6 ||
        positional[0].empty()) {
        return false;
    }
    options.model_path = positional[0];
    if (positional.size() >= 2) options.backend = positional[1];
    if (positional.size() >= 3) options.trt_cache_path = positional[2];
    if (positional.size() >= 4) {
        options.video_directory = positional[3];
        options.has_video_directory = true;
    }
    if (positional.size() >= 5) options.report_path = positional[4];
    if (positional.size() >= 6) options.input_mode = positional[5];
    if (!options.visibility_directory.empty() &&
        !options.has_video_directory) {
        std::cerr << "可见性标注只能用于视频基准\n";
        return false;
    }
    if (!options.aim_annotation_directory.empty() &&
        !options.has_video_directory) {
        std::cerr << "Aim 真值标注只能用于视频基准\n";
        return false;
    }
    if (options.enable_aim_continuity && !options.has_video_directory) {
        std::cerr << "Aim 控制连续性评价只能用于视频基准\n";
        return false;
    }
    return true;
}

struct VideoBenchmarkResult {
    std::string scene;
    int source_width = 0;
    int source_height = 0;
    int evaluated_width = 0;
    int evaluated_height = 0;
    int model_input_width = 0;
    int model_input_height = 0;
    double source_fps = 0.0;
    std::size_t frame_count = 0;
    std::size_t failed_frame_count = 0;
    std::size_t detected_frame_count = 0;
    std::size_t longest_empty_sequence = 0;
    std::size_t detection_count_sum = 0;
    double best_confidence_sum = 0.0;
    DetectionStatus status = DetectionStatus::NOT_RUN;
    bool explicit_device_copy = false;
    bool gpu_preprocess = false;
    std::uint64_t input_upload_bytes = 0;
    std::string visibility_policy;
    detector::detail::VideoVisibilityMetrics visibility;
    bool aim_annotations_present = false;
    bool aim_continuity_enabled = false;
    std::string aim_policy;
    AimConfig aim_config;
    aim::detail::AimEvaluationConfig aim_evaluation_config;
    double aim_timebase_fps = 0.0;
    aim::detail::AimEvaluationMetrics aim_evaluation;
    std::vector<double> preprocess_ms;
    std::vector<double> inference_ms;
    std::vector<double> h2d_ms;
    std::vector<double> gpu_preprocess_ms;
    std::vector<double> execution_ms;
    std::vector<double> d2h_ms;
    std::vector<double> postprocess_ms;
    std::vector<double> total_ms;
};

std::string path_to_utf8(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

std::wstring normalized_absolute_path(
    const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(path, error);
    if (error) return {};
    std::wstring normalized = absolute.lexically_normal().wstring();
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](wchar_t character) {
                       return static_cast<wchar_t>(std::towlower(character));
                   });
    return normalized;
}

struct FileIdentity {
    DWORD volume_serial = 0;
    DWORD file_index_high = 0;
    DWORD file_index_low = 0;
};

bool get_file_identity(const std::filesystem::path& path,
                       FileIdentity& identity) noexcept {
    HANDLE handle = CreateFileW(
        path.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION information{};
    const bool succeeded = GetFileInformationByHandle(handle, &information);
    CloseHandle(handle);
    if (!succeeded) return false;
    identity.volume_serial = information.dwVolumeSerialNumber;
    identity.file_index_high = information.nFileIndexHigh;
    identity.file_index_low = information.nFileIndexLow;
    return true;
}

bool same_windows_path(const std::filesystem::path& left,
                       const std::filesystem::path& right) {
    const std::wstring normalized_left = normalized_absolute_path(left);
    const std::wstring normalized_right = normalized_absolute_path(right);
    if (!normalized_left.empty() && normalized_left == normalized_right) {
        return true;
    }
    FileIdentity left_identity;
    FileIdentity right_identity;
    return get_file_identity(left, left_identity) &&
           get_file_identity(right, right_identity) &&
           left_identity.volume_serial == right_identity.volume_serial &&
           left_identity.file_index_high == right_identity.file_index_high &&
           left_identity.file_index_low == right_identity.file_index_low;
}

bool has_video_extension(const std::filesystem::path& path) {
    std::string extension = path_to_utf8(path.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return extension == ".mp4" || extension == ".avi";
}

double average(const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    return std::accumulate(values.begin(), values.end(), 0.0) /
           static_cast<double>(values.size());
}

double percentile(std::vector<double> values, double ratio) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const double position = ratio * static_cast<double>(values.size() - 1);
    const auto lower = static_cast<std::size_t>(position);
    const auto upper = std::min(lower + 1, values.size() - 1);
    const double fraction = position - static_cast<double>(lower);
    return values[lower] + (values[upper] - values[lower]) * fraction;
}

std::string csv_escape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (const char character : value) {
        if (character == '"') escaped.push_back('"');
        escaped.push_back(character);
    }
    escaped.push_back('"');
    return escaped;
}

std::string join_ints(const std::vector<int>& values) {
    std::ostringstream output;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) output << ';';
        output << values[index];
    }
    return output.str();
}

bool detections_match(const std::vector<Detection>& left,
                      const std::vector<Detection>& right) {
    constexpr float kCoordinateTolerance = 1e-3f;
    constexpr float kConfidenceTolerance = 1e-5f;
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (left[index].class_id != right[index].class_id ||
            std::fabs(left[index].x1 - right[index].x1) >
                kCoordinateTolerance ||
            std::fabs(left[index].y1 - right[index].y1) >
                kCoordinateTolerance ||
            std::fabs(left[index].x2 - right[index].x2) >
                kCoordinateTolerance ||
            std::fabs(left[index].y2 - right[index].y2) >
                kCoordinateTolerance ||
            std::fabs(left[index].confidence - right[index].confidence) >
                kConfidenceTolerance) {
            return false;
        }
    }
    return true;
}

cv::Mat select_video_input(const cv::Mat& frame,
                           const Detector& detector,
                           bool use_center_crop) {
    if (!use_center_crop ||
        frame.cols < detector.input_width() ||
        frame.rows < detector.input_height()) {
        return frame;
    }

    // 只创建共享原帧数据的 ROI 视图，不为每帧复制 320x320 图像。
    const int left = (frame.cols - detector.input_width()) / 2;
    const int top = (frame.rows - detector.input_height()) / 2;
    return frame(cv::Rect(
        left, top, detector.input_width(), detector.input_height()));
}

class TemporaryReportGuard {
public:
    explicit TemporaryReportGuard(std::filesystem::path path)
        : path_(std::move(path)) {}

    ~TemporaryReportGuard() {
        if (released_) return;
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    TemporaryReportGuard(const TemporaryReportGuard&) = delete;
    TemporaryReportGuard& operator=(const TemporaryReportGuard&) = delete;

    void release() noexcept { released_ = true; }

private:
    std::filesystem::path path_;
    bool released_ = false;
};

bool benchmark_video(Detector& detector,
                     const std::filesystem::path& video_path,
                     bool use_center_crop,
                     const std::filesystem::path& annotation_path,
                     bool has_visibility_annotations,
                     const std::filesystem::path& aim_annotation_path,
                     bool has_aim_annotations,
                     bool run_aim_continuity,
                     VideoBenchmarkResult& result) {
    const std::string open_path = path_to_utf8(video_path);
    cv::VideoCapture capture(open_path, cv::CAP_ANY);
    if (!capture.isOpened()) {
        std::cerr << "无法打开视频：" << open_path << '\n';
        return false;
    }

    result.scene = path_to_utf8(video_path.stem());
    result.source_width = static_cast<int>(
        capture.get(cv::CAP_PROP_FRAME_WIDTH));
    result.source_height = static_cast<int>(
        capture.get(cv::CAP_PROP_FRAME_HEIGHT));
    result.source_fps = capture.get(cv::CAP_PROP_FPS);
    result.model_input_width = detector.input_width();
    result.model_input_height = detector.input_height();

    cv::Mat frame;
    if (!capture.read(frame) || frame.empty()) {
        std::cerr << "视频没有可读取帧：" << open_path << '\n';
        return false;
    }
    // 首个实际解码帧是源几何真值。容器元数据可能缺失或不精确，不能让无标注
    // 基准因此失败；后续每帧仍必须与该几何一致。
    result.source_width = frame.cols;
    result.source_height = frame.rows;

    const cv::Mat warmup_frame = select_video_input(
        frame, detector, use_center_crop);
    result.evaluated_width = warmup_frame.cols;
    result.evaluated_height = warmup_frame.rows;

    detector::detail::VideoVisibilityAnnotation visibility_annotation;
    if (has_visibility_annotations) {
        const double declared_frames = capture.get(cv::CAP_PROP_FRAME_COUNT);
        const double rounded_frames = std::round(declared_frames);
        if (!std::isfinite(declared_frames) || declared_frames <= 0.0 ||
            std::fabs(declared_frames - rounded_frames) > 1e-6 ||
            rounded_frames > static_cast<double>(
                std::numeric_limits<int>::max())) {
            std::cerr << "视频容器没有可用于绑定标注的精确帧数："
                      << open_path << '\n';
            return false;
        }

        std::string video_sha256;
        std::string annotation_error;
        if (!detector::detail::compute_file_sha256(
                video_path, video_sha256, annotation_error)) {
            std::cerr << annotation_error << '\n';
            return false;
        }
        detector::detail::VideoVisibilityExpectation expected;
        expected.video_file = path_to_utf8(video_path.filename());
        expected.video_sha256 = video_sha256;
        expected.source_width = result.source_width;
        expected.source_height = result.source_height;
        expected.frame_count = static_cast<std::size_t>(rounded_frames);
        expected.input_mode = "center";
        expected.roi_x = (result.source_width - result.evaluated_width) / 2;
        expected.roi_y = (result.source_height - result.evaluated_height) / 2;
        expected.roi_width = result.evaluated_width;
        expected.roi_height = result.evaluated_height;
        if (!detector::detail::load_video_visibility_annotation(
                annotation_path, expected, visibility_annotation,
                annotation_error)) {
            std::cerr << "视频可见性标注无效：" << annotation_error << '\n';
            return false;
        }
        result.visibility_policy = visibility_annotation.policy;
    }

    aim::detail::AimGroundTruthAnnotation aim_annotation;
    const AimConfig aim_config;
    aim::detail::AimEvaluationConfig aim_evaluation_config;
    aim_evaluation_config.max_counts_per_frame =
        aim_config.max_counts_per_frame;
    Aim aim_runner(aim_config);
    if (run_aim_continuity) {
        result.aim_continuity_enabled = true;
        result.aim_config = aim_config;
        result.aim_evaluation_config = aim_evaluation_config;
    }
    if (has_aim_annotations) {
        if (!use_center_crop) {
            std::cerr << "Aim 真值标注只支持 center 输入模式\n";
            return false;
        }
        // 当前入口只评价未经缩放的中心 ROI。网络录像若缩放编码，必须从发送端
        // 元数据取得主机 ROI 与编码 ROI 比例，不能用辅机显示分辨率反推。
        if (result.evaluated_width != result.model_input_width ||
            result.evaluated_height != result.model_input_height) {
            std::cerr << "Aim 真值评价要求 center ROI 与模型输入保持 1:1\n";
            return false;
        }
        const double declared_frames = capture.get(cv::CAP_PROP_FRAME_COUNT);
        const double rounded_frames = std::round(declared_frames);
        if (!std::isfinite(declared_frames) || declared_frames <= 0.0 ||
            std::fabs(declared_frames - rounded_frames) > 1e-6 ||
            rounded_frames > static_cast<double>(
                std::numeric_limits<int>::max())) {
            std::cerr << "视频容器没有可用于绑定 Aim 真值的精确帧数："
                      << open_path << '\n';
            return false;
        }

        std::string video_sha256;
        std::string annotation_error;
        if (!detector::detail::compute_file_sha256(
                video_path, video_sha256, annotation_error)) {
            std::cerr << annotation_error << '\n';
            return false;
        }
        aim::detail::AimGroundTruthExpectation expected;
        expected.video_file = path_to_utf8(video_path.filename());
        expected.video_sha256 = video_sha256;
        expected.source_width = result.source_width;
        expected.source_height = result.source_height;
        expected.frame_count = static_cast<std::size_t>(rounded_frames);
        expected.input_mode = "center";
        expected.roi_x = (result.source_width - result.evaluated_width) / 2;
        expected.roi_y = (result.source_height - result.evaluated_height) / 2;
        expected.roi_width = result.evaluated_width;
        expected.roi_height = result.evaluated_height;
        if (!aim::detail::load_aim_ground_truth_annotation(
                aim_annotation_path, expected, aim_annotation,
                annotation_error)) {
            std::cerr << "Aim 真值标注无效：" << annotation_error << '\n';
            return false;
        }
        result.aim_annotations_present = true;
        result.aim_policy = aim_annotation.policy;
    }

    // 固定重复第一帧只用于预热 Session、GPU 时钟和缓存，不计入正式样本；
    // 输入裁剪口径必须与正式测量完全一致，否则预热了不同的前处理路径。
    constexpr int kWarmupFrames = 50;
    for (int index = 0; index < kWarmupFrames; ++index) {
        detector.detect(warmup_frame);
        if (detector.profile().status != DetectionStatus::SUCCESS) {
            std::cerr << "视频预热推理失败：" << open_path << '\n';
            return false;
        }
    }

    capture.release();
    capture.open(open_path, cv::CAP_ANY);
    if (!capture.isOpened()) {
        std::cerr << "预热后无法重新打开视频：" << open_path << '\n';
        return false;
    }

    std::size_t current_empty_sequence = 0;
    const double source_fps = std::isfinite(result.source_fps) &&
            result.source_fps > 1.0
        ? result.source_fps : 60.0;
    if (run_aim_continuity) result.aim_timebase_fps = source_fps;
    const auto aim_time_origin = std::chrono::steady_clock::now();
    while (capture.read(frame)) {
        if (frame.empty()) {
            std::cerr << "视频解码产生空帧：" << open_path << '\n';
            return false;
        }
        if (frame.cols != result.source_width ||
            frame.rows != result.source_height) {
            std::cerr << "视频源帧尺寸在基准期间发生变化："
                      << open_path << '\n';
            return false;
        }

        // 游戏实时链路通常只采集准星附近 FOV。全屏录像必须在推理前恢复同样的
        // 中心 ROI，否则把 2560x1440 压到 320x320 会把人物缩小约八倍。
        const cv::Mat evaluated_frame = select_video_input(
            frame, detector, use_center_crop);
        if (evaluated_frame.cols != result.evaluated_width ||
            evaluated_frame.rows != result.evaluated_height) {
            std::cerr << "视频帧尺寸在基准期间发生变化：" << open_path << '\n';
            return false;
        }

        const std::size_t frame_index = result.frame_count;
        if (has_visibility_annotations &&
            frame_index >= visibility_annotation.frames.size()) {
            std::cerr << "视频实际帧数超过标注绑定帧数：" << open_path << '\n';
            return false;
        }

        auto detections = detector.detect(evaluated_frame);
        const InferenceProfile profile = detector.profile();
        ++result.frame_count;

        const auto make_aim_evaluation_frame =
            [&](const AimResult& aim_result) {
                aim::detail::AimEvaluationFrame evaluation_frame;
                evaluation_frame.frame_index = frame_index;
                evaluation_frame.source_width = result.source_width;
                evaluation_frame.source_height = result.source_height;
                evaluation_frame.source_roi_x = static_cast<float>(
                    (result.source_width - result.evaluated_width) / 2);
                evaluation_frame.source_roi_y = static_cast<float>(
                    (result.source_height - result.evaluated_height) / 2);
                evaluation_frame.roi_width = evaluated_frame.cols;
                evaluation_frame.roi_height = evaluated_frame.rows;
                evaluation_frame.source_pixels_per_roi_pixel_x = 1.0f;
                evaluation_frame.source_pixels_per_roi_pixel_y = 1.0f;
                evaluation_frame.aim_status = aim_result.status;
                evaluation_frame.has_target = aim_result.has_target;
                evaluation_frame.has_command = aim_result.has_command;
                evaluation_frame.target = aim_result.target;
                evaluation_frame.command = aim_result.command;
                return evaluation_frame;
            };

        // 空检测是合法结果；pipeline 状态是区分零检测与执行失败的唯一依据。
        if (profile.status != DetectionStatus::SUCCESS) {
            if (run_aim_continuity) {
                AimResult failed_aim;
                failed_aim.status = AimStatus::NOT_RUN;
                const auto failed_frame = make_aim_evaluation_frame(failed_aim);
                std::string evaluation_error;
                if (!aim::detail::record_aim_control_continuity(
                        aim_evaluation_config, failed_frame,
                        result.aim_evaluation.control, evaluation_error)) {
                    std::cerr << "Aim 控制连续性帧无效："
                              << evaluation_error << '\n';
                    return false;
                }
                if (has_aim_annotations &&
                    !aim::detail::record_aim_evaluation(
                        aim_annotation, aim_evaluation_config, failed_frame,
                        result.aim_evaluation, evaluation_error)) {
                    std::cerr << "Aim 评价帧无效：" << evaluation_error << '\n';
                    return false;
                }
            }
            if (result.status == DetectionStatus::NOT_RUN ||
                result.status == DetectionStatus::SUCCESS) {
                result.status = profile.status;
            }
            ++result.failed_frame_count;
            continue;
        }
        if (result.status == DetectionStatus::NOT_RUN) {
            result.status = DetectionStatus::SUCCESS;
        }

        result.preprocess_ms.push_back(profile.preprocess_ms);
        result.inference_ms.push_back(profile.inference_ms);
        result.h2d_ms.push_back(profile.h2d_ms);
        result.gpu_preprocess_ms.push_back(profile.gpu_preprocess_ms);
        result.execution_ms.push_back(profile.execution_ms);
        result.d2h_ms.push_back(profile.d2h_ms);
        result.postprocess_ms.push_back(profile.postprocess_ms);
        result.total_ms.push_back(profile.total_ms);
        result.explicit_device_copy =
            result.explicit_device_copy || profile.explicit_device_copy;
        result.gpu_preprocess =
            result.gpu_preprocess || profile.gpu_preprocess;
        if (result.input_upload_bytes == 0) {
            result.input_upload_bytes = profile.input_upload_bytes;
        } else if (result.input_upload_bytes != profile.input_upload_bytes) {
            std::cerr << "输入上传字节数在固定 shape 基准期间发生变化\n";
            return false;
        }
        result.detection_count_sum += detections.size();
        if (has_visibility_annotations) {
            detector::detail::record_video_visibility(
                visibility_annotation.frames[frame_index],
                !detections.empty(), result.visibility);
        }

        const bool has_detections = !detections.empty();
        if (has_detections) {
            const auto best = std::max_element(
                detections.begin(), detections.end(),
                [](const Detection& left, const Detection& right) {
                    return left.confidence < right.confidence;
                });
            result.best_confidence_sum += best->confidence;
        }
        if (run_aim_continuity) {
            AimFrame aim_frame;
            aim_frame.sequence = static_cast<std::uint64_t>(frame_index + 1U);
            aim_frame.captured_at = aim_time_origin +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(
                        static_cast<double>(frame_index) / source_fps));
            aim_frame.roi_width = evaluated_frame.cols;
            aim_frame.roi_height = evaluated_frame.rows;
            aim_frame.control_center_x = evaluated_frame.cols * 0.5f;
            aim_frame.control_center_y = evaluated_frame.rows * 0.5f;
            aim_frame.source_pixels_per_roi_pixel_x = 1.0f;
            aim_frame.source_pixels_per_roi_pixel_y = 1.0f;
            aim_frame.detections = std::move(detections);
            const AimResult aim_result = aim_runner.process(aim_frame);
            const auto evaluation_frame =
                make_aim_evaluation_frame(aim_result);
            std::string evaluation_error;
            if (!aim::detail::record_aim_control_continuity(
                    aim_evaluation_config, evaluation_frame,
                    result.aim_evaluation.control, evaluation_error)) {
                std::cerr << "Aim 控制连续性帧无效："
                          << evaluation_error << '\n';
                return false;
            }
            if (has_aim_annotations &&
                !aim::detail::record_aim_evaluation(
                    aim_annotation, aim_evaluation_config, evaluation_frame,
                    result.aim_evaluation, evaluation_error)) {
                std::cerr << "Aim 评价帧无效：" << evaluation_error << '\n';
                return false;
            }
            if (aim_result.status != AimStatus::SUCCESS) {
                std::cerr << "Aim 处理失败：" << open_path
                          << "，frame=" << frame_index
                          << "，status=" << AimStatusName(aim_result.status)
                          << '\n';
                return false;
            }
        }

        if (!has_detections) {
            ++current_empty_sequence;
            result.longest_empty_sequence = std::max(
                result.longest_empty_sequence, current_empty_sequence);
            continue;
        }

        current_empty_sequence = 0;
        ++result.detected_frame_count;
    }

    if (result.frame_count == 0 || result.total_ms.empty()) {
        std::cerr << "视频没有产生有效基准样本：" << open_path << '\n';
        return false;
    }
    if (result.failed_frame_count != 0) {
        std::cerr << "视频存在推理失败帧：" << open_path
                  << "，failed=" << result.failed_frame_count << '\n';
        return false;
    }
    if (has_visibility_annotations &&
        (result.frame_count != visibility_annotation.frames.size() ||
         result.visibility.annotated_frames != result.frame_count)) {
        std::cerr << "视频实际解码帧数与可见性标注帧数不一致："
                  << open_path << '\n';
        return false;
    }
    if (run_aim_continuity) {
        std::string evaluation_error;
        if (!aim::detail::finalize_aim_control_continuity(
                result.frame_count, result.aim_evaluation.control,
                evaluation_error)) {
            std::cerr << "Aim 控制连续性未完整覆盖视频："
                      << evaluation_error << '\n';
            return false;
        }
        if (result.aim_evaluation.control.invalid_aim_frames != 0) {
            std::cerr << "Aim 控制连续性存在失败帧：" << open_path
                      << "，failed="
                      << result.aim_evaluation.control.invalid_aim_frames
                      << '\n';
            return false;
        }
    }
    if (has_aim_annotations) {
        std::string evaluation_error;
        if (!aim::detail::finalize_aim_evaluation(
                aim_annotation, result.aim_evaluation, evaluation_error)) {
            std::cerr << "Aim 评价未完整覆盖视频：" << evaluation_error << '\n';
            return false;
        }
        if (result.aim_evaluation.invalid_aim_frames != 0) {
            std::cerr << "Aim 存在失败帧：" << open_path
                      << "，failed="
                      << result.aim_evaluation.invalid_aim_frames << '\n';
            return false;
        }
    }
    return true;
}

void write_video_result(std::ostream& output,
                        const VideoBenchmarkResult& result) {
    const double valid_frames = static_cast<double>(result.total_ms.size());
    const double detection_rate = valid_frames > 0.0
        ? static_cast<double>(result.detected_frame_count) / valid_frames
        : 0.0;
    const double mean_detection_count = valid_frames > 0.0
        ? static_cast<double>(result.detection_count_sum) / valid_frames
        : 0.0;
    const double mean_best_confidence = result.detected_frame_count > 0
        ? result.best_confidence_sum /
              static_cast<double>(result.detected_frame_count)
        : 0.0;
    const bool recall_available =
        detector::detail::video_visibility_recall_available(
            result.visibility);
    const bool aim_recall_available =
        aim::detail::aim_roi_recall_available(result.aim_evaluation);

    output << csv_escape(result.scene) << ','
           << result.source_width << ',' << result.source_height << ','
           << result.evaluated_width << ',' << result.evaluated_height << ','
           << result.model_input_width << ',' << result.model_input_height << ','
           << result.source_fps << ',' << result.frame_count << ','
           << result.failed_frame_count << ','
           << DetectionStatusName(result.status) << ','
           << (result.explicit_device_copy ? 1 : 0) << ','
           << (result.gpu_preprocess ? 1 : 0) << ','
           << result.input_upload_bytes << ','
           << result.detected_frame_count
           << ',' << detection_rate << ',' << result.longest_empty_sequence
           << ',' << mean_detection_count << ',' << mean_best_confidence
           << ',' << (result.visibility.annotations_present ? 1 : 0)
           << ',' << (recall_available ? 1 : 0)
           << ',' << csv_escape(result.visibility_policy)
           << ',' << result.visibility.annotated_frames
           << ',' << result.visibility.visible_frames
           << ',' << result.visibility.visible_detected_frames
           << ',' << result.visibility.visible_missed_frames << ',';
    if (recall_available) {
        output << detector::detail::video_visibility_recall(
            result.visibility);
    }
    output << ',' << result.visibility.longest_visible_miss_sequence
           << ',' << result.visibility.not_visible_frames
           << ',' << result.visibility.not_visible_detected_frames
           << ',' << result.visibility.ignored_frames
           << ',' << result.visibility.ignored_detected_frames << ',';
    if (result.visibility.annotations_present) {
        output << detector::detail::video_visibility_evaluable_rate(
            result.visibility);
    }
    const auto& control = result.aim_evaluation.control;
    output << ',' << (result.aim_continuity_enabled ? 1 : 0)
           << ',' << (control.complete ? 1 : 0)
           << ',' << control.evaluated_frames
           << ',' << control.invalid_aim_frames
           << ',' << (result.aim_annotations_present ? 1 : 0)
           << ',' << (result.aim_evaluation.complete ? 1 : 0)
           << ',' << csv_escape(result.aim_policy)
           << ',' << result.aim_evaluation.annotated_frames
           << ',' << result.aim_evaluation.visible_frames
           << ',' << result.aim_evaluation.matched_visible_frames
           << ',' << result.aim_evaluation.missed_visible_frames << ',';
    if (aim_recall_available) {
        output << aim::detail::aim_roi_recall(result.aim_evaluation);
    }
    output << ',' << result.aim_evaluation.id_switches
           << ',' << result.aim_evaluation.track_fragments
           << ',' << result.aim_evaluation.track_fragmentation_events
           << ',' << result.aim_evaluation.unnecessary_switches
           << ',' << result.aim_evaluation.not_visible_frames
           << ',' << result.aim_evaluation.ignored_frames
           << ',' << result.aim_evaluation.output_target_frames
           << ',' << result.aim_evaluation.invalid_aim_frames;
    const double limit_boundary_rate = control.command_frames > 0
        ? static_cast<double>(control.limit_boundary_frames) /
              static_cast<double>(control.command_frames)
        : 0.0;
    output << ',' << control.command_frames
           << ',' << control.observed_command_frames
           << ',' << control.predicted_command_frames
           << ',' << control.target_without_command_frames
           << ',' << control.no_target_frames
           << ',' << control.predicted_target_frames
           << ',' << control.continuity_segments
           << ',' << control.target_switches
           << ',' << control.target_state_changes
           << ',' << control.prediction_state_changes
           << ',' << control.direction_reversals
           << ',' << control.limit_boundary_frames
           << ',' << limit_boundary_rate;
    const auto write_distribution = [&](
            const aim::detail::AimDistributionSummary& summary) {
        output << ',' << summary.samples
               << ',' << summary.mean
               << ',' << summary.p50
               << ',' << summary.p95
               << ',' << summary.p99
               << ',' << summary.maximum;
    };
    write_distribution(control.abs_dx_counts);
    write_distribution(control.abs_dy_counts);
    write_distribution(control.magnitude_counts);
    write_distribution(control.delta_counts);
    write_distribution(control.acceleration_counts);
    if (result.aim_continuity_enabled) {
        output << ',' << csv_escape(join_ints(
                               result.aim_config.person_class_ids))
               << ',' << csv_escape(join_ints(result.aim_config.head_class_ids))
               << ',' << result.aim_config.high_confidence
               << ',' << result.aim_config.low_confidence
               << ',' << result.aim_config.min_confirmed_hits
               << ',' << result.aim_config.max_lost_frames
               << ',' << result.aim_config.min_iou
               << ',' << result.aim_config.max_center_distance
               << ',' << result.aim_config.switch_margin
               << ',' << result.aim_config.switch_confirm_frames
               << ',' << result.aim_config.switch_cooldown_frames
               << ',' << result.aim_config.body_aim_height_ratio
               << ',' << result.aim_config.deadzone_pixels
               << ',' << result.aim_config.smoothing
               << ',' << result.aim_config.counts_per_pixel_x
               << ',' << result.aim_config.counts_per_pixel_y
               << ',' << result.aim_config.max_counts_per_frame
               << ',' << result.aim_config.predicted_gain
               << ',' << result.aim_evaluation_config.min_iou
               << ',' << result.aim_evaluation_config.max_center_distance
               << ',' << result.aim_timebase_fps;
    } else {
        // 未启用控制连续性时不运行 Aim，也不伪造一套看似参与了本次基准的配置。
        constexpr int kAimConfigurationColumns = 21;
        for (int index = 0; index < kAimConfigurationColumns; ++index) {
            output << ',';
        }
    }
    output
           << ',' << average(result.preprocess_ms)
           << ',' << percentile(result.preprocess_ms, 0.50)
           << ',' << percentile(result.preprocess_ms, 0.95)
           << ',' << percentile(result.preprocess_ms, 0.99)
           << ',' << average(result.inference_ms)
           << ',' << percentile(result.inference_ms, 0.50)
           << ',' << percentile(result.inference_ms, 0.95)
           << ',' << percentile(result.inference_ms, 0.99)
           << ',' << average(result.h2d_ms)
           << ',' << percentile(result.h2d_ms, 0.50)
           << ',' << percentile(result.h2d_ms, 0.95)
           << ',' << percentile(result.h2d_ms, 0.99)
           << ',' << average(result.gpu_preprocess_ms)
           << ',' << percentile(result.gpu_preprocess_ms, 0.50)
           << ',' << percentile(result.gpu_preprocess_ms, 0.95)
           << ',' << percentile(result.gpu_preprocess_ms, 0.99)
           << ',' << average(result.execution_ms)
           << ',' << percentile(result.execution_ms, 0.50)
           << ',' << percentile(result.execution_ms, 0.95)
           << ',' << percentile(result.execution_ms, 0.99)
           << ',' << average(result.d2h_ms)
           << ',' << percentile(result.d2h_ms, 0.50)
           << ',' << percentile(result.d2h_ms, 0.95)
           << ',' << percentile(result.d2h_ms, 0.99)
           << ',' << average(result.postprocess_ms)
           << ',' << percentile(result.postprocess_ms, 0.50)
           << ',' << percentile(result.postprocess_ms, 0.95)
           << ',' << percentile(result.postprocess_ms, 0.99)
           << ',' << average(result.total_ms)
           << ',' << percentile(result.total_ms, 0.50)
           << ',' << percentile(result.total_ms, 0.95)
           << ',' << percentile(result.total_ms, 0.99) << '\n';
}

bool benchmark_videos(Detector& detector,
                      const std::filesystem::path& video_directory,
                      const std::filesystem::path& report_path,
                      bool use_center_crop,
                      const std::filesystem::path& visibility_directory,
                      bool has_visibility_annotations,
                      const std::filesystem::path& aim_annotation_directory,
                      bool has_aim_annotations,
                      bool run_aim_continuity) {
    std::error_code error;
    if (!std::filesystem::is_directory(video_directory, error)) {
        std::cerr << "视频目录不存在："
                  << path_to_utf8(video_directory) << '\n';
        return false;
    }

    std::vector<std::filesystem::path> video_paths;
    for (std::filesystem::directory_iterator iterator(video_directory, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (iterator->is_regular_file() &&
            has_video_extension(iterator->path())) {
            video_paths.push_back(iterator->path());
        }
    }
    if (error || video_paths.empty()) {
        std::cerr << "视频目录中没有可用 MP4/AVI："
                  << path_to_utf8(video_directory) << '\n';
        return false;
    }
    std::sort(video_paths.begin(), video_paths.end());

    if (has_visibility_annotations) {
        if (!use_center_crop) {
            std::cerr << "可见性标注只支持 center 输入模式\n";
            return false;
        }
        std::string annotation_error;
        if (!detector::detail::validate_video_visibility_annotation_set(
                visibility_directory, video_paths, annotation_error)) {
            std::cerr << annotation_error << '\n';
            return false;
        }
    }
    if (has_aim_annotations) {
        if (!use_center_crop) {
            std::cerr << "Aim 真值标注只支持 center 输入模式\n";
            return false;
        }
        std::string annotation_error;
        std::vector<std::filesystem::path> aim_video_paths = video_paths;
        if (!aim::detail::validate_aim_ground_truth_annotation_set(
                aim_annotation_directory, aim_video_paths, annotation_error)) {
            std::cerr << annotation_error << '\n';
            return false;
        }
    }

    std::filesystem::path temporary_report_path = report_path;
    temporary_report_path += ".tmp";
    for (const auto& video_path : video_paths) {
        if (same_windows_path(report_path, video_path) ||
            same_windows_path(temporary_report_path, video_path)) {
            std::cerr << "基准报告路径与视频输入冲突："
                      << path_to_utf8(video_path) << '\n';
            return false;
        }
        if (has_visibility_annotations) {
            const auto annotation_path =
                detector::detail::video_visibility_annotation_path(
                    visibility_directory, video_path);
            if (same_windows_path(report_path, annotation_path) ||
                same_windows_path(temporary_report_path, annotation_path)) {
                std::cerr << "基准报告路径与可见性标注冲突："
                          << path_to_utf8(annotation_path) << '\n';
                return false;
            }
        }
        if (has_aim_annotations) {
            const auto annotation_path =
                aim::detail::aim_ground_truth_annotation_path(
                    aim_annotation_directory, video_path);
            if (same_windows_path(report_path, annotation_path) ||
                same_windows_path(temporary_report_path, annotation_path)) {
                std::cerr << "基准报告路径与 Aim 真值标注冲突："
                          << path_to_utf8(annotation_path) << '\n';
                return false;
            }
        }
    }

    if (!report_path.parent_path().empty()) {
        std::filesystem::create_directories(report_path.parent_path(), error);
        if (error) {
            std::cerr << "无法创建报告目录：" << error.message() << '\n';
            return false;
        }
    }
    TemporaryReportGuard temporary_report_guard(temporary_report_path);
    std::ofstream report(
        temporary_report_path, std::ios::binary | std::ios::trunc);
    if (!report) {
        std::cerr << "无法创建临时报告："
                  << path_to_utf8(temporary_report_path) << '\n';
        return false;
    }

    // 写入 UTF-8 BOM，保证 Windows Excel 直接打开时中文场景名不乱码。
    report << "\xEF\xBB\xBF";
    report << "scene,source_width,source_height,evaluated_width,"
              "evaluated_height,model_input_width,model_input_height,"
              "source_fps,frames,failed_frames,status,"
              "explicit_device_copy,gpu_preprocess,input_upload_bytes,"
              "detected_frames,detection_frame_rate,longest_empty_sequence,"
              "mean_detection_count,mean_best_confidence,"
              "visibility_annotations,recall_available,visibility_policy,"
              "annotated_frames,visible_frames,visible_detected_frames,"
              "visible_missed_frames,visible_frame_recall,"
              "longest_visible_miss_sequence,not_visible_frames,"
              "not_visible_detected_frames,ignored_frames,"
              "ignored_detected_frames,evaluable_frame_rate,"
              "aim_continuity,aim_control_complete,"
              "aim_control_evaluated_frames,aim_control_invalid_frames,"
              "aim_annotations,aim_complete,aim_policy,"
              "aim_annotated_frames,aim_visible_frames,"
              "aim_matched_visible_frames,"
              "aim_missed_visible_frames,aim_roi_recall,aim_id_switches,"
              "aim_track_fragments,aim_track_fragmentation_events,"
              "aim_unnecessary_switches,aim_not_visible_frames,"
              "aim_ignored_frames,aim_output_target_frames,"
              "aim_invalid_frames,aim_command_frames,"
              "aim_observed_command_frames,aim_predicted_command_frames,"
              "aim_target_without_command_frames,aim_no_target_frames,"
              "aim_predicted_target_frames,aim_continuity_segments,"
              "aim_target_switches,aim_target_state_changes,"
              "aim_prediction_state_changes,aim_direction_reversals,"
              "aim_limit_boundary_frames,aim_limit_boundary_rate,"
              "aim_abs_dx_samples,aim_abs_dx_mean_counts,"
              "aim_abs_dx_p50_counts,aim_abs_dx_p95_counts,"
              "aim_abs_dx_p99_counts,aim_abs_dx_max_counts,"
              "aim_abs_dy_samples,aim_abs_dy_mean_counts,"
              "aim_abs_dy_p50_counts,aim_abs_dy_p95_counts,"
              "aim_abs_dy_p99_counts,aim_abs_dy_max_counts,"
              "aim_magnitude_samples,aim_magnitude_mean_counts,"
              "aim_magnitude_p50_counts,aim_magnitude_p95_counts,"
              "aim_magnitude_p99_counts,aim_magnitude_max_counts,"
              "aim_delta_samples,aim_delta_mean_counts,"
              "aim_delta_p50_counts,aim_delta_p95_counts,"
              "aim_delta_p99_counts,aim_delta_max_counts,"
              "aim_acceleration_samples,aim_acceleration_mean_counts,"
              "aim_acceleration_p50_counts,aim_acceleration_p95_counts,"
              "aim_acceleration_p99_counts,aim_acceleration_max_counts,"
              "aim_person_class_ids,aim_head_class_ids,"
              "aim_high_confidence,aim_low_confidence,"
              "aim_min_confirmed_hits,aim_max_lost_frames,aim_min_iou,"
              "aim_max_center_distance,aim_switch_margin,"
              "aim_switch_confirm_frames,aim_switch_cooldown_frames,"
              "aim_body_aim_height_ratio,aim_deadzone_pixels,"
              "aim_smoothing,aim_counts_per_pixel_x,"
              "aim_counts_per_pixel_y,aim_max_counts_per_frame,"
              "aim_predicted_gain,aim_evaluation_min_iou,"
              "aim_evaluation_max_center_distance,aim_timebase_fps,"
              "preprocess_mean_ms,preprocess_p50_ms,preprocess_p95_ms,"
              "preprocess_p99_ms,inference_mean_ms,inference_p50_ms,"
              "inference_p95_ms,inference_p99_ms,"
              "h2d_mean_ms,h2d_p50_ms,h2d_p95_ms,h2d_p99_ms,"
              "gpu_preprocess_mean_ms,gpu_preprocess_p50_ms,"
              "gpu_preprocess_p95_ms,gpu_preprocess_p99_ms,"
              "execution_mean_ms,execution_p50_ms,execution_p95_ms,"
              "execution_p99_ms,d2h_mean_ms,d2h_p50_ms,d2h_p95_ms,"
              "d2h_p99_ms,postprocess_mean_ms,"
              "postprocess_p50_ms,postprocess_p95_ms,postprocess_p99_ms,"
              "total_mean_ms,total_p50_ms,total_p95_ms,total_p99_ms\n";
    report << std::fixed << std::setprecision(4);

    std::uint64_t aim_visible_frames = 0;
    for (const auto& video_path : video_paths) {
        VideoBenchmarkResult result;
        std::cout << "开始测试场景：" << path_to_utf8(video_path.filename())
                  << std::endl;
        const auto annotation_path =
            detector::detail::video_visibility_annotation_path(
                visibility_directory, video_path);
        const auto aim_annotation_path =
            aim::detail::aim_ground_truth_annotation_path(
                aim_annotation_directory, video_path);
        if (!benchmark_video(
                detector, video_path, use_center_crop, annotation_path,
                has_visibility_annotations, aim_annotation_path,
                has_aim_annotations, run_aim_continuity, result)) {
            return false;
        }
        aim_visible_frames += result.aim_evaluation.visible_frames;
        write_video_result(report, result);
        if (!report) {
            std::cerr << "基准报告写入失败："
                      << path_to_utf8(temporary_report_path) << '\n';
            return false;
        }

        const double valid_frames = static_cast<double>(result.total_ms.size());
        const double detection_rate = valid_frames > 0.0
            ? 100.0 * static_cast<double>(result.detected_frame_count) /
                  valid_frames
            : 0.0;
        std::cout << std::fixed << std::setprecision(2)
                  << "场景完成：frames=" << result.frame_count
                  << ", status=" << DetectionStatusName(result.status)
                  << ", detected=" << detection_rate << "%"
                  << ", longest_empty=" << result.longest_empty_sequence
                  << ", visible_recall=";
        if (detector::detail::video_visibility_recall_available(
                result.visibility)) {
            std::cout << 100.0 * detector::detail::video_visibility_recall(
                result.visibility) << "%";
        } else {
            std::cout << "n/a";
        }
        std::cout << ", aim_recall=";
        if (aim::detail::aim_roi_recall_available(
                result.aim_evaluation)) {
            std::cout << 100.0 * aim::detail::aim_roi_recall(
                result.aim_evaluation) << "%";
        } else {
            std::cout << "n/a";
        }
        std::cout << ", aim_commands="
                  << result.aim_evaluation.control.command_frames
                  << ", total_p50=" << percentile(result.total_ms, 0.50)
                  << "ms, total_p95=" << percentile(result.total_ms, 0.95)
                  << "ms, total_p99=" << percentile(result.total_ms, 0.99)
                  << "ms" << std::endl;
    }

    // 全 ignore 模板只能用于继续人工标注。正式集合允许包含纯负样本场景，
    // 但整套输入至少要有一个可见目标，否则所有追踪指标都没有可评价分母。
    if (has_aim_annotations && aim_visible_frames == 0) {
        std::cerr << "Aim 真值集合没有可见目标，拒绝发布全 ignore 模板报告\n";
        return false;
    }

    report.flush();
    report.close();
    if (!report) {
        std::cerr << "基准报告落盘失败："
                  << path_to_utf8(temporary_report_path) << '\n';
        return false;
    }

    // 临时文件与最终文件位于同一目录。MoveFileExW 只在全部场景成功、流已关闭后
    // 原子替换最终报告，进程中途失败不会留下可被误读的半份 CSV。
    if (!MoveFileExW(
            temporary_report_path.c_str(), report_path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::cerr << "无法发布基准报告，Win32 error="
                  << GetLastError() << '\n';
        return false;
    }
    temporary_report_guard.release();

    std::cout << "视频基准报告：" << path_to_utf8(report_path) << '\n';
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    CommandLineOptions options;
    if (!parse_command_line(argc, argv, options)) {
        std::cerr << "用法：detector_model_test <模型路径> "
                     "[cpu|cuda|tensorrt|directml] [TensorRT缓存目录] "
                     "[视频目录] [CSV报告路径] [center|full] "
                     "[--output-format "
                     "auto|channel_first|objectness|end_to_end] "
                     "[--comparison-image <图像路径>] "
                     "[--gpu-preprocess on|off] "
                     "[--visibility-directory <标注目录>] "
                     "[--aim-annotation-directory <标注目录>] "
                     "[--aim-continuity]\n";
        return 2;
    }

    LogScope log_scope;

    DetectorConfig config;
    config.model_path = options.model_path;
    const std::string& requested_backend = options.backend;
    if (requested_backend == "cpu") {
        config.backend = BackendType::CPU;
    } else if (requested_backend == "cuda") {
        config.backend = BackendType::CUDA;
    } else if (requested_backend == "tensorrt") {
        config.backend = BackendType::TENSORRT;
        config.enable_fp16 = true;
        if (!options.trt_cache_path.empty()) {
            config.trt_cache_path = options.trt_cache_path;
        }
    } else if (requested_backend == "directml") {
        config.backend = BackendType::DIRECTML;
    } else {
        std::cerr << "未知后端：" << requested_backend << '\n';
        return 2;
    }
    config.output_format = options.output_format;
    config.enable_output_fingerprint = true;
    config.enable_gpu_preprocess = options.enable_gpu_preprocess;

    const auto load_start = std::chrono::steady_clock::now();
    Detector detector(config);
    if (!detector.load()) {
        std::cerr << "真实模型加载失败：" << config.model_path << '\n';
        return 1;
    }
    const auto load_finished = std::chrono::steady_clock::now();
    if (detector.input_width() <= 0 || detector.input_height() <= 0) {
        std::cerr << "模型输入尺寸无效\n";
        return 1;
    }
    std::string expected_provider = "CPUExecutionProvider";
    if (requested_backend == "cuda") {
        expected_provider = "CUDAExecutionProvider";
    } else if (requested_backend == "tensorrt") {
        expected_provider = "TensorrtExecutionProvider";
    } else if (requested_backend == "directml") {
        expected_provider = "DmlExecutionProvider";
    }
    if (detector.backend_name() != expected_provider) {
        std::cerr << "集成测试实际后端不符合请求："
                  << detector.backend_name() << '\n';
        return 1;
    }

    // 两种输入必须在同一 Session 连续执行。raw 模型比较 ONNX 输出张量可覆盖两帧都
    // 零检测的重放回归；图内 NMS 模型必须提供能改变最终输出的真实对照图。
    cv::Mat black_image(detector.input_height(), detector.input_width(),
                        CV_8UC3, cv::Scalar(0, 0, 0));
    const auto black_detections = detector.detect(black_image);
    const InferenceProfile black_profile = detector.profile();
    if (black_profile.status != DetectionStatus::SUCCESS) {
        std::cerr << "黑图推理失败，status="
                  << DetectionStatusName(black_profile.status) << '\n';
        return 1;
    }

    cv::Mat comparison_image;
    std::string comparison_source = "white";
    if (options.comparison_image_path.empty()) {
        comparison_image = cv::Mat(
            detector.input_height(), detector.input_width(),
            CV_8UC3, cv::Scalar(255, 255, 255));
    } else {
        comparison_image = cv::imread(
            options.comparison_image_path, cv::IMREAD_COLOR);
        comparison_source = options.comparison_image_path;
        if (comparison_image.empty()) {
            std::cerr << "无法读取对照图像：" << comparison_source << '\n';
            return 1;
        }
    }
    const auto comparison_detections = detector.detect(comparison_image);
    const InferenceProfile comparison_profile = detector.profile();
    if (comparison_profile.status != DetectionStatus::SUCCESS) {
        std::cerr << "对照图推理失败，status="
                  << DetectionStatusName(comparison_profile.status) << '\n';
        return 1;
    }
    if (requested_backend == "tensorrt" &&
        config.enable_trt_cuda_graph &&
        (!black_profile.explicit_device_copy ||
         !comparison_profile.explicit_device_copy)) {
        std::cerr << "TensorRT CUDA Graph 未走固定设备缓冲显式复制路径\n";
        return 1;
    }
    if (requested_backend == "tensorrt" &&
        config.enable_trt_cuda_graph && config.enable_gpu_preprocess) {
        const std::uint64_t expected_upload_bytes =
            static_cast<std::uint64_t>(detector.input_width()) *
            static_cast<std::uint64_t>(detector.input_height()) * 3U;
        if (!black_profile.gpu_preprocess ||
            !comparison_profile.gpu_preprocess ||
            black_profile.gpu_preprocess_ms <= 0.0 ||
            comparison_profile.gpu_preprocess_ms <= 0.0 ||
            black_profile.input_upload_bytes != expected_upload_bytes ||
            comparison_profile.input_upload_bytes != expected_upload_bytes) {
            std::cerr << "TensorRT CUDA Graph 未走固定 uint8 GPU 前处理路径\n";
            return 1;
        }
    }
    if (black_profile.output_fingerprint ==
        comparison_profile.output_fingerprint) {
        std::cerr << "黑图与对照图的原始输出指纹相同，可能重放了首次输入，"
                     "或模型把两者归并为同一端到端结果\n";
        return 1;
    }

    if (requested_backend == "tensorrt" &&
        config.enable_trt_cuda_graph) {
        DetectorConfig no_graph_config = config;
        no_graph_config.enable_trt_cuda_graph = false;
        Detector no_graph_detector(no_graph_config);
        if (!no_graph_detector.load() ||
            no_graph_detector.backend_name() != expected_provider) {
            std::cerr << "TensorRT Graph off 对照 Session 加载失败\n";
            return 1;
        }

        const auto no_graph_black = no_graph_detector.detect(black_image);
        const InferenceProfile no_graph_black_profile =
            no_graph_detector.profile();
        const auto no_graph_comparison =
            no_graph_detector.detect(comparison_image);
        const InferenceProfile no_graph_comparison_profile =
            no_graph_detector.profile();
        if (no_graph_black_profile.status != DetectionStatus::SUCCESS ||
            no_graph_comparison_profile.status != DetectionStatus::SUCCESS ||
            no_graph_black_profile.explicit_device_copy ||
            no_graph_comparison_profile.explicit_device_copy ||
            no_graph_black_profile.gpu_preprocess ||
            no_graph_comparison_profile.gpu_preprocess) {
            std::cerr << "TensorRT Graph off 对照执行状态错误\n";
            return 1;
        }
        if (black_profile.output_fingerprint !=
                no_graph_black_profile.output_fingerprint ||
            comparison_profile.output_fingerprint !=
                no_graph_comparison_profile.output_fingerprint ||
            !detections_match(black_detections, no_graph_black) ||
            !detections_match(
                comparison_detections, no_graph_comparison)) {
            std::cerr << "TensorRT Graph on/off 输出不一致\n";
            return 1;
        }
    }

    detector.detect(cv::Mat{});
    const InferenceProfile invalid_profile = detector.profile();
    if (invalid_profile.status != DetectionStatus::INVALID_INPUT) {
        std::cerr << "非法输入状态错误："
                  << DetectionStatusName(invalid_profile.status) << '\n';
        return 1;
    }
    if (invalid_profile.preprocess_ms != 0.0 ||
        invalid_profile.inference_ms != 0.0 ||
        invalid_profile.h2d_ms != 0.0 ||
        invalid_profile.gpu_preprocess_ms != 0.0 ||
        invalid_profile.execution_ms != 0.0 ||
        invalid_profile.d2h_ms != 0.0 ||
        invalid_profile.postprocess_ms != 0.0 ||
        invalid_profile.total_ms != 0.0 ||
        invalid_profile.output_fingerprint != 0 ||
        invalid_profile.explicit_device_copy ||
        invalid_profile.gpu_preprocess ||
        invalid_profile.input_upload_bytes != 0) {
        std::cerr << "非法输入未清空上一帧耗时或诊断数据\n";
        return 1;
    }

    std::cout << "真实模型测试通过：input="
              << detector.input_width() << 'x' << detector.input_height()
              << ", provider=" << detector.backend_name()
              << ", output_format="
              << output_format_name(config.output_format)
              << ", load_ms="
              << std::chrono::duration<double, std::milli>(
                     load_finished - load_start).count()
              << ", detections=" << black_detections.size()
              << ", total_ms=" << black_profile.total_ms
              << ", gpu_preprocess=" << black_profile.gpu_preprocess
              << ", upload_bytes=" << black_profile.input_upload_bytes
              << ", black_fingerprint=" << black_profile.output_fingerprint
              << ", comparison_fingerprint="
              << comparison_profile.output_fingerprint
              << ", comparison_source=" << comparison_source
              << '\n';

    if (options.has_video_directory) {
        // 指纹会额外遍历完整输出张量，不能污染性能基准。先释放冒烟 Session，
        // 再用相同配置但关闭指纹创建正式基准实例。
        detector.reset();
        config.enable_output_fingerprint = false;
        Detector benchmark_detector(config);
        if (!benchmark_detector.load()) {
            std::cerr << "视频基准模型加载失败：" << config.model_path << '\n';
            return 1;
        }
        if (benchmark_detector.backend_name() != expected_provider) {
            std::cerr << "视频基准后端不符合请求："
                      << benchmark_detector.backend_name() << '\n';
            return 1;
        }
        const std::filesystem::path video_directory = options.video_directory;
        const std::filesystem::path report_path = options.report_path;
        const std::filesystem::path visibility_directory =
            options.visibility_directory;
        const std::filesystem::path aim_annotation_directory =
            options.aim_annotation_directory;
        const std::string& input_mode = options.input_mode;
        if (input_mode != "center" && input_mode != "full") {
            std::cerr << "未知视频输入模式：" << input_mode << '\n';
            return 2;
        }
        if (!benchmark_videos(
                benchmark_detector, video_directory, report_path,
                input_mode == "center", visibility_directory,
                !options.visibility_directory.empty(),
                aim_annotation_directory,
                !options.aim_annotation_directory.empty(),
                options.enable_aim_continuity ||
                    !options.aim_annotation_directory.empty())) {
            return 1;
        }
    }
    return 0;
}
