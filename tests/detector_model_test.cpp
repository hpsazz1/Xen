#include "detector/detector.h"
#include "log/log.h"

#include <opencv2/core.hpp>

#include <chrono>
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

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2 || argc > 4 || !argv[1] || argv[1][0] == '\0') {
        std::cerr << "用法：detector_model_test <模型路径> "
                     "[cpu|tensorrt] [TensorRT缓存目录]\n";
        return 2;
    }

    LogScope log_scope;

    DetectorConfig config;
    config.model_path = argv[1];
    const std::string requested_backend = argc >= 3 ? argv[2] : "cpu";
    if (requested_backend == "cpu") {
        config.backend = BackendType::CPU;
    } else if (requested_backend == "tensorrt") {
        config.backend = BackendType::TENSORRT;
        config.enable_fp16 = true;
        if (argc == 4) config.trt_cache_path = argv[3];
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
    const std::string expected_provider = requested_backend == "tensorrt"
        ? "TensorrtExecutionProvider"
        : "CPUExecutionProvider";
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
    return 0;
}
