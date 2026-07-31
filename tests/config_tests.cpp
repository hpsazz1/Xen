#include "config/config.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << message << '\n';
}

void test_round_trip() {
    AppConfig source;
    source.detector.model_path = "models/test.onnx";
    source.detector.backend = BackendType::TENSORRT;
    source.capture.roi_width = 416;
    source.capture.roi_height = 416;
    source.aim.person_class_ids = {0, 2};
    source.aim.head_class_ids = {1, 3};
    source.mouse.allow_send_input = true;
    source.ui.width = 1024;
    source.ui.theme = UiTheme::DARK;

    const auto path = std::filesystem::temp_directory_path() /
                      "xen_config_round_trip.ini";
    std::string error;
    expect(save_app_config(path.string(), source, error),
           "有效配置应成功写入: " + error);
    AppConfig loaded;
    expect(load_app_config(path.string(), loaded, error),
           "写入后的配置应成功读取: " + error);
    expect(loaded.detector.backend == BackendType::TENSORRT &&
           loaded.capture.roi_width == 416 &&
           loaded.aim.person_class_ids == source.aim.person_class_ids &&
           loaded.mouse.allow_send_input && loaded.ui.width == 1024 &&
           loaded.ui.theme == UiTheme::DARK,
           "配置往返后关键字段必须保持一致");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void test_invalid_config() {
    AppConfig config;
    std::string error;
    expect(!validate_app_config(config, error) && !error.empty(),
           "缺少模型路径的默认配置必须失败关闭");
    config.detector.model_path = "model.onnx";
    config.aim.high_confidence = 0.05f;
    config.aim.low_confidence = 0.10f;
    expect(!validate_app_config(config, error),
           "高置信度阈值低于低阈值时必须拒绝配置");
    config.aim.high_confidence = 0.25f;
    config.ui.theme = static_cast<UiTheme>(99);
    expect(!validate_app_config(config, error),
           "未知 UI 主题必须拒绝配置");
}

} // namespace

int main() {
    test_round_trip();
    test_invalid_config();
    if (failures != 0) {
        std::cerr << "Config 测试失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "Config 测试全部通过。\n";
    return 0;
}
