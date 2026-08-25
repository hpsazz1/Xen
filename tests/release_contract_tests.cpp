#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "app/release_contract_internal.h"

#include <Windows.h>

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
                ("xen-release-contract-" + std::to_string(suffix));
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
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
}

void test_backend_ownership() {
    using namespace app::detail;
    expect(std::string(runtime_for_backend(BackendType::CPU)) == "nvidia" &&
               std::string(runtime_for_backend(BackendType::CUDA)) == "nvidia" &&
               std::string(runtime_for_backend(BackendType::TENSORRT)) == "nvidia",
           "CPU/CUDA/TensorRT 必须归属 NVIDIA 运行时");
    expect(std::string(runtime_for_backend(BackendType::DIRECTML)) == "directml" &&
               std::string(runtime_for_backend(BackendType::OPENVINO)) == "openvino",
           "DirectML/OpenVINO 必须保持独立运行时");
}

void test_release_environment() {
    TemporaryDirectory temporary;
    SetEnvironmentVariableW(L"XEN_RELEASE_ROOT", temporary.path().c_str());
    SetEnvironmentVariableW(L"XEN_RUNTIME_ID", L"nvidia");
    SetEnvironmentVariableW(
        L"XEN_RELEASE_BACKENDS", L"cpu,cuda,tensorrt,directml,openvino");
    app::detail::ReleaseEnvironment environment;
    std::string error;
    expect(app::detail::load_release_environment(environment, error) &&
               environment.managed &&
               environment.available_backends.size() == 5,
           "受管 Worker 必须读取发布根、当前运行时和全局能力: " + error);

    SetEnvironmentVariableW(L"XEN_RUNTIME_ID", nullptr);
    expect(!app::detail::load_release_environment(environment, error),
           "不完整发布环境必须失败关闭");
    SetEnvironmentVariableW(L"XEN_RELEASE_ROOT", nullptr);
    SetEnvironmentVariableW(L"XEN_RELEASE_BACKENDS", nullptr);
}

void test_manifest_validation() {
    TemporaryDirectory temporary;
    const auto root = temporary.path();
    write_file(root / "runtimes/nvidia/Xen.exe", "abc");
    write_file(root / "runtimes/directml/Xen.exe", "abc");
    write_file(root / "runtimes/openvino/Xen.exe", "abc");
    constexpr const char* kSha256 =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    const std::string manifest = std::string(
        "{\n"
        "  \"schema\": 1,\n"
        "  \"product\": \"Xen\",\n"
        "  \"git_commit\": \"0123456789012345678901234567890123456789\",\n"
        "  \"runtimes\": [\n"
        "    {\"id\":\"nvidia\",\"executable\":\"runtimes/nvidia/Xen.exe\",\"backends\":[\"cpu\",\"cuda\",\"tensorrt\"]},\n"
        "    {\"id\":\"directml\",\"executable\":\"runtimes/directml/Xen.exe\",\"backends\":[\"directml\"]},\n"
        "    {\"id\":\"openvino\",\"executable\":\"runtimes/openvino/Xen.exe\",\"backends\":[\"openvino\"]}\n"
        "  ],\n"
        "  \"files\": [\n") +
        "    {\"path\":\"runtimes/nvidia/Xen.exe\",\"runtime\":\"nvidia\",\"size\":3,\"sha256\":\"" + kSha256 + "\"},\n" +
        "    {\"path\":\"runtimes/directml/Xen.exe\",\"runtime\":\"directml\",\"size\":3,\"sha256\":\"" + kSha256 + "\"},\n" +
        "    {\"path\":\"runtimes/openvino/Xen.exe\",\"runtime\":\"openvino\",\"size\":3,\"sha256\":\"" + kSha256 + "\"}\n" +
        "  ]\n}\n";
    write_file(root / "manifest.json", manifest);

    app::detail::ReleaseManifest parsed;
    std::string error;
    expect(app::detail::load_release_manifest(
               root / "manifest.json", parsed, error) &&
               app::detail::validate_release_manifest(root, parsed, error),
           "合法三运行时路由必须通过启动校验: " + error);
    expect(app::detail::find_runtime_for_backend(
               parsed, BackendType::DIRECTML)->id == "directml",
           "后端必须映射到清单声明的隔离运行时");

    write_file(root / "cache/runtime/report.json", "runtime report");
    write_file(root / "cache/tensorrt/model.engine", "provider cache");
    write_file(root / "logs/xen.log", "runtime log");
    write_file(root / "models/personal.onnx", "personal model");
    write_file(root / "notes.txt", "personal release note");
    expect(app::detail::validate_release_manifest(root, parsed, error),
           "个人新增文件不得触发 Launcher 全目录清单拒绝: " + error);

    write_file(root / "runtimes/directml/Xen.exe", "locally rebuilt");
    expect(app::detail::validate_release_manifest(root, parsed, error),
           "本地重建 Worker 不得因旧 manifest 哈希而拒绝启动: " + error);

    std::filesystem::remove(root / "runtimes/directml/Xen.exe");
    expect(!app::detail::validate_release_manifest(root, parsed, error),
           "所选发布运行时入口缺失时必须失败关闭");
    write_file(root / "runtimes/directml/Xen.exe", "restored");

    const auto original_executable = parsed.runtimes.front().executable;
    parsed.runtimes.front().executable = "../outside.exe";
    expect(!app::detail::validate_release_manifest(root, parsed, error),
           "发布运行时入口不得越过发布根目录");
    parsed.runtimes.front().executable = original_executable;
}

} // namespace

int main() {
    test_backend_ownership();
    test_release_environment();
    test_manifest_validation();
    if (failures != 0) {
        std::cerr << "发布契约测试失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "发布契约测试全部通过。\n";
    return 0;
}
