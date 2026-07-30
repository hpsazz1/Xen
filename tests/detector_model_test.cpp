#include "detector/detector.h"

#include <opencv2/core.hpp>

#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    if (argc != 2 || !argv[1] || argv[1][0] == '\0') {
        std::cerr << "用法：detector_model_test <模型路径>\n";
        return 2;
    }

    DetectorConfig config;
    config.model_path = argv[1];
    // 集成测试验证模型契约而非本机 GPU 环境，固定使用可移植的 CPU EP。
    config.backend = BackendType::CPU;
    config.output_format = OutputFormat::AUTO;

    Detector detector(config);
    if (!detector.load()) {
        std::cerr << "真实模型加载失败：" << config.model_path << '\n';
        return 1;
    }
    if (detector.input_width() <= 0 || detector.input_height() <= 0) {
        std::cerr << "模型输入尺寸无效\n";
        return 1;
    }
    if (detector.backend_name() != "CPUExecutionProvider") {
        std::cerr << "集成测试未使用 CPU EP\n";
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
              << ", detections=" << detections.size()
              << ", total_ms=" << profile.total_ms << '\n';
    return 0;
}
