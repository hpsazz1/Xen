#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#ifdef ERROR
#undef ERROR
#endif
#include "model_workspace/model_workspace.h"
#include "log/log.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <thread>

namespace {
namespace fs = std::filesystem;
using namespace model_workspace;
int failures = 0;
void expect(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "[失败] " << message << '\n'; }
}
std::string utf8(const fs::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
Snapshot wait(Workspace& workspace, Settings& settings) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        const auto view = workspace.poll(&settings);
        if (!view.job_running) return view;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    expect(false, "fixture应在5秒内结束");
    return workspace.poll(&settings);
}

std::string bytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void test_result_consistency(const fs::path& root, const fs::path& scripts, const char* python) {
    if (!fs::create_directory(root)) { expect(false, "一致性fixture独占目录"); return; }
    Workspace workspace(nullptr);
    Settings settings;
    std::string error;
    if (!workspace.initialize(root, settings, error)) { expect(false, error.c_str()); return; }
    settings.base_python_executable = python;
    settings.python_executable = python;
    settings.script_path = utf8(scripts / "model_data_pipeline.py");
    settings.dataset_path = "keep-dataset";
    settings.review_manifest = "keep-review";
    settings.prelabels_path = "keep-prelabels";
    settings.model_path = "keep-model";
    const auto saved = root / "cache/model-workspace/settings.json";
    const auto other_python = root / "other-interpreter.exe";
    { std::ofstream file(other_python); file << "fixture placeholder; never executed"; }
    const auto unrelated_unchanged = [&] {
        return settings.dataset_path == "keep-dataset" && settings.review_manifest == "keep-review" &&
            settings.prelabels_path == "keep-prelabels" && settings.model_path == "keep-model";
    };
    const auto launch = [&](const char* mode) {
        settings.environment_root = utf8(root / mode);
        const bool ok = workspace.execute(Action::ENV_CHECK, settings, false, false, "");
        expect(ok, "ENV_CHECK一致性fixture启动");
        return ok;
    };
    if (!launch("healthy")) return;
    const auto first = wait(workspace, settings);
    expect(first.job_state == "SUCCEEDED" && first.environment_ready, "健康请求生成真实旧作业状态");
    fs::copy_file(fs::u8path(first.job_directory) / "status.json", root / "old-status.json");

    for (const char* mode : {"null-result", "array-result", "missing-result", "missing-ready",
             "missing-python-field", "missing-device", "missing-output", "wrong-python-type",
             "wrong-device-type", "wrong-output-type", "old-result", "foreign-operation",
             "wrong-interpreter", "wrong-device"}) {
        settings.python_executable = python;
        settings.environment_root = utf8(root / mode);
        expect(workspace.execute(Action::SAVE_SETTINGS, settings, false, false, ""), "保存测试前配置基线");
        const auto before = bytes(saved);
        if (!launch(mode)) return;
        const auto directory = workspace.poll().job_directory;
        const auto result = wait(workspace, settings);
        std::cout << mode << " state=" << result.job_state << " message=" << result.job_message << '\n';
        expect(result.job_state == "FAILED" && !result.environment_ready && !result.weights_ready,
               "结果结构或请求身份不符必须失败关闭就绪");
        expect(result.job_directory == directory && result.job_operation == "env_check" &&
               result.job_message.find("作业结果") != std::string::npos,
               "拒绝结果保留真实作业身份和错误回显");
        expect(settings.python_executable == python && unrelated_unchanged(), "拒绝结果不部分更新内存配置");
        expect(bytes(saved) == before, "拒绝结果不改已保存配置字节");
        expect(workspace.poll(&settings).job_state == "FAILED", "拒绝结果不会在后续poll恢复成功");
    }

    if (!launch("unrelated-fields")) return;
    const auto extras = wait(workspace, settings);
    expect(extras.job_state == "SUCCEEDED" && extras.environment_ready && unrelated_unchanged(),
           "环境结果只应用自身字段，不消费其他作业的模型或数据字段");

    for (const bool edit_python : {true, false}) {
        settings.python_executable = python;
        settings.device = "0";
        expect(workspace.execute(Action::SAVE_SETTINGS, settings, false, false, ""), "保存编辑前配置");
        const auto before = bytes(saved);
        if (!launch("hold-success")) return;
        const auto directory = fs::u8path(workspace.poll().job_directory);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!fs::exists(directory / "started.flag") && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        expect(fs::exists(directory / "started.flag"), "先确认子进程读到原始请求再编辑");
        if (edit_python) settings.python_executable = utf8(other_python);
        else settings.device = "1";
        const auto selected_python = settings.python_executable;
        const auto selected_device = settings.device;
        { std::ofstream release(directory / "release.flag"); release << "continue"; }
        const auto stale = wait(workspace, settings);
        std::cout << (edit_python ? "edited-python" : "edited-device") << " state=" << stale.job_state << '\n';
        expect(stale.job_state == "FAILED" && !stale.environment_ready,
               "检查期间用户改变解释器或设备后，旧检查不能覆盖选择或授予就绪");
        expect(settings.python_executable == selected_python && settings.device == selected_device,
               "旧请求保留用户当前内存编辑");
        expect(bytes(saved) == before, "旧请求不能把过期或未审核配置写入磁盘");
    }
    settings.python_executable = python;
    if (!launch("healthy")) return;
    const auto retry = wait(workspace, settings);
    expect(retry.job_state == "SUCCEEDED" && retry.environment_ready && unrelated_unchanged(),
           "明确重试按当前设备选择成功，保留无关配置");
}

}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    LogConfig log; log.enable_console = false; log.enable_file = false;
    Log::init(log);
    // 自有临时根保留供失败诊断；不递归删除或接触用户配置。
    const auto parent = fs::weakly_canonical(fs::temp_directory_path());
    const auto root = parent / ("xen-workspace-status-" + std::to_string(GetCurrentProcessId()) +
        "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (root.parent_path() != parent || !fs::create_directory(root)) return 2;
    const auto scripts = root / "scripts";
    fs::create_directory(scripts);
    fs::copy_file(fs::u8path(argv[2]), scripts / "model_training_environment.py");
    {
        Workspace workspace(nullptr);
        Settings settings;
        std::string error;
        if (!workspace.initialize(root, settings, error)) {
            std::cerr << error << '\n'; return 2;
        }
        settings.base_python_executable = argv[1];
        settings.python_executable = argv[1];
        settings.script_path = utf8(scripts / "model_data_pipeline.py");
        const auto original_python = settings.python_executable;
        for (const char* mode : {"missing-python", "not-ready", "bad-type"}) {
            settings.environment_root = utf8(root / mode);
            if (!workspace.execute(Action::ENV_CHECK, settings, false, false, "")) {
                std::cerr << workspace.poll().message << '\n'; return 2;
            }
            const auto directory = workspace.poll().job_directory;
            expect(!directory.empty(), "已启动作业有可定位目录");
            const auto view = wait(workspace, settings);
            std::cout << mode << " state=" << view.job_state << " message=" << view.job_message << '\n';
            expect(!view.job_running && view.job_state == "FAILED", "无效完成结果应显式失败");
            expect(view.job_operation == "env_check" && view.job_directory == directory,
                   "失败保留作业身份及打开当前作业入口");
            expect(view.job_message.find("作业结果") != std::string::npos,
                   "失败应解释结果不可用，不能显示尚未启动");
            if (std::string_view(mode) == "missing-python")
                expect(view.job_message.find("自检解释器已不存在") != std::string::npos,
                       "缺失解释器原因可见");
            expect(!view.environment_ready && !view.weights_ready &&
                   settings.python_executable == original_python,
                   "拒绝失败结果，不改变解释器或授予环境就绪");
            for (int i = 0; i < 3; ++i) {
                const auto repeated = workspace.poll(&settings);
                expect(repeated.job_state == "FAILED" && repeated.job_directory == directory &&
                       repeated.job_message == view.job_message, "连续poll保留失败诊断");
            }
            expect(workspace.poll().job_state == "FAILED", "无settings读取也不能冒充成功");
        }

        settings.environment_root = utf8(root / "wait");
        expect(workspace.execute(Action::ENV_CHECK, settings, false, false, ""), "失败后允许明确重试");
        const auto running = workspace.poll(&settings);
        expect(running.job_running && running.job_state == "RUNNING", "新作业恢复运行态");
        expect(!workspace.execute(Action::ENV_CHECK, settings, false, false, ""), "活动作业拒绝重复提交");
        expect(workspace.execute(Action::CANCEL_JOB, settings, false, false, ""), "运行态仍可取消");
        const auto cancelled = wait(workspace, settings);
        expect(!cancelled.job_running && cancelled.job_state == "CANCELLED" &&
               cancelled.job_directory == running.job_directory, "取消保留身份与终态");
        expect(!cancelled.environment_ready && !cancelled.weights_ready, "取消不授予就绪");

        settings.environment_root = utf8(root / "healthy");
        expect(workspace.execute(Action::ENV_CHECK, settings, false, false, ""), "取消后允许健康重试");
        const auto healthy = wait(workspace, settings);
        expect(healthy.job_state == "SUCCEEDED" && healthy.environment_ready &&
               !healthy.weights_ready, "健康协议结果恢复环境就绪，不能代替PT审核");
        expect(healthy.job_message.find("作业结果") == std::string::npos, "新成功作业不残留旧失败说明");
        expect(fs::is_regular_file(root / "cache/model-workspace/settings.json"), "健康结果正常保存配置");

        // 仅在自有根内用目录占位复现原子写入失败；不改权限、不删除文件。
        const auto workspace_root = fs::weakly_canonical(root / "cache/model-workspace");
        const auto saved = workspace_root / "settings.json";
        const auto backup = workspace_root / "settings.retained.json";
        const auto blocker = workspace_root / "settings-blocker-retained";
        if (workspace_root != root / "cache/model-workspace" || fs::exists(backup) || fs::exists(blocker))
            return 2;
        settings.python_executable = original_python;
        fs::rename(saved, backup);
        if (!fs::create_directory(saved)) return 2;
        expect(workspace.execute(Action::ENV_CHECK, settings, false, false, ""), "启动配置保存失败对照");
        const auto saved_before_failure = bytes(backup);
        const auto write_failed = wait(workspace, settings);
        expect(settings.python_executable == original_python && bytes(backup) == saved_before_failure,
               "配置保存失败保留原内存选择和磁盘字节");
        expect(write_failed.job_state == "FAILED" && !write_failed.environment_ready &&
               !write_failed.weights_ready && !write_failed.job_directory.empty(),
               "保存失败不能遗留此前在内存中置位的就绪状态");
        expect(write_failed.job_message.find("原子提交失败") != std::string::npos,
               "配置保存失败原因可见");
        expect(workspace.poll().job_state == "FAILED", "保存失败也保留终态");
        fs::rename(saved, blocker);
        fs::rename(backup, saved);
        expect(workspace.execute(Action::ENV_CHECK, settings, false, false, ""), "移走自有占位后可重试");
        const auto recovered = wait(workspace, settings);
        expect(recovered.job_state == "SUCCEEDED" && recovered.environment_ready,
               "保存恢复后的明确重试可成功");
    }
    test_result_consistency(root / "consistency", scripts, argv[1]);
    Log::shutdown();
    std::cout << "fixture_root=" << utf8(root) << " native_windows=0 model_loads=0 failures=" << failures << '\n';
    return failures ? 1 : 0;
}
