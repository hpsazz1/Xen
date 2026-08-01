#include "detector/detector.h"
#include "log/log.h"

#include <algorithm>
#include <cctype>
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

bool parse_device(std::string value, OpenVinoDevice& device) noexcept {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    if (value == "cpu") device = OpenVinoDevice::CPU;
    else if (value == "gpu") device = OpenVinoDevice::GPU;
    else if (value == "npu") device = OpenVinoDevice::NPU;
    else return false;
    return true;
}

bool run_test(const std::string& model_path,
              OpenVinoDevice device,
              const std::string& profile_prefix) {
    try {
        const std::filesystem::path prefix(profile_prefix);
        const auto parent = prefix.parent_path();
        if (!parent.empty()) {
            std::error_code error;
            std::filesystem::create_directories(parent, error);
            if (error) {
                std::cerr << "无法创建 ORT profile 目录: "
                          << error.message() << '\n';
                return false;
            }
        }

        DetectorConfig config;
        config.model_path = model_path;
        config.backend = BackendType::OPENVINO;
        config.openvino_device = device;
        config.device_id = 0;
        config.enable_output_fingerprint = true;
        config.enable_ort_profiling = true;
        config.ort_profile_prefix = profile_prefix;

        Detector detector(config);
        if (!detector.load()) {
            std::cerr << "OpenVINO Detector 加载失败，目标设备="
                      << OpenVinoDeviceName(device) << '\n';
            return false;
        }
        if (detector.backend_name() != "OpenVINOExecutionProvider") {
            std::cerr << "实际 Provider 不符合请求: "
                      << detector.backend_name() << '\n';
            return false;
        }

        // 固定 shape 下连续提交内容完全不同的帧，验证同一 Session 没有
        // 重放旧输入；原始输出指纹比检测数量更能覆盖合法空结果场景。
        const cv::Mat black(
            detector.input_height(), detector.input_width(), CV_8UC3,
            cv::Scalar(0, 0, 0));
        const cv::Mat white(
            detector.input_height(), detector.input_width(), CV_8UC3,
            cv::Scalar(255, 255, 255));
        detector.detect(black);
        const InferenceProfile black_profile = detector.profile();
        detector.detect(white);
        const InferenceProfile white_profile = detector.profile();
        if (black_profile.status != DetectionStatus::SUCCESS ||
            white_profile.status != DetectionStatus::SUCCESS ||
            black_profile.output_fingerprint == 0 ||
            white_profile.output_fingerprint == 0 ||
            black_profile.output_fingerprint ==
                white_profile.output_fingerprint) {
            std::cerr << "OpenVINO 变化输入回归失败: first="
                      << black_profile.output_fingerprint << ", second="
                      << white_profile.output_fingerprint << '\n';
            return false;
        }

        detector.detect(cv::Mat{});
        if (detector.profile().status != DetectionStatus::INVALID_INPUT) {
            std::cerr << "非法输入没有与合法空检测结果分离。\n";
            return false;
        }

        std::string generated_profile;
        if (!detector.end_profiling(generated_profile) ||
            generated_profile.empty() ||
            !std::filesystem::is_regular_file(generated_profile) ||
            std::filesystem::file_size(generated_profile) == 0) {
            std::cerr << "OpenVINO ORT profile 未完整生成。\n";
            return false;
        }
        std::cout << "provider_profile=" << generated_profile << '\n';
        std::cout << "provider=OpenVINOExecutionProvider, device="
                  << OpenVinoDeviceName(device) << ", fingerprints="
                  << black_profile.output_fingerprint << ','
                  << white_profile.output_fingerprint << '\n';
        return true;
    } catch (const std::exception& exception) {
        std::cerr << "OpenVINO 真实模型测试异常: "
                  << exception.what() << '\n';
        return false;
    } catch (...) {
        std::cerr << "OpenVINO 真实模型测试发生未知异常。\n";
        return false;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4 || !argv[1] || !argv[2] || !argv[3] ||
        argv[1][0] == '\0' || argv[2][0] == '\0' || argv[3][0] == '\0') {
        std::cerr << "用法: openvino_model_test <model.onnx> "
                     "<cpu|gpu|npu> <profile-prefix>\n";
        return 2;
    }

    OpenVinoDevice device = OpenVinoDevice::CPU;
    if (!parse_device(argv[2], device)) {
        std::cerr << "OpenVINO 设备必须是 cpu/gpu/npu。\n";
        return 2;
    }

    LogScope log_scope;
    if (!Log::initialized()) {
        std::cerr << "Log 初始化失败。\n";
        return 2;
    }
    return run_test(argv[1], device, argv[3]) ? 0 : 1;
}
