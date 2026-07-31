#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "config/config.h"
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

    LogConfig log_config;
    Log::init(log_config);
    Log::register_module("app", LogLevel::INFO);

    Runtime runtime;
    DebugReport debug_report;
    bool debug_session_active = false;
    std::vector<RuntimePipelineSample> pending_debug_samples;
    Overlay overlay;
    if (!overlay.init(config.ui)) {
        LOG_ERROR("app", "Overlay 初始化失败");
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

    while (overlay.pump_messages()) {
        runtime.poll_keyboard();
        drain_debug_samples();
        const RuntimeSnapshot snapshot = runtime.snapshot();
        OverlayActions actions;
        if (!overlay.render(snapshot, config, app_message, actions)) {
            LOG_ERROR("app", "Overlay 渲染失败");
            break;
        }

        if (actions.start_requested) {
            if (!runtime.start(config)) {
                app_message = "Runtime 启动失败。";
            } else {
                DebugReportConfig report_config;
                report_config.session_id =
                    std::to_string(GetCurrentProcessId()) + "-" +
                    std::to_string(GetTickCount64());
                report_config.model_path = config.detector.model_path;
                report_config.provider = runtime.snapshot().provider;
                report_config.capture_backend =
                    CaptureBackendName(config.capture.backend);
                report_config.mouse_backend =
                    MouseBackendName(config.mouse.backend);
                std::string report_error;
                debug_session_active = debug_report.start(
                    report_config, report_error);
                if (!debug_session_active) {
                    LOG_WARN("app", "Debug 报告未启动: {}", report_error);
                }
                app_message = debug_session_active
                    ? "Runtime 已启动，Debug 报告已启用。"
                    : "Runtime 已启动，Debug 报告未启用。";
            }
        }
        if (actions.stop_requested) {
            runtime.stop();
            finish_debug_report();
            app_message = "Runtime 已停止。";
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
    Log::shutdown();
    return 0;
}
