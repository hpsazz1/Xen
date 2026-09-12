#include "runtime/runtime.h"

#include "log/log.h"
#include "runtime/runtime_internal.h"
#include "runtime/aim_frame_internal.h"
#include "auto_stop/auto_stop_worker.h"
#include "trigger/trigger_worker.h"
#include "recoil/recoil_worker.h"
#include "recoil/recoil_store.h"
#include <unordered_map>
#include <cstdlib>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

double percentile(std::vector<double> values, double quantile) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const double position = quantile * static_cast<double>(values.size() - 1);
    const std::size_t lower = static_cast<std::size_t>(std::floor(position));
    const std::size_t upper = static_cast<std::size_t>(std::ceil(position));
    if (lower == upper) return values[lower];
    const double fraction = position - static_cast<double>(lower);
    return values[lower] * (1.0 - fraction) + values[upper] * fraction;
}

} // namespace

struct Runtime::Impl {
    static constexpr std::size_t kDebugSampleCapacity = 4096;
    using DebugSampleRing = runtime::detail::BoundedSampleRing<
        RuntimePipelineSample, kDebugSampleCapacity>;

    struct SnapshotUpdateResult {
        DebugSampleRing::PendingToken pending_token;
        double snapshot_ms = 0.0;
        double snapshot_lock_wait_ms = 0.0;
        double debug_ring_ms = 0.0;
        double profile_window_ms = 0.0;
    };

    mutable std::mutex lifecycle_mutex;
    mutable std::mutex snapshot_mutex;
    // Detector 不支持 detect() 与资源切换并发；候选加载始终在锁外完成。
    mutable std::mutex detector_mutex;
    AppConfig config;
    RuntimeSnapshot current_snapshot;
    std::shared_ptr<AutoStopOutputArbiter> output_arbiter;
    // 公有控制入口可与停止并发；原子共享引用保证清理期间对象仍存活。
    std::atomic<std::shared_ptr<AutoStopWorker>> auto_stop_worker;
    std::atomic<std::shared_ptr<TriggerWorker>> trigger_worker;
    source_context::SourceContextClient source_context_client;
    weapon::GsiReceiver gsi_receiver;
    std::atomic<std::shared_ptr<RecoilWorker>> recoil_worker;
    std::optional<RecoilArchiveConfig> recoil_archive_config;
    std::atomic<std::shared_ptr<RecoilBatchArchive>> recoil_archive;
    std::shared_ptr<MotionLedger> motion_ledger;
    std::unordered_map<std::string, std::shared_ptr<const RecoilProfile>> recoil_profiles;
    // 只在启动时写入，并受snapshot_mutex保护；就绪提示按当前武器查询。
    std::unordered_map<std::string, std::string> recoil_profile_statuses;
    std::atomic<std::int64_t> recoil_observation_ns{0};
    std::atomic<std::uint64_t> stop_request_watermark{0};
    runtime::detail::LatestFrameQueue frame_queue;
    runtime::detail::RuntimePreviewChannel preview_channel;
    runtime::detail::SafetyGate safety_gate;
    std::unique_ptr<ICapture> capture;
    std::unique_ptr<Detector> detector;
    std::unique_ptr<Aim> aim;
    std::shared_ptr<IMouseController> mouse;
    bool owns_mouse = false;
    std::thread capture_thread;
    std::thread pipeline_thread;
    std::thread detector_reload_thread;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> aim_reset_requested{false};
    std::atomic<bool> detector_reload_running{false};
    std::deque<double> pipeline_samples;
    std::deque<double> control_latency_samples;
    DebugSampleRing debug_samples;
    std::chrono::steady_clock::time_point fps_started{};
    std::uint64_t fps_frame_count = 0;

    bool validate_d3d11_interop_detector(
            const Detector& candidate,
            std::string& error) const noexcept {
        if (!gpu_interop_enabled()) return true;
        if (config.capture.enable_d3d11_cuda_interop) {
            if (candidate.backend_name() != "TensorrtExecutionProvider" ||
                !candidate.d3d11_interop_supported()) {
                error = "D3D11/CUDA 互操作要求实际 TensorRT CUDA Graph GPU 前处理";
                return false;
            }
        } else if (candidate.backend_name() != "DmlExecutionProvider" ||
                   !candidate.d3d11_interop_supported()) {
            error = "D3D11/DirectML 互操作要求实际严格 DirectML 固定输入工作区";
            return false;
        }
        if (candidate.input_width() != config.capture.roi_width ||
            candidate.input_height() != config.capture.roi_height) {
            error = "D3D11 GPU 互操作要求 Capture ROI 与模型输入尺寸完全一致";
            return false;
        }
        error.clear();
        return true;
    }

    bool gpu_interop_enabled() const noexcept {
        return config.capture.enable_d3d11_cuda_interop ||
               config.capture.enable_d3d11_directml_interop;
    }

    void set_error(const std::string& error) noexcept {
        try {
            std::lock_guard<std::mutex> lock(snapshot_mutex);
            current_snapshot.last_error = error;
        } catch (...) {
        }
    }

    void set_state(RuntimeState state) noexcept {
        try {
            std::lock_guard<std::mutex> lock(snapshot_mutex);
            current_snapshot.state = state;
        } catch (...) {
        }
    }

    void fail_runtime(const std::string& error) noexcept {
        // 故障后旧图像不再代表实时状态。递增预览代际可同时阻止已经在锁外
        // 执行颜色转换的旧帧重新发布，但保留用户开关和已分配的大缓冲。
        const auto preview_stats = preview_channel.finish_session();
        safety_gate.emergency_stop();
        if (auto worker = auto_stop_worker.load()) worker->cancel();
        if (auto trigger = trigger_worker.load()) trigger->cancel();
        if (auto recoil = recoil_worker.load()) recoil->cancel();
        stop_requested.store(true, std::memory_order_release);
        frame_queue.stop();
        try {
            std::lock_guard<std::mutex> lock(snapshot_mutex);
            current_snapshot.state = RuntimeState::FAILED;
            current_snapshot.last_error = error;
            current_snapshot.output_armed = false;
            current_snapshot.emergency_stopped = true;
            current_snapshot.preview_enabled = preview_stats.enabled;
            current_snapshot.preview_sampled_frames = std::max(
                current_snapshot.preview_sampled_frames,
                preview_stats.sampled_frames);
            current_snapshot.preview_dropped_frames = std::max(
                current_snapshot.preview_dropped_frames,
                preview_stats.dropped_frames);
        } catch (...) {
        }
        LOG_ERROR("runtime", "{}", error);
    }

    bool initialize(const AppConfig& value,
                    std::shared_ptr<IMouseController> input_device = {}) {
        std::string validation_error;
        if (!validate_app_config(value, validation_error)) {
            set_error(validation_error);
            return false;
        }
        config = value;
        frame_queue.reset();
        preview_channel.set_session_active(false);
        stop_requested.store(false, std::memory_order_release);
        aim_reset_requested.store(false, std::memory_order_release);
        // 新会话清除上次武装/按住/急停，但保留 App 持续投递的独立
        // input-health；未验证输入时可以运行计算链，仍不能武装。
        safety_gate.reset_session();

        detector = std::make_unique<Detector>(config.detector);
        if (!detector->load()) {
            set_error("Detector 模型加载失败");
            return false;
        }
        if (!validate_d3d11_interop_detector(*detector, validation_error)) {
            set_error(validation_error);
            return false;
        }
        if (gpu_interop_enabled()) {
            // 预览目前只接受 CPU BGR。互操作会话强制关闭诊断支路，避免为
            // UI 恢复逐帧 readback；后续应另做最高 10 FPS 的 GPU readback。
            if (!preview_channel.set_enabled(false)) {
                set_error("关闭不兼容的 ROI 预览失败");
                return false;
            }
        }
        aim = std::make_unique<Aim>(config.aim);
        CaptureConfig capture_config = config.capture;
        capture_config.enable_performance_probes =
            config.runtime.enable_performance_probes;
        capture = create_capture(capture_config);
        if (!capture || !capture->open()) {
            set_error(capture ? capture->last_error() : "创建 Capture 失败");
            return false;
        }
        if (gpu_interop_enabled()) {
            // 三槽纹理必须在 Capture/Pipeline 线程启动前完成创建和跨 API 预备。
            // 若先提交 D3D copy 再从另一线程首次注册/打开，驱动同步可能互等。
            const auto slots = frame_queue.initialization_slots();
            for (const auto& slot : slots) {
                if (!slot || !capture->prepare_frame(*slot)) {
                    set_error(capture->last_error().empty()
                        ? "预创建 D3D11 GPU 帧槽失败"
                        : capture->last_error());
                    return false;
                }
                if (!detector->prepare_d3d11({
                        slot->native_storage,
                        slot->native_synchronization,
                        slot->native_fence,
                        slot->native_fence_value,
                        slot->width,
                        slot->height})) {
                    set_error("预备 D3D11 GPU 帧槽失败");
                    return false;
                }
            }
            LOG_INFO("runtime", "D3D11 GPU 三槽纹理已预创建并预备");
        }
        if (input_device) {
            mouse = std::move(input_device);
            owns_mouse = false;
            if (mouse->status() == MouseStatus::CLOSED && !mouse->open()) {
                set_error(mouse->last_error().empty()
                              ? "共享 Mouse 设备打开失败" : mouse->last_error());
                return false;
            }
            if (mouse->status() != MouseStatus::READY &&
                mouse->status() != MouseStatus::DISABLED) {
                set_error(mouse->last_error().empty()
                              ? "共享 Mouse 设备不可用" : mouse->last_error());
                return false;
            }
        } else {
            auto owned_mouse = MouseDeviceFactory::create(config.mouse);
            if (owned_mouse) {
                mouse = std::shared_ptr<IMouseController>(std::move(owned_mouse));
            }
            owns_mouse = true;
        }
        if (!mouse || (owns_mouse && !mouse->open())) {
            set_error(mouse ? mouse->last_error() : "创建 Mouse 失败");
            return false;
        }
        preview_channel.set_session_active(true);
        const bool preview_enabled = preview_channel.enabled();

        {
            std::lock_guard<std::mutex> lock(snapshot_mutex);
            current_snapshot = {};
            current_snapshot.state = RuntimeState::STARTING;
            current_snapshot.capture_status = capture->status();
            current_snapshot.mouse_status = mouse->status();
            current_snapshot.auto_stop = assess_auto_stop_availability(
                config.auto_stop, config.mouse.backend == MouseBackend::KMBOX_NET,
                mouse->supports_wasd_keyboard(), false);
            current_snapshot.provider = detector->backend_name();
            current_snapshot.active_model_path = config.detector.model_path;
            current_snapshot.detector_generation = 1;
            current_snapshot.output_allowed_by_config =
                config.mouse.allow_send_input;
            current_snapshot.input_healthy = safety_gate.input_healthy();
            current_snapshot.preview_enabled = preview_enabled;
            current_snapshot.d3d11_cuda_interop =
                config.capture.enable_d3d11_cuda_interop;
            current_snapshot.d3d11_directml_interop =
                config.capture.enable_d3d11_directml_interop;
        }
        pipeline_samples.clear();
        control_latency_samples.clear();
        debug_samples.reset();
        fps_started = std::chrono::steady_clock::now();
        fps_frame_count = 0;
        if (config.auto_stop.enabled || config.trigger.enabled || config.recoil.enabled) output_arbiter = std::make_shared<AutoStopOutputArbiter>();
        if (config.auto_stop.enabled) {
            Log::register_module("auto_stop", LogLevel::INFO);
            if (config.mouse.backend == MouseBackend::KMBOX_NET &&
                mouse->supports_wasd_keyboard()) {

                auto worker = std::make_shared<AutoStopWorker>(mouse, output_arbiter,
                    [this] {
                        return config.mouse.allow_send_input &&
                            !stop_requested.load(std::memory_order_acquire) &&
                            safety_gate.can_dispatch_auxiliary();
                    });
                if (!worker->start(config.auto_stop, config.mouse.kmbox_command_timeout_ms)) {
                    set_error("启动自动急停调度失败");
                    return false;
                }
                auto_stop_worker.store(std::move(worker));
            }
            LOG_INFO("auto_stop", "自动急停已接入请求接口；允许键不生成请求，预测不授予开火");
        }
        if ((config.trigger.enabled || config.recoil.enabled) && config.source_context.enabled) {
                auto context_config = config.source_context;
                char* token = nullptr; std::size_t token_size = 0;
                if (_dupenv_s(&token, &token_size, "XEN_SOURCE_CONTEXT_TOKEN") == 0 && token) {
                    context_config.token = token;
                    std::free(token);
                }
                if (!source_context_client.start(context_config)) {
                    set_error("源状态桥接启动失败，请核对配置和认证环境变量"); return false;
                }
            }
        if (config.gsi.enabled) {
            auto gsi_config = config.gsi;
            char* token = nullptr; std::size_t token_size = 0;
            if (_dupenv_s(&token, &token_size, "XEN_GSI_TOKEN") == 0 && token) {
                gsi_config.token = token; std::free(token);
            }
            if (!gsi_receiver.start(gsi_config)) { set_error("GSI接收启动失败，请核对身份/地址/认证环境变量"); return false; }
        }
        if (config.trigger.enabled) {
            Log::register_module("trigger", LogLevel::INFO);
            auto trigger_config = config.trigger;
            trigger_config.person_class_ids = config.aim.person_class_ids;
            trigger_config.head_class_ids = config.aim.head_class_ids;
            auto worker = std::make_shared<TriggerWorker>(mouse, output_arbiter,
                [this] { return config.mouse.allow_send_input && !stop_requested.load() && safety_gate.can_dispatch_auxiliary(); },
                [this, previous_session = std::uint64_t{0} ]() mutable {
                    const auto source = source_context_client.snapshot();
                    if (!source.available || !source.focused) { previous_session = 0; return false; }
                    if (source.session_id != previous_session) { previous_session = source.session_id; return false; }
                    return true;
                },
                [this] { const auto id = stop_request_watermark.load(); return id == UINT64_MAX ? 0 : id + 1; },
                [this](std::uint64_t id) {
                    if (!safety_gate.can_dispatch_auxiliary() || stop_requested.load() || id == 0) return false;
                    auto previous = stop_request_watermark.load();
                    do { if (id <= previous) return false; } while (!stop_request_watermark.compare_exchange_weak(previous, id));
                    auto stop = auto_stop_worker.load();
                    return stop && stop->request(id);
                },
                [this](std::uint64_t id) { if (auto stop = auto_stop_worker.load()) stop->cancel(id); });
            if (!worker->start(trigger_config)) { set_error("自动扳机启动失败或设备不支持左键"); return false; }
            trigger_worker.store(std::move(worker));
        }
        if (config.recoil.enabled) {
            motion_ledger = std::make_shared<MotionLedger>();
            motion_ledger->reset(config.aim.max_counts_per_frame, config.recoil.budget_window_ms, RecoilClock::now());
            recoil_profiles.clear(); recoil_observation_ns.store(0);
            { std::lock_guard lock(snapshot_mutex); recoil_profile_statuses.clear(); }
            RecoilStore store(config.recoil.profile_directory);
            std::vector<RecoilStoredProfile> profiles;
            std::string error;
            if (!store.list(profiles, error)) { set_error("弹道目录读取失败：" + error); return false; }
            for (const auto& entry : profiles) {
                const auto& id = entry.profile->weapon_id;
                { std::lock_guard lock(snapshot_mutex); if (recoil_profile_statuses.contains(id)) continue; }
                auto resolved = store.resolve(config.recoil, id, error);
                const auto status = resolved ? id + "：已匹配 " + resolved->id + " / r" + std::to_string(resolved->revision)
                                             : id + "：" + error;
                if (resolved) recoil_profiles[id] = std::move(resolved);
                std::lock_guard lock(snapshot_mutex);
                recoil_profile_statuses[id] = status;
            }
            if (profiles.empty()) {
                std::lock_guard lock(snapshot_mutex);
                current_snapshot.recoil_profile_status = "尚无弹道，请先导入并校准";
            }
            auto worker = std::make_shared<RecoilWorker>(mouse, output_arbiter, motion_ledger,
                [this, previous_weapon = std::string{}, previous_epoch = std::uint64_t{0}, generation = std::uint64_t{1},
                    focus_session = std::uint64_t{0}]() mutable {
                    RecoilInput input;
                    const auto weapon = gsi_receiver.snapshot();
                    const auto focus = source_context_client.snapshot();
                    if (weapon.canonical_id != previous_weapon || weapon.source_epoch != previous_epoch) {
                        previous_weapon = weapon.canonical_id; previous_epoch = weapon.source_epoch; ++generation;
                    }
                    input.device_epoch = 1; input.weapon_generation = generation;
                    const auto found = recoil_profiles.find(weapon.canonical_id);
                    if (found != recoil_profiles.end()) input.profile = found->second;
                    input.profile_conditions_match = weapon.valid && weapon.identity_match &&
                        weapon.state == weapon::WeaponState::ACTIVE && weapon.ammo_clip && *weapon.ammo_clip > 0 &&
                        weapon.valid_until > RecoilClock::now() && input.profile != nullptr;
                    input.focused = focus.available && focus.focused && focus.session_id == focus_session;
                    focus_session = focus.available && focus.focused ? focus.session_id : 0;
                    input.permission = config.mouse.allow_send_input && !stop_requested.load() && safety_gate.can_dispatch_auxiliary();
                    if (config.recoil.mixed_aim) {
                        const auto stamp = RecoilTime(std::chrono::nanoseconds(recoil_observation_ns.load()));
                        const auto now = RecoilClock::now();
                        input.permission = input.permission && stamp != RecoilTime{} && stamp <= now &&
                            now - stamp < std::chrono::milliseconds(config.recoil.max_observation_age_ms);
                    }
                    return input;
                },
                [this] { if (auto trigger = trigger_worker.load()) return trigger->firing_signal(); return TriggerFiringSignal{}; });
            if (!worker->start(config.recoil)) { set_error("压枪调度启动失败"); return false; }
            recoil_worker.store(worker);
            if (recoil_archive_config) {
                auto archive = std::make_shared<RecoilBatchArchive>();
                recoil_archive.store(archive);
                if (!archive->start(*recoil_archive_config, [worker](std::uint64_t after, std::size_t maximum) {
                        return worker->read_execution_events(after, maximum);
                    })) {
                    set_error("压枪归档启动失败：" + archive->snapshot().error);
                    return false;
                }
            }
        }
        return true;
    }

    void capture_loop() noexcept {
        const bool probes_enabled = config.runtime.enable_performance_probes;
        runtime::detail::CaptureFramePublisher frame_publisher(frame_queue);
        while (!stop_requested.load(std::memory_order_acquire)) {
            auto write_slot = frame_queue.acquire_write();
            if (!write_slot) {
                std::this_thread::yield();
                continue;
            }
            const auto outcome = frame_publisher.capture_and_publish(
                *capture, write_slot, probes_enabled);
            const CaptureStatus capture_status = outcome.capture_status;
            if (capture_status == CaptureStatus::FRAME) {
                if (!outcome.frame_publish_result ||
                    *outcome.frame_publish_result ==
                        runtime::detail::FramePublishResult::INTERNAL_ERROR) {
                    fail_runtime("Runtime 帧队列发布失败");
                    return;
                }
                if (*outcome.frame_publish_result ==
                    runtime::detail::FramePublishResult::INVALID) {
                    fail_runtime("Capture 返回不符合 Runtime 契约的 FRAME");
                    return;
                }
                if (*outcome.frame_publish_result ==
                    runtime::detail::FramePublishResult::STOPPED) {
                    return;
                }
                const auto now = std::chrono::steady_clock::now();
                ++fps_frame_count;
                const double elapsed =
                    std::chrono::duration<double>(now - fps_started).count();
                std::lock_guard<std::mutex> lock(snapshot_mutex);
                current_snapshot.capture_status = capture_status;
                current_snapshot.captured_frames =
                    frame_publisher.published_frames();
                current_snapshot.last_profile.capture_ms =
                    write_slot->timing.capture_ms;
                current_snapshot.source_dropped_frames =
                    write_slot->timing.source_dropped_frames;
                current_snapshot.duplication_recoveries =
                    write_slot->timing.duplication_recoveries;
                current_snapshot.transport_dropped_frames =
                    write_slot->timing.transport_dropped_frames;
                current_snapshot.transport_invalid_packets =
                    write_slot->timing.transport_invalid_packets;
                current_snapshot.source_received_frames =
                    write_slot->timing.source_received_frames;
                current_snapshot.source_sequence =
                    write_slot->timing.source_sequence;
                current_snapshot.source_sequence_valid =
                    write_slot->timing.source_sequence_valid;
                current_snapshot.source_fps = write_slot->timing.source_fps;
                current_snapshot.source_timecode =
                    write_slot->timing.source_timecode;
                current_snapshot.source_timecode_valid =
                    write_slot->timing.source_timecode_valid;
                current_snapshot.source_timestamp =
                    write_slot->timing.source_timestamp;
                current_snapshot.source_timestamp_valid =
                    write_slot->timing.source_timestamp_valid;
                current_snapshot.encoded_width = write_slot->encoded_width;
                current_snapshot.encoded_height = write_slot->encoded_height;
                current_snapshot.source_width = write_slot->source_width;
                current_snapshot.source_height = write_slot->source_height;
                current_snapshot.capture_roi_width = write_slot->width;
                current_snapshot.capture_roi_height = write_slot->height;
                current_snapshot.capture_roi_x = write_slot->roi_x;
                current_snapshot.capture_roi_y = write_slot->roi_y;
                current_snapshot.source_pixels_per_pixel_x =
                    write_slot->source_pixels_per_pixel_x;
                current_snapshot.source_pixels_per_pixel_y =
                    write_slot->source_pixels_per_pixel_y;
                if (elapsed >= 1.0) {
                    current_snapshot.capture_fps =
                        static_cast<double>(fps_frame_count) / elapsed;
                    fps_started = now;
                    fps_frame_count = 0;
                }
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(snapshot_mutex);
                current_snapshot.capture_status = capture_status;
            }
            if (capture_status == CaptureStatus::NO_FRAME) continue;
            fail_runtime(capture->last_error().empty()
                ? "Capture 运行失败" : capture->last_error());
            return;
        }
    }

    SnapshotUpdateResult update_pipeline_snapshot(
                                  const CapturedFrame& frame,
                                  const PipelineProfile& profile,
                                  const RuntimeServiceProfile& service,
                                  std::uint64_t sample_overwritten_frames,
                                  const AimResult& aim_result,
                                  std::span<const Detection> detections,
                                  const AimFrame& aim_frame,
                                  float aim_control_center_x,
                                  float aim_control_center_y,
                                  MouseStatus mouse_status,
                                  bool mouse_sent,
                                  bool aim_lock_active) {
        SnapshotUpdateResult result;
        const aim_landmark::Diagnostic landmark =
            aim_landmark::inspect_head_landmark(
                frame.timing.sequence, detections, config.aim, aim_result);
        const bool probes_enabled = config.runtime.enable_performance_probes;
        const auto snapshot_started = probes_enabled
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        const auto preview_stats = preview_channel.stats();
        const auto snapshot_lock_requested = probes_enabled
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        std::unique_lock<std::mutex> lock(snapshot_mutex);
        if (probes_enabled) {
            result.snapshot_lock_wait_ms =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() -
                    snapshot_lock_requested).count();
        }
        current_snapshot.last_sequence = frame.timing.sequence;
        current_snapshot.last_profile = profile;
        current_snapshot.detection_status = profile.detector.status;
        current_snapshot.aim_status = aim_result.status;
        current_snapshot.mouse_status = mouse_status;
        current_snapshot.last_aim = aim_result;
        const std::uint64_t runtime_overwritten_frames =
            frame_queue.overwritten_frames();
        current_snapshot.overwritten_frames = runtime_overwritten_frames;
        ++current_snapshot.processed_frames;
        if (profile.detector.status != DetectionStatus::SUCCESS ||
            aim_result.status != AimStatus::SUCCESS) {
            ++current_snapshot.failed_frames;
        }
        if (mouse_sent) ++current_snapshot.mouse_commands;
        current_snapshot.output_armed = safety_gate.output_armed();
        current_snapshot.input_healthy = safety_gate.input_healthy();
        current_snapshot.aim_hold_active = safety_gate.hold_active();
        current_snapshot.emergency_stopped =
            safety_gate.emergency_stopped();

        const auto debug_ring_started = probes_enabled
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        RuntimePipelineSample sample;
        sample.sequence = frame.timing.sequence;
        sample.aim_observation_epoch = aim_frame.observation_epoch;
        sample.background_motion_x = aim_frame.background_motion_x;
        sample.frame_timing = runtime::detail::make_frame_timing_evidence(
            frame.timing, aim_frame, profile.control_timing_valid);
        sample.geometry.encoded_width = frame.encoded_width;
        sample.geometry.encoded_height = frame.encoded_height;
        sample.geometry.source_width = frame.source_width;
        sample.geometry.source_height = frame.source_height;
        sample.geometry.roi_width = frame.width;
        sample.geometry.roi_height = frame.height;
        sample.geometry.roi_x = frame.roi_x;
        sample.geometry.roi_y = frame.roi_y;
        sample.geometry.source_pixels_per_pixel_x =
            frame.source_pixels_per_pixel_x;
        sample.geometry.source_pixels_per_pixel_y =
            frame.source_pixels_per_pixel_y;
        sample.profile = profile;
        sample.capture_stages = frame.timing.capture_stages;
        sample.service = service;
        sample.source_dropped_frames = frame.timing.source_dropped_frames;
        sample.transport_dropped_frames =
            frame.timing.transport_dropped_frames;
        sample.transport_invalid_packets =
            frame.timing.transport_invalid_packets;
        sample.runtime_overwritten_frames = sample_overwritten_frames;
        sample.detection_status = profile.detector.status;
        sample.aim_status = aim_result.status;
        sample.mouse_status = mouse_status;
        sample.mouse_sent = mouse_sent;
        sample.aim_lock_active = aim_lock_active;
        sample.aim_control_center_x = aim_control_center_x;
        sample.aim_control_center_y = aim_control_center_y;
        sample.aim_acquisition_range_radius =
            aim_result.acquisition_range_radius;
        sample.aim_active_range_radius = aim_result.active_range_radius;
        sample.aim_has_target = aim_result.has_target;
        sample.aim_has_command = aim_result.has_command;
        sample.aim_range_locked = aim_result.range_locked;
        sample.aim_range_allows_control = aim_result.range_allows_control;
        sample.aim_target = aim_result.target;
        sample.aim_control = aim_result.control;
        sample.aim_command = aim_result.command;
        sample.aim_landmark = landmark;
        if (aim_result.has_target) {
            sample.aim_base_point_inside_box =
                aim_result.target.base_aim_x >= aim_result.target.x1 &&
                aim_result.target.base_aim_x <= aim_result.target.x2 &&
                aim_result.target.base_aim_y >= aim_result.target.y1 &&
                aim_result.target.base_aim_y <= aim_result.target.y2;
            sample.aim_prediction_point_outside_box =
                aim_result.target.lead_active &&
                (aim_result.target.aim_x < aim_result.target.x1 ||
                 aim_result.target.aim_x > aim_result.target.x2 ||
                 aim_result.target.aim_y < aim_result.target.y1 ||
                 aim_result.target.aim_y > aim_result.target.y2);
        }
        if (aim_result.has_command) {
            const double desired_x =
                (aim_result.target.aim_x - aim_control_center_x) *
                frame.source_pixels_per_pixel_x *
                config.aim.counts_per_pixel_x;
            const double desired_y =
                (aim_result.target.aim_y - aim_control_center_y) *
                frame.source_pixels_per_pixel_y *
                config.aim.counts_per_pixel_y;
            const double command_x = aim_result.command.dx_counts;
            const double command_y = aim_result.command.dy_counts;
            const double error_magnitude = std::hypot(
                (aim_result.target.aim_x - aim_control_center_x) *
                    frame.source_pixels_per_pixel_x,
                (aim_result.target.aim_y - aim_control_center_y) *
                    frame.source_pixels_per_pixel_y);
            const double hold_band = std::max(
                2.0, static_cast<double>(config.aim.deadzone_pixels) * 1.5);
            sample.aim_command_toward_target =
                (error_magnitude <= hold_band ||
                 command_x * desired_x + command_y * desired_y > 0.0) &&
                std::hypot(command_x, command_y) <=
                    config.aim.max_counts_per_frame + 0.001;
        }
        if (profile.detector.status == DetectionStatus::SUCCESS) {
            runtime::detail::summarize_detections(
                detections, config.aim, sample);
        }
        if (probes_enabled) {
            result.pending_token = debug_samples.push_pending(sample);
        } else {
            debug_samples.push(sample);
        }
        current_snapshot.debug_samples_dropped = debug_samples.dropped();
        if (probes_enabled) {
            result.debug_ring_ms =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() -
                    debug_ring_started).count();
        }
        current_snapshot.preview_enabled = preview_stats.enabled;
        current_snapshot.preview_sampled_frames =
            preview_stats.sampled_frames;
        current_snapshot.preview_dropped_frames =
            preview_stats.dropped_frames;

        const auto profile_window_started = probes_enabled
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        pipeline_samples.push_back(profile.total_ms);
        while (pipeline_samples.size() >
               static_cast<std::size_t>(config.runtime.profile_window)) {
            pipeline_samples.pop_front();
        }
        const std::vector<double> samples(
            pipeline_samples.begin(), pipeline_samples.end());
        current_snapshot.pipeline_p50_ms = percentile(samples, 0.50);
        current_snapshot.pipeline_p95_ms = percentile(samples, 0.95);
        if (profile.mouse_backend_completion_timing_valid) {
            control_latency_samples.push_back(
                profile.capture_to_mouse_backend_completion_ms);
            while (control_latency_samples.size() >
                   static_cast<std::size_t>(config.runtime.profile_window)) {
                control_latency_samples.pop_front();
            }
            const std::vector<double> control_samples(
                control_latency_samples.begin(),
                control_latency_samples.end());
            current_snapshot.control_latency_available = true;
            current_snapshot.control_latency_sample_count =
                control_samples.size();
            current_snapshot.control_latency_last_ms =
                profile.capture_to_mouse_backend_completion_ms;
            current_snapshot.control_latency_p50_ms =
                percentile(control_samples, 0.50);
            current_snapshot.control_latency_p95_ms =
                percentile(control_samples, 0.95);
        }
        if (probes_enabled) {
            result.profile_window_ms =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() -
                    profile_window_started).count();
        }
        lock.unlock();
        if (probes_enabled) {
            result.snapshot_ms =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - snapshot_started)
                    .count();
        }
        return result;
    }

    void pipeline_loop() noexcept {
        std::uint64_t last_sequence = 0;
        runtime::detail::RuntimeObservationClock observation_clock;
        runtime::detail::CameraMotionEstimator camera_motion;
        const bool probes_enabled = config.runtime.enable_performance_probes;
        while (!stop_requested.load(std::memory_order_acquire)) {
            std::uint64_t overwritten_frames_at_consume = 0;
            const auto frame = frame_queue.wait_latest(
                last_sequence, stop_requested,
                &overwritten_frames_at_consume);
            if (!frame) continue;
            last_sequence = frame->timing.sequence;
            const auto pipeline_started = std::chrono::steady_clock::now();
            PipelineProfile profile;
            profile.capture_ms = frame->timing.capture_ms;
            profile.source_time_basis = frame->timing.source_time_basis;
            profile.source_clock_status = frame->timing.source_clock_status;
            profile.source_clock_uncertainty_ms =
                frame->timing.source_clock_uncertainty_ms;
            profile.source_clock_round_trip_ms =
                frame->timing.source_clock_round_trip_ms;
            profile.source_clock_rate = frame->timing.source_clock_rate;
            profile.source_clock_mapping_age_ms =
                frame->timing.source_clock_mapping_age_ms;
            profile.source_clock_sample_count =
                frame->timing.source_clock_sample_count;
            profile.source_clock_session_id =
                frame->timing.source_clock_session_id;
            if (frame->timing.source_time_timing_valid) {
                profile.source_timing_valid = true;
                profile.source_to_capture_ms =
                    std::chrono::duration<double, std::milli>(
                        frame->timing.captured_at -
                        frame->timing.source_time_at).count();
            }
            profile.queue_ms = std::chrono::duration<double, std::milli>(
                pipeline_started - frame->timing.captured_at).count();

            std::vector<Detection> detections;
            {
                // profile() 必须与同一次 detect() 使用同一代 Detector；重载只会
                // 在两帧之间取得此锁并交换指针。
                std::lock_guard<std::mutex> lock(detector_mutex);
                if (aim_reset_requested.exchange(
                        false, std::memory_order_acq_rel)) {
                    aim->reset();
                    camera_motion.reset();
                }
                if (frame->storage == CapturedFrameStorage::D3D11_BGRA8 ||
                    frame->storage ==
                        CapturedFrameStorage::D3D11_BGRA8_DIRECTML) {
                    detections = detector->detect_d3d11({
                        frame->native_storage,
                        frame->native_synchronization,
                        frame->native_fence,
                        frame->native_fence_value,
                        frame->width,
                        frame->height});
                } else {
                    detections = detector->detect(frame->bgr);
                }
                profile.detector = detector->profile();
            }
            if (profile.detector.status != DetectionStatus::SUCCESS) {
                if (auto trigger = trigger_worker.load()) trigger->publish(std::make_shared<TriggerObservation>());
                recoil_observation_ns.store(0);
                if (config.recoil.mixed_aim) if (auto recoil = recoil_worker.load()) recoil->cancel();
            }
            AimResult aim_result;
            AimFrame aim_frame;
            bool mouse_sent = false;
            double mouse_elapsed_ms = 0.0;
            if (profile.detector.status == DetectionStatus::SUCCESS) {
                // WARMING→VALID 会从辅机 frame-ready 切到更早的 NDI
                // submission 时刻；source session 重启或拟合更新也可能让
                // 映射跳回。跨时间基准的旧轨迹不能混算 dt，先重置再消费。
                auto prepared = runtime::detail::prepare_aim_frame(
                    *frame, std::move(detections), observation_clock,
                    camera_motion, safety_gate.can_dispatch());
                if (prepared.reset_aim) {
                    if (auto recoil = recoil_worker.load()) recoil->cancel();
                    aim->reset();
                }
                aim_frame = std::move(prepared.frame);
                profile.background_motion_ms = prepared.background_motion_ms;
                profile.control_timing_valid = true;
                profile.capture_to_control_ms =
                    std::chrono::duration<double, std::milli>(
                        aim_frame.control_at -
                        frame->timing.captured_at).count();
                if (profile.source_timing_valid) {
                    profile.source_to_control_ms =
                        std::chrono::duration<double, std::milli>(
                            aim_frame.control_at -
                            frame->timing.source_time_at).count();
                }
                if (auto trigger = trigger_worker.load()) {
                    auto observation = std::make_shared<TriggerObservation>();
                    observation->detections = aim_frame.detections;
                    observation->center_x = aim_frame.control_center_x;
                    observation->center_y = aim_frame.control_center_y;
                    observation->roi_width = aim_frame.roi_width;
                    observation->roi_height = aim_frame.roi_height;
                    observation->epoch = aim_frame.observation_epoch;
                    observation->sequence = aim_frame.sequence;
                    observation->observed_at = frame->timing.source_time_at;
                    observation->timing_valid = frame->timing.source_time_timing_valid &&
                        std::isfinite(frame->timing.source_clock_uncertainty_ms) && frame->timing.source_clock_uncertainty_ms >= 0.0 &&
                        frame->timing.source_clock_uncertainty_ms <= config.trigger.max_observation_age_ms;
                    if (observation->timing_valid) observation->uncertainty = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::duration<double, std::milli>(frame->timing.source_clock_uncertainty_ms));
                    observation->valid = true;
                    trigger->publish(std::move(observation));
                }
                std::int64_t candidate_recoil_observation_ns = 0;
                if (config.recoil.enabled) {
                    if (!config.recoil.mixed_aim) aim_frame.lock_active = false;
                    else aim_frame.external_motion = motion_ledger->snapshot(RecoilClock::now());
                    const bool fresh_source = frame->timing.source_time_timing_valid &&
                        std::isfinite(frame->timing.source_clock_uncertainty_ms) && frame->timing.source_clock_uncertainty_ms >= 0 &&
                        frame->timing.source_clock_uncertainty_ms < config.recoil.max_observation_age_ms;
                    candidate_recoil_observation_ns = fresh_source ? std::chrono::duration_cast<std::chrono::nanoseconds>(
                        (frame->timing.source_time_at - std::chrono::duration_cast<RecoilClock::duration>(
                            std::chrono::duration<double, std::milli>(frame->timing.source_clock_uncertainty_ms))).time_since_epoch()).count() : 0;
                }
                aim_result = aim->process(aim_frame);
                if (config.recoil.enabled && aim_result.status == AimStatus::SUCCESS)
                    recoil_observation_ns.store(candidate_recoil_observation_ns);
                if (config.recoil.mixed_aim && aim_result.status != AimStatus::SUCCESS) {
                    recoil_observation_ns.store(0);
                    if (auto recoil = recoil_worker.load()) recoil->cancel();
                }
                profile.aim = aim_result.profile;

                if (aim_result.status == AimStatus::SUCCESS &&
                    aim_result.has_command) {
                    const MouseMoveCommand command{
                        aim_result.command.dx_counts,
                        aim_result.command.dy_counts};
                    bool dispatch_allowed = safety_gate.can_dispatch() && (!config.recoil.enabled || config.recoil.mixed_aim);
                    std::unique_lock<std::timed_mutex> output_guard;
                    if (dispatch_allowed && output_arbiter) {
                        output_guard = output_arbiter->try_enter_aim();
                        dispatch_allowed = output_guard.owns_lock() &&
                            safety_gate.can_dispatch();
                    }
                    if (dispatch_allowed && config.recoil.enabled) {
                        dispatch_allowed = motion_ledger->revision() == aim_frame.external_motion.revision &&
                            motion_ledger->permits(command, RecoilClock::now());
                    }
                    auto mouse_backend_completed =
                        std::chrono::steady_clock::now();
                    MouseMoveReceipt mouse_receipt;
                    if (dispatch_allowed) {
                        const auto mouse_started = mouse_backend_completed;
                        mouse_receipt = mouse->move(command);
                        mouse_sent = mouse_receipt.succeeded;
                        if (config.recoil.enabled) {
                            if (!motion_ledger->record(command, mouse_receipt, false)) output_arbiter->latch_output_fault();
                        }
                        mouse_backend_completed =
                            mouse_receipt.backend_completed_at ==
                                std::chrono::steady_clock::time_point{}
                                ? std::chrono::steady_clock::now()
                                : mouse_receipt.backend_completed_at;
                        mouse_elapsed_ms =
                            std::chrono::duration<double, std::milli>(
                                mouse_backend_completed - mouse_started)
                                .count();
                        profile.mouse_backend_completion_timing_valid = true;
                        profile.control_to_mouse_backend_completion_ms =
                            std::chrono::duration<double, std::milli>(
                                mouse_backend_completed -
                                aim_frame.control_at).count();
                        profile.capture_to_mouse_backend_completion_ms =
                            std::chrono::duration<double, std::milli>(
                                mouse_backend_completed -
                                frame->timing.captured_at).count();
                        if (profile.source_timing_valid) {
                            profile.source_to_mouse_backend_completion_ms =
                                std::chrono::duration<double, std::milli>(
                                    mouse_backend_completed -
                                    frame->timing.source_time_at).count();
                        }
                        if (mouse_receipt.protocol_ack_received &&
                            mouse_receipt.protocol_ack_received_at !=
                                std::chrono::steady_clock::time_point{}) {
                            profile.mouse_protocol_ack_timing_valid = true;
                            profile.control_to_mouse_protocol_ack_ms =
                                std::chrono::duration<double, std::milli>(
                                    mouse_receipt.protocol_ack_received_at -
                                    aim_frame.control_at).count();
                            profile.capture_to_mouse_protocol_ack_ms =
                                std::chrono::duration<double, std::milli>(
                                    mouse_receipt.protocol_ack_received_at -
                                    frame->timing.captured_at).count();
                            if (profile.source_timing_valid) {
                                profile.source_to_mouse_protocol_ack_ms =
                                    std::chrono::duration<double, std::milli>(
                                        mouse_receipt.protocol_ack_received_at -
                                        frame->timing.source_time_at).count();
                            }
                        }
                        if (mouse_receipt.physical_effect_observed &&
                            mouse_receipt.physical_effect_at !=
                                std::chrono::steady_clock::time_point{}) {
                            profile.mouse_physical_effect_timing_valid = true;
                            profile.control_to_mouse_physical_effect_ms =
                                std::chrono::duration<double, std::milli>(
                                    mouse_receipt.physical_effect_at -
                                    aim_frame.control_at).count();
                            profile.capture_to_mouse_physical_effect_ms =
                                std::chrono::duration<double, std::milli>(
                                    mouse_receipt.physical_effect_at -
                                    frame->timing.captured_at).count();
                            if (profile.source_timing_valid) {
                                profile.source_to_mouse_physical_effect_ms =
                                    std::chrono::duration<double, std::milli>(
                                        mouse_receipt.physical_effect_at -
                                        frame->timing.source_time_at).count();
                            }
                        }
                    }
                    // Aim 先记录预计算命令，Mouse 返回后再用同一序号原位
                    // 确认。安全门在两次检查间释放或后端失败时确认零，
                    // 绝不让未被后端接受的命令进入下一帧的延迟命令与在途库存。
                    const bool backend_completion_recorded =
                        aim->record_backend_completed_command(
                            aim_result.command.sequence,
                            mouse_backend_completed,
                            mouse_sent ? command.dx_counts : 0,
                            mouse_sent ? command.dy_counts : 0);
                    if (!backend_completion_recorded) {
                        safety_gate.emergency_stop();
                        aim_reset_requested.store(
                            true, std::memory_order_release);
                        set_error("Aim 后端完成反馈与预计算历史不一致");
                    } else if (dispatch_allowed && !mouse_sent) {
                        safety_gate.emergency_stop();
                        aim_reset_requested.store(true,
                                                  std::memory_order_release);
                        set_error(mouse->last_error());
                    }
                }
            } else {
                aim_result.status = AimStatus::NOT_RUN;
                camera_motion.reset();
                aim_reset_requested.store(true, std::memory_order_release);
            }
            const auto finished = std::chrono::steady_clock::now();
            profile.mouse_ms = mouse_elapsed_ms;
            profile.total_ms = std::chrono::duration<double, std::milli>(
                finished - frame->timing.captured_at).count();
            const std::span<const Detection> preview_detections =
                profile.detector.status == DetectionStatus::SUCCESS
                    ? std::span<const Detection>(aim_frame.detections)
                    : std::span<const Detection>(detections);
            const float control_center_x = frame->source_pixels_per_pixel_x > 0.0
                ? static_cast<float>(
                    (frame->source_width * 0.5 - frame->roi_x) /
                    frame->source_pixels_per_pixel_x)
                : 0.0f;
            const float control_center_y = frame->source_pixels_per_pixel_y > 0.0
                ? static_cast<float>(
                    (frame->source_height * 0.5 - frame->roi_y) /
                    frame->source_pixels_per_pixel_y)
                : 0.0f;
            RuntimeServiceProfile service;
            service.preview_attempted =
                frame->storage == CapturedFrameStorage::CPU_BGR;
            if (frame->storage == CapturedFrameStorage::CPU_BGR) {
                const auto preview_started = probes_enabled
                    ? std::chrono::steady_clock::now()
                    : std::chrono::steady_clock::time_point{};
                service.preview_published = preview_channel.publish(
                    frame->bgr, frame->timing.sequence,
                    control_center_x, control_center_y,
                    profile.detector.status, aim_result.status,
                    preview_detections, aim_result,
                    std::chrono::steady_clock::now());
                if (probes_enabled) {
                    service.preview_ms =
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() -
                            preview_started).count();
                }
            }
            SnapshotUpdateResult snapshot_result = update_pipeline_snapshot(
                *frame, profile, service, overwritten_frames_at_consume,
                aim_result, preview_detections, aim_frame,
                aim_frame.control_center_x, aim_frame.control_center_y,
                mouse->status(), mouse_sent, aim_frame.lock_active);
            if (probes_enabled) {
                const auto tail_finished = std::chrono::steady_clock::now();
                service.valid = true;
                service.snapshot_ms = snapshot_result.snapshot_ms;
                service.snapshot_lock_wait_ms =
                    snapshot_result.snapshot_lock_wait_ms;
                service.debug_ring_ms = snapshot_result.debug_ring_ms;
                service.profile_window_ms =
                    snapshot_result.profile_window_ms;
                service.service_tail_ms =
                    std::chrono::duration<double, std::milli>(
                        tail_finished - finished).count();
                service.pipeline_service_ms =
                    std::chrono::duration<double, std::milli>(
                        tail_finished - pipeline_started).count();
                service.pipeline_complete_ms =
                    std::chrono::duration<double, std::milli>(
                        tail_finished - frame->timing.captured_at).count();
                if (!debug_samples.finalize(
                        snapshot_result.pending_token,
                        [&service](RuntimePipelineSample& sample) noexcept {
                            sample.service = service;
                        })) {
                    fail_runtime("性能探针样本两阶段发布失败");
                    return;
                }
            }
        }
    }

    void release_modules() noexcept {
        if (auto worker = trigger_worker.exchange(std::shared_ptr<TriggerWorker>{})) {
            worker->stop();
            std::lock_guard lock(snapshot_mutex);
            current_snapshot.trigger = worker->snapshot();
            current_snapshot.trigger_telemetry_available = true;
            current_snapshot.trigger_execution_log = worker->execution_log();
        }
        if (auto worker = recoil_worker.exchange(std::shared_ptr<RecoilWorker>{})) {
            worker->stop();
            std::lock_guard lock(snapshot_mutex);
            current_snapshot.recoil = worker->snapshot(); current_snapshot.recoil_telemetry_available = true;
            current_snapshot.recoil_execution_log = worker->execution_log();
        }
        // 先停止事件生产，再读尽最终边界；磁盘工作始终在输出锁外。
        if (auto archive = recoil_archive.exchange(std::shared_ptr<RecoilBatchArchive>{})) {
            archive->stop();
            std::lock_guard lock(snapshot_mutex);
            current_snapshot.recoil_archive = archive->snapshot();
        }
        {
            std::lock_guard lock(snapshot_mutex);
            current_snapshot.weapon_snapshot = gsi_receiver.snapshot();
            if (config.recoil.enabled) {
                const auto& id = current_snapshot.weapon_snapshot.canonical_id;
                const auto found = recoil_profile_statuses.find(id);
                current_snapshot.recoil_profile_status = id.empty() ? "等待有效武器身份" :
                    found == recoil_profile_statuses.end() ? id + "：无匹配曲线" : found->second;
            }
        }
        gsi_receiver.stop();
        source_context_client.stop();
        if (auto worker = auto_stop_worker.exchange(
                std::shared_ptr<AutoStopWorker>{})) {
            worker->stop();
            std::lock_guard<std::mutex> lock(snapshot_mutex);
            current_snapshot.auto_stop = worker->snapshot();
        }
        if (output_arbiter) {
            std::lock_guard lock(snapshot_mutex);
            current_snapshot.output_arbitration = output_arbiter->snapshot();
            current_snapshot.output_arbitration_available = true;
        }
        output_arbiter.reset();
        if (mouse && owns_mouse) mouse->close();
        // CUDA registration 持有 D3D11 资源引用。先销毁 Detector/registration，
        // 再关闭 Capture 的 D3D11 设备，保持跨 API 释放顺序可解释。
        if (detector) detector->reset();
        if (capture) capture->close();
        if (aim) aim->reset();
        mouse.reset();
        owns_mouse = false;
        capture.reset();
        detector.reset();
        aim.reset();
    }
};

const char* RuntimeStateName(RuntimeState state) noexcept {
    switch (state) {
        case RuntimeState::STOPPED: return "STOPPED";
        case RuntimeState::STARTING: return "STARTING";
        case RuntimeState::RUNNING: return "RUNNING";
        case RuntimeState::STOPPING: return "STOPPING";
        case RuntimeState::FAILED: return "FAILED";
    }
    return "UNKNOWN";
}

const char* DetectorReloadStateName(DetectorReloadState state) noexcept {
    switch (state) {
        case DetectorReloadState::IDLE: return "IDLE";
        case DetectorReloadState::LOADING: return "LOADING";
        case DetectorReloadState::SUCCEEDED: return "SUCCEEDED";
        case DetectorReloadState::FAILED: return "FAILED";
    }
    return "UNKNOWN";
}

Runtime::Runtime() : impl_(std::make_unique<Impl>()) {
    Log::register_module("runtime", LogLevel::INFO);
}

Runtime::~Runtime() {
    stop();
}

bool Runtime::start(const AppConfig& config) noexcept {
    return start(config, {});
}

bool Runtime::start(const AppConfig& config,
                    std::shared_ptr<IMouseController> input_device,
                    std::optional<RecoilArchiveConfig> archive) noexcept {
    if (!impl_) return false;
    std::lock_guard<std::mutex> lifecycle_lock(impl_->lifecycle_mutex);
    // 上次失败可能留下已退出但仍 joinable 的线程，先完整回收。
    if (impl_->capture_thread.joinable() || impl_->pipeline_thread.joinable() ||
        impl_->detector_reload_thread.joinable()) {
        impl_->stop_requested.store(true, std::memory_order_release);
        impl_->frame_queue.stop();
        if (impl_->capture_thread.joinable()) impl_->capture_thread.join();
        if (impl_->pipeline_thread.joinable()) impl_->pipeline_thread.join();
        if (impl_->detector_reload_thread.joinable()) {
            impl_->detector_reload_thread.join();
        }
        impl_->detector_reload_running.store(false,
                                             std::memory_order_release);
        impl_->release_modules();
    }
    impl_->set_state(RuntimeState::STARTING);
    try {
        // 证据配置只取实际执行配置，调用者只能提供归档身份与资源位置。
        if (archive) archive->recoil = config.recoil;
        impl_->recoil_archive_config = std::move(archive);
        if (!impl_->initialize(config, std::move(input_device))) {
            impl_->release_modules();
            impl_->set_state(RuntimeState::FAILED);
            return false;
        }
        impl_->capture_thread = std::thread([this] {
            impl_->capture_loop();
        });
        impl_->pipeline_thread = std::thread([this] {
            impl_->pipeline_loop();
        });
        impl_->set_state(RuntimeState::RUNNING);
        LOG_INFO("runtime", "Runtime 已启动: provider={}",
                 impl_->detector->backend_name());
        return true;
    } catch (...) {
        impl_->fail_runtime("启动 Runtime 时发生未知异常");
        return false;
    }
}

void Runtime::stop() noexcept {
    if (!impl_) return;
    std::lock_guard<std::mutex> lifecycle_lock(impl_->lifecycle_mutex);
    impl_->safety_gate.emergency_stop();
    if (auto worker = impl_->auto_stop_worker.load()) worker->cancel();
            if (auto trigger = impl_->trigger_worker.load()) trigger->cancel();
            if (auto recoil = impl_->recoil_worker.load()) recoil->cancel();
    impl_->set_state(RuntimeState::STOPPING);
    impl_->stop_requested.store(true, std::memory_order_release);
    impl_->frame_queue.stop();
    try {
        if (impl_->capture_thread.joinable()) impl_->capture_thread.join();
        if (impl_->pipeline_thread.joinable()) impl_->pipeline_thread.join();
        // ORT/TensorRT Session 创建不可取消。停止请求先让候选线程放弃切换，
        // 再等待加载自然结束，保证 Runtime 析构后没有后台资源访问。
        if (impl_->detector_reload_thread.joinable()) {
            impl_->detector_reload_thread.join();
        }
    } catch (...) {
    }
    const std::uint64_t final_runtime_overwritten_frames =
        impl_->frame_queue.overwritten_frames();
    impl_->detector_reload_running.store(false, std::memory_order_release);
    impl_->release_modules();
    // Pipeline 已退出后清除旧预览，但保留三槽大缓冲和用户的启用选择。
    // 下次启动会从新会话首帧重新发布，不把停止前图像误当成实时画面。
    const auto preview_stats = impl_->preview_channel.finish_session();
    {
        std::lock_guard<std::mutex> lock(impl_->snapshot_mutex);
        impl_->current_snapshot.state = RuntimeState::STOPPED;
        impl_->current_snapshot.capture_status = CaptureStatus::CLOSED;
        impl_->current_snapshot.mouse_status = MouseStatus::CLOSED;
        impl_->current_snapshot.detector_reload_state =
            DetectorReloadState::IDLE;
        impl_->current_snapshot.detector_reload_error.clear();
        impl_->current_snapshot.output_armed = false;
        impl_->current_snapshot.input_healthy =
            impl_->safety_gate.input_healthy();
        impl_->current_snapshot.aim_hold_active = false;
        impl_->current_snapshot.emergency_stopped = true;
        impl_->current_snapshot.preview_enabled = preview_stats.enabled;
        impl_->current_snapshot.preview_sampled_frames = std::max(
            impl_->current_snapshot.preview_sampled_frames,
            preview_stats.sampled_frames);
        impl_->current_snapshot.preview_dropped_frames = std::max(
            impl_->current_snapshot.preview_dropped_frames,
            preview_stats.dropped_frames);
        // 最后一个 Pipeline 样本完成后到 stop() 封口之间仍可能发生 latest-only
        // 覆盖。线程 join 后读取终值，正式覆盖阶段才能包含这段尾差。
        impl_->current_snapshot.overwritten_frames =
            final_runtime_overwritten_frames;
    }
}

bool Runtime::reload_detector(const DetectorConfig& config) noexcept {
    if (!impl_) return false;
    try {
        std::lock_guard<std::mutex> lifecycle_lock(impl_->lifecycle_mutex);

        {
            std::lock_guard<std::mutex> lock(impl_->snapshot_mutex);
            if (impl_->current_snapshot.state != RuntimeState::RUNNING ||
                impl_->stop_requested.load(std::memory_order_acquire) ||
                impl_->detector_reload_running.load(
                    std::memory_order_acquire)) {
                return false;
            }
        }

        if (impl_->gpu_interop_enabled()) {
            // 互操作预处理器会跨帧缓存三槽 D3D11 资源的 CUDA 注册。
            // 热重载销毁旧 Session 时 Capture 线程仍可能向空闲槽提交 GPU copy，
            // 因此首版契约要求先停止 Runtime，再更换模型并重新启动。
            std::lock_guard<std::mutex> lock(impl_->snapshot_mutex);
            impl_->current_snapshot.detector_reload_state =
                DetectorReloadState::FAILED;
            impl_->current_snapshot.detector_reload_error =
                "D3D11 GPU 互操作启用时不支持 Detector 热重载；"
                "请停止 Runtime 后更换模型";
            return false;
        }

        AppConfig candidate_config = impl_->config;
        candidate_config.detector = config;
        std::string validation_error;
        if (!validate_app_config(candidate_config, validation_error)) {
            std::lock_guard<std::mutex> lock(impl_->snapshot_mutex);
            impl_->current_snapshot.detector_reload_state =
                DetectorReloadState::FAILED;
            impl_->current_snapshot.detector_reload_error = validation_error;
            return false;
        }

        // 已结束的 std::thread 仍是 joinable；创建下一次请求前先回收句柄。
        if (impl_->detector_reload_thread.joinable()) {
            impl_->detector_reload_thread.join();
        }

        {
            std::lock_guard<std::mutex> lock(impl_->snapshot_mutex);
            impl_->current_snapshot.detector_reload_state =
                DetectorReloadState::LOADING;
            impl_->current_snapshot.detector_reload_error.clear();
        }
        impl_->detector_reload_running.store(true, std::memory_order_release);

        Impl* const state = impl_.get();
        impl_->detector_reload_thread = std::thread([state, config]() mutable {
            struct ReloadCompletion final {
                std::atomic<bool>& running;
                ~ReloadCompletion() {
                    running.store(false, std::memory_order_release);
                }
            } completion{state->detector_reload_running};

            try {
                LOG_INFO("runtime", "开始异步加载 Detector: model={}",
                         config.model_path);
                auto candidate = std::make_unique<Detector>(config);
                if (!candidate->load()) candidate.reset();
                std::string interop_error;
                if (candidate &&
                    !state->validate_d3d11_interop_detector(
                        *candidate, interop_error)) {
                    candidate.reset();
                }

                if (state->stop_requested.load(std::memory_order_acquire)) {
                    return;
                }

                if (!candidate) {
                    std::string reload_error = interop_error.empty()
                        ? "Detector 模型加载失败: " + config.model_path
                        : interop_error;
                    {
                        std::lock_guard<std::mutex> lock(
                            state->snapshot_mutex);
                        state->current_snapshot.detector_reload_state =
                            DetectorReloadState::FAILED;
                        state->current_snapshot.detector_reload_error.swap(
                            reload_error);
                    }
                    LOG_WARN(
                        "runtime", "Detector 热重载失败，继续使用旧模型: {}",
                        config.model_path);
                    return;
                }

                std::string provider = candidate->backend_name();
                std::string active_model_path = config.model_path;
                const std::string loaded_provider = provider;
                const std::string loaded_model_path = active_model_path;
                state->config.detector = std::move(config);
                std::unique_ptr<Detector> retired;
                std::uint64_t generation = 0;
                {
                    std::scoped_lock lock(
                        state->detector_mutex, state->snapshot_mutex);
                    if (state->stop_requested.load(
                            std::memory_order_acquire)) {
                        return;
                    }
                    retired = std::move(state->detector);
                    state->detector = std::move(candidate);
                    state->safety_gate.disarm();
                    state->aim_reset_requested.store(
                        true, std::memory_order_release);
                    state->current_snapshot.provider.swap(provider);
                    state->current_snapshot.active_model_path.swap(
                        active_model_path);
                    state->current_snapshot.detector_reload_state =
                        DetectorReloadState::SUCCEEDED;
                    state->current_snapshot.detector_reload_error.clear();
                    generation = ++state->current_snapshot.detector_generation;
                    state->current_snapshot.output_armed = false;
                }

                // 新模型不能继承旧轨迹和旧武装状态。retired 在本加载线程析构，
                // 避免 Pipeline 热路径释放 ORT/TensorRT 大型资源。
                LOG_INFO(
                    "runtime", "Detector 热重载成功: generation={}, provider={}, model={}",
                    generation, loaded_provider, loaded_model_path);
                retired.reset();
            } catch (...) {
                if (!state->stop_requested.load(std::memory_order_acquire)) {
                    try {
                        std::lock_guard<std::mutex> lock(
                            state->snapshot_mutex);
                        state->current_snapshot.detector_reload_state =
                            DetectorReloadState::FAILED;
                        state->current_snapshot.detector_reload_error =
                            "Detector 热重载发生未知异常";
                    } catch (...) {
                    }
                    LOG_ERROR("runtime", "Detector 热重载发生未知异常");
                }
            }
        });
        return true;
    } catch (...) {
        impl_->detector_reload_running.store(false, std::memory_order_release);
        try {
            std::lock_guard<std::mutex> lock(impl_->snapshot_mutex);
            impl_->current_snapshot.detector_reload_state =
                DetectorReloadState::FAILED;
            impl_->current_snapshot.detector_reload_error =
                "创建 Detector 重载线程失败";
        } catch (...) {
        }
        return false;
    }
}

bool Runtime::post_intent(const RuntimeIntent& intent) noexcept {
    if (!impl_) return false;
    switch (intent.type) {
        case RuntimeIntentType::ARM_OUTPUT:
            // 重载完成会强制解除控制武装；加载窗口拒绝新的武装请求，
            // 避免指针交换与主线程 ARM 意图竞争后意外恢复状态。
            if (impl_->detector_reload_running.load(
                    std::memory_order_acquire) ||
                !impl_->safety_gate.arm()) {
                return false;
            }
            break;
        case RuntimeIntentType::DISARM_OUTPUT:
            impl_->safety_gate.disarm();
            if (auto worker = impl_->auto_stop_worker.load()) worker->cancel();
            if (auto trigger = impl_->trigger_worker.load()) trigger->cancel();
            if (auto recoil = impl_->recoil_worker.load()) recoil->cancel();
            break;
        case RuntimeIntentType::INPUT_HEALTH_CHANGED:
            impl_->safety_gate.set_input_health(intent.active);
            if (!intent.active) {
                if (auto worker = impl_->auto_stop_worker.load()) worker->cancel();
            if (auto trigger = impl_->trigger_worker.load()) trigger->cancel();
            if (auto recoil = impl_->recoil_worker.load()) recoil->cancel();
            }
            break;
        case RuntimeIntentType::AIM_HOLD_CHANGED:
            impl_->safety_gate.set_hold(intent.active);
            // 按住键只控制物理发送门。Aim 在未按键期间仍按每个观测帧持续
            // 更新轨迹、预测和最新命令，下一次按下即可直接放行当前结果。
            break;
        case RuntimeIntentType::EMERGENCY_STOP:
            impl_->safety_gate.emergency_stop();
            if (auto worker = impl_->auto_stop_worker.load()) worker->cancel();
            if (auto trigger = impl_->trigger_worker.load()) trigger->cancel();
            if (auto recoil = impl_->recoil_worker.load()) recoil->cancel();
            impl_->aim_reset_requested.store(true, std::memory_order_release);
            break;
        case RuntimeIntentType::RESET_EMERGENCY:
            if (!impl_->safety_gate.reset_emergency()) return false;
            break;
        case RuntimeIntentType::SET_AUTO_STOP_PAUSED: {
            auto worker = impl_->auto_stop_worker.load();
            if (!worker) return false;
            worker->set_paused(intent.active);
            return true;
        }
        case RuntimeIntentType::START:
        case RuntimeIntentType::STOP:
            return false;
    }
    try {
        std::lock_guard<std::mutex> lock(impl_->snapshot_mutex);
        impl_->current_snapshot.output_armed =
            impl_->safety_gate.output_armed();
        impl_->current_snapshot.input_healthy =
            impl_->safety_gate.input_healthy();
        impl_->current_snapshot.aim_hold_active =
            impl_->safety_gate.hold_active();
        impl_->current_snapshot.emergency_stopped =
            impl_->safety_gate.emergency_stopped();
    } catch (...) {
        return false;
    }
    return true;
}

bool Runtime::request_auto_stop(std::uint64_t request_id) noexcept {
    if (!impl_ || !impl_->safety_gate.can_dispatch_auxiliary() ||
        impl_->stop_requested.load(std::memory_order_acquire)) return false;
    {
        std::lock_guard<std::mutex> lock(impl_->snapshot_mutex);
        if (impl_->current_snapshot.state != RuntimeState::RUNNING) return false;
    }
    if (request_id == 0) return false;
    auto previous = impl_->stop_request_watermark.load();
    do { if (request_id <= previous) return false; } while (!impl_->stop_request_watermark.compare_exchange_weak(previous, request_id));
    auto worker = impl_->auto_stop_worker.load();
    return worker && worker->request(request_id);
}

void Runtime::cancel_auto_stop(std::uint64_t request_id) noexcept {
    if (!impl_) return;
    if (auto worker = impl_->auto_stop_worker.load()) worker->cancel(request_id);
}

RuntimeSnapshot Runtime::snapshot() const noexcept {
    if (!impl_) return {};
    try {
        std::lock_guard<std::mutex> lock(impl_->snapshot_mutex);
        auto result = impl_->current_snapshot;
        if (auto archive = impl_->recoil_archive.load()) result.recoil_archive = archive->snapshot();
        if (auto trigger = impl_->trigger_worker.load()) {
            result.trigger = trigger->snapshot(); result.trigger_telemetry_available = true;
        }
        result.source_context = impl_->source_context_client.snapshot();
        if (impl_->recoil_worker.load() || impl_->trigger_worker.load() || result.state == RuntimeState::RUNNING)
            result.weapon_snapshot = impl_->gsi_receiver.snapshot();
        if (auto recoil = impl_->recoil_worker.load()) {
            result.recoil = recoil->snapshot(); result.recoil_telemetry_available = true;
            const auto& weapon_id = result.weapon_snapshot.canonical_id;
            const auto found = impl_->recoil_profile_statuses.find(weapon_id);
            result.recoil_profile_status = weapon_id.empty() ? "等待有效武器身份" :
                found == impl_->recoil_profile_statuses.end() ? weapon_id + "：无匹配曲线" : found->second;
        }
        if (auto worker = impl_->auto_stop_worker.load()) {
            result.auto_stop = worker->snapshot();
        }
        return result;
    } catch (...) {
        return {};
    }
}

bool Runtime::set_preview_enabled(bool enabled) noexcept {
    if (!impl_) return false;
    if (enabled && impl_->gpu_interop_enabled()) {
        return false;
    }
    const bool succeeded = impl_->preview_channel.set_enabled(enabled);
    if (!succeeded) return false;
    try {
        const auto stats = impl_->preview_channel.stats();
        std::lock_guard<std::mutex> lock(impl_->snapshot_mutex);
        impl_->current_snapshot.preview_enabled = stats.enabled;
        impl_->current_snapshot.preview_sampled_frames = stats.sampled_frames;
        impl_->current_snapshot.preview_dropped_frames = stats.dropped_frames;
        return true;
    } catch (...) {
        return false;
    }
}

std::shared_ptr<const RuntimePreviewFrame>
Runtime::preview_frame() const noexcept {
    if (!impl_) return nullptr;
    return impl_->preview_channel.latest();
}

bool Runtime::drain_pipeline_samples(
        std::vector<RuntimePipelineSample>& samples) noexcept {
    if (!impl_) return false;
    return impl_->debug_samples.drain(samples);
}

RecoilExecutionLog Runtime::recoil_execution_log() const {
    if (!impl_) return {};
    if (auto worker = impl_->recoil_worker.load()) return worker->execution_log();
    std::lock_guard lock(impl_->snapshot_mutex);
    return impl_->current_snapshot.recoil_execution_log;
}

TriggerExecutionLog Runtime::trigger_execution_log() const {
    if (!impl_) return {};
    if (auto worker = impl_->trigger_worker.load()) return worker->execution_log();
    std::lock_guard lock(impl_->snapshot_mutex);
    return impl_->current_snapshot.trigger_execution_log;
}

OutputArbiterSnapshot Runtime::output_arbitration() const {
    if (!impl_) return {};
    // 冷路径与启停串行，避免读取正在重置的仲裁器；不在逐帧UI调用。
    std::lock_guard lifecycle_lock(impl_->lifecycle_mutex);
    if (impl_->output_arbiter) return impl_->output_arbiter->snapshot();
    std::lock_guard lock(impl_->snapshot_mutex);
    return impl_->current_snapshot.output_arbitration;
}
