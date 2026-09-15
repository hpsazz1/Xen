#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#ifdef ERROR
#undef ERROR
#endif
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>
#include <opencv2/imgcodecs.hpp>
#include "config/config.h"
#include "auto_stop_probe/counterpulse_internal.h"
#include "auto_stop_probe/debug_runner.h"
#include "auto_stop_probe/capture_evidence_internal.h"
#include "auto_stop_probe/readiness_internal.h"
#include "auto_stop_probe/preroll_internal.h"
#include "auto_stop_probe/training_evaluation_internal.h"
#include "auto_stop_probe/hud_feedback_internal.h"
#include "auto_stop_probe/sampling_analysis_internal.h"
#include "auto_stop_probe/debug_report_internal.h"
#include "auto_stop_probe/counterpulse_hud.h"
#include "auto_stop_probe/manual_recording_internal.h"
#include "auto_stop_probe/manual_labels_internal.h"
#include "auto_stop_probe/default_baseline_internal.h"
#include "runtime/input_training_internal.h"
#include "source_context/source_context.h"

namespace {
using namespace auto_stop_probe_detail;
std::atomic<bool> stopped{false};
std::atomic<bool> cleanup_finished{false};
BOOL WINAPI control(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
        stopped.store(true);
        if (event == CTRL_CLOSE_EVENT) {
            const auto until = Clock::now() + std::chrono::seconds(4);
            while (!cleanup_finished.load() && Clock::now() < until) Sleep(10);
        }
        return TRUE;
    }
    return FALSE;
}
struct Resources {
    std::shared_ptr<IMouseController> mouse;
    source_context::SourceContextClient focus;
    ~Resources() { focus.stop(); if (mouse) mouse->close(); cleanup_finished.store(true); }
};
void write_json(const std::filesystem::path& path, const Json& report) {
    auto temporary = path; temporary += ".writing";
    {
        std::ofstream file(temporary); file.exceptions(std::ios::badbit | std::ios::failbit);
        file << report.dump(2) << '\n';
        file.flush();
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("报告原子保存失败");
}
Json read_bounded_json(const std::filesystem::path& path, std::uintmax_t limit = 64 * 1024 * 1024) {
    if (std::filesystem::file_size(path) > limit) throw std::runtime_error("输入文件过大");
    std::ifstream stream(path);
    return Json::parse(stream, nullptr, true, true);
}
void require_explicit_monitor_config(const std::filesystem::path& path) {
    if (std::filesystem::file_size(path) > 4 * 1024 * 1024) throw std::runtime_error("配置过大");
    std::ifstream stream(path);
    std::string line, section;
    std::set<std::string> keys;
    const auto trim = [](std::string text) {
        const auto first = text.find_first_not_of(" \t\r\n");
        return first == std::string::npos ? std::string{} : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    };
    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.starts_with("\xEF\xBB\xBF")) line.erase(0, 3);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') { section = line; continue; }
        if (section != "[mouse]") continue;
        const auto equals = line.find('=');
        if (equals != std::string::npos && !trim(line.substr(equals + 1)).empty()) keys.insert(trim(line.substr(0, equals)));
    }
    for (const auto* key : {"kmbox_ip", "kmbox_port", "kmbox_uuid"})
        if (!keys.contains(key)) throw std::runtime_error("人工录制拒绝缺省设备地址");
}
void keep_hud_visible(CounterpulseHud& hud) {
    cleanup_finished.store(true);
    while (!stopped.load() && !hud.closed() && hud.status()["state"] != "FAILED")
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::filesystem::path output;
    bool created = false;
    std::string stage = "VALIDATION";
    std::string failure_reason;
    Json readiness = {{"reason", "NOT_CHECKED"}};
    Json capture_diagnostic = Json::object();
    Json cancellation_context = Json::object();
    bool execution_entered = false;
    auto progress = [&](const char* next) {
        stage = next;
        if (created) write_json(output / "startup.json", {{"stage", stage}, {"readiness", readiness}, {"capture", capture_diagnostic}});
    };
    try {
        std::string config_path, plan_path, confirmation, evaluation_path, migration_path, sampling_path, hud_path, manual_path;
        bool dry = false, allowed = false, capture_check = false, current_plan = false, manual = false, derive_defaults = false;
        int recording_duration_ms = 120000;
        std::set<std::string> seen;
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout << "单组1至30次开火按住测试：--plan JSON --dry-run；真实运行另需--config INI --output NEW_DIR "
                         "--allow-physical-output --confirm AUTO_STOP_COUNTERPULSE。需源焦点、全松与独占设备，End/Ctrl+C取消。\n"
                         "离线重评：--evaluate-result RESULT_JSON --output NEW_DIR；不连接设备。\n"
                         "采样校准：--sampling-settings JSONC；真实运行显示模型HUD，dry-run与离线重评不打开HUD。\n"
                         "人工监听：--record-manual --config INI --output NEW_DIR --sampling-settings JSONC；只读KMBOX，默认120秒。\n"
                         "人工重评：--evaluate-manual RECORD_DIR --output NEW_DIR；可传--sampling-settings比较新参数。\n"
                         "公式基线：--derive-defaults --output NEW_DIR；可传--sampling-settings重算，假设与推导分别保存，不连接设备。\n"
                         "常驻显示：--show-hud SAMPLING_ANALYSIS_JSON；只显示已保存数据，关闭窗口退出。\n"
                         "计划迁移：--migrate-plan OLD_JSON --output NEW_JSON；不连接设备。\n"
                         "旧计划纯采集诊断：--plan JSON --config INI --output NEW_DIR --capture-check。\n";
            return 0;
        }
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (!seen.insert(option).second) throw std::runtime_error("参数重复");
            if (option == "--dry-run") dry = true;
            else if (option == "--require-current-plan") current_plan = true;
            else if (option == "--capture-check") capture_check = true;
            else if (option == "--allow-physical-output") allowed = true;
            else if (option == "--record-manual") manual = true;
            else if (option == "--derive-defaults") derive_defaults = true;
            else if (i + 1 < argc && option == "--plan") plan_path = argv[++i];
            else if (i + 1 < argc && option == "--config") config_path = argv[++i];
            else if (i + 1 < argc && option == "--output") output = argv[++i];
            else if (i + 1 < argc && option == "--confirm") confirmation = argv[++i];
            else if (i + 1 < argc && option == "--evaluate-result") evaluation_path = argv[++i];
            else if (i + 1 < argc && option == "--migrate-plan") migration_path = argv[++i];
            else if (i + 1 < argc && option == "--sampling-settings") sampling_path = argv[++i];
            else if (i + 1 < argc && option == "--show-hud") hud_path = argv[++i];
            else if (i + 1 < argc && option == "--evaluate-manual") manual_path = argv[++i];
            else if (i + 1 < argc && option == "--recording-duration-ms") {
                const std::string text = argv[++i]; std::size_t used = 0;
                recording_duration_ms = std::stoi(text, &used);
                if (used != text.size() || recording_duration_ms < 1000 || recording_duration_ms > 600000)
                    throw std::runtime_error("录制时长应为1000至600000毫秒");
            }
            else throw std::runtime_error("无效参数");
        }
        if (int(derive_defaults) + int(manual) + int(!manual_path.empty()) + int(!hud_path.empty()) + int(!migration_path.empty()) + int(!evaluation_path.empty()) > 1)
            throw std::runtime_error("入口模式冲突");
        if (!manual && seen.contains("--recording-duration-ms")) throw std::runtime_error("录制时长仅用于人工录制");
        if (!hud_path.empty()) {
            if (seen.size() != 1) throw std::runtime_error("常驻HUD仅接受报告路径");
            const auto analysis = read_bounded_json(hud_path);
            if (!analysis.is_object() || !analysis.contains("shots") || !analysis["shots"].is_array() ||
                (analysis.value("source", "") != "KMBOX_MONITOR" && analysis.value("source", "") != "COMMAND_ACK_PROXY"))
                throw std::runtime_error("不是有效采样报告");
            const auto settings = parse_sampling_settings(analysis.at("settings"));
            SetConsoleCtrlHandler(control, TRUE);
            CounterpulseHud hud(settings, true);
            auto display = analysis; display["recording"] = false;
            hud.publish(display);
            keep_hud_visible(hud);
            return hud.status()["state"] == "FAILED" ? 2 : 0;
        }
        if (!migration_path.empty()) {
            if (dry || capture_check || allowed || current_plan || !evaluation_path.empty() ||
                !confirmation.empty() || !config_path.empty() || !plan_path.empty() || !sampling_path.empty() || output.empty())
                throw std::runtime_error("计划迁移参数冲突");
            if (std::filesystem::file_size(migration_path) > 16384 || std::filesystem::exists(output))
                throw std::runtime_error("计划过大或输出已存在");
            std::ifstream file(migration_path);
            auto migrated = parse_counterpulse_plan(Json::parse(file, nullptr, true, true));
            if (migrated.schema_version != 2 && migrated.baseline == "stationary")
                migrated.fire_delay_ms = std::max(1, migrated.shot_interval_ms - migrated.shot_hold_ms);
            migrated.schema_version = 2;
            migrated.capture_enabled = false;
            migrated.shot_interval_ms = 0;
            const auto normalized = counterpulse_plan_json(migrated);
            (void)parse_counterpulse_plan(normalized);
            write_json(output, normalized);
            return 0;
        }
        auto sampling_settings = SamplingSettings{};
        if (!sampling_path.empty()) {
            if (std::filesystem::file_size(sampling_path) > 16384) throw std::runtime_error("采样设置过大");
            std::ifstream settings_file(sampling_path);
            sampling_settings = parse_sampling_settings(Json::parse(settings_file, nullptr, true, true));
        }
        if (derive_defaults) {
            if (dry || allowed || capture_check || current_plan || !confirmation.empty() || !config_path.empty() ||
                !plan_path.empty() || output.empty()) throw std::runtime_error("公式基线仅接受新输出目录和可选采样设置");
            DebugRunRequest request; request.mode = DebugRunMode::DeriveDefaults; request.output = output;
            if (!sampling_path.empty()) request.sampling_settings = sampling_settings_json(sampling_settings);
            (void)run_debug(request);
            return 0;
        }
        if (manual || !manual_path.empty()) {
            if (allowed || !confirmation.empty() || capture_check || current_plan || !plan_path.empty())
                throw std::runtime_error("人工监听与重评拒绝物理输出参数");
            if (manual) {
                if (config_path.empty() || (dry ? !output.empty() : output.empty())) throw std::runtime_error("人工录制参数不完整");
                require_explicit_monitor_config(config_path);
                AppConfig config; std::string error;
                if (!load_app_config(config_path, config, error)) throw std::runtime_error("人工录制配置无效");
                (void)manual_monitor_config(config.mouse);
                if (dry) { std::cout << "人工录制配置有效；未连接设备，软件输入固定禁用。\n"; return 0; }
                if (std::filesystem::exists(output) ||
                    (!output.parent_path().empty() && !std::filesystem::is_directory(output.parent_path())))
                    throw std::runtime_error("人工录制需要可创建的新目录");
                SetConsoleCtrlHandler(control, TRUE);
                Resources resources;
                resources.mouse = MouseDeviceFactory::create(manual_monitor_config(config.mouse));
                if (!resources.mouse || !resources.mouse->open() || !resources.mouse->output_owner_exclusive())
                    throw std::runtime_error("人工监听连接或独占失败");
                DebugRunRequest request; request.mode = DebugRunMode::ManualRecording; request.config = config;
                request.device = resources.mouse; request.output = output; request.recording_duration_ms = recording_duration_ms;
                request.sampling_settings = sampling_settings_json(sampling_settings);
                request.hud = std::make_shared<CounterpulseHud>(sampling_settings,true);
                auto result = run_debug(request,{[] { return stopped.load(); },{}});
                resources.mouse->close(); cleanup_finished.store(true);
                keep_hud_visible(*request.hud);
                return result.at("archive").value("success",false) && request.hud->status().value("success",false) ? 0 : 2;
            }
            if (dry || !config_path.empty() || output.empty()) throw std::runtime_error("人工重评参数冲突");
            DebugRunRequest request; request.mode = DebugRunMode::EvaluateManual; request.input = manual_path; request.output = output;
            if (!sampling_path.empty()) request.sampling_settings = sampling_settings_json(sampling_settings);
            (void)run_debug(request);
            return 0;
        }
        if (!evaluation_path.empty()) {
            if (dry || capture_check || allowed || current_plan || !confirmation.empty() || !config_path.empty() ||
                !plan_path.empty() || output.empty()) throw std::runtime_error("离线重评参数冲突");
            DebugRunRequest request; request.mode = DebugRunMode::EvaluateCommands; request.input = evaluation_path; request.output = output;
            if (!sampling_path.empty()) request.sampling_settings = sampling_settings_json(sampling_settings);
            const auto result = run_debug(request);
            return result.value("task_success", false) ? 0 : 2;
        }
        std::ifstream input(plan_path);
        if (!input || std::filesystem::file_size(plan_path) > 16384) throw std::runtime_error("计划不可读或过大");
        // 仅人工计划允许JSONC注释；字段和时序边界仍由正式解析器严格校验。
        const auto document = Json::parse(input, nullptr, true, true);
        const auto plan = parse_counterpulse_plan(document);
        if (current_plan && plan.schema_version != 2) throw std::runtime_error("正式入口只接受版本2计划");
        if (dry) {
            if (capture_check || allowed || !confirmation.empty() || !output.empty() || !config_path.empty()) throw std::runtime_error("dry-run不接受输出授权或配置");
            std::cout << "计划有效；未连接设备或采集，未产生任何输入。\n"; return 0;
        }
        if (capture_check && !plan.capture_enabled) throw std::runtime_error("计划已禁用采集");
        if (capture_check && (allowed || !confirmation.empty())) throw std::runtime_error("纯采集检查拒绝物理授权");
        if ((!capture_check && (!allowed || confirmation != "AUTO_STOP_COUNTERPULSE")) || config_path.empty() || output.empty())
            throw std::runtime_error("缺少真实输入双授权");
        AppConfig config;
        std::string error;
        if (!load_app_config(config_path, config, error)) throw std::runtime_error("配置不可用");
        if (capture_check) {
            // 诊断只接收图像，不创建Mouse或SourceContext；与物理入口共用Evidence。
            constexpr int observation_ms = 14000;
            if (!std::filesystem::create_directory(output)) throw std::runtime_error("需要新诊断目录");
            created = true;
            progress("CAPTURE_CHECK");
            Evidence evidence(config.capture);
            CounterpulsePrerollGate gate(evidence.opened_ns);
            Json samples = Json::array();
            auto next_sample = Clock::now();
            auto observation_end = Clock::time_point{};
            PrerollDecision decision{PrerollState::WAIT, "WAIT_FIRST_FRAME"};
            while (true) {
                decision = evidence.evaluate(gate, std::filesystem::exists(output / "STOP"));
                if (decision.state == PrerollState::READY && observation_end == Clock::time_point{})
                    observation_end = Clock::now() + std::chrono::milliseconds(observation_ms);
                const bool complete = observation_end != Clock::time_point{} && Clock::now() >= observation_end;
                if (Clock::now() >= next_sample || decision.state == PrerollState::FAILED || complete) {
                    auto sample = evidence.snapshot(); sample["reason"] = decision.reason;
                    samples.push_back(sample);
                    next_sample = Clock::now() + std::chrono::milliseconds(100);
                }
                if (decision.state == PrerollState::FAILED || complete) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            capture_diagnostic = evidence.snapshot();
            evidence.finish();
            write_json(output / "capture-check.json", {{"capture", capture_diagnostic}, {"samples", samples},
                {"reason", decision.reason}, {"observation_ms", observation_ms},
                {"success", decision.state == PrerollState::READY}, {"physical_output", false}});
            return decision.state == PrerollState::READY ? 0 : 2;
        }
        if (config.mouse.backend != MouseBackend::KMBOX_NET || config.mouse.kmbox_connect_timeout_ms > 2000)
            throw std::runtime_error("设备或命令超时不适用");
        config.mouse.kmbox_command_timeout_ms = std::min(config.mouse.kmbox_command_timeout_ms, 100);
        config.mouse.allow_send_input = true;
        auto source_config = config.source_context;
        if (const char* token = std::getenv("XEN_SOURCE_CONTEXT_TOKEN")) source_config.token = token;
        if (source_config.token.empty() || source_config.host.empty() || source_config.port == 0)
            throw std::runtime_error("缺少源焦点配置或进程环境凭据");
        if (std::filesystem::exists(output) ||
            (!output.parent_path().empty() && !std::filesystem::is_directory(output.parent_path())))
            throw std::runtime_error("需要可创建的新输出目录");
        if (!SetConsoleCtrlHandler(control, TRUE)) throw std::runtime_error("无法注册取消处理");
        Resources resources;
        resources.mouse = MouseDeviceFactory::create(config.mouse);
        if (!resources.mouse || !resources.mouse->open() || !resources.mouse->output_owner_exclusive())
            throw std::runtime_error("设备独占不可用，请停止生产Runtime");
        DebugRunRequest request;
        request.plan = document; request.sampling_settings = sampling_settings_json(sampling_settings);
        request.output = output; request.config = config; request.device = resources.mouse;
        request.allow_physical_output = allowed; request.confirmation = confirmation;
        if (sampling_settings.hud_enabled) request.hud = std::make_shared<CounterpulseHud>(sampling_settings);
        auto report = run_debug(request, { [&] { return stopped.load() || std::filesystem::exists(output / "STOP"); }, {} });
        resources.mouse->close(); cleanup_finished.store(true);
        return report.value("task_success",false) ? 0 : 2;
    } catch (...) {
        if (created) {
            try { write_json(output / "failure.json", {{"success", false},
                {"reason", failure_reason.empty() ? stage + "_FAILED" : failure_reason},
                {"stage", stage}, {"readiness", readiness}, {"capture", capture_diagnostic},
                {"execution_entered", execution_entered}}); } catch (...) {}
        }
        // 不打印可能包含设备凭据或配置内容的异常。
        std::cerr << "启动/执行失败，检查双授权、计划、源焦点、全松和输出目录；不自动重试。\n";
        return 1;
    }
}
