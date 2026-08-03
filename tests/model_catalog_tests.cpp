#include "app/model_catalog_internal.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

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
                ("xen-model-catalog-" + std::to_string(suffix));
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

void write_file(const std::filesystem::path& path) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "fixture";
}

void test_prepare_and_list_models() {
    TemporaryDirectory temporary;
    const auto executable = temporary.path() / "bin" / "Xen.exe";
    std::filesystem::create_directories(executable.parent_path());

    std::filesystem::path model_directory;
    std::string error;
    expect(app::detail::prepare_model_directory(
               executable, model_directory, error),
           "程序旁缺失的 models 目录必须自动创建: " + error);
    expect(model_directory ==
               std::filesystem::weakly_canonical(
                   executable.parent_path() / "models"),
           "模型目录必须固定在可执行文件同级 models");

    write_file(model_directory / "zeta.onnx");
    write_file(model_directory / "Alpha.ONNX");
    write_file(model_directory / std::filesystem::u8path("模型.onnx"));
    write_file(model_directory / "notes.txt");
    std::filesystem::create_directories(model_directory / "nested");
    write_file(model_directory / "nested" / "hidden.onnx");

    std::vector<std::string> models;
    expect(app::detail::list_models(model_directory, models, error),
           "模型目录必须可枚举: " + error);
    expect(models == std::vector<std::string>{
               "Alpha.ONNX", "zeta.onnx", "模型.onnx"},
           "只应按稳定顺序列出根目录普通 ONNX 文件");

    write_file(model_directory / "middle.onnx");
    expect(app::detail::list_models(model_directory, models, error) &&
               models == std::vector<std::string>{
                   "Alpha.ONNX", "middle.onnx", "zeta.onnx", "模型.onnx"},
           "刷新必须原子替换清单并看到新增模型");
}

void test_normalize_and_resolve_selection() {
    TemporaryDirectory temporary;
    const auto executable = temporary.path() / "Xen.exe";
    std::filesystem::path model_directory;
    std::string error;
    expect(app::detail::prepare_model_directory(
               executable, model_directory, error),
           "测试模型目录必须准备成功: " + error);
    write_file(model_directory / "active.onnx");
    write_file(temporary.path() / "outside.onnx");

    expect(app::detail::normalize_model_selection(
               "C:/legacy/models/active.onnx") == "active.onnx",
           "旧绝对路径迁移时只能保留 ONNX 文件名");
    expect(app::detail::normalize_model_selection(
               "models/active.onnx") == "active.onnx",
           "旧相对 models 路径迁移时只能保留文件名");
    expect(app::detail::normalize_model_selection("model.engine").empty(),
           "非 ONNX 配置值必须拒绝");

    std::string resolved;
    expect(app::detail::resolve_model_selection(
               model_directory, "active.onnx", resolved, error),
           "目录内普通 ONNX 文件必须可解析: " + error);
    expect(std::filesystem::u8path(resolved) ==
               std::filesystem::canonical(model_directory / "active.onnx"),
           "运行时路径必须是 models 中模型的规范绝对路径");
    expect(!app::detail::resolve_model_selection(
               model_directory, "../outside.onnx", resolved, error),
           "目录穿越不得访问 models 外部文件");
    expect(!app::detail::resolve_model_selection(
               model_directory, "missing.onnx", resolved, error),
           "不存在的选择必须显式失败");
    expect(!app::detail::resolve_model_selection(
               model_directory, "notes.txt", resolved, error),
           "非 ONNX 选择必须显式失败");
}

void test_reject_non_directory_models_path() {
    TemporaryDirectory temporary;
    const auto executable = temporary.path() / "Xen.exe";
    write_file(temporary.path() / "models");

    std::filesystem::path model_directory;
    std::string error;
    expect(!app::detail::prepare_model_directory(
               executable, model_directory, error) && !error.empty(),
           "models 被同名文件占用时必须失败关闭");
}

void test_release_root_model_directory() {
    TemporaryDirectory temporary;
    std::filesystem::path model_directory;
    std::string error;
    expect(app::detail::prepare_model_directory_at_root(
               temporary.path(), model_directory, error) &&
               model_directory == std::filesystem::weakly_canonical(
                   temporary.path() / "models"),
           "隔离 Worker 必须共享发布根 models，而不是自身运行时目录: " + error);
}

} // namespace

int main() {
    test_prepare_and_list_models();
    test_normalize_and_resolve_selection();
    test_reject_non_directory_models_path();
    test_release_root_model_directory();
    if (failures != 0) {
        std::cerr << "模型目录测试失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "模型目录测试全部通过。\n";
    return 0;
}
