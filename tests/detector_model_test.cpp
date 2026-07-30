#include "detector/detector.h"
#include "log/log.h"

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
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
    double source_fps = 0.0;
    std::size_t frame_count = 0;
    std::size_t failed_frame_count = 0;
    std::size_t detected_frame_count = 0;
    std::size_t longest_empty_sequence = 0;
    std::size_t detection_count_sum = 0;
    double best_confidence_sum = 0.0;
    std::vector<double> preprocess_ms;
    std::vector<double> inference_ms;
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

    cv::Mat frame;
    if (!capture.read(frame) || frame.empty()) {
        std::cerr << "视频没有可读取帧：" << open_path << '\n';
        return false;
    }

    // 固定重复第一帧只用于预热 Session、GPU 时钟和缓存，不计入正式样本。
    constexpr int kWarmupFrames = 50;
    for (int index = 0; index < kWarmupFrames; ++index) {
        detector.detect(frame);
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

        cv::Mat evaluated_frame = frame;
        if (use_center_crop &&
            frame.cols >= detector.input_width() &&
            frame.rows >= detector.input_height()) {
            // 游戏实时链路通常只采集准星附近 FOV。全屏录像必须在推理前恢复同样的
            // 中心 ROI，否则把 2560x1440 压到 320x320 会把人物缩小约八倍。
            const int left = (frame.cols - detector.input_width()) / 2;
            const int top = (frame.rows - detector.input_height()) / 2;
            evaluated_frame = frame(cv::Rect(
                left, top, detector.input_width(), detector.input_height()));
        }
        result.evaluated_width = evaluated_frame.cols;
        result.evaluated_height = evaluated_frame.rows;

        const auto detections = detector.detect(evaluated_frame);
        const InferenceProfile profile = detector.profile();
        ++result.frame_count;

        // 空检测是合法结果；只有没有生成完整耗时才视为本帧推理失败。
        if (profile.total_ms <= 0.0 || profile.inference_ms <= 0.0) {
            ++result.failed_frame_count;
            continue;
        }

        result.preprocess_ms.push_back(profile.preprocess_ms);
        result.inference_ms.push_back(profile.inference_ms);
        result.postprocess_ms.push_back(profile.postprocess_ms);
        result.total_ms.push_back(profile.total_ms);
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
           << result.source_fps << ',' << result.frame_count << ','
           << result.failed_frame_count << ',' << result.detected_frame_count
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
    std::ofstream report(report_path, std::ios::binary | std::ios::trunc);
    if (!report) {
        std::cerr << "无法创建报告：" << path_to_utf8(report_path) << '\n';
        return false;
    }

    // 写入 UTF-8 BOM，保证 Windows Excel 直接打开时中文场景名不乱码。
    report << "\xEF\xBB\xBF";
    report << "scene,source_width,source_height,evaluated_width,"
              "evaluated_height,source_fps,frames,failed_frames,"
              "detected_frames,detection_frame_rate,longest_empty_sequence,"
              "mean_detection_count,mean_best_confidence,"
              "preprocess_mean_ms,preprocess_p50_ms,preprocess_p95_ms,"
              "preprocess_p99_ms,inference_mean_ms,inference_p50_ms,"
              "inference_p95_ms,inference_p99_ms,postprocess_mean_ms,"
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

        const double valid_frames = static_cast<double>(result.total_ms.size());
        const double detection_rate = valid_frames > 0.0
            ? 100.0 * static_cast<double>(result.detected_frame_count) /
                  valid_frames
            : 0.0;
        std::cout << std::fixed << std::setprecision(2)
                  << "场景完成：frames=" << result.frame_count
                  << ", detected=" << detection_rate << "%"
                  << ", longest_empty=" << result.longest_empty_sequence
                  << ", total_p50=" << percentile(result.total_ms, 0.50)
                  << "ms, total_p95=" << percentile(result.total_ms, 0.95)
                  << "ms, total_p99=" << percentile(result.total_ms, 0.99)
                  << "ms" << std::endl;
    }

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

    // 黑色合成图不要求检出目标；本测试关注完整 pipeline 能否安全运行并
    // 识别实际输出契约，空检测结果同样是合法结果。
    cv::Mat image(detector.input_height(), detector.input_width(),
                  CV_8UC3, cv::Scalar(0, 0, 0));
    const auto detections = detector.detect(image);
    const auto& profile = detector.profile();
    if (profile.total_ms <= 0.0 || profile.inference_ms <= 0.0) {
        std::cerr << "推理未生成有效耗时数据\n";
        return 1;
    }

    std::cout << "真实模型测试通过：input="
              << detector.input_width() << 'x' << detector.input_height()
              << ", provider=" << detector.backend_name()
              << ", load_ms="
              << std::chrono::duration<double, std::milli>(
                     load_finished - load_start).count()
              << ", detections=" << detections.size()
              << ", total_ms=" << profile.total_ms << '\n';

    if (argc >= 5) {
        const std::filesystem::path video_directory = argv[4];
        const std::filesystem::path report_path = argc >= 6
            ? std::filesystem::path(argv[5])
            : std::filesystem::path("cache/benchmarks/detector-videos.csv");
        const std::string input_mode = argc >= 7 ? argv[6] : "center";
        if (input_mode != "center" && input_mode != "full") {
            std::cerr << "未知视频输入模式：" << input_mode << '\n';
            return 2;
        }
        if (!benchmark_videos(detector, video_directory, report_path,
                              input_mode == "center")) {
            return 1;
        }
    }
    return 0;
}
