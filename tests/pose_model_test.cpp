#include "detector/detector.h"
#include "log/log.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>

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

bool detections_match(const std::vector<Detection>& left,
                      const std::vector<Detection>& right) noexcept {
    constexpr float kCoordinateTolerance = 1e-3f;
    constexpr float kConfidenceTolerance = 1e-5f;
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const Detection& a = left[index];
        const Detection& b = right[index];
        if (a.class_id != b.class_id ||
            std::fabs(a.confidence - b.confidence) >
                kConfidenceTolerance ||
            std::fabs(a.x1 - b.x1) > kCoordinateTolerance ||
            std::fabs(a.y1 - b.y1) > kCoordinateTolerance ||
            std::fabs(a.x2 - b.x2) > kCoordinateTolerance ||
            std::fabs(a.y2 - b.y2) > kCoordinateTolerance) {
            return false;
        }
    }
    return true;
}

bool validate_pose(const PoseResult& result,
                   int image_width,
                   int image_height,
                   std::size_t& visible_keypoints) noexcept {
    visible_keypoints = 0;
    if (result.detections.empty() ||
        result.keypoints_per_detection == 0 ||
        (result.keypoint_dimensions != 2 &&
         result.keypoint_dimensions != 3) ||
        result.detections.size() >
            std::numeric_limits<std::size_t>::max() /
                result.keypoints_per_detection ||
        result.keypoints.size() !=
            result.detections.size() *
                result.keypoints_per_detection) {
        return false;
    }

    for (std::size_t instance = 0;
         instance < result.detections.size(); ++instance) {
        const PoseKeypoint* row = result.keypoint_row(instance);
        if (!row) return false;
        for (std::size_t index = 0;
             index < result.keypoints_per_detection; ++index) {
            const PoseKeypoint& keypoint = row[index];
            if (!std::isfinite(keypoint.x) ||
                !std::isfinite(keypoint.y) ||
                !std::isfinite(keypoint.confidence) ||
                keypoint.x < 0.0f || keypoint.x > image_width ||
                keypoint.y < 0.0f || keypoint.y > image_height ||
                keypoint.confidence < 0.0f ||
                keypoint.confidence > 1.0f) {
                return false;
            }
            if (keypoint.confidence >= 0.5f) ++visible_keypoints;
        }
    }
    return visible_keypoints > 0;
}

bool dump_result(const PoseResult& result,
                 int image_width,
                 int image_height,
                 const std::filesystem::path& directory) {
    try {
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) return false;

        cv::FileStorage report(
            (directory / "summary.json").string(),
            cv::FileStorage::WRITE | cv::FileStorage::FORMAT_JSON);
        if (!report.isOpened()) return false;
        report << "image_width" << image_width;
        report << "image_height" << image_height;
        report << "keypoints_per_detection"
               << static_cast<int>(result.keypoints_per_detection);
        report << "keypoint_dimensions" << result.keypoint_dimensions;
        report << "instances" << "[";
        for (std::size_t instance = 0;
             instance < result.detections.size(); ++instance) {
            const Detection& detection = result.detections[instance];
            report << "{";
            report << "class_id" << detection.class_id;
            report << "confidence" << detection.confidence;
            report << "x1" << detection.x1;
            report << "y1" << detection.y1;
            report << "x2" << detection.x2;
            report << "y2" << detection.y2;
            report << "keypoints" << "[";
            const PoseKeypoint* row = result.keypoint_row(instance);
            for (std::size_t index = 0;
                 index < result.keypoints_per_detection; ++index) {
                report << "{";
                report << "x" << row[index].x;
                report << "y" << row[index].y;
                report << "confidence" << row[index].confidence;
                report << "}";
            }
            report << "]";
            report << "}";
        }
        report << "]";
        report.release();
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 3 || argc > 5 || !argv[1] || !argv[2]) {
        std::cerr << "用法：pose_model_test <模型路径> <真实图像路径> "
                     "[cpu|cuda|tensorrt|directml] [诊断输出目录]\n";
        return 2;
    }

    LogScope log_scope;
    DetectorConfig config;
    config.model_path = argv[1];
    config.backend = BackendType::CPU;
    config.enable_output_fingerprint = true;
    const char* dump_directory = nullptr;
    if (argc >= 4 && argv[3]) {
        const std::string option = argv[3];
        if (option == "cpu") {
            config.backend = BackendType::CPU;
        } else if (option == "cuda") {
            config.backend = BackendType::CUDA;
        } else if (option == "tensorrt") {
            config.backend = BackendType::TENSORRT;
        } else if (option == "directml") {
            config.backend = BackendType::DIRECTML;
        } else if (argc == 4) {
            // 向后兼容只给出诊断目录的调用形式。
            dump_directory = argv[3];
        } else {
            std::cerr << "未知推理后端：" << option << '\n';
            return 2;
        }
    }
    if (argc == 5) dump_directory = argv[4];

    Detector detector(config);
    if (!detector.load() || !detector.pose_supported() ||
        detector.backend_name() != BackendName(config.backend)) {
        std::cerr << "真实姿态模型加载或任务识别失败\n";
        return 1;
    }

    const cv::Mat image = cv::imread(argv[2], cv::IMREAD_COLOR);
    if (image.empty()) {
        std::cerr << "无法读取真实姿态测试图像\n";
        return 1;
    }

    const auto boxes_only = detector.detect(image);
    const InferenceProfile detection_profile = detector.profile();
    if (detection_profile.status != DetectionStatus::SUCCESS) {
        std::cerr << "姿态模型仅框路径失败，status="
                  << DetectionStatusName(detection_profile.status) << '\n';
        return 1;
    }

    const PoseResult pose = detector.pose(image);
    const InferenceProfile pose_profile = detector.profile();
    if (pose_profile.status != DetectionStatus::SUCCESS ||
        !detections_match(boxes_only, pose.detections) ||
        detection_profile.output_fingerprint !=
            pose_profile.output_fingerprint) {
        std::cerr << "detect()/pose() 框结果或原始输出不一致\n";
        return 1;
    }

    std::size_t visible_keypoints = 0;
    if (!validate_pose(
            pose, image.cols, image.rows, visible_keypoints)) {
        std::cerr << "真实姿态关键点数量、布局、坐标或置信度契约无效\n";
        return 1;
    }
    if (dump_directory &&
        (dump_directory[0] == '\0' ||
         !dump_result(pose, image.cols, image.rows, dump_directory))) {
        std::cerr << "姿态诊断产物写入失败\n";
        return 1;
    }

    const cv::Mat black(image.rows, image.cols, CV_8UC3,
                        cv::Scalar(0, 0, 0));
    (void)detector.pose(black);
    const InferenceProfile black_profile = detector.profile();
    if (black_profile.status != DetectionStatus::SUCCESS ||
        black_profile.output_fingerprint == pose_profile.output_fingerprint) {
        std::cerr << "变化输入没有传播到姿态输出\n";
        return 1;
    }

    (void)detector.pose(cv::Mat{});
    if (detector.profile().status != DetectionStatus::INVALID_INPUT) {
        std::cerr << "姿态入口非法输入状态错误\n";
        return 1;
    }

    std::cout << "真实姿态模型测试通过：input="
              << detector.input_width() << 'x' << detector.input_height()
              << ", provider=" << detector.backend_name()
              << ", image=" << image.cols << 'x' << image.rows
              << ", instances=" << pose.detections.size()
              << ", keypoints_per_instance="
              << pose.keypoints_per_detection
              << ", visible_keypoints=" << visible_keypoints
              << ", detect_post_ms=" << detection_profile.postprocess_ms
              << ", pose_post_ms=" << pose_profile.postprocess_ms << '\n';
    return 0;
}
