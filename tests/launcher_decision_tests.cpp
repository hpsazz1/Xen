#include "app/launcher_decision_internal.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << message << '\n';
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto suffix = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("xen-launcher-decision-" + std::to_string(suffix));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path,
                const std::string& content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
}

app::detail::ReleaseManifest test_manifest() {
    using app::detail::ReleaseRuntimeEntry;
    app::detail::ReleaseManifest manifest;
    manifest.runtimes = {
        ReleaseRuntimeEntry{
            "nvidia", "runtimes/nvidia/Xen.exe",
            {BackendType::CPU, BackendType::CUDA, BackendType::TENSORRT}},
        ReleaseRuntimeEntry{
            "directml", "runtimes/directml/Xen.exe",
            {BackendType::DIRECTML}},
    };
    return manifest;
}

struct ErrorCapture {
    int call_count = 0;
    std::string message;
};

void capture_error(void* context, const std::string& message) {
    auto& capture = *static_cast<ErrorCapture*>(context);
    ++capture.call_count;
    capture.message = message;
}

app::detail::LauncherDecisionAdapter adapter_for(ErrorCapture& capture) {
    return {&capture, capture_error};
}

void test_existing_invalid_config_fails_before_worker_selection() {
    TemporaryDirectory temporary;
    const auto config_path = temporary.path() / "config.ini";
    write_file(
        config_path,
        "[detector]\n"
        "model_path=model.onnx\n"
        "backend=tensor_rt_typo\n");

    const auto manifest = test_manifest();
    const app::detail::ReleaseRuntimeEntry* runtime =
        &manifest.runtimes.back();
    ErrorCapture error;
    const int exit_code = app::detail::resolve_launcher_runtime(
        manifest, config_path.string(), adapter_for(error), runtime);

    expect(exit_code == app::detail::kLauncherDecisionFailureExitCode &&
               runtime == nullptr && error.call_count == 1 &&
               error.message.find("detector.backend") != std::string::npos,
           "既有非法 config.ini 必须在 Worker 选择前固定失败并展示 owned error；"
           "exit=" + std::to_string(exit_code) +
               "，message=" + error.message);
}

void test_valid_config_selects_declared_runtime_without_error() {
    TemporaryDirectory temporary;
    const auto config_path = temporary.path() / "config.ini";
    write_file(
        config_path,
        "[detector]\n"
        "model_path=model.onnx\n"
        "backend=directml\n");

    const auto manifest = test_manifest();
    const app::detail::ReleaseRuntimeEntry* runtime = nullptr;
    ErrorCapture error;
    const int exit_code = app::detail::resolve_launcher_runtime(
        manifest, config_path.string(), adapter_for(error), runtime);

    expect(exit_code == 0 && runtime && runtime->id == "directml" &&
               error.call_count == 0,
           "合法配置必须选择清单声明的 Worker 且不展示错误");
}

void test_missing_config_preserves_default_runtime_contract() {
    TemporaryDirectory temporary;
    const auto config_path = temporary.path() / "missing-config.ini";

    const auto manifest = test_manifest();
    const app::detail::ReleaseRuntimeEntry* runtime = nullptr;
    ErrorCapture error;
    const int exit_code = app::detail::resolve_launcher_runtime(
        manifest, config_path.string(), adapter_for(error), runtime);

    expect(exit_code == 0 && runtime && runtime->id == "nvidia" &&
               error.call_count == 0,
           "缺失配置必须保留 AppConfig 默认 TensorRT→NVIDIA Worker 合同");
}

} // namespace

int main() {
    test_existing_invalid_config_fails_before_worker_selection();
    test_valid_config_selects_declared_runtime_without_error();
    test_missing_config_preserves_default_runtime_contract();
    if (failures != 0) {
        std::cerr << "Launcher 决策测试失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "Launcher 决策测试全部通过。\n";
    return 0;
}
