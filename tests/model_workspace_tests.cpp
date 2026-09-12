#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include "model_workspace/model_workspace.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
namespace fs = std::filesystem;
int failures = 0;
void expect(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "[失败] " << message << '\n'; }
}
std::string utf8(const fs::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
model_workspace::Snapshot wait(model_workspace::Workspace& workspace) {
    for (int i = 0; i < 150; ++i) {
        const auto view = workspace.poll();
        if (!view.job_running) return view;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return workspace.poll();
}

void test_tool_paths(const fs::path& test_root, const char* python) {
    using Action = model_workspace::Action;
    const auto root = test_root / "original package";
    model_workspace::Workspace workspace(nullptr);
    model_workspace::Settings settings;
    std::string error;
    expect(workspace.initialize(root, settings, error), "工具路径工作区初始化");
    const auto bundled_relative = fs::path("tools/model-data/model_data_pipeline.py");
    expect(fs::u8path(settings.script_path) == root / bundled_relative,
           "默认工具来自数据根的完整发布资源，不依赖Worker或源码目录");
    settings.script_path = utf8(root / bundled_relative);
    settings.python_executable = python;
    settings.environment_root = utf8(test_root / "existing environment");
    expect(workspace.execute(Action::SAVE_SETTINGS, settings, false, false, ""),
           "保存包内工具及显式外部训练环境");
    const auto relocated = test_root / L"中文 新程序目录";
    fs::create_directories(relocated / "cache/model-workspace");
    fs::copy_file(root / "cache/model-workspace/settings.json",
                  relocated / "cache/model-workspace/settings.json");
    {
        model_workspace::Workspace reopened(nullptr);
        model_workspace::Settings restored;
        expect(reopened.initialize(relocated, restored, error) &&
               fs::u8path(restored.script_path) == relocated / bundled_relative,
               "复制到新程序根后，包内工具不得继续指回旧程序目录");
        expect(restored.python_executable == settings.python_executable &&
               restored.environment_root == settings.environment_root &&
               restored.root_directory == settings.root_directory,
               "程序根变化不搬移已有解释器、环境或用户数据");
    }
    wchar_t executable[32768]{};
    const auto length = GetModuleFileNameW(nullptr, executable, 32768);
    expect(length > 0 && length < 32768, "旧默认路径夹具定位程序");
    settings.script_path = utf8(fs::path(executable).parent_path() /
                                "scripts/model_data_pipeline.py");
    workspace.execute(Action::SAVE_SETTINGS, settings, false, false, "");
    {
        model_workspace::Workspace reopened(nullptr);
        model_workspace::Settings restored;
        expect(reopened.initialize(root, restored, error) &&
               fs::u8path(restored.script_path) == root / bundled_relative,
               "识别本程序旧默认scripts路径并迁移到正式工具目录");
    }
    settings.script_path = utf8(test_root / "custom/scripts/model_data_pipeline.py");
    workspace.execute(Action::SAVE_SETTINGS, settings, false, false, "");
    {
        model_workspace::Workspace reopened(nullptr);
        model_workspace::Settings restored;
        expect(reopened.initialize(root, restored, error) &&
               restored.script_path == settings.script_path,
               "同名自定义脚本不能被猜测成默认路径而覆盖");
    }
}
}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const auto root = fs::temp_directory_path() /
        (L"xen-model-workspace-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count())) /
        L"中文 space & literal";
    fs::create_directories(root);
    test_tool_paths(root, argv[1]);
    auto collector = std::make_shared<data_collection::Collector>();
    model_workspace::Workspace workspace(collector);
    model_workspace::Settings settings;
    std::string error;
    expect(workspace.initialize(root, settings, error), "工作区初始化");
    settings.python_executable = argv[1];
    settings.script_path = argv[2];
    settings.class_names = "person,head";
    using Action = model_workspace::Action;
    expect(!workspace.execute(Action::START_COLLECTION, settings, false, false, ""),
           "停止Runtime时不得开始采集");
    expect(!workspace.execute(Action::EXPORT_DATASET, settings, false, false, ""),
           "未确认schema不得准备数据");
    settings.class_schema_confirmed = true;
    expect(!workspace.execute(Action::START_COLLECTION, settings, true, true, ""),
           "GPU帧不静默改CPU采集");
    expect(!workspace.execute(Action::TRAIN, settings, true, false, ""),
           "Runtime运行时拒绝训练");
    expect(workspace.execute(Action::SAVE_SETTINGS, settings, false, false, ""),
           "独立设置持久化");
    {
        model_workspace::Workspace reopened(collector);
        model_workspace::Settings restored;
        expect(reopened.initialize(root, restored, error) &&
               restored.class_names == settings.class_names &&
               restored.python_executable == settings.python_executable,
               "设置保存重开保真");
    }
    expect(workspace.execute(Action::INSPECT_DATA, settings, false, false, ""),
           "真实Python进程可启动，支持中文空格与shell元字符路径");
    expect(!workspace.execute(Action::INSPECT_DATA, settings, false, false, ""),
           "运行中的作业拒绝重入");
    auto view = wait(workspace);
    expect(!view.job_running && view.job_state == "SUCCEEDED", "作业状态和退出码联合确认成功");
    {
        std::ifstream stream(fs::u8path(view.job_directory) / "status.json");
        nlohmann::json status; stream >> status;
        expect(status.at("result").at("root") == settings.root_directory,
               "参数经过JSON完整传递，不经过shell解释");
    }
    expect(!workspace.execute(Action::IMPORT_CANDIDATE, settings, false, false, ""),
           "inspect成功不授予模型导入资格");
    const auto model = root / "fixture-only.onnx";
    { std::ofstream file(model); file << "仅测试作业身份，不是推理模型"; }
    settings.model_path = utf8(model);
    settings.class_schema_confirmed = false;
    expect(workspace.execute(Action::INSPECT_DATA, settings, false, false, ""),
           "首次无schema可检查模型身份");
    wait(workspace);
    workspace.poll(&settings);
    expect(!settings.schema_model_sha256.empty() && settings.class_names == "person,head" &&
           !settings.class_schema_confirmed, "metadata回填但不代替用户确认");
    settings.class_schema_confirmed = true;
    expect(workspace.execute(Action::START_COLLECTION, settings, true, false, utf8(model)),
           "已确认相同模型身份可创建采集会话，不启动Runtime");
    workspace.execute(Action::STOP_COLLECTION, settings, false, false, "");
    { std::ofstream file(model, std::ios::app); file << "changed"; }
    expect(!workspace.execute(Action::START_COLLECTION, settings, true, false, utf8(model)),
           "同路径模型变化使类别确认失效");
    const auto dataset = root / "fixture-dataset";
    fs::create_directory(dataset);
    const auto dataset_record = dataset / "dataset.json";
    {
        // 验证累积数据清单不受小状态文件的1MiB限制；不模拟真实训练。
        nlohmann::json manifest{{"class_names", {"person", "head"}},
            {"samples", nlohmann::json::array()}, {"fixture_padding", std::string(1100000, 'x')}};
        std::ofstream file(dataset_record); file << manifest;
    }
    settings.dataset_path = utf8(dataset);
    expect(workspace.execute(Action::EVALUATE, settings, false, false, ""), "评价编排夹具启动");
    wait(workspace);
    { std::ofstream file(dataset_record, std::ios::app); file << '\n'; }
    expect(!workspace.execute(Action::IMPORT_CANDIDATE, settings, false, false, ""),
           "评价后数据身份变化拒绝导入");
    expect(workspace.execute(Action::EVALUATE, settings, false, false, ""), "重新评价数据身份");
    wait(workspace);
    { std::ofstream file(model, std::ios::app); file << "again"; }
    expect(!workspace.execute(Action::IMPORT_CANDIDATE, settings, false, false, ""),
           "评价后模型变化拒绝导入");
    expect(workspace.execute(Action::EVALUATE, settings, false, false, ""), "重新评价模型身份");
    wait(workspace);
    expect(workspace.execute(Action::IMPORT_CANDIDATE, settings, false, false, ""),
           "大于1MiB数据清单身份和指标绑定后原子导入候选");
    const auto imported = fs::u8path(workspace.poll().candidate_path);
    expect(fs::is_regular_file(imported) && fs::is_regular_file(fs::path(imported.wstring() + L".json")),
           "可见候选同时具备评价sidecar");
    expect(!workspace.execute(Action::IMPORT_CANDIDATE, settings, false, false, ""),
           "重复导入不覆盖既有候选");
    expect(workspace.execute(Action::TRAIN, settings, false, false, ""), "取消测试启动");
    const auto job_path = fs::u8path(workspace.poll().job_directory);
    expect(workspace.execute(Action::CANCEL_JOB, settings, false, false, ""), "发送合作取消");
    view = wait(workspace);
    expect(!view.job_running && view.job_state == "CANCELLED" && fs::exists(job_path / "cancel.flag"),
           "取消有文件事实且不会被成功状态覆盖");
    const auto fixture_scripts = root / "scripts";
    fs::create_directory(fixture_scripts);
    fs::copy_file(fs::u8path(argv[2]), fixture_scripts / "model_training_environment.py");
    settings.script_path = utf8(fixture_scripts / "model_data_pipeline.py");
    settings.base_python_executable = argv[1];
    expect(workspace.execute(Action::ENV_CHECK, settings, false, false, ""),
           "环境检查通过基础解释器和独立脚本启动");
    wait(workspace);
    workspace.poll(&settings);
    expect(workspace.poll().environment_ready, "只有GPU自检成功结果回填环境可用");
    const auto weights = root / "fixture-only.pt";
    { std::ofstream file(weights); file << "仅验证权重身份协议"; }
    settings.weights_path = utf8(weights);
    settings.class_schema_confirmed = false;
    expect(!workspace.execute(Action::PT_CHECK, settings, false, false, ""),
           "PT来源未确认不能加载pickle模型");
    settings.trusted_weights = true;
    expect(workspace.execute(Action::PT_CHECK, settings, false, false, ""),
           "PT检查无需先确认类别，发送实际文件哈希");
    wait(workspace);
    workspace.poll(&settings);
    expect(workspace.poll().weights_ready && !settings.class_schema_confirmed,
           "PT检查成功不代替类别语义人工确认");
    settings.device = "1";
    expect(!workspace.poll(&settings).environment_ready && !workspace.poll().weights_ready,
           "切换GPU同时失效环境和权重检查");
    settings.device = "0";
    expect(workspace.execute(Action::PT_CHECK, settings, false, false, ""), "重做PT兼容检查");
    wait(workspace); workspace.poll(&settings);
    settings.trusted_weights = false;
    expect(!workspace.execute(Action::PT_CHECK, settings, false, false, "") &&
           !workspace.poll().weights_ready, "再次检查在启动前失败不得残留已通过");
    settings.class_schema_confirmed = true;
    expect(!workspace.execute(Action::TRAIN, settings, false, false, ""),
           "训练入口不能绕过PT来源确认");
    settings.weights_path += ".changed";
    expect(!workspace.poll(&settings).weights_ready, "换权重路径不沿用旧兼容标记");
    settings.script_path = argv[2];
    settings.python_executable = utf8(root / "missing.exe");
    expect(!workspace.execute(Action::INSPECT_DATA, settings, false, false, ""),
           "缺失Python显式失败");
    settings.class_names = "person,person";
    expect(!workspace.execute(Action::EXPORT_DATASET, settings, false, false, ""),
           "重复类别拒绝");
    workspace.shutdown();
    // 临时目录属于本测试，保留供失败诊断，不触及用户数据。
    std::cout << "工作区专项 failures=" << failures << " root=" << utf8(root) << '\n';
    return failures ? 1 : 0;
}
