#include "detector/detector.h"
#include "log/log.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <iostream>
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

bool validate_obbs(const ObbResult& result,
                   int image_width,
                   int image_height) noexcept {
    if (result.detections.empty() ||
        result.detections.size() != result.oriented_detections.size()) {
        return false;
    }
    for (std::size_t index = 0;
         index < result.detections.size(); ++index) {
        const Detection& envelope = result.detections[index];
        const OrientedDetection& oriented =
            result.oriented_detections[index];
        if (!std::isfinite(oriented.center_x) ||
            !std::isfinite(oriented.center_y) ||
            !std::isfinite(oriented.width) ||
            !std::isfinite(oriented.height) ||
            !std::isfinite(oriented.angle_radians) ||
            !std::isfinite(oriented.confidence) ||
            oriented.width <= 0.0f || oriented.height <= 0.0f ||
            oriented.confidence < 0.0f ||
            oriented.confidence > 1.0f || oriented.class_id < 0 ||
            envelope.class_id != oriented.class_id ||
            std::fabs(envelope.confidence - oriented.confidence) > 1e-6f ||
            envelope.x1 < 0.0f || envelope.y1 < 0.0f ||
            envelope.x2 > image_width || envelope.y2 > image_height ||
            envelope.x2 <= envelope.x1 || envelope.y2 <= envelope.y1) {
            return false;
        }
    }
    return true;
}

bool dump_result(const ObbResult& result,
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
        report << "instances" << "[";
        for (std::size_t index = 0;
             index < result.detections.size(); ++index) {
            const Detection& envelope = result.detections[index];
            const OrientedDetection& oriented =
                result.oriented_detections[index];
            report << "{";
            report << "class_id" << oriented.class_id;
            report << "confidence" << oriented.confidence;
            report << "center_x" << oriented.center_x;
            report << "center_y" << oriented.center_y;
            report << "width" << oriented.width;
            report << "height" << oriented.height;
            report << "angle_radians" << oriented.angle_radians;
            report << "envelope_x1" << envelope.x1;
            report << "envelope_y1" << envelope.y1;
            report << "envelope_x2" << envelope.x2;
            report << "envelope_y2" << envelope.y2;
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
        std::cerr << "用法：obb_model_test <模型路径> <真实图像路径> "
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
            dump_directory = argv[3];
        } else {
            std::cerr << "未知推理后端：" << option << '\n';
            return 2;
        }
    }
    if (argc == 5) dump_directory = argv[4];

    Detector detector(config);
    if (!detector.load() || !detector.obb_supported() ||
        detector.backend_name() != BackendName(config.backend)) {
        std::cerr << "真实 OBB 模型加载或任务识别失败\n";
        return 1;
    }

    const cv::Mat image = cv::imread(argv[2], cv::IMREAD_COLOR);
    if (image.empty()) {
        std::cerr << "无法读取真实 OBB 测试图像\n";
        return 1;
    }

    const auto boxes_only = detector.detect(image);
    const InferenceProfile detection_profile = detector.profile();
    if (detection_profile.status != DetectionStatus::SUCCESS) {
        std::cerr << "OBB 模型仅框路径失败，status="
                  << DetectionStatusName(detection_profile.status) << '\n';
        return 1;
    }

    const ObbResult obb = detector.obb(image);
    const InferenceProfile obb_profile = detector.profile();
    if (obb_profile.status != DetectionStatus::SUCCESS ||
        !detections_match(boxes_only, obb.detections) ||
        detection_profile.output_fingerprint !=
            obb_profile.output_fingerprint ||
        !validate_obbs(obb, image.cols, image.rows)) {
        std::cerr << "detect()/obb() 框、原始输出或旋转框契约不一致\n";
        return 1;
    }
    if (dump_directory &&
        (dump_directory[0] == '\0' ||
         !dump_result(obb, image.cols, image.rows, dump_directory))) {
        std::cerr << "OBB 诊断产物写入失败\n";
        return 1;
    }

    const cv::Mat black(image.rows, image.cols, CV_8UC3,
                        cv::Scalar(0, 0, 0));
    (void)detector.obb(black);
    const InferenceProfile black_profile = detector.profile();
    if (black_profile.status != DetectionStatus::SUCCESS ||
        black_profile.output_fingerprint == obb_profile.output_fingerprint) {
        std::cerr << "变化输入没有传播到 OBB 输出\n";
        return 1;
    }

    (void)detector.obb(cv::Mat{});
    if (detector.profile().status != DetectionStatus::INVALID_INPUT) {
        std::cerr << "OBB 入口非法输入状态错误\n";
        return 1;
    }

    std::cout << "真实 OBB 模型测试通过：input="
              << detector.input_width() << 'x' << detector.input_height()
              << ", provider=" << detector.backend_name()
              << ", image=" << image.cols << 'x' << image.rows
              << ", instances=" << obb.oriented_detections.size()
              << ", detect_post_ms=" << detection_profile.postprocess_ms
              << ", obb_post_ms=" << obb_profile.postprocess_ms << '\n';
    return 0;
}
