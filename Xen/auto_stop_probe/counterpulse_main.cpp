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
#include "auto_stop_probe/readiness_internal.h"
#include "auto_stop_probe/preroll_internal.h"
#include "auto_stop_probe/training_evaluation_internal.h"
#include "auto_stop_probe/sampling_analysis_internal.h"
#include "auto_stop_probe/debug_report_internal.h"
#include "auto_stop_probe/counterpulse_hud.h"
#include "auto_stop_probe/manual_recording_internal.h"
#include "auto_stop_probe/manual_labels_internal.h"
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
struct Evidence {
    static constexpr std::size_t frame_limit = 1000;
    static constexpr std::size_t byte_limit = 384ULL * 1024 * 1024;
    struct Frame { cv::Mat pixels; FrameTiming timing; std::int64_t received_ns; };
    std::unique_ptr<ICapture> capture;
    std::vector<Frame> frames;
    std::jthread thread;
    std::atomic<bool> failed{false};
    std::atomic<int> count{0};
    std::atomic<std::int64_t> latest_ns{0};
    std::atomic<std::int64_t> first_ns{0};
    std::atomic<int> last_status{static_cast<int>(CaptureStatus::CLOSED)};
    std::atomic<int> failure_code{0};
    std::atomic<int> capture_phase{0}; // 0等待，1抓帧，2复制/发布
    std::atomic<std::int64_t> grab_started_ns{0}, grab_returned_ns{0};
    std::atomic<int> no_frame_count{0};
    std::int64_t opened_ns = 0;
    mutable std::mutex observation_mutex;
    CaptureBackend backend;
    explicit Evidence(CaptureConfig config) : backend(config.backend) {
        config.enable_d3d11_cuda_interop = false;
        config.enable_d3d11_directml_interop = false;
        capture = create_capture(config);
        if (!capture || !capture->open()) throw std::runtime_error("采集源不可用");
        opened_ns = ns(Clock::now());
        frames.reserve(frame_limit);
        thread = std::jthread([this](std::stop_token stop) {
            std::size_t bytes = 0;
            try {
                while (!stop.stop_requested()) {
                    const auto began = Clock::now();
                    CapturedFrame frame;
                    capture_phase.store(1);
                    grab_started_ns.store(ns(Clock::now()));
                    const auto status = capture->grab(frame);
                    grab_returned_ns.store(ns(Clock::now()));
                    last_status.store(static_cast<int>(status));
                    if (status == CaptureStatus::FRAME) {
                        capture_phase.store(2);
                        const auto size = frame.bgr.total() * frame.bgr.elemSize();
                        // 20发650ms连同前后观察窗留在约16秒容量内；首帧检查整组像素内存预算。
                        if (frame.bgr.empty() || size > byte_limit / frame_limit ||
                            frames.size() >= frame_limit || size > byte_limit - bytes) {
                            failure_code.store(1); failed.store(true); break;
                        }
                        frames.push_back({frame.bgr.clone(), frame.timing, ns(Clock::now())});
                        bytes += size;
                        {
                            std::lock_guard lock(observation_mutex);
                            latest_ns.store(ns(Clock::now()));
                            if (first_ns.load() == 0) first_ns.store(latest_ns.load());
                            count.store(static_cast<int>(frames.size()));
                        }
                    } else if (status != CaptureStatus::NO_FRAME) { failure_code.store(2); failed.store(true); break; }
                    else no_frame_count.fetch_add(1);
                    capture_phase.store(0);
                    std::this_thread::sleep_until(began + std::chrono::milliseconds(16));
                }
            } catch (...) { failure_code.store(3); failed.store(true); }
        });
    }
    void finish() {
        if (thread.joinable()) { thread.request_stop(); thread.join(); }
        if (capture) capture->close();
    }
    ~Evidence() { finish(); }
    Json snapshot() const {
        std::lock_guard lock(observation_mutex);
        const auto first = first_ns.load();
        const auto latest = latest_ns.load();
        return {{"frames", count.load()}, {"failed", failed.load()}, {"failure_code", failure_code.load()},
            {"observed_at_ns", ns(Clock::now())}, {"last_frame_received_ns", latest},
            {"capture_phase", capture_phase.load()}, {"grab_started_ns", grab_started_ns.load()},
            {"grab_returned_ns", grab_returned_ns.load()}, {"no_frame_count", no_frame_count.load()},
            {"last_status", CaptureStatusName(static_cast<CaptureStatus>(last_status.load()))},
            {"first_frame_delay_ns", first ? Json(first - opened_ns) : Json(nullptr)},
            {"last_frame_age_ns", latest ? Json(ns(Clock::now()) - latest) : Json(nullptr)}};
    }
    PrerollDecision evaluate(CounterpulsePrerollGate& gate, bool cancelled) const {
        std::lock_guard lock(observation_mutex);
        return gate.evaluate(ns(Clock::now()), first_ns.load(), latest_ns.load(), count.load(), failed.load(), cancelled);
    }
    void save(const std::filesystem::path& directory) {
        finish();
        std::filesystem::create_directory(directory);
        std::ofstream csv(directory / "frames.csv");
        csv.exceptions(std::ios::badbit | std::ios::failbit);
        csv << "status,error,png,captured_at_ns,receive_ns,source_time_at_ns,source_time_valid,source_clock_uncertainty_ms,time_basis\n";
        for (std::size_t i = 0; i < frames.size(); ++i) {
            const auto& frame = frames[i];
            const auto file = "frame_" + std::to_string(i) + ".png";
            if (!cv::imwrite((directory / file).string(), frame.pixels)) throw std::runtime_error("帧保存失败");
            // captured_at保持Capture原语义，映射源时刻另列，不能暗中冒充曝光时间。
            csv << "FRAME,," << file << ',' << ns(frame.timing.captured_at) << ',' << frame.received_ns << ','
                << ns(frame.timing.source_time_at) << ',' << frame.timing.source_time_timing_valid << ','
                << frame.timing.source_clock_uncertainty_ms << ',' << CaptureBackendName(backend) << '\n';
        }
    }
};
struct Resources {
    std::shared_ptr<IMouseController> mouse;
    source_context::SourceContextClient focus;
    ~Resources() { focus.stop(); if (mouse) mouse->close(); cleanup_finished.store(true); }
};
// 复用生产适配器；独立订阅保留真实monitor包，不把输出ACK写成物理输入。
class MonitorTraining {
public:
    MonitorTraining(std::shared_ptr<IMouseController> mouse, const std::filesystem::path& directory)
        : mouse_(std::move(mouse)) {
        if (!mouse_->set_input_report_subscription(true)) throw std::runtime_error("输入报告订阅失败");
        auto source = std::make_shared<runtime::detail::InputTrainingSource>(mouse_);
        if (!session_.start(directory, {}, [source] { return source->read(); })) {
            mouse_->freeze_input_reports();
            throw std::runtime_error("输入报告归档启动失败");
        }
    }
    ~MonitorTraining() { stop(); }
    void stop() noexcept { mouse_->freeze_input_reports(); session_.stop(); }
    bool recording() const noexcept {
        const auto state = session_.snapshot();
        return state && state->status == input_training::Status::RECORDING;
    }
    Json report() const { return training_snapshot_json(*session_.snapshot(), "KMBOX_MONITOR"); }
private:
    std::shared_ptr<IMouseController> mouse_;
    input_training::Session session_;
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
void write_analysis_files(const std::filesystem::path& directory, const Json& analysis) {
    write_json(directory / "sampling-analysis.json", analysis);
    const auto path = directory / "debug-report.html";
    auto temporary = path; temporary += ".writing";
    {
        std::ofstream file(temporary, std::ios::binary);
        file.exceptions(std::ios::badbit | std::ios::failbit);
        file << render_counterpulse_debug_report(analysis);
        file.flush();
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("调试报告原子保存失败");
}
void write_sampling_report(const std::filesystem::path& directory, const Json& report) {
    write_analysis_files(directory, analyze_counterpulse_sampling(report));
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
void describe_manual_quality(Json& analysis) {
    if (!analysis.value("received_events", 0ULL) || analysis.value("invalid_events", 0ULL) ||
        analysis.value("gap_count", 0ULL)) analysis["archive_complete"] = false;
    analysis["quality_issues"] = Json::array();
    for (const auto* key : {"gap_count", "invalid_events", "missing_start_count"})
        if (analysis.value(key, 0ULL)) analysis["quality_issues"].push_back(std::string("MANUAL_") + key);
    if (!analysis.value("received_events", 0ULL)) analysis["quality_issues"].push_back("NO_INPUT_EVENTS");
    if (!analysis.value("archive_complete", false)) analysis["quality_issues"].push_back("ARCHIVE_INCOMPLETE");
    for (const auto& shot : analysis["shots"]) if (!shot.value("complete_hold", false)) {
        analysis["quality_issues"].push_back("INCOMPLETE_HOLD"); break;
    }
    analysis["analysis_complete"] = analysis["quality_issues"].empty();
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
        bool dry = false, allowed = false, capture_check = false, current_plan = false, manual = false;
        int recording_duration_ms = 120000;
        std::set<std::string> seen;
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout << "单组1至30次开火按住测试：--plan JSON --dry-run；真实运行另需--config INI --output NEW_DIR "
                         "--allow-physical-output --confirm AUTO_STOP_COUNTERPULSE。需源焦点、全松与独占设备，End/Ctrl+C取消。\n"
                         "离线重评：--evaluate-result RESULT_JSON --output NEW_DIR；不连接设备。\n"
                         "采样校准：--sampling-settings JSONC；真实运行显示模型HUD，dry-run与离线重评不打开HUD。\n"
                         "人工监听：--record-manual --config INI --output NEW_DIR --sampling-settings JSONC；只读KMBOX，默认120秒。\n"
                         "人工重评：--evaluate-manual RECORD_DIR --output NEW_DIR；可传--sampling-settings比较新参数。\n"
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
        if (int(manual) + int(!manual_path.empty()) + int(!hud_path.empty()) + int(!migration_path.empty()) + int(!evaluation_path.empty()) > 1)
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
                if (!std::filesystem::create_directory(output)) throw std::runtime_error("人工录制必须使用新目录");
                created = true; stage = "MANUAL_RECORDING";
                write_json(output / "sampling-settings.json", sampling_settings_json(sampling_settings));
                write_json(output / "labels.json", {{"schema_version", 1}, {"recording_id", output.filename().string()},
                    {"qualified_shot_ranges", Json::array()}, {"rejected_shot_ranges", Json::array()},
                    {"note", "仅按用户明确反馈填写，未标记不是合格"}});
                SetConsoleCtrlHandler(control, TRUE);
                CounterpulseHud hud(sampling_settings, true);
                std::cout << "人工录制中：自行操作KMBOX键鼠；点击HUD停止录制，结果继续悬浮。\n";
                auto recorded = record_manual_monitor(config.mouse, output, sampling_settings, recording_duration_ms, hud,
                    [] { return stopped.load(); });
                recorded.analysis["archive_complete"] = recorded.archive.value("success", false) &&
                    recorded.archive.value("received_events", 0ULL) != 0 &&
                    recorded.archive.value("dropped_events", 0ULL) == 0 && recorded.archive.value("invalid_events", 0ULL) == 0;
                describe_manual_quality(recorded.analysis);
                recorded.analysis = apply_manual_labels(std::move(recorded.analysis), read_bounded_json(output / "labels.json", 16384));
                recorded.analysis["hud_status"] = hud.status();
                write_json(output / "training-evaluation.json", recorded.archive);
                write_analysis_files(output, recorded.analysis);
                cleanup_finished.store(true);
                std::cout << "录制已停止并保存；设备已关闭，HUD保留至关闭窗口。\n";
                keep_hud_visible(hud);
                return recorded.archive.value("success", false) && hud.status().value("success", false) ? 0 : 2;
            }
            if (dry || !config_path.empty() || output.empty()) throw std::runtime_error("人工重评参数冲突");
            const auto original = read_bounded_json(std::filesystem::path(manual_path) / "sampling-settings.json", 16384);
            if (sampling_path.empty()) sampling_settings = parse_sampling_settings(original);
            const auto original_analysis = read_bounded_json(std::filesystem::path(manual_path) / "sampling-analysis.json");
            auto analysis = finalize_manual_archive(std::filesystem::path(manual_path) / "raw",
                sampling_settings, original_analysis.at("time_ns").get<std::int64_t>());
            analysis["recording_id"] = std::filesystem::path(manual_path).filename().string();
            analysis["original_sampling_settings"] = original;
            analysis["sampling_settings_overridden"] = !sampling_path.empty();
            describe_manual_quality(analysis);
            analysis = apply_manual_labels(std::move(analysis), read_bounded_json(std::filesystem::path(manual_path) / "labels.json", 16384));
            if (!std::filesystem::create_directory(output)) throw std::runtime_error("人工重评必须使用新目录");
            created = true;
            write_analysis_files(output, analysis);
            return 0;
        }
        if (!evaluation_path.empty()) {
            if (dry || capture_check || allowed || current_plan || !confirmation.empty() || !config_path.empty() ||
                !plan_path.empty() || output.empty()) throw std::runtime_error("离线重评参数冲突");
            if (std::filesystem::file_size(evaluation_path) > 4 * 1024 * 1024)
                throw std::runtime_error("命令报告过大");
            std::ifstream file(evaluation_path);
            auto commands = Json::parse(file);
            if (!sampling_path.empty()) {
                commands["original_sampling_settings"] = commands.value("sampling_settings", Json(nullptr));
                commands["sampling_settings"] = sampling_settings_json(sampling_settings);
                commands["sampling_settings_overridden"] = true;
            }
            if (!std::filesystem::create_directory(output)) throw std::runtime_error("重评需要新目录");
            created = true;
            const auto evaluation = evaluate_counterpulse_training(commands, output / "command-training");
            write_json(output / "training-evaluation.json", {{"schema_version", 1},
                {"command_ack", evaluation}, {"monitor", nullptr}, {"physical_output", false}});
            write_sampling_report(output, commands);
            return evaluation.value("success", false) ? 0 : 2;
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
        if (config.mouse.backend != MouseBackend::KMBOX_NET ||
            config.mouse.kmbox_connect_timeout_ms > 2000) throw std::runtime_error("设备或命令超时不适用");
        config.mouse.kmbox_command_timeout_ms = std::min(config.mouse.kmbox_command_timeout_ms, 100);
        // 凭据仅进内存，既有生产配置的启用状态不被写回。
        auto source_config = config.source_context;
        source_config.enabled = true;
        if (const char* token = std::getenv("XEN_SOURCE_CONTEXT_TOKEN")) source_config.token = token;
        if (source_config.token.empty() || source_config.host.empty() || source_config.port == 0)
            throw std::runtime_error("缺少源焦点配置或进程环境凭据");
        if (!std::filesystem::create_directory(output)) throw std::runtime_error("需要不存在的输出目录");
        created = true;
        write_json(output / "plan.json", document);
        if (!SetConsoleCtrlHandler(control, TRUE)) throw std::runtime_error("无法注册取消处理");
        Resources resources;
        progress("SOURCE_START");
        if (!resources.focus.start(source_config)) throw std::runtime_error("源焦点服务不可用");
        progress("DEVICE_OPEN");
        config.mouse.allow_send_input = true;
        resources.mouse = MouseDeviceFactory::create(config.mouse);
        if (!resources.mouse || !resources.mouse->open() || !resources.mouse->output_owner_exclusive())
            throw std::runtime_error("设备独占不可用，请停止生产Runtime");
        const auto ready_deadline = Clock::now() + std::chrono::seconds(15);
        std::uint64_t focus_session = 0;
        CounterpulseReadinessAccumulator readiness_gate;
        auto next_status = Clock::now();
        progress("READINESS");
        std::cout << "等待源程序聚焦和键鼠全松，最多15秒；就绪后自动执行一组，End/Ctrl+C停止。\n";
        while (Clock::now() < ready_deadline && !stopped.load()) {
            const auto focus = resources.focus.snapshot();
            InputSnapshot physical;
            const bool polled = resources.mouse->poll_input(physical);
            readiness = readiness_gate.update(focus, physical, polled,
                stopped.load() || std::filesystem::exists(output / "STOP"), ns(Clock::now()));
            if (Clock::now() >= next_status || readiness["ready"].get<bool>()) {
                progress("READINESS");
                next_status = Clock::now() + std::chrono::milliseconds(250);
            }
            if (readiness["flags"]["cancelled"].get<bool>()) { stopped.store(true); break; }
            if (readiness["ready"].get<bool>()) { focus_session = focus.session_id; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (focus_session == 0 || stopped.load()) {
            failure_reason = stopped.load() ? "USER_CANCELLED" : "READINESS_TIMEOUT";
            progress("READINESS");
            throw std::runtime_error("未就绪或已取消");
        }
        progress("EVENT_SUBSCRIPTION");
        if (!resources.mouse->set_wasd_event_subscription(true)) throw std::runtime_error("原始输入事件不可用");
        std::unique_ptr<Evidence> evidence;
        if (plan.capture_enabled) {
            progress("CAPTURE_OPEN");
            evidence = std::make_unique<Evidence>(config.capture);
        } else {
            capture_diagnostic = {{"enabled", false}, {"reason", "DISABLED_BY_PLAN"}};
        }
        std::unique_ptr<MonitorTraining> training;
        auto cancel = [&]() -> std::string {
            if (stopped.load() || std::filesystem::exists(output / "STOP")) return "USER_STOP";
            if (training && !training->recording()) return "INPUT_TRAINING_STOPPED";
            const auto focus = resources.focus.snapshot();
            if (!focus.available || !focus.focused || focus.session_id != focus_session) return "SOURCE_FOCUS";
            if (!cleanup_finished.load()) {
                InputSnapshot physical;
                const bool polled = resources.mouse->poll_input(physical);
                const auto permission = evaluate_counterpulse_readiness(focus, physical, polled, false);
                if (permission["flags"]["cancelled"].get<bool>()) { stopped.store(true); return "USER_STOP"; }
                if (permission["flags"]["monitor_invalid"].get<bool>()) return "MONITOR_INVALID";
                if (permission["flags"]["physical_keys_held"].get<bool>()) return "PHYSICAL_INPUT";
            }
            if (evidence && evidence->failed.load()) {
                if (cancellation_context.empty()) cancellation_context = evidence->snapshot();
                return "CAPTURE_FAILED";
            }
            if (evidence && evidence->latest_ns.load() != 0 && ns(Clock::now()) - evidence->latest_ns.load() > 100000000) {
                if (cancellation_context.empty()) cancellation_context = evidence->snapshot();
                return "CAPTURE_STALE";
            }
            return {};
        };
        if (evidence) {
            CounterpulsePrerollGate preroll(evidence->opened_ns);
            std::string previous_phase;
            while (true) {
                const auto pre_failure = cancel();
                const auto decision = evidence->evaluate(preroll, !pre_failure.empty());
                const std::string phase = pre_failure.empty() ? decision.reason : pre_failure;
                if (phase != previous_phase) {
                    capture_diagnostic = evidence->snapshot();
                    progress(phase.c_str());
                    previous_phase = phase;
                }
                if (!pre_failure.empty() || decision.state == PrerollState::FAILED) {
                    capture_diagnostic = evidence->snapshot();
                    failure_reason = pre_failure.empty() ? decision.reason : pre_failure;
                    throw std::runtime_error("射前证据或焦点无效");
                }
                if (decision.state == PrerollState::READY) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        progress("EXECUTION");
        if (document.value("schema_version", 0) == 2)
            training = std::make_unique<MonitorTraining>(resources.mouse, output / "monitor-training");
        execution_entered = true;
        std::unique_ptr<CounterpulseHud> hud;
        if (sampling_settings.hud_enabled) hud = std::make_unique<CounterpulseHud>(sampling_settings);
        auto report = execute_counterpulse(*resources.mouse, plan, cancel, {}, true,
            [&](const Json& command) { if (hud) hud->observe(command); });
        report["sampling_settings"] = sampling_settings_json(sampling_settings);
        if (training) training->stop();
        resources.mouse->close();
        cleanup_finished.store(true);
        report["command_timeout_ms"] = config.mouse.kmbox_command_timeout_ms;
        report["cancellation_context"] = cancellation_context;
        report["capture_enabled"] = plan.capture_enabled;
        report["capture_complete"] = false;
        report["capture_frames"] = 0;
        report["scene_settled"] = nullptr;
        // 无采集模式只保存命令结果；不创建采集源，不等待图像或执行图像门禁。
        progress(evidence ? "SAVE_EVIDENCE" : "SAVE_RESULT");
        write_json(output / "result.json", report);
        if (training) {
            const auto command_evaluation = evaluate_counterpulse_training(report, output / "command-training");
            const auto monitor_evaluation = training->report();
            report["monitor_evidence"] = {{"source", "KMBOX_MONITOR"},
                {"received_events", monitor_evaluation.value("received_events", Json(nullptr))},
                {"samples_present", monitor_evaluation.value("samples_present", Json(nullptr))}};
            write_json(output / "training-evaluation.json", {{"schema_version", 1},
                {"command_ack", command_evaluation}, {"monitor", monitor_evaluation},
                {"game_shot_stability", nullptr}, {"scene_settled", nullptr}});
            report["training_archive_success"] = command_evaluation.value("success", false) &&
                monitor_evaluation.value("success", false);
            write_json(output / "result.json", report);
        }
        if (evidence) {
            const auto post_end = Clock::now() + std::chrono::milliseconds(std::max(300, plan.cycle_budget_ms()));
            while (Clock::now() < post_end && cancel().empty()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            const auto post_failure = cancel();
            const bool post_complete = Clock::now() >= post_end && post_failure.empty();
            evidence->save(output / "frames");
            report["capture_frames"] = evidence->count.load();
            report["post_roll_failure"] = post_failure;
            report["last_frame_received_ns"] = evidence->latest_ns.load();
            report["capture_complete"] = !evidence->failed.load() && post_complete;
            report["cancellation_context"] = cancellation_context;
        } else {
            report["capture_status"] = "DISABLED_BY_PLAN";
        }
        write_json(output / "result.json", report);
        if (hud) {
            // 设备已关闭；仅保留显示，让末次短按的延迟样本按真实时间到期。
            std::this_thread::sleep_for(std::chrono::milliseconds(sampling_settings.fire_sample_delay_ms + 40));
            hud->finish(report);
            const auto hud_deadline = Clock::now() + std::chrono::milliseconds(80);
            while (Clock::now() < hud_deadline) {
                const auto state = hud->status();
                if (state["state"] == "FAILED" ||
                    (state["state"] == "FINISHED_VISIBLE" && state["queued_commands"] == 0)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        report["hud_status"] = hud ? hud->status() : Json{{"state", "DISABLED"}, {"physical_validation_passed", false}};
        write_json(output / "result.json", report);
        write_sampling_report(output, report);
        std::cout << "组结束，执行结果与采样调试报告已保存；停稳效果由人工观察判断。\n";
        return report.value("success", false) && report.value("training_archive_success", true) &&
            (!plan.capture_enabled || report.value("capture_complete", false)) ? 0 : 2;
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
