#include "detector/detector.h"
#include "log/log.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <sstream>
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

bool validate_masks(const SegmentationResult& result,
                    int image_width,
                    int image_height,
                    std::uint64_t& positive_pixels) noexcept {
    positive_pixels = 0;
    if (result.detections.empty() ||
        result.masks.size() != result.detections.size() ||
        !result.mask_pixels) {
        return false;
    }

    std::size_t expected_offset = 0;
    for (std::size_t index = 0; index < result.masks.size(); ++index) {
        const InstanceMask& mask = result.masks[index];
        if (mask.x < 0 || mask.y < 0 || mask.width <= 0 ||
            mask.height <= 0 || mask.x + mask.width > image_width ||
            mask.y + mask.height > image_height ||
            mask.data_offset != expected_offset ||
            mask.row_stride != static_cast<std::size_t>(mask.width)) {
            return false;
        }

        std::uint64_t instance_positive_pixels = 0;
        for (int row = 0; row < mask.height; ++row) {
            const std::uint8_t* pixels = result.mask_row(index, row);
            if (!pixels) return false;
            for (int column = 0; column < mask.width; ++column) {
                if (pixels[column] > 1U) return false;
                instance_positive_pixels += pixels[column];
            }
        }
        // 标准 Ultralytics 结果会丢弃没有任何正像素的实例。Xen 保留框/掩码
        // 索引一致性，但真实 COCO 冒烟图上的每个结果仍应包含有效区域。
        if (instance_positive_pixels == 0) return false;
        positive_pixels += instance_positive_pixels;
        expected_offset += static_cast<std::size_t>(mask.width) *
            static_cast<std::size_t>(mask.height);
    }
    return expected_offset == result.mask_pixels->size();
}

bool dump_result(const SegmentationResult& result,
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
        for (std::size_t index = 0; index < result.masks.size(); ++index) {
            std::ostringstream name;
            name << "mask_";
            name.width(3);
            name.fill('0');
            name << index << ".png";

            cv::Mat full_mask(
                image_height, image_width, CV_8UC1, cv::Scalar(0));
            const InstanceMask& mask = result.masks[index];
            for (int row = 0; row < mask.height; ++row) {
                const std::uint8_t* source = result.mask_row(index, row);
                std::uint8_t* destination =
                    full_mask.ptr<std::uint8_t>(mask.y + row) + mask.x;
                for (int column = 0; column < mask.width; ++column) {
                    destination[column] = source[column] != 0U ? 255U : 0U;
                }
            }
            if (!cv::imwrite(
                    (directory / name.str()).string(), full_mask)) {
                return false;
            }

            const Detection& detection = result.detections[index];
            report << "{";
            report << "class_id" << detection.class_id;
            report << "confidence" << detection.confidence;
            report << "x1" << detection.x1;
            report << "y1" << detection.y1;
            report << "x2" << detection.x2;
            report << "y2" << detection.y2;
            report << "mask_file" << name.str();
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
        std::cerr << "用法：segmentation_model_test <模型路径> <真实图像路径> "
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
            // 向后兼容仅给出诊断目录的调用形式。
            dump_directory = argv[3];
        } else {
            std::cerr << "未知推理后端：" << option << '\n';
            return 2;
        }
    }
    if (argc == 5) dump_directory = argv[4];

    Detector detector(config);
    if (!detector.load() || !detector.segmentation_supported() ||
        detector.backend_name() != BackendName(config.backend)) {
        std::cerr << "真实分割模型加载或任务识别失败\n";
        return 1;
    }

    const cv::Mat image = cv::imread(argv[2], cv::IMREAD_COLOR);
    if (image.empty()) {
        std::cerr << "无法读取真实分割测试图像\n";
        return 1;
    }

    const auto boxes_only = detector.detect(image);
    const InferenceProfile detection_profile = detector.profile();
    if (detection_profile.status != DetectionStatus::SUCCESS) {
        std::cerr << "分割模型仅框路径失败，status="
                  << DetectionStatusName(detection_profile.status) << '\n';
        return 1;
    }

    const SegmentationResult segmentation = detector.segment(image);
    const InferenceProfile segmentation_profile = detector.profile();
    if (segmentation_profile.status != DetectionStatus::SUCCESS ||
        !detections_match(boxes_only, segmentation.detections) ||
        detection_profile.output_fingerprint !=
            segmentation_profile.output_fingerprint) {
        std::cerr << "detect()/segment() 框结果或原始输出不一致\n";
        return 1;
    }

    std::uint64_t positive_pixels = 0;
    if (!validate_masks(
            segmentation, image.cols, image.rows, positive_pixels)) {
        std::cerr << "真实分割掩码几何、布局或像素契约无效\n";
        return 1;
    }
    if (dump_directory &&
        (dump_directory[0] == '\0' ||
         !dump_result(segmentation, image.cols, image.rows,
                      dump_directory))) {
        std::cerr << "实例分割诊断产物写入失败\n";
        return 1;
    }

    const SegmentationResult copied = segmentation;
    if (!copied.mask_pixels ||
        copied.mask_pixels.get() != segmentation.mask_pixels.get()) {
        std::cerr << "复制分割结果时发生了掩码缓冲深拷贝\n";
        return 1;
    }

    const cv::Mat black(image.rows, image.cols, CV_8UC3,
                        cv::Scalar(0, 0, 0));
    (void)detector.segment(black);
    const InferenceProfile black_profile = detector.profile();
    if (black_profile.status != DetectionStatus::SUCCESS ||
        black_profile.output_fingerprint ==
            segmentation_profile.output_fingerprint) {
        std::cerr << "变化输入没有传播到分割双输出\n";
        return 1;
    }

    (void)detector.segment(cv::Mat{});
    if (detector.profile().status != DetectionStatus::INVALID_INPUT) {
        std::cerr << "分割入口非法输入状态错误\n";
        return 1;
    }

    std::cout << "真实分割模型测试通过：input="
              << detector.input_width() << 'x' << detector.input_height()
              << ", provider=" << detector.backend_name()
              << ", image=" << image.cols << 'x' << image.rows
              << ", instances=" << segmentation.detections.size()
              << ", mask_bytes=" << segmentation.mask_pixels->size()
              << ", positive_pixels=" << positive_pixels
              << ", detect_post_ms=" << detection_profile.postprocess_ms
              << ", segment_post_ms="
              << segmentation_profile.postprocess_ms << '\n';
    return 0;
}
