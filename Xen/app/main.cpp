#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "app/model_catalog_internal.h"
#include "config/config.h"
#include "crash/crash.h"
#include "debug/debug.h"
#include "keyboard/keyboard.h"
#include "log/log.h"
#include "overlay/overlay.h"
#include "runtime/runtime.h"

#include <Windows.h>

#ifdef ERROR
#undef ERROR
#endif

#include <filesystem>
#include <string>
#include <vector>

namespace {

void append_message(std::string& message, const std::string& addition) {
    if (addition.empty()) return;
    if (!message.empty()) message += "；";
    message += addition;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    constexpr const char* kConfigPath = "config.ini";
    AppConfig config;
    std::string app_message;
    std::string config_error;
    if (!load_app_config(kConfigPath, config, config_error)) {
        app_message = config_error + "；请在配置页填写并保存。";
    }

    std::filesystem::path model_directory;
    OverlayModelCatalog model_catalog;
    std::string model_error;
    const bool model_directory_ready =
        app::detail::prepare_program_model_directory(
            model_directory, model_error);
    if (model_directory_ready) {
        model_catalog.directory =
            app::detail::path_to_utf8(model_directory);
    } else {
        append_message(app_message, model_error);
    }

    const std::string configured_model_path = config.detector.model_path;
    config.detector.model_path =
        app::detail::normalize_model_selection(configured_model_path);
    if (!configured_model_path.empty() &&
        config.detector.model_path.empty()) {
        append_message(
            app_message,
            "配置中的模型不是 ONNX 文件，请从模型列表重新选择");
    }

    const auto refresh_models = [&]() noexcept {
        if (!model_directory_ready) return false;
        std::vector<std::string> refreshed;
        if (!app::detail::list_models(
                model_directory, refreshed, model_error)) {
            return false;
        }
        model_catalog.model_names.swap(refreshed);
        if (config.detector.model_path.empty() &&
            model_catalog.model_names.size() == 1) {
            config.detector.model_path = model_catalog.model_names.front();
        }
        return true;
    };
    if (model_directory_ready && !refresh_models()) {
        append_message(app_message, model_error);
    }

    const auto resolve_detector_config = [&](DetectorConfig& detector) {
        std::string resolved_path;
        if (!model_directory_ready ||
            !app::detail::resolve_model_selection(
                model_directory, detector.model_path,
                resolved_path, model_error)) {
            if (!model_directory_ready && model_error.empty()) {
                model_error = "模型目录不可用";
            }
            return false;
        }
        detector.model_path.swap(resolved_path);
        return true;
    };

    Log::init(config.log);
    Log::register_module("app", LogLevel::INFO);
    if (model_directory_ready) {
        LOG_INFO(
            "app", "模型目录已准备: {}, 可用模型={}",
            model_catalog.directory, model_catalog.model_names.size());
    } else {
        LOG_ERROR("app", "模型目录不可用: {}", model_error);
    }
    CrashHandler crash_handler;
    const std::string crash_log_dir =
        config.log.log_dir.empty() ? "logs" : config.log.log_dir;
    if (!crash_handler.install(crash_log_dir)) {
        LOG_ERROR("app", "崩溃诊断安装失败");
    }

    Runtime runtime;
    KeyboardListener keyboard(config.keyboard);
    if (!keyboard.open()) {
        append_message(app_message, "全局快捷键初始化失败");
    }
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

    const auto start_runtime_session = [&]() {
        AppConfig runtime_config = config;
        if (!resolve_detector_config(runtime_config.detector)) {
            app_message = model_error;
            return;
        }
        if (!runtime.start(runtime_config)) {
            app_message = "Runtime 启动失败。";
            return;
        }
        debug_run_id =
            std::to_string(GetCurrentProcessId()) + "-" +
            std::to_string(GetTickCount64());
        debug_segment = 0;
        detector_reload_pending = false;
        debug_session_active = start_debug_report(runtime.snapshot());
        app_message = debug_session_active
            ? "Runtime 已启动，Debug 报告已启用。"
            : "Runtime 已启动，Debug 报告未启用。";
    };
    const auto stop_runtime_session = [&]() {
        runtime.stop();
        finish_debug_report();
        detector_reload_pending = false;
        app_message = "Runtime 已停止。";
    };

    while (overlay.pump_messages()) {
        const std::vector<KeyboardEvent> keyboard_events = keyboard.poll();
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
        const auto preview = snapshot.preview_enabled
            ? runtime.preview_frame()
            : nullptr;
        if (!overlay.render(
                snapshot, preview, model_catalog,
                config, app_message, actions)) {
            LOG_ERROR("app", "Overlay 渲染失败");
            break;
        }

        bool emergency_pressed = false;
        bool runtime_toggle_pressed = false;
        if (!actions.hotkey_capture_consumed) {
            for (const auto& event : keyboard_events) {
                if (event.type == KeyboardEventType::AIM_HOLD_CHANGED) {
                    runtime.post_intent({
                        RuntimeIntentType::AIM_HOLD_CHANGED, event.active});
                } else if (event.type ==
                           KeyboardEventType::EMERGENCY_STOP) {
                    emergency_pressed = true;
                    runtime.post_intent({
                        RuntimeIntentType::EMERGENCY_STOP, true});
                } else if (event.type == KeyboardEventType::RUNTIME_TOGGLE) {
                    runtime_toggle_pressed = true;
                }
            }
        }

        if (actions.preview_enabled_changed &&
            !runtime.set_preview_enabled(actions.preview_enabled)) {
            app_message = "ROI 预览通道切换失败。";
        }

        if (actions.refresh_models_requested) {
            if (refresh_models()) {
                LOG_INFO(
                    "app", "模型目录已刷新: {}, 可用模型={}",
                    model_catalog.directory,
                    model_catalog.model_names.size());
                app_message = model_catalog.model_names.empty()
                    ? "模型目录已刷新，未发现 ONNX 模型。"
                    : "模型目录已刷新。";
            } else {
                LOG_WARN("app", "模型目录刷新失败: {}", model_error);
                app_message = model_error;
            }
        }

        const bool toggle_requests_stop = runtime_toggle_pressed &&
            !emergency_pressed &&
            (snapshot.state == RuntimeState::RUNNING ||
             snapshot.state == RuntimeState::STARTING);
        const bool toggle_requests_start = runtime_toggle_pressed &&
            !emergency_pressed &&
            (snapshot.state == RuntimeState::STOPPED ||
             snapshot.state == RuntimeState::FAILED);
        if (actions.stop_requested ||
            (!actions.start_requested && toggle_requests_stop)) {
            stop_runtime_session();
        } else if (actions.start_requested || toggle_requests_start) {
            start_runtime_session();
        }
        if (actions.reload_detector_requested &&
            !actions.stop_requested) {
            DetectorConfig detector_config = config.detector;
            const bool detector_config_resolved =
                resolve_detector_config(detector_config);
            if (!detector_config_resolved) {
                app_message = model_error;
            } else {
                finish_debug_report();
            }
            if (detector_config_resolved &&
                runtime.reload_detector(detector_config)) {
                detector_reload_pending = true;
                app_message = "Detector 正在后台加载。";
            } else if (detector_config_resolved) {
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
            DetectorConfig detector_config = config.detector;
            if (!resolve_detector_config(detector_config)) {
                app_message = model_error;
            } else if (save_app_config(
                           kConfigPath, config, config_error)) {
                KeyboardListener replacement(config.keyboard);
                if (replacement.open()) {
                    keyboard.close();
                    keyboard = std::move(replacement);
                    app_message = "配置已保存，快捷键已立即生效。";
                } else {
                    app_message = "配置已保存，但快捷键重载失败。";
                }
            } else {
                app_message = config_error;
            }
        }
    }

    runtime.stop();
    finish_debug_report();
    keyboard.close();
    overlay.shutdown();
    crash_handler.uninstall();
    Log::shutdown();
    return 0;
}
