#include "detector/detector.h"
#include "platform/filesystem_path_internal.h"
#include "log/log.h"
#include <clocale>
#include <chrono>
#include <string_view>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
int failures = 0;
void expect(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "失败：" << message << '\n'; }
}
}

int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) return 2;
    std::cout << std::unitbuf;
    if (argc == 3 && std::string_view(argv[2]) == "--existing-thread-locale") {
        _configthreadlocale(_ENABLE_PER_THREAD_LOCALE);
        if (!std::setlocale(LC_CTYPE, ".UTF8")) return 3;
    }
    const std::string original_locale = std::setlocale(LC_CTYPE, nullptr);
    const int original_mode = _configthreadlocale(0);
    const auto locale_preserved = [&] {
        expect(original_locale == std::setlocale(LC_CTYPE, nullptr) &&
                   original_mode == _configthreadlocale(0), "模型加载返回后保持调用线程原字符locale及共享模式");
    };
    LogConfig logging;
    logging.enable_console = logging.enable_file = logging.enable_debug_file = false;
    Log::init(logging);
    try {
        const auto source = std::filesystem::u8path(argv[1]);
        const auto root = std::filesystem::temp_directory_path() /
            ("xen-external-model-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())) /
            (argc == 3 && std::string_view(argv[2]) == "--ascii" ? L"plain" : L"模型-\U0001F680");
        std::filesystem::create_directories(root);
        const auto model = root / source.filename();
        const auto weights = root / "detector_identity_weights.bin";
        std::filesystem::copy_file(source, model);
        std::filesystem::copy_file(source.parent_path() / weights.filename(), weights);
        DetectorConfig config;
        config.backend = BackendType::CPU;
        config.model_path = xen::filesystem_detail::path_utf8(model);
        config.output_format = OutputFormat::CHANNEL_FIRST;
        config.conf_threshold = .5f;
        Detector detector(config);
        expect(detector.model_sha256().empty(), "未加载模型没有字节身份");
        std::cout << "load original " << config.model_path << '\n';
        if (!detector.load()) throw std::runtime_error("Unicode目录的外置权重模型应可加载");
        locale_preserved();
        constexpr auto expected_sha = "3bb6bf8d936d2574cbf693a1ef67b4205afe88eca38442a49bcf552db760651b";
        expect(detector.model_sha256() == expected_sha, "模型容器SHA与Python hashlib独立结果一致");
        const auto infer = [&] {
            const auto boxes = detector.detect(cv::Mat(4, 4, CV_8UC3, cv::Scalar(255, 255, 255)));
            expect(detector.profile().status == DetectionStatus::SUCCESS && boxes.size() == 1 &&
                       boxes[0].class_id == 0 && boxes[0].confidence > .89f && boxes[0].confidence < .91f &&
                       boxes[0].x1 == 1 && boxes[0].x2 == 3,
                   "实际CPU从外置权重计算得到独立已知检测框");
        };
        std::cout << "infer original\n";
        infer();
        const auto backup = root / "saved-weights.bin";
        std::cout << "remove external\n";
        std::filesystem::rename(weights, backup);
        std::cout << "load missing external\n";
        expect(!detector.load() && detector.loaded() && detector.model_sha256() == expected_sha,
               "外置权重缺失导致候选加载失败但保留原模型及身份");
        locale_preserved();
        infer();
        std::filesystem::rename(backup, weights);
        expect(detector.load() && detector.model_sha256() == expected_sha,
               "外置权重恢复后同模型可重新加载");
        infer();
        locale_preserved();
        detector.reset();
        expect(!detector.loaded() && detector.model_sha256().empty(), "reset同时清除模型与身份");
        std::cout << "external fixture=" << xen::filesystem_detail::path_utf8(root) << '\n';
    } catch (const std::exception& error) { ++failures; std::cerr << error.what() << '\n'; }
    Log::shutdown();
    return failures ? 1 : 0;
}
