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
}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const auto root = fs::temp_directory_path() /
        (L"xen-model-workspace-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count())) /
        L"中文 space & literal";
    fs::create_directories(root);
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
