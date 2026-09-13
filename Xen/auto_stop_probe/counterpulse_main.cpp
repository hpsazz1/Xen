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
    std::unique_ptr<IMouseController> mouse;
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
        std::string config_path, plan_path, confirmation;
        bool dry = false, allowed = false, capture_check = false;
        std::set<std::string> seen;
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout << "单组7至20发反向时长测试：--plan JSON --dry-run；真实运行另需--config INI --output NEW_DIR "
                         "--allow-physical-output --confirm AUTO_STOP_COUNTERPULSE。需源焦点、全松与独占设备，End/Ctrl+C取消。\n"
                         "纯采集诊断：--plan JSON --config INI --output NEW_DIR --capture-check；不连接键鼠，拒绝物理授权。\n";
            return 0;
        }
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (!seen.insert(option).second) throw std::runtime_error("参数重复");
            if (option == "--dry-run") dry = true;
            else if (option == "--capture-check") capture_check = true;
            else if (option == "--allow-physical-output") allowed = true;
            else if (i + 1 < argc && option == "--plan") plan_path = argv[++i];
            else if (i + 1 < argc && option == "--config") config_path = argv[++i];
            else if (i + 1 < argc && option == "--output") output = argv[++i];
            else if (i + 1 < argc && option == "--confirm") confirmation = argv[++i];
            else throw std::runtime_error("无效参数");
        }
        std::ifstream input(plan_path);
        if (!input || std::filesystem::file_size(plan_path) > 16384) throw std::runtime_error("计划不可读或过大");
        const auto document = Json::parse(input);
        const auto plan = parse_counterpulse_plan(document);
        if (dry) {
            if (capture_check || allowed || !confirmation.empty() || !output.empty() || !config_path.empty()) throw std::runtime_error("dry-run不接受输出授权或配置");
            std::cout << "计划有效；未连接设备或采集，未产生任何输入。\n"; return 0;
        }
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
        progress("CAPTURE_OPEN");
        Evidence evidence(config.capture);
        auto cancel = [&]() -> std::string {
            if (stopped.load() || std::filesystem::exists(output / "STOP")) return "USER_STOP";
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
            if (evidence.failed.load()) {
                if (cancellation_context.empty()) cancellation_context = evidence.snapshot();
                return "CAPTURE_FAILED";
            }
            if (evidence.latest_ns.load() != 0 && ns(Clock::now()) - evidence.latest_ns.load() > 100000000) {
                if (cancellation_context.empty()) cancellation_context = evidence.snapshot();
                return "CAPTURE_STALE";
            }
            return {};
        };
        CounterpulsePrerollGate preroll(evidence.opened_ns);
        std::string previous_phase;
        while (true) {
            const auto pre_failure = cancel();
            const auto decision = evidence.evaluate(preroll, !pre_failure.empty());
            const std::string phase = pre_failure.empty() ? decision.reason : pre_failure;
            if (phase != previous_phase) {
                capture_diagnostic = evidence.snapshot();
                progress(phase.c_str());
                previous_phase = phase;
            }
            if (!pre_failure.empty() || decision.state == PrerollState::FAILED) {
                capture_diagnostic = evidence.snapshot();
                failure_reason = pre_failure.empty() ? decision.reason : pre_failure;
                throw std::runtime_error("射前证据或焦点无效");
            }
            if (decision.state == PrerollState::READY) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        progress("EXECUTION");
        execution_entered = true;
        auto report = execute_counterpulse(*resources.mouse, plan, cancel, {}, true);
        resources.mouse->close();
        cleanup_finished.store(true);
        report["command_timeout_ms"] = config.mouse.kmbox_command_timeout_ms;
        report["cancellation_context"] = cancellation_context;
        // 输出已清理后再编码落盘；失败结果保留，不把图像缺失当作可重射。
        progress("SAVE_EVIDENCE");
        write_json(output / "result.json", report);
        // 慢单发也保留末枪完整候选观察窗，与离线分析的shot_interval_ms一致。
        const auto post_end = Clock::now() + std::chrono::milliseconds(std::max(300, plan.shot_interval_ms));
        while (Clock::now() < post_end && cancel().empty()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const auto post_failure = cancel();
        const bool post_complete = Clock::now() >= post_end && post_failure.empty();
        evidence.save(output / "frames");
        report["capture_frames"] = evidence.count.load();
        report["post_roll_failure"] = post_failure;
        report["last_frame_received_ns"] = evidence.latest_ns.load();
        report["capture_complete"] = !evidence.failed.load() && post_complete;
        report["cancellation_context"] = cancellation_context;
        report["scene_settled"] = nullptr;
        write_json(output / "result.json", report);
        std::cout << "组结束，命令与图像已保存；尚无停稳/推荐时间结论。\n";
        return report.value("success", false) && report.value("capture_complete", false) ? 0 : 2;
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
