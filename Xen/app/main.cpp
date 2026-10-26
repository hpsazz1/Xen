#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "app/input_router.h"
#include "app/model_catalog_internal.h"
#include "app/release_contract_internal.h"
#include "app/startup_internal.h"
#include "app/report_lifecycle_internal.h"
#include "app/workspace_actions_internal.h"
#include "config/config.h"
#include "crash/crash.h"
#include "debug/debug.h"
#include "debug/session_archive.h"
#include "debug_session/debug_session.h"
#include "keyboard/keyboard.h"
#include "log/log.h"
#include "overlay/overlay.h"
#include "runtime/runtime.h"
#include "model_workspace/model_workspace.h"
#include "voice_switch/voice_switch.h"
#include "voice_switch/controller.h"
#include "grenade_web/server.h"

#include <Windows.h>

#ifdef ERROR
#undef ERROR
#endif

#include <filesystem>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

void append_message(std::string& message, const std::string& addition) {
    if (addition.empty()) return;
    if (!message.empty()) message += "；";
    message += addition;
}

void show_startup_error(void*, const std::string& message) {
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, message.data(),
        static_cast<int>(message.size()), nullptr, 0);
    std::wstring wide(
        length > 0 ? static_cast<std::size_t>(length) : 0, L'\0');
    if (length > 0) {
        MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, message.data(),
            static_cast<int>(message.size()), wide.data(), length);
    } else {
        wide = L"Xen 启动失败";
    }
    MessageBoxW(
        nullptr, wide.c_str(), L"Xen", MB_OK | MB_ICONERROR);
}

bool same_mouse_config(const MouseConfig& first,
                       const MouseConfig& second) noexcept {
    return first.backend == second.backend &&
           first.allow_send_input == second.allow_send_input &&
           first.kmbox_ip == second.kmbox_ip &&
           first.kmbox_port == second.kmbox_port &&
           first.kmbox_uuid == second.kmbox_uuid &&
           first.kmbox_connect_timeout_ms == second.kmbox_connect_timeout_ms &&
           first.kmbox_command_timeout_ms == second.kmbox_command_timeout_ms &&
           first.makcu_port == second.makcu_port &&
           first.makcu_baud_rate == second.makcu_baud_rate &&
           first.makcu_connect_timeout_ms == second.makcu_connect_timeout_ms &&
           first.makcu_command_timeout_ms == second.makcu_command_timeout_ms;
}

bool same_gsi_config(const weapon::GsiConfig& a, const weapon::GsiConfig& b) noexcept {
    return a.enabled == b.enabled && a.bind_address == b.bind_address && a.port == b.port &&
        a.allowed_peer_ipv4 == b.allowed_peer_ipv4 && a.ttl_ms == b.ttl_ms &&
        a.request_timeout_ms == b.request_timeout_ms && a.max_body_bytes == b.max_body_bytes &&
        a.max_clock_skew_ms == b.max_clock_skew_ms;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    app::detail::ReleaseEnvironment release_environment;
    std::string release_error;
    if (!app::detail::load_release_environment(
            release_environment, release_error) ||
        !app::detail::apply_release_working_directory(
            release_environment, release_error)) {
        MessageBoxA(
            nullptr, release_error.c_str(), "Xen",
            MB_OK | MB_ICONERROR);
        return 1;
    }
    const std::string config_path = app::detail::path_to_utf8(
        release_environment.root / L"config.ini");
    AppConfig config;
    std::string app_message;
    std::string config_error;
    bool config_created = false;
    if (!load_or_create_app_config(
            config_path, config, config_created, config_error)) {
        app_message = config_error + "；请在配置页填写并保存。";
    } else if (config_created) {
        app_message = "未找到 config.ini，已按代码默认值生成；物理输出保持禁用。";
    }
    bool diagnostics_preference = config.runtime.diagnostics_enabled;
    wchar_t diagnostics_override[2]{};
    bool temporary_diagnostics = GetEnvironmentVariableW(
        L"XEN_RUNTIME_DIAGNOSTICS", diagnostics_override, 2) == 1 && diagnostics_override[0] == L'1';
    if (temporary_diagnostics) {
        config.runtime.diagnostics_enabled = true;
        append_message(app_message, "本次调试临时开启详细记录，日常偏好保留");
    }
    const auto save_current_config = [&](std::string& error) {
        AppConfig saved = config;
        saved.runtime.diagnostics_enabled = diagnostics_preference;
        return save_app_config(config_path, saved, error);
    };

    std::filesystem::path model_directory;
    OverlayModelCatalog model_catalog;
    std::string model_error;
    const bool model_directory_ready = app::detail::prepare_model_directory_at_root(
        release_environment.root, model_directory, model_error);
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

    OverlayBackendCatalog backend_catalog;
    backend_catalog.backends = release_environment.available_backends;
    if (!app::detail::backend_allowed(
            backend_catalog.backends, config.detector.backend)) {
        append_message(app_message, "配置请求的推理后端未被发布清单授权");
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

    app::detail::AppStartupBoundary startup_boundary(
        {nullptr, show_startup_error});
    Log::init(config.log);
    if (!startup_boundary.observe_log_initialization(Log::initialized())) {
        Log::shutdown();
        return startup_boundary.finish(0);
    }
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

    auto shared_gsi = std::make_shared<weapon::GsiReceiver>();
    auto active_gsi_config = config.gsi;
    if (active_gsi_config.enabled && !shared_gsi->start(active_gsi_config)) append_message(app_message, "GSI 接收失败：" + shared_gsi->last_error());
    Runtime runtime(shared_gsi);
    auto data_collector = std::make_shared<data_collection::Collector>();
    runtime.set_data_collector(data_collector);
    model_workspace::Workspace model_workspace(data_collector);
    model_workspace::Settings workspace_settings;
    std::string workspace_error;
    if (!model_workspace.initialize(model_directory.parent_path(),
                                    workspace_settings, workspace_error)) {
        append_message(app_message, workspace_error);
    }
    std::shared_ptr<IMouseController> input_device;
    if (auto created_mouse = MouseDeviceFactory::create(config.mouse)) {
        input_device = std::shared_ptr<IMouseController>(
            std::move(created_mouse));
    }
    if (!input_device || !input_device->open()) {
        append_message(app_message, input_device
            ? "物理键鼠监听后端初始化失败"
            : "物理键鼠监听后端创建失败");
    }
    MouseConfig active_input_config = config.mouse;
    KeyboardConfig active_keyboard_config = config.keyboard;
    KeyboardListener keyboard(config.keyboard, input_device);
    if (!keyboard.open()) {
        append_message(app_message, "全局快捷键初始化失败");
    }
    DebugReport debug_report;
    SessionArchive session_archive;
    bool session_archive_active = false;
    app::detail::ReportBoundary report_boundary;
    app::detail::ReportFinalization report_finalization;
    app::detail::ReportResume report_resume;
    auto archive_trigger_poll = std::chrono::steady_clock::time_point{};
    bool archive_drain_failed = false;
    bool debug_session_active = false;
    bool release_restart_requested = false;
    app::detail::ReportLifecycle report_lifecycle(debug_session_active, session_archive_active, report_finalization);
    std::optional<DetectorConfig> pending_detector_config;
    std::string debug_run_id;
    std::optional<AimConfig> debug_aim_config;
    std::optional<AutoStopConfig> debug_auto_stop_config;
    std::optional<TriggerConfig> debug_trigger_config;
    std::optional<RecoilConfig> debug_recoil_config;
    std::uint64_t debug_segment = 0;
    std::vector<RuntimePipelineSample> pending_debug_samples;
    Overlay overlay;
    debug_session::Session debug_workspace;
    voice_switch::Server voice_server;
    voice_switch::Controller voice_controller(voice_server,debug_workspace);
    const auto configure_voice = [&] {
        voice_controller.cancel();
        voice_server.stop();
        if (active_keyboard_config.grenade_assist_enabled || !active_keyboard_config.voice_virtual_keys.empty()) {
            voice_switch::Config voice_config;
            voice_config.bind_address = active_gsi_config.bind_address;
            voice_config.allowed_peer_ipv4 = active_gsi_config.allowed_peer_ipv4;
            if (!voice_server.start(voice_config))
                append_message(app_message,"主机方案通道未启动：" + voice_server.snapshot().error);
        }
    };
    configure_voice();
    debug_session::Request catalog_request; catalog_request.mode = debug_session::Mode::MOVEMENT_TEST;
    debug_workspace.dispatch(debug_session::Action::LIST_MOVEMENT_PLANS,catalog_request,{});
    if (!startup_boundary.observe_overlay_initialization(
            overlay.init(config.ui))) {
        LOG_ERROR("app", "Overlay 初始化失败");
        crash_handler.uninstall();
        Log::shutdown();
        return startup_boundary.finish(0);
    }

    // 手机只读页面由辅机应用持有，直接共享 GSI；不依赖 Runtime 或主机语音工具。
    grenade_web::Server mobile_web;
    std::optional<grenade_web::SelectionRequest> mobile_selection;
    std::string observed_reference_path;
    std::optional<std::filesystem::file_time_type> observed_reference_time;
    auto reference_poll_at=std::chrono::steady_clock::time_point{};
    if (!mobile_web.start({},shared_gsi)) {
        append_message(app_message,"手机道具清单未启动：" + mobile_web.snapshot().error);
    } else {
        for (const auto& url : mobile_web.snapshot().urls) {
            LOG_INFO("app","手机道具清单：{}",url);
            if (url.find("127.0.0.1") == std::string::npos)
                append_message(app_message,"手机道具清单：" + url);
        }
    }

    const auto drain_debug_samples = [&](bool final_drain = false, const RuntimeSnapshot* frozen = nullptr) noexcept {
        if (!debug_session_active && !session_archive_active) return;
        pending_debug_samples.clear();
        if (!runtime.drain_pipeline_samples(pending_debug_samples)) {
            if (!archive_drain_failed) {
                LOG_WARN("app", "Runtime样本读取失败，归档覆盖存在未确定缺口；不会重交旧帧。");
                if (session_archive_active) session_archive.mark("样本读取失败：覆盖缺口待核实");
                if (session_archive_active) session_archive.note_gap("样本读取失败：覆盖缺口待核实");
            }
            archive_drain_failed = true;
            pending_debug_samples.clear();
        }
        if (debug_session_active) {
            debug_report.ingest(pending_debug_samples);
        }
        if (session_archive_active) {
            auto archive_snapshot = frozen ? *frozen : runtime.snapshot();
            const auto now = std::chrono::steady_clock::now();
            if (final_drain || now - archive_trigger_poll >= std::chrono::milliseconds(100)) {
                if (!frozen) archive_snapshot.trigger_execution_log = runtime.trigger_execution_log(report_boundary.archive_after_event);
                else std::erase_if(archive_snapshot.trigger_execution_log.events,
                    [&](const auto& event) { return event.sequence <= report_boundary.archive_after_event; });
                archive_trigger_poll = now;
            }
            if (session_archive.submit(pending_debug_samples, archive_snapshot) &&
                !archive_snapshot.trigger_execution_log.events.empty()) {
                report_boundary.archive_after_event = archive_snapshot.trigger_execution_log.events.back().sequence;
            }
        }
        pending_debug_samples.clear();
    };
    const auto report_snapshot = [&]() {
        auto snapshot = runtime.snapshot();
        snapshot.trigger_execution_log = runtime.trigger_execution_log();
        snapshot.recoil_execution_log = runtime.recoil_execution_log();
        snapshot.output_arbitration = runtime.output_arbitration();
        snapshot.output_arbitration_available = debug_auto_stop_config &&
            (debug_auto_stop_config->enabled || debug_trigger_config->enabled || debug_recoil_config->enabled);
        return snapshot;
    };
    const auto finish_debug_report = [&](std::optional<RuntimeSnapshot> frozen = {}) noexcept {
        const bool finished = report_finalization.finish(debug_session_active, session_archive_active,
            report_boundary, debug_report, session_archive,
            [&] { return frozen ? std::move(*frozen) : report_snapshot(); },
            [&](const auto& snapshot) { drain_debug_samples(true, &snapshot); });
        if (report_finalization.failed()) {
            LOG_WARN("app", "Debug 报告生成失败: {}", report_finalization.error());
        }
        return finished;
    };
    const auto start_debug_report = [&](const RuntimeSnapshot& snapshot) {
        if (!config.runtime.diagnostics_enabled || snapshot.state != RuntimeState::RUNNING) return false;
        if (report_finalization.pending()) {
            app_message = "旧报告保存失败；请修复磁盘问题后点击停止或启动重试，退出会丢失未保存数据。";
            return false;
        }
        report_boundary.begin_segment(runtime);
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
        report_config.aim_config = debug_aim_config;
        report_config.auto_stop_config = debug_auto_stop_config;
        report_config.trigger_config = debug_trigger_config;
        report_config.recoil_config = debug_recoil_config;
        std::string report_error;
        debug_session_active = debug_report.start(
            report_config, report_error);
        SessionArchiveConfig archive_config;
        archive_config.directory = "cache/runtime/" + segment_id + "-archive";
        archive_config.report_config = report_config;
        archive_config.trigger_sequence_baseline = report_boundary.archive_after_event;
        std::string archive_error;
        session_archive_active = session_archive.start(archive_config, archive_error);
        archive_trigger_poll = {};
        if (!session_archive_active) LOG_WARN("app", "全程归档未启动: {}", archive_error);
        if (!debug_session_active) {
            LOG_WARN("app", "Debug 报告未启动: {}", report_error);
        }
        RecoilArchiveConfig recoil_archive;
        recoil_archive.acquisition_run_id = debug_run_id;
        recoil_archive.directory = std::filesystem::path("cache/runtime") / (segment_id + "-recoil-batches");
        runtime.set_recoil_archive(recoil_archive);
        runtime.set_diagnostics_enabled(debug_session_active || session_archive_active);
        return debug_session_active;
    };
    const auto finish_reports_async = [&]() {
        if (report_lifecycle.busy()) return;
        runtime.set_diagnostics_enabled(false);
        auto frozen = report_snapshot();
        report_lifecycle.finish_async([&, frozen = std::move(frozen)]() mutable {
            return app::detail::finish_reports_after_archive(runtime, std::move(frozen), finish_debug_report);
        });
    };

    const auto stop_runtime_session = [&](bool retry_report = true) {
        if (report_lifecycle.stopping()) return;
        report_resume.cancel();
        pending_detector_config.reset();
        const auto requested = report_lifecycle.stop(runtime, [&] { return finish_debug_report(); }, retry_report);
        if (requested == app::detail::ReportStopRequest::IDLE && !report_finalization.failed())
            app_message = "Runtime 已停止。";
        else if (requested == app::detail::ReportStopRequest::STARTED)
            app_message = "正在后台停止Runtime并保存报告。";
    };

    const auto start_runtime_session = [&]() {
        if (report_lifecycle.busy() || debug_workspace.busy() || debug_workspace.snapshot()->cleanup_unknown || overlay.background_busy()) {
            app_message = "请先结束调试任务并确认设备清理，再启动Runtime。";
            return;
        }
        report_lifecycle.cancel_restart();
        if (report_lifecycle.defer_runtime_restart(stop_runtime_session)) return;
        if (debug_workspace.snapshot()->movement_test_mode!=debug_session::Mode::GRENADE_TEST)
            debug_workspace.invalidate_repeat();
        else debug_workspace.cancel_grenade("Runtime启动，旧对齐结果已撤销");
        if (release_environment.managed &&
            app::detail::runtime_for_backend(config.detector.backend) !=
                release_environment.runtime_id) {
            app_message = "该后端属于其他隔离运行时，请先保存配置以安全重启。";
            return;
        }
        AppConfig runtime_config = config;
        runtime_config.gsi = active_gsi_config;
        if (!resolve_detector_config(runtime_config.detector)) {
            app_message = model_error;
            return;
        }
        debug_run_id =
            std::to_string(GetCurrentProcessId()) + "-" +
            std::to_string(GetTickCount64());
        if (!runtime.start(runtime_config, input_device)) {
            app_message = "Runtime 启动失败。";
            return;
        }
        debug_aim_config = runtime_config.aim;
        debug_auto_stop_config = runtime_config.auto_stop;
        debug_trigger_config = runtime_config.trigger;
        debug_recoil_config = runtime_config.recoil;
        // Runtime 的扳机使用启动时 Aim 类别映射；保存有效值，不保存 UI 后续编辑值。
        debug_trigger_config->person_class_ids = runtime_config.aim.person_class_ids;
        debug_trigger_config->head_class_ids = runtime_config.aim.head_class_ids;
        debug_segment = 0;
        archive_drain_failed = false;
        report_resume.cancel();
        debug_session_active = start_debug_report(runtime.snapshot());
        app_message = debug_session_active
            ? "运行已启动，详细记录已开启。"
            : "运行已启动，详细记录已关闭。";
        if (temporary_diagnostics) append_message(app_message, "详细记录仅本次调试开启，日常偏好未改");
    };

    const auto collect_report_result = [&](bool succeeded, app::detail::ReportJobKind kind) {
        if (succeeded) app_message = kind == app::detail::ReportJobKind::STOP
            ? "Runtime已停止，后台报告处理结束。" : "详细记录已结束；已有资料保留。";
        else {
            report_resume.cancel();
            pending_detector_config.reset();
            if (*report_lifecycle.error()) {
                app_message = std::string("后台任务失败，已取消自动重启/切段操作。原因：") + report_lifecycle.error();
            } else {
                app_message = "旧报告保存失败，已取消自动重启/切段并保留当前进程内的数据。"
                              "请修复磁盘问题后点击停止或启动重试；退出会丢失未保存数据。原因：" + report_finalization.error();
            }
        }
    };

    while (overlay.pump_messages(true)) {
        debug_workspace.update_map(shared_gsi->map_snapshot());
        debug_workspace.set_theme(config.ui.theme);
        debug_workspace.poll();
        report_lifecycle.poll(debug_workspace.busy(), stop_runtime_session, collect_report_result);
        if (overlay.close_requested()) report_lifecycle.cancel_restart();
        if (overlay.close_requested()) {
            debug_workspace.request_shutdown();
            overlay.cancel_background();
            if (!debug_workspace.busy() && !overlay.background_busy()) stop_runtime_session(false);
            if (!debug_workspace.busy() && !overlay.background_busy() && !report_lifecycle.busy()) break;
            app_message = "正在结束调试任务并回收资源；设备清理未知时仍会明确报告。";
        }
        const KeyboardPollResult keyboard_poll = keyboard.poll();
        // health 不属于可被热键绑定 UI 吞掉的语义事件；先投递后再生成
        // Snapshot 和渲染，保证武装控件与实际 SafetyGate 同帧一致。
        app::detail::route_input_health(runtime, keyboard_poll);
        if (!report_lifecycle.busy() && !report_finalization.pending()) drain_debug_samples();
        RuntimeSnapshot snapshot = runtime.snapshot();
        if (pending_detector_config && !report_lifecycle.busy()) {
            const auto requested = std::move(*pending_detector_config);
            pending_detector_config.reset();
            const bool accepted = runtime.reload_detector(requested);
            report_resume.reload_requested(accepted);
            snapshot = runtime.snapshot();
            if (!accepted) {
                report_resume.diagnostics_changed(config.runtime.diagnostics_enabled);
                app_message = "模型重载请求未接受，当前模型继续运行。";
            }
        }
        const auto resumed = report_resume.poll(snapshot,
            report_lifecycle.busy() || report_lifecycle.stop_pending(),
            [&](const auto& current) { return start_debug_report(current); });
        if (resumed.reload_finished) {
            if (snapshot.detector_reload_state ==
                    DetectorReloadState::SUCCEEDED) {
                app_message = resumed.report_started
                    ? "Detector 已热重载，Debug 报告已切换分段。"
                    : "Detector 已热重载，Debug 报告未启用。";
            } else {
                app_message = resumed.report_started
                    ? "Detector 重载失败，旧模型继续运行，Debug 报告已恢复。"
                    : "Detector 重载失败，旧模型继续运行。";
            }
        }
        OverlayActions actions;
        const auto preview = snapshot.preview_enabled
            ? runtime.preview_frame()
            : nullptr;
        const auto archive_status = session_archive.status();
        const bool overlay_rendered = overlay.render(
            snapshot, preview, model_catalog, backend_catalog,
            config, workspace_settings, model_workspace.poll(&workspace_settings), app_message, actions, &keyboard_poll,
            debug_workspace.snapshot().get(), &archive_status);
        if (!startup_boundary.observe_overlay_render(
                overlay_rendered,
                overlay.last_error())) {
            LOG_ERROR("app", "Overlay 渲染失败");
            break;
        }

        if (actions.log_level_changed) {
            Log::set_global_level(config.log.global_level);
            app_message = config.log.global_level == LogLevel::OFF
                ? "已停止新增日志；已有记录保留。保存配置后保留此设置。"
                : "日志输出等级已立即更新。保存配置后保留此设置。";
        }
        if (actions.diagnostics_enabled_changed) {
            temporary_diagnostics = false;
            diagnostics_preference = config.runtime.diagnostics_enabled;
            report_resume.diagnostics_changed(config.runtime.diagnostics_enabled);
            if (!config.runtime.diagnostics_enabled && !report_lifecycle.stopping()) finish_reports_async();
            app_message = config.runtime.diagnostics_enabled
                ? "详细记录已开启；保存配置后保留设置。"
                : "已停止新增详细记录，正在后台结束存量；保存配置后保留设置。";
        }

        // 辅助页身法参数可在运行中调试，其他配置仍遵循原启停规则。
        if (snapshot.state == RuntimeState::RUNNING && !report_lifecycle.busy())
            runtime.set_movement_config(config.movement);

        const auto keyboard_routing = app::detail::route_keyboard_events(
            runtime, keyboard_poll, actions.hotkey_capture_consumed);
        const bool emergency_pressed =
            app::detail::debug_emergency_requested(keyboard_routing.emergency_pressed, actions.runtime_intents);
        const bool runtime_toggle_pressed =
            keyboard_routing.runtime_toggle_pressed;
        if ((actions.anomaly_mark_requested || keyboard_routing.anomaly_mark_pressed) && !report_lifecycle.busy()) {
            app_message = !config.runtime.diagnostics_enabled ? "详细运行记录已关闭，未保存异常标记。" :
                session_archive.mark(actions.anomaly_mark_requested ? "界面标记" : "快捷键标记")
                ? "异常已标记，继续游玩即可；归档将关联前后各30秒数据。"
                : "异常标记未保存，请检查归档状态。";
        }
        if (actions.debug_plan_edited) debug_workspace.invalidate_repeat();

        if (actions.preview_enabled_changed &&
            !runtime.set_preview_enabled(actions.preview_enabled)) {
            app_message = "ROI 预览通道切换失败。";
        }

        const auto make_debug_context = [&] {
            debug_session::Context context;
            context.config = config;
            context.config.gsi = active_gsi_config;
            // 已打开设备的参数才是事实；未保存草稿不能冒充连接已重配。
            context.config.mouse = active_input_config;
            context.config.keyboard = active_keyboard_config;
            context.config.keyboard.debug_test_enabled = active_keyboard_config.debug_test_enabled && config.keyboard.debug_test_enabled;
            context.config.keyboard.grenade_assist_enabled = active_keyboard_config.grenade_assist_enabled && config.keyboard.grenade_assist_enabled;
            context.device = input_device;
            context.map = shared_gsi->map_snapshot();
            context.runtime_idle = !report_lifecycle.stopping() &&
                (snapshot.state == RuntimeState::STOPPED || snapshot.state == RuntimeState::FAILED);
            context.runtime = !report_lifecycle.stopping() && snapshot.state==RuntimeState::RUNNING ? &runtime : nullptr;
            context.input_recording_idle = !snapshot.training ||
                (!input_training::is_busy(snapshot.training->status) &&
                 snapshot.training->status != input_training::Status::STOP_TIMEOUT);
            context.cleanup_known = !snapshot.auto_stop.cleanup_unknown && !snapshot.trigger.faulted &&
                (context.runtime || (!snapshot.trigger.button_may_be_down && !snapshot.output_armed)) &&
                !debug_workspace.snapshot()->cleanup_unknown;
            return context;
        };
        if (actions.movement_draft_changed)
            debug_workspace.update_movement_draft(actions.debug_request,make_debug_context());
        debug_workspace.maintain_grenade(make_debug_context(),keyboard_poll.input_healthy && !actions.hotkey_capture_consumed);
        if (keyboard_routing.grenade_cancel_pressed) debug_workspace.cancel_grenade();
        if (emergency_pressed || actions.training_stop_requested || actions.stop_requested) {
            if (emergency_pressed || actions.stop_requested) report_lifecycle.cancel_restart();
            debug_workspace.cancel();
            // 停止同时撤销文件处理后的准备意图，后台结果回迁不能恢复下一步。
            overlay.cancel_background();
        }
        if (!emergency_pressed && !keyboard_routing.grenade_cancel_pressed && !keyboard_routing.grenade_menu_pressed && !actions.stop_requested &&
            !runtime_toggle_pressed && !actions.start_requested && !actions.save_config_requested &&
            !overlay.close_requested() && !report_lifecycle.busy() && !overlay.background_busy() &&
            actions.debug_action == debug_session::Action::NONE) {
            if (keyboard_routing.grenade_align_pressed)
                debug_workspace.align_grenade(make_debug_context());
            else if (keyboard_routing.grenade_throw_pressed)
                debug_workspace.throw_grenade(make_debug_context());
        }
        if (keyboard_routing.debug_test_pressed && actions.debug_action == debug_session::Action::NONE &&
            !keyboard_routing.grenade_align_pressed && !keyboard_routing.grenade_throw_pressed &&
            !keyboard_routing.grenade_menu_pressed &&
            !emergency_pressed && !keyboard_routing.grenade_cancel_pressed && !actions.stop_requested &&
            !runtime_toggle_pressed && !actions.start_requested && !overlay.close_requested()) {
            if (report_lifecycle.stopping() || overlay.background_busy()) {
                app_message = "后台停止或弹道任务未结束，本次测试快捷键忽略，不排队。";
            } else {
                auto context = make_debug_context();
                context.config.keyboard = active_keyboard_config;
                context.config.keyboard.debug_test_enabled = active_keyboard_config.debug_test_enabled && config.keyboard.debug_test_enabled;
                app_message = debug_workspace.repeat(context) ? "快捷键测试已提交，等待本组测试的源端就绪条件。" :
                    "快捷键测试未执行：" + debug_workspace.snapshot()->message;
            }
        }
        if (!overlay.close_requested() && !emergency_pressed && !actions.stop_requested) {
            if (actions.training_start_requested) {
                app_message = !report_lifecycle.stopping() && !overlay.background_busy() && debug_workspace.record_inputs(runtime,actions.training_directory,make_debug_context())
                    ? "已提交原始输入记录；开始和结束归档均在后台处理。" : "已有任务或记录未结束，不能开始第二组。";
            } else if (actions.training_load_requested) {
                app_message = !report_lifecycle.stopping() && !overlay.background_busy() && debug_workspace.load_inputs(runtime,actions.training_load_path)
                    ? "正在后台读取离线记录，不连接设备。" : "请先结束当前调试任务。";
            }
            if (actions.debug_action != debug_session::Action::NONE) {
                const bool cancel_or_display = actions.debug_action == debug_session::Action::CANCEL ||
                    actions.debug_action == debug_session::Action::STOP_MOVEMENT_RECORDING ||
                    actions.debug_action == debug_session::Action::SHOW_HUD || actions.debug_action == debug_session::Action::HIDE_HUD;
                if (((!report_lifecycle.stopping() && !overlay.background_busy()) || cancel_or_display) && debug_workspace.dispatch(actions.debug_action,
                    actions.debug_request,make_debug_context(),actions.debug_prepared_id,
                    actions.debug_allow_physical_output,actions.debug_confirmation))
                    app_message = "调试请求已接收。";
                else app_message = "调试请求未接受：" + debug_workspace.snapshot()->message;
            }
        }

        app::detail::route_workspace_action(model_workspace, actions.workspace_action,
            workspace_settings, runtime, snapshot, config.detector, resolve_detector_config, app_message);

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
            report_lifecycle.cancel_restart();
            if (debug_workspace.busy()) {
                debug_workspace.cancel();
                report_lifecycle.defer_stop();
                runtime.post_intent({RuntimeIntentType::DISARM_OUTPUT, true});
                app_message = "正在停止调试任务；生产Runtime不会并发接管设备。";
            } else stop_runtime_session();
        } else if (!emergency_pressed && (actions.start_requested || toggle_requests_start)) {
            if (model_workspace.poll().job_running || overlay.close_requested()) {
                app_message = "请先结束离线数据或训练作业，再启动Runtime。";
            } else {
                start_runtime_session();
            }
        }
        if (actions.reload_detector_requested && !report_lifecycle.stopping() && !debug_workspace.busy() &&
            !actions.stop_requested) {
            DetectorConfig detector_config = config.detector;
            const bool wrong_release_runtime =
                release_environment.managed &&
                app::detail::runtime_for_backend(detector_config.backend) !=
                    release_environment.runtime_id;
            const bool detector_config_resolved = !wrong_release_runtime &&
                resolve_detector_config(detector_config);
            if (wrong_release_runtime) {
                app_message =
                    "该后端属于其他隔离运行时，请保存配置以安全重启。";
            }
            if (!detector_config_resolved) {
                if (!wrong_release_runtime) app_message = model_error;
            } else {
                finish_reports_async();
                pending_detector_config = detector_config;
                app_message = "正在结束旧模型记录，随后切换模型。";
            }
        }
        for (const auto& intent : actions.runtime_intents) {
            if (!app::detail::allow_runtime_intent(intent, report_lifecycle.stopping() || report_lifecycle.stop_pending() ||
                overlay.close_requested() || (debug_workspace.busy() && debug_workspace.snapshot()->physical))) continue;
            if (!runtime.post_intent(intent)) {
                app_message = "意图被安全门拒绝。";
            }
        }
        const bool device_change_requires_stop = (!same_mouse_config(active_input_config, config.mouse) ||
            !same_gsi_config(active_gsi_config,config.gsi)) &&
            (runtime.snapshot().state != RuntimeState::STOPPED || report_lifecycle.finishing());
        if (actions.save_config_requested && (report_lifecycle.stopping() || debug_workspace.busy() || overlay.background_busy() || device_change_requires_stop)) {
            app_message = "请先结束后台调试与弹道任务，再保存或切换设备配置。";
        } else if (actions.save_config_requested && !overlay.close_requested()) {
            // 保存草稿不要求模型文件已经到位；实际启动/应用模型时再解析存在性。
            if (save_current_config(config_error)) {
                voice_controller.cancel();
                debug_workspace.cancel_grenade("配置已保存，请重新选择与对齐");
                if (!same_gsi_config(active_gsi_config,config.gsi)) {
                    shared_gsi->stop(); active_gsi_config = config.gsi;
                    if (active_gsi_config.enabled && !shared_gsi->start(active_gsi_config))
                        append_message(app_message,"GSI 接收失败：" + shared_gsi->last_error());
                    debug_workspace.update_map(shared_gsi->map_snapshot());
                }
                const bool cross_runtime_restart =
                    release_environment.managed &&
                    app::detail::runtime_for_backend(config.detector.backend) !=
                        release_environment.runtime_id;
                if (cross_runtime_restart) {
                    if (emergency_pressed || actions.stop_requested || toggle_requests_stop) {
                        report_lifecycle.cancel_restart();
                        app_message = "配置已保存；本轮停止或急停已取消跨运行时重启。";
                    } else {
                        report_lifecycle.request_application_restart(stop_runtime_session);
                        app_message = "配置已保存，正在结束旧报告；保存成功后重启到所选运行时。";
                    }
                    continue;
                }
                const bool input_backend_changed =
                    !same_mouse_config(active_input_config, config.mouse);
                bool input_reloaded = true;
                bool input_rollback_persisted = true;
                if (input_backend_changed) {
                    debug_workspace.invalidate_repeat();
                    stop_runtime_session();
                    keyboard.close();
                    if (input_device) input_device->close();
                    std::shared_ptr<IMouseController> replacement_device;
                    if (auto created = MouseDeviceFactory::create(config.mouse)) {
                        replacement_device = std::shared_ptr<IMouseController>(
                            std::move(created));
                    }
                    input_reloaded = replacement_device &&
                                     replacement_device->open();
                    if (input_reloaded) {
                        input_device = std::move(replacement_device);
                        active_input_config = config.mouse;
                    } else {
                        // 新配置失败时恢复旧会话，避免保存操作让急停/F8 一并失效。
                        config.mouse = active_input_config;
                        std::string rollback_error;
                        if (!save_current_config(rollback_error)) {
                            input_rollback_persisted = false;
                            LOG_ERROR("app", "键鼠后端配置回滚写入失败: {}",
                                      rollback_error);
                        }
                        if (auto rollback = MouseDeviceFactory::create(
                                active_input_config)) {
                            input_device = std::shared_ptr<IMouseController>(
                                std::move(rollback));
                            if (!input_device->open()) input_device.reset();
                        } else {
                            input_device.reset();
                        }
                    }
                }
                KeyboardListener replacement(config.keyboard, input_device);
                if (replacement.open()) {
                    keyboard.close();
                    keyboard = std::move(replacement);
                    active_keyboard_config = config.keyboard;
                    if (input_reloaded) {
                        app_message =
                            "配置已保存，键鼠后端与快捷键已立即生效。";
                        if (temporary_diagnostics) append_message(app_message, "详细记录仅本次临时开启，日常偏好未改");
                    } else if (input_rollback_persisted) {
                        app_message =
                            "新键鼠后端打开失败，配置与监听均已恢复旧会话。";
                    } else {
                        app_message = "新键鼠后端打开失败，当前监听已恢复旧会话，"
                                      "但配置文件回滚失败。";
                    }
                } else {
                    app_message = "配置已保存，但快捷键重载失败。";
                }
                configure_voice();
            } else {
                app_message = config_error;
            }
        }
        auto interface_context = make_debug_context();
        const bool interface_blocked =
            emergency_pressed || keyboard_routing.grenade_cancel_pressed || actions.stop_requested || actions.training_stop_requested ||
            actions.start_requested || runtime_toggle_pressed || actions.save_config_requested ||
            actions.debug_action == debug_session::Action::CANCEL || overlay.close_requested() ||
            report_lifecycle.busy() || report_lifecycle.stop_pending() || overlay.background_busy();
        if (mobile_selection) {
            const bool current=!interface_blocked && !actions.hotkey_capture_consumed &&
                mobile_web.selection_current(*mobile_selection);
            if (const auto completed=debug_workspace.finish_voice(interface_context,current)) {
                mobile_web.finish_selection(*mobile_selection,*completed ? debug_workspace.snapshot() : nullptr,
                    *completed ? "方案已切换" : "方案或游戏状态已变化，请重新点选");
                mobile_selection.reset();
            }
        }
        if (!mobile_selection) if (auto selected=mobile_web.poll_selection()) {
            debug_session::Request request;request.mode=debug_session::Mode::GRENADE_TEST;
            request.load_path=selected->plan_path;
            if (!interface_blocked && !actions.hotkey_capture_consumed && mobile_web.selection_current(*selected) &&
                debug_workspace.select_grenade(request,interface_context)) mobile_selection=std::move(selected);
            else mobile_web.finish_selection(*selected,nullptr,"当前操作未结束，或道具辅助尚未启用，请稍后点选");
        }
        // 重采原子发布后只检查小清单时间，撤销旧对齐；图片由手机按资源版本刷新。
        const auto reference_now=std::chrono::steady_clock::now();
        if (reference_now>=reference_poll_at) {
            reference_poll_at=reference_now+std::chrono::seconds(1);
            const auto selected=debug_workspace.snapshot()->grenade_selected_path;
            std::error_code error;
            const auto plan=std::filesystem::u8path(selected);
            const auto reference=plan.parent_path().parent_path()/"references"/plan.stem()/"reference.json";
            const auto modified=std::filesystem::last_write_time(reference,error);
            if (selected!=observed_reference_path) {observed_reference_path=selected;observed_reference_time.reset();}
            else if (!error && observed_reference_time && *observed_reference_time!=modified)
                debug_workspace.cancel_grenade("参考图已更新，请按上键重新找点");
            if (!error) observed_reference_time=modified;
        }
        voice_controller.tick(interface_context,keyboard_poll,active_keyboard_config,
            interface_blocked,actions.hotkey_capture_consumed);
        // F10只读当前已保存选择；每轮心跳撤销旧许可，不在网页线程访问可变App状态。
        interface_context.config.keyboard = active_keyboard_config;
        mobile_web.publish_capture(debug_workspace.snapshot(),interface_context.config,
            (interface_context.runtime_idle || interface_context.runtime) && interface_context.input_recording_idle && interface_context.cleanup_known &&
            !debug_workspace.busy() && !interface_blocked && !actions.hotkey_capture_consumed,
            "请先结束当前道具动作或配置操作，再按F10采集");
        // 本轮键盘与界面停止/急停优先，不能先自动重启再清除刚收到的急停。
        const auto restart_target = report_lifecycle.take_restart(debug_workspace.busy() || overlay.background_busy() ||
                model_workspace.poll().job_running,
                emergency_pressed || actions.stop_requested || toggle_requests_stop || overlay.close_requested());
        if (restart_target == app::detail::ReportRestartTarget::RUNTIME)
            start_runtime_session();
        else if (restart_target == app::detail::ReportRestartTarget::APPLICATION) {
            release_restart_requested = true;
            break;
        }
    }

    // 渲染失败和系统退出同样经过屏障；不可先关闭后台仍使用的设备或日志。
    mobile_web.stop();
    voice_controller.cancel();
    voice_server.stop();
    debug_workspace.request_shutdown();
    overlay.cancel_background();
    for (;;) {
        debug_workspace.poll();
        overlay.poll_background();
        report_lifecycle.poll(true, stop_runtime_session, collect_report_result);
        if (!debug_workspace.busy() && !overlay.background_busy()) stop_runtime_session(false);
        if (!debug_workspace.busy() && !overlay.background_busy() && !report_lifecycle.busy()) break;
        overlay.pump_messages(true);
        MsgWaitForMultipleObjectsEx(0, nullptr, 16, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    runtime.stop();
    shared_gsi->stop();
    model_workspace.shutdown();
    if (!report_finalization.failed()) finish_debug_report();
    if (report_finalization.failed()) LOG_WARN("app", "退出时仍有未保存报告，内存数据无法保留到下次启动: {}", report_finalization.error());
    keyboard.close();
    if (input_device) input_device->close();
    overlay.shutdown();
    crash_handler.uninstall();
    Log::shutdown();
    return startup_boundary.finish(
        release_restart_requested
            ? static_cast<int>(app::detail::kReleaseRestartExitCode)
            : 0);
}
