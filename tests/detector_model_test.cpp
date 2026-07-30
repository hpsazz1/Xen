#include "detector/detector.h"
#include "log/log.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
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
    std::vector<double> preprocess_ms;
    std::vector<double> inference_ms;
    std::vector<double> h2d_ms;
    std::vector<double> execution_ms;
    std::vector<double> d2h_ms;
    std::vector<double> postprocess_ms;
    std::vector<double> total_ms;
};

std::string path_to_utf8(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

bool has_mp4_extension(const std::filesystem::path& path) {
    std::string extension = path_to_utf8(path.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return extension == ".mp4";
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

    const cv::Mat warmup_frame = select_video_input(
        frame, detector, use_center_crop);
    result.evaluated_width = warmup_frame.cols;
    result.evaluated_height = warmup_frame.rows;

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
    while (capture.read(frame)) {
        if (frame.empty()) continue;

        // 游戏实时链路通常只采集准星附近 FOV。全屏录像必须在推理前恢复同样的
        // 中心 ROI，否则把 2560x1440 压到 320x320 会把人物缩小约八倍。
        const cv::Mat evaluated_frame = select_video_input(
            frame, detector, use_center_crop);
        if (evaluated_frame.cols != result.evaluated_width ||
            evaluated_frame.rows != result.evaluated_height) {
            std::cerr << "视频帧尺寸在基准期间发生变化：" << open_path << '\n';
            return false;
        }

        const auto detections = detector.detect(evaluated_frame);
        const InferenceProfile profile = detector.profile();
        ++result.frame_count;

        // 空检测是合法结果；pipeline 状态是区分零检测与执行失败的唯一依据。
        if (profile.status != DetectionStatus::SUCCESS) {
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
        result.execution_ms.push_back(profile.execution_ms);
        result.d2h_ms.push_back(profile.d2h_ms);
        result.postprocess_ms.push_back(profile.postprocess_ms);
        result.total_ms.push_back(profile.total_ms);
        result.explicit_device_copy =
            result.explicit_device_copy || profile.explicit_device_copy;
        result.detection_count_sum += detections.size();

        if (detections.empty()) {
            ++current_empty_sequence;
            result.longest_empty_sequence = std::max(
                result.longest_empty_sequence, current_empty_sequence);
            continue;
        }

        current_empty_sequence = 0;
        ++result.detected_frame_count;
        const auto best = std::max_element(
            detections.begin(), detections.end(),
            [](const Detection& left, const Detection& right) {
                return left.confidence < right.confidence;
            });
        result.best_confidence_sum += best->confidence;
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

    output << csv_escape(result.scene) << ','
           << result.source_width << ',' << result.source_height << ','
           << result.evaluated_width << ',' << result.evaluated_height << ','
           << result.model_input_width << ',' << result.model_input_height << ','
           << result.source_fps << ',' << result.frame_count << ','
           << result.failed_frame_count << ','
           << DetectionStatusName(result.status) << ','
           << (result.explicit_device_copy ? 1 : 0) << ','
           << result.detected_frame_count
           << ',' << detection_rate << ',' << result.longest_empty_sequence
           << ',' << mean_detection_count << ',' << mean_best_confidence
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
                      bool use_center_crop) {
    std::error_code error;
    if (!std::filesystem::is_directory(video_directory, error)) {
        std::cerr << "视频目录不存在："
                  << path_to_utf8(video_directory) << '\n';
        return false;
    }

    std::vector<std::filesystem::path> video_paths;
    for (std::filesystem::directory_iterator iterator(video_directory, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (iterator->is_regular_file() && has_mp4_extension(iterator->path())) {
            video_paths.push_back(iterator->path());
        }
    }
    if (error || video_paths.empty()) {
        std::cerr << "视频目录中没有可用 MP4："
                  << path_to_utf8(video_directory) << '\n';
        return false;
    }
    std::sort(video_paths.begin(), video_paths.end());

    if (!report_path.parent_path().empty()) {
        std::filesystem::create_directories(report_path.parent_path(), error);
        if (error) {
            std::cerr << "无法创建报告目录：" << error.message() << '\n';
            return false;
        }
    }
    std::filesystem::path temporary_report_path = report_path;
    temporary_report_path += ".tmp";
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
              "explicit_device_copy,"
              "detected_frames,detection_frame_rate,longest_empty_sequence,"
              "mean_detection_count,mean_best_confidence,"
              "preprocess_mean_ms,preprocess_p50_ms,preprocess_p95_ms,"
              "preprocess_p99_ms,inference_mean_ms,inference_p50_ms,"
              "inference_p95_ms,inference_p99_ms,"
              "h2d_mean_ms,h2d_p50_ms,h2d_p95_ms,h2d_p99_ms,"
              "execution_mean_ms,execution_p50_ms,execution_p95_ms,"
              "execution_p99_ms,d2h_mean_ms,d2h_p50_ms,d2h_p95_ms,"
              "d2h_p99_ms,postprocess_mean_ms,"
              "postprocess_p50_ms,postprocess_p95_ms,postprocess_p99_ms,"
              "total_mean_ms,total_p50_ms,total_p95_ms,total_p99_ms\n";
    report << std::fixed << std::setprecision(4);

    for (const auto& video_path : video_paths) {
        VideoBenchmarkResult result;
        std::cout << "开始测试场景：" << path_to_utf8(video_path.filename())
                  << std::endl;
        if (!benchmark_video(
                detector, video_path, use_center_crop, result)) return false;
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
                  << ", total_p50=" << percentile(result.total_ms, 0.50)
                  << "ms, total_p95=" << percentile(result.total_ms, 0.95)
                  << "ms, total_p99=" << percentile(result.total_ms, 0.99)
                  << "ms" << std::endl;
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
    if (argc < 2 || argc > 7 || !argv[1] || argv[1][0] == '\0') {
        std::cerr << "用法：detector_model_test <模型路径> "
                     "[cpu|cuda|tensorrt|directml] [TensorRT缓存目录] "
                     "[视频目录] [CSV报告路径] [center|full]\n";
        return 2;
    }

    LogScope log_scope;

    DetectorConfig config;
    config.model_path = argv[1];
    const std::string requested_backend = argc >= 3 ? argv[2] : "cpu";
    if (requested_backend == "cpu") {
        config.backend = BackendType::CPU;
    } else if (requested_backend == "cuda") {
        config.backend = BackendType::CUDA;
    } else if (requested_backend == "tensorrt") {
        config.backend = BackendType::TENSORRT;
        config.enable_fp16 = true;
        if (argc >= 4) config.trt_cache_path = argv[3];
    } else if (requested_backend == "directml") {
        config.backend = BackendType::DIRECTML;
    } else {
        std::cerr << "未知后端：" << requested_backend << '\n';
        return 2;
    }
    config.output_format = OutputFormat::AUTO;
    config.enable_output_fingerprint = true;

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

    // 两种输入必须在同一 Session 连续执行。比较原始 float 输出而不是最终框，
    // 可覆盖 CUDA Graph 只重放首次设备输入、但两帧都恰好零检测的回归。
    cv::Mat black_image(detector.input_height(), detector.input_width(),
                        CV_8UC3, cv::Scalar(0, 0, 0));
    const auto black_detections = detector.detect(black_image);
    const InferenceProfile black_profile = detector.profile();
    if (black_profile.status != DetectionStatus::SUCCESS) {
        std::cerr << "黑图推理失败，status="
                  << DetectionStatusName(black_profile.status) << '\n';
        return 1;
    }

    cv::Mat white_image(detector.input_height(), detector.input_width(),
                        CV_8UC3, cv::Scalar(255, 255, 255));
    const auto white_detections = detector.detect(white_image);
    const InferenceProfile white_profile = detector.profile();
    if (white_profile.status != DetectionStatus::SUCCESS) {
        std::cerr << "白图推理失败，status="
                  << DetectionStatusName(white_profile.status) << '\n';
        return 1;
    }
    if (requested_backend == "tensorrt" &&
        config.enable_trt_cuda_graph &&
        (!black_profile.explicit_device_copy ||
         !white_profile.explicit_device_copy)) {
        std::cerr << "TensorRT CUDA Graph 未走固定设备缓冲显式复制路径\n";
        return 1;
    }
    if (black_profile.output_fingerprint ==
        white_profile.output_fingerprint) {
        std::cerr << "黑白输入的原始输出指纹相同，可能重放了首次输入\n";
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
        const auto no_graph_white = no_graph_detector.detect(white_image);
        const InferenceProfile no_graph_white_profile =
            no_graph_detector.profile();
        if (no_graph_black_profile.status != DetectionStatus::SUCCESS ||
            no_graph_white_profile.status != DetectionStatus::SUCCESS ||
            no_graph_black_profile.explicit_device_copy ||
            no_graph_white_profile.explicit_device_copy) {
            std::cerr << "TensorRT Graph off 对照执行状态错误\n";
            return 1;
        }
        if (black_profile.output_fingerprint !=
                no_graph_black_profile.output_fingerprint ||
            white_profile.output_fingerprint !=
                no_graph_white_profile.output_fingerprint ||
            !detections_match(black_detections, no_graph_black) ||
            !detections_match(white_detections, no_graph_white)) {
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
        invalid_profile.execution_ms != 0.0 ||
        invalid_profile.d2h_ms != 0.0 ||
        invalid_profile.postprocess_ms != 0.0 ||
        invalid_profile.total_ms != 0.0 ||
        invalid_profile.output_fingerprint != 0 ||
        invalid_profile.explicit_device_copy) {
        std::cerr << "非法输入未清空上一帧耗时或诊断数据\n";
        return 1;
    }

    std::cout << "真实模型测试通过：input="
              << detector.input_width() << 'x' << detector.input_height()
              << ", provider=" << detector.backend_name()
              << ", load_ms="
              << std::chrono::duration<double, std::milli>(
                     load_finished - load_start).count()
              << ", detections=" << black_detections.size()
              << ", total_ms=" << black_profile.total_ms
              << ", black_fingerprint=" << black_profile.output_fingerprint
              << ", white_fingerprint=" << white_profile.output_fingerprint
              << '\n';

    if (argc >= 5) {
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
        const std::filesystem::path video_directory = argv[4];
        const std::filesystem::path report_path = argc >= 6
            ? std::filesystem::path(argv[5])
            : std::filesystem::path("cache/benchmarks/detector-videos.csv");
        const std::string input_mode = argc >= 7 ? argv[6] : "center";
        if (input_mode != "center" && input_mode != "full") {
            std::cerr << "未知视频输入模式：" << input_mode << '\n';
            return 2;
        }
        if (!benchmark_videos(benchmark_detector, video_directory, report_path,
                              input_mode == "center")) {
            return 1;
        }
    }
    return 0;
}
