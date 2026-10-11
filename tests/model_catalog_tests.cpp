#include "app/model_catalog_internal.h"

#include <algorithm>
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

void test_literal_names_and_directory_identity() {
    TemporaryDirectory temporary;
    std::filesystem::path first, second;
    std::string error;
    const bool first_ready=app::detail::prepare_model_directory_at_root(temporary.path()/"A",first,error);
    const bool second_ready=app::detail::prepare_model_directory_at_root(temporary.path()/"B",second,error);
    expect(first_ready && second_ready,"两个自有发布根应可准备");
    if (!first_ready || !second_ready) return;
    const bool roots_owned=first==std::filesystem::canonical(temporary.path()/"A"/"models") &&
        second==std::filesystem::canonical(temporary.path()/"B"/"models");
    expect(roots_owned,"只在fixture自有根下创建虚拟模型文件");
    if (!roots_owned) return;
    const std::vector<std::string> names{
        "plain.onnx","model##v2.onnx","alpha###same.onnx","beta###same.onnx","模型##版本.onnx"};
    for (const auto& name:names) {
        write_file(first/std::filesystem::u8path(name));
        write_file(second/std::filesystem::u8path(name));
    }
    std::filesystem::create_directories(first/"nested");
    write_file(first/"nested"/"plain.onnx");
    write_file(second/"only-in-B.onnx");
    std::vector<std::string> listed;
    expect(app::detail::list_models(first,listed,error) && listed.size()==names.size(),
        "只列当前根，不混入嵌套或另一目录的同名文件");
    for (const auto& name:names) {
        expect(std::find(listed.begin(),listed.end(),name)!=listed.end(),
            "合法##/###及Unicode文件名必须按原字节保留");
        std::string path_a,path_b;
        expect(app::detail::resolve_model_selection(first,name,path_a,error) &&
            app::detail::resolve_model_selection(second,name,path_b,error) && path_a!=path_b,
            "相同文件名必须解析到各自明确目录，不能复用另一目录的目标");
        expect(std::filesystem::u8path(path_a)==std::filesystem::canonical(first/std::filesystem::u8path(name)),
            "选择结果必须是当前目录的准确文件");
    }
    std::string resolved;
    expect(!app::detail::resolve_model_selection(first,"../B/models/plain.onnx",resolved,error),
        "字面标签支持不能放宽跨目录选择");
    const auto old_path=first/"beta###same.onnx", new_path=first/"renamed###same.onnx";
    const auto expected_root=std::filesystem::canonical(temporary.path()/"A"/"models");
    const bool owned=std::filesystem::canonical(old_path.parent_path())==expected_root &&
        std::filesystem::canonical(new_path.parent_path())==expected_root && !std::filesystem::exists(new_path);
    expect(owned,"仅允许重命名本fixture明确拥有且目标不存在的文件");
    if (owned) std::filesystem::rename(old_path,new_path);
    expect(app::detail::list_models(first,listed,error) &&
        std::find(listed.begin(),listed.end(),"beta###same.onnx")==listed.end() &&
        std::find(listed.begin(),listed.end(),"renamed###same.onnx")!=listed.end(),
        "刷新必须反映重命名，不保留旧名称别名");
    expect(!app::detail::resolve_model_selection(first,"beta###same.onnx",resolved,error),
        "重命名后的旧选择必须拒绝，不回退到另一目录同名文件");
    expect(app::detail::resolve_model_selection(second,"beta###same.onnx",resolved,error),
        "第一目录重命名不得影响另一目录的同名文件");
}

} // namespace

int main() {
    test_prepare_and_list_models();
    test_normalize_and_resolve_selection();
    test_reject_non_directory_models_path();
    test_release_root_model_directory();
    test_literal_names_and_directory_identity();
    if (failures != 0) {
        std::cerr << "模型目录测试失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "模型目录测试全部通过。\n";
    return 0;
}
