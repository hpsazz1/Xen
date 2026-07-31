#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "config/config.h"
#include "crash/crash.h"
#include "debug/debug.h"
#include "log/log.h"
#include "overlay/overlay.h"
#include "runtime/runtime.h"

#include <Windows.h>

#ifdef ERROR
#undef ERROR
#endif

#include <string>
#include <vector>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    constexpr const char* kConfigPath = "config.ini";
    AppConfig config;
    std::string app_message;
    std::string config_error;
    if (!load_app_config(kConfigPath, config, config_error)) {
        app_message = config_error + "；请在配置页填写并保存。";
    }

    Log::init(config.log);
    Log::register_module("app", LogLevel::INFO);
    CrashHandler crash_handler;
    const std::string crash_log_dir =
        config.log.log_dir.empty() ? "logs" : config.log.log_dir;
    if (!crash_handler.install(crash_log_dir)) {
        LOG_ERROR("app", "崩溃诊断安装失败");
    }

    Runtime runtime;
    DebugReport debug_report;
    bool debug_session_active = false;
    bool detector_reload_pending = false;
    std::string debug_run_id;
    std::uint64_t debug_segment = 0;
    std::vector<RuntimePipelineSample> pending_debug_samples;
    Overlay overlay;
    if (!overlay.init(config.ui)) {
        LOG_ERROR("app", "Overlay 初始化失败");
        crash_handler.uninstall();
        Log::shutdown();
        return 1;
    }

    const auto drain_debug_samples = [&]() noexcept {
        if (!runtime.drain_pipeline_samples(pending_debug_samples)) return;
        if (debug_session_active) {
            debug_report.ingest(pending_debug_samples);
        }
        pending_debug_samples.clear();
    };
    const auto finish_debug_report = [&]() noexcept {
        drain_debug_samples();
        if (!debug_session_active) return;
        std::string report_error;
        if (!debug_report.finalize(
                runtime.snapshot(), report_error)) {
            LOG_WARN("app", "Debug 报告生成失败: {}", report_error);
        }
        debug_session_active = false;
    };
    const auto start_debug_report = [&](const RuntimeSnapshot& snapshot) {
        DebugReportConfig report_config;
        const std::string segment_id =
            debug_run_id + "-g" +
            std::to_string(snapshot.detector_generation) + "-s" +
            std::to_string(++debug_segment);
        report_config.session_id = segment_id;
        report_config.csv_path =
            "cache/runtime/" + segment_id + ".csv";
        report_config.json_path =
            "cache/runtime/" + segment_id + ".json";
        report_config.model_path = snapshot.active_model_path;
        report_config.provider = snapshot.provider;
        report_config.capture_backend =
            CaptureBackendName(config.capture.backend);
        report_config.mouse_backend = MouseBackendName(config.mouse.backend);
        std::string report_error;
        debug_session_active = debug_report.start(
            report_config, report_error);
        if (!debug_session_active) {
            LOG_WARN("app", "Debug 报告未启动: {}", report_error);
        }
        return debug_session_active;
    };

    while (overlay.pump_messages()) {
        runtime.poll_keyboard();
        drain_debug_samples();
        RuntimeSnapshot snapshot = runtime.snapshot();
        if (detector_reload_pending &&
            snapshot.state != RuntimeState::RUNNING) {
            detector_reload_pending = false;
        } else if (detector_reload_pending &&
                   snapshot.detector_reload_state !=
                       DetectorReloadState::LOADING) {
            // 丢弃加载窗口内的尾部样本。SUCCEEDED 已在指针交换后发布，
            // 因此此后入环的样本只属于当前 active_model_path。
            drain_debug_samples();
            detector_reload_pending = false;
            const bool report_started = start_debug_report(snapshot);
            if (snapshot.detector_reload_state ==
                    DetectorReloadState::SUCCEEDED) {
                app_message = report_started
                    ? "Detector 已热重载，Debug 报告已切换分段。"
                    : "Detector 已热重载，Debug 报告未启用。";
            } else {
                app_message = report_started
                    ? "Detector 重载失败，旧模型继续运行，Debug 报告已恢复。"
                    : "Detector 重载失败，旧模型继续运行。";
            }
        }
        OverlayActions actions;
        if (!overlay.render(snapshot, config, app_message, actions)) {
            LOG_ERROR("app", "Overlay 渲染失败");
            break;
        }

        if (actions.start_requested) {
            if (!runtime.start(config)) {
                app_message = "Runtime 启动失败。";
            } else {
                debug_run_id =
                    std::to_string(GetCurrentProcessId()) + "-" +
                    std::to_string(GetTickCount64());
                debug_segment = 0;
                detector_reload_pending = false;
                debug_session_active = start_debug_report(runtime.snapshot());
                app_message = debug_session_active
                    ? "Runtime 已启动，Debug 报告已启用。"
                    : "Runtime 已启动，Debug 报告未启用。";
            }
        }
        if (actions.stop_requested) {
            runtime.stop();
            finish_debug_report();
            detector_reload_pending = false;
            app_message = "Runtime 已停止。";
        }
        if (actions.reload_detector_requested &&
            !actions.stop_requested) {
            finish_debug_report();
            if (runtime.reload_detector(config.detector)) {
                detector_reload_pending = true;
                app_message = "Detector 正在后台加载。";
            } else {
                // 请求未进入异步加载时仍继续记录当前活动模型。
                snapshot = runtime.snapshot();
                drain_debug_samples();
                const bool report_started = start_debug_report(snapshot);
                app_message = snapshot.detector_reload_error.empty()
                    ? "Detector 重载请求被拒绝。"
                    : report_started
                        ? "Detector 重载请求无效，旧模型继续运行。"
                        : "Detector 重载请求无效，Debug 报告未启用。";
            }
        }
        for (const auto& intent : actions.runtime_intents) {
            if (!runtime.post_intent(intent)) {
                app_message = "意图被安全门拒绝。";
            }
        }
        if (actions.save_config_requested) {
            app_message = save_app_config(
                kConfigPath, config, config_error)
                ? "配置已保存。"
                : config_error;
        }
    }

    runtime.stop();
    finish_debug_report();
    overlay.shutdown();
    crash_handler.uninstall();
    Log::shutdown();
    return 0;
}
