#include "runtime/runtime.h"

#include "keyboard/keyboard.h"
#include "log/log.h"
#include "runtime/runtime_internal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
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
    mutable std::mutex lifecycle_mutex;
    mutable std::mutex snapshot_mutex;
    // Detector 不支持 detect() 与资源切换并发；候选加载始终在锁外完成。
    mutable std::mutex detector_mutex;
    AppConfig config;
    RuntimeSnapshot current_snapshot;
    runtime::detail::LatestFrameQueue frame_queue;
    runtime::detail::RuntimePreviewChannel preview_channel;
    runtime::detail::SafetyGate safety_gate;
    std::unique_ptr<ICapture> capture;
    std::unique_ptr<Detector> detector;
    std::unique_ptr<Aim> aim;
    std::unique_ptr<IMouseController> mouse;
    std::unique_ptr<KeyboardListener> keyboard;
    std::thread capture_thread;
    std::thread pipeline_thread;
    std::thread detector_reload_thread;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> aim_reset_requested{false};
    std::atomic<bool> detector_reload_running{false};
    std::deque<double> pipeline_samples;
    runtime::detail::BoundedSampleRing<
        RuntimePipelineSample, kDebugSampleCapacity> debug_samples;
    std::chrono::steady_clock::time_point fps_started{};
    std::uint64_t fps_frame_count = 0;

    bool validate_d3d11_interop_detector(
            const Detector& candidate,
            std::string& error) const noexcept {
        if (!config.capture.enable_d3d11_cuda_interop) return true;
        if (candidate.backend_name() != "TensorrtExecutionProvider" ||
            !candidate.d3d11_interop_supported()) {
            error = "D3D11/CUDA 互操作要求实际 TensorRT CUDA Graph GPU 前处理";
            return false;
        }
        if (candidate.input_width() != config.capture.roi_width ||
            candidate.input_height() != config.capture.roi_height) {
            error = "D3D11/CUDA 互操作要求 Capture ROI 与模型输入尺寸完全一致";
            return false;
        }
        error.clear();
        return true;
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
        preview_channel.set_session_active(false);
        const auto preview_stats = preview_channel.stats();
        safety_gate.emergency_stop();
        stop_requested.store(true, std::memory_order_release);
        frame_queue.stop();
        try {
            std::lock_guard<std::mutex> lock(snapshot_mutex);
            current_snapshot.state = RuntimeState::FAILED;
            current_snapshot.last_error = error;
            current_snapshot.output_armed = false;
            current_snapshot.emergency_stopped = true;
            current_snapshot.preview_enabled = preview_stats.enabled;
            current_snapshot.preview_sampled_frames = 0;
            current_snapshot.preview_dropped_frames = 0;
        } catch (...) {
        }
        LOG_ERROR("runtime", "{}", error);
    }

    bool initialize(const AppConfig& value) {
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
        safety_gate.emergency_stop();
        safety_gate.set_hold(false);
        safety_gate.reset_emergency();

        detector = std::make_unique<Detector>(config.detector);
        if (!detector->load()) {
            set_error("Detector 模型加载失败");
            return false;
        }
        if (!validate_d3d11_interop_detector(*detector, validation_error)) {
            set_error(validation_error);
            return false;
        }
        if (config.capture.enable_d3d11_cuda_interop) {
            // 预览目前只接受 CPU BGR。互操作会话强制关闭诊断支路，避免为
            // UI 恢复逐帧 readback；后续应另做最高 10 FPS 的 GPU readback。
            if (!preview_channel.set_enabled(false)) {
                set_error("关闭不兼容的 ROI 预览失败");
                return false;
            }
        }
        aim = std::make_unique<Aim>(config.aim);
        capture = create_capture(config.capture);
        if (!capture || !capture->open()) {
            set_error(capture ? capture->last_error() : "创建 Capture 失败");
            return false;
        }
        if (config.capture.enable_d3d11_cuda_interop) {
            // 三槽纹理必须在 Capture/Pipeline 线程启动前完成创建和 CUDA 注册。
            // 若先提交 D3D copy 再从另一线程首次注册，驱动同步可能形成互等。
            const auto slots = frame_queue.initialization_slots();
            for (const auto& slot : slots) {
                if (!slot || !capture->prepare_frame(*slot)) {
                    set_error(capture->last_error().empty()
                        ? "预创建 D3D11/CUDA 帧槽失败"
                        : capture->last_error());
                    return false;
                }
                if (!detector->prepare_d3d11({
                        slot->native_storage,
                        slot->native_synchronization,
                        slot->width,
                        slot->height})) {
                    set_error("预注册 D3D11/CUDA 帧槽失败");
                    return false;
                }
            }
            LOG_INFO("runtime", "D3D11/CUDA 三槽纹理已预创建并注册");
        }
        mouse = MouseDeviceFactory::create(config.mouse);
        if (!mouse || !mouse->open()) {
            set_error(mouse ? mouse->last_error() : "创建 Mouse 失败");
            return false;
        }
        keyboard = std::make_unique<KeyboardListener>(config.keyboard);
        if (!keyboard->open()) {
            set_error("Keyboard 初始化失败");
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
            current_snapshot.provider = detector->backend_name();
            current_snapshot.active_model_path = config.detector.model_path;
            current_snapshot.detector_generation = 1;
            current_snapshot.output_allowed_by_config =
                config.mouse.allow_send_input;
            current_snapshot.preview_enabled = preview_enabled;
            current_snapshot.d3d11_cuda_interop =
                config.capture.enable_d3d11_cuda_interop;
        }
        pipeline_samples.clear();
        debug_samples.reset();
        fps_started = std::chrono::steady_clock::now();
        fps_frame_count = 0;
        return true;
    }

    void capture_loop() noexcept {
        while (!stop_requested.load(std::memory_order_acquire)) {
            auto write_slot = frame_queue.acquire_write();
            if (!write_slot) {
                std::this_thread::yield();
                continue;
            }
            const CaptureStatus capture_status = capture->grab(*write_slot);
            if (capture_status == CaptureStatus::FRAME) {
                frame_queue.publish(write_slot);
                const auto now = std::chrono::steady_clock::now();
                ++fps_frame_count;
                const double elapsed =
                    std::chrono::duration<double>(now - fps_started).count();
                std::lock_guard<std::mutex> lock(snapshot_mutex);
                current_snapshot.capture_status = capture_status;
                ++current_snapshot.captured_frames;
                current_snapshot.last_profile.capture_ms =
                    write_slot->timing.capture_ms;
                current_snapshot.source_dropped_frames =
                    write_slot->timing.source_dropped_frames;
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

    void update_pipeline_snapshot(const CapturedFrame& frame,
                                  const PipelineProfile& profile,
                                  const AimResult& aim_result,
                                  MouseStatus mouse_status,
                                  bool mouse_sent) {
        const auto preview_stats = preview_channel.stats();
        std::lock_guard<std::mutex> lock(snapshot_mutex);
        current_snapshot.last_sequence = frame.timing.sequence;
        current_snapshot.last_profile = profile;
        current_snapshot.detection_status = profile.detector.status;
        current_snapshot.aim_status = aim_result.status;
        current_snapshot.mouse_status = mouse_status;
        current_snapshot.last_aim = aim_result;
        current_snapshot.overwritten_frames =
            frame_queue.overwritten_frames();
        ++current_snapshot.processed_frames;
        if (profile.detector.status != DetectionStatus::SUCCESS ||
            aim_result.status != AimStatus::SUCCESS) {
            ++current_snapshot.failed_frames;
        }
        if (mouse_sent) ++current_snapshot.mouse_commands;
        current_snapshot.output_armed = safety_gate.armed();
        current_snapshot.aim_hold_active = safety_gate.hold_active();
        current_snapshot.emergency_stopped =
            safety_gate.emergency_stopped();

        RuntimePipelineSample sample;
        sample.sequence = frame.timing.sequence;
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
        sample.detection_status = profile.detector.status;
        sample.aim_status = aim_result.status;
        sample.mouse_status = mouse_status;
        sample.mouse_sent = mouse_sent;
        debug_samples.push(sample);
        current_snapshot.debug_samples_dropped = debug_samples.dropped();
        current_snapshot.preview_enabled = preview_stats.enabled;
        current_snapshot.preview_sampled_frames =
            preview_stats.sampled_frames;
        current_snapshot.preview_dropped_frames =
            preview_stats.dropped_frames;

        pipeline_samples.push_back(profile.total_ms);
        while (pipeline_samples.size() >
               static_cast<std::size_t>(config.runtime.profile_window)) {
            pipeline_samples.pop_front();
        }
        const std::vector<double> samples(
            pipeline_samples.begin(), pipeline_samples.end());
        current_snapshot.pipeline_p50_ms = percentile(samples, 0.50);
        current_snapshot.pipeline_p95_ms = percentile(samples, 0.95);
    }

    void pipeline_loop() noexcept {
        std::uint64_t last_sequence = 0;
        while (!stop_requested.load(std::memory_order_acquire)) {
            const auto frame = frame_queue.wait_latest(
                last_sequence, stop_requested);
            if (!frame) continue;
            last_sequence = frame->timing.sequence;
            const auto pipeline_started = std::chrono::steady_clock::now();
            PipelineProfile profile;
            profile.capture_ms = frame->timing.capture_ms;
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
                }
                if (frame->storage ==
                    CapturedFrameStorage::D3D11_BGRA8) {
                    detections = detector->detect_d3d11({
                        frame->native_storage,
                        frame->native_synchronization,
                        frame->width,
                        frame->height});
                } else {
                    detections = detector->detect(frame->bgr);
                }
                profile.detector = detector->profile();
            }
            AimResult aim_result;
            AimFrame aim_frame;
            bool mouse_sent = false;
            double mouse_elapsed_ms = 0.0;
            if (profile.detector.status == DetectionStatus::SUCCESS) {
                aim_frame.sequence = frame->timing.sequence;
                aim_frame.captured_at = frame->timing.captured_at;
                aim_frame.roi_width = frame->width;
                aim_frame.roi_height = frame->height;
                aim_frame.control_center_x = static_cast<float>(
                    (frame->source_width * 0.5 - frame->roi_x) /
                    frame->source_pixels_per_pixel_x);
                aim_frame.control_center_y = static_cast<float>(
                    (frame->source_height * 0.5 - frame->roi_y) /
                    frame->source_pixels_per_pixel_y);
                aim_frame.source_pixels_per_roi_pixel_x =
                    static_cast<float>(frame->source_pixels_per_pixel_x);
                aim_frame.source_pixels_per_roi_pixel_y =
                    static_cast<float>(frame->source_pixels_per_pixel_y);
                aim_frame.detections = std::move(detections);
                aim_result = aim->process(aim_frame);
                profile.aim = aim_result.profile;

                if (aim_result.status == AimStatus::SUCCESS &&
                    aim_result.has_command && safety_gate.can_dispatch()) {
                    const MouseMoveCommand command{
                        aim_result.command.dx_counts,
                        aim_result.command.dy_counts};
                    const auto mouse_started = std::chrono::steady_clock::now();
                    mouse_sent = mouse->move(command);
                    mouse_elapsed_ms =
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() -
                            mouse_started).count();
                    if (!mouse_sent) {
                        safety_gate.emergency_stop();
                        aim_reset_requested.store(true,
                                                  std::memory_order_release);
                        set_error(mouse->last_error());
                    }
                }
            } else {
                aim_result.status = AimStatus::NOT_RUN;
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
            if (frame->storage == CapturedFrameStorage::CPU_BGR) {
                preview_channel.publish(
                    frame->bgr, frame->timing.sequence,
                    control_center_x, control_center_y,
                    profile.detector.status, aim_result.status,
                    preview_detections, aim_result,
                    std::chrono::steady_clock::now());
            }
            update_pipeline_snapshot(*frame, profile, aim_result,
                                     mouse->status(), mouse_sent);
        }
    }

    void release_modules() noexcept {
        if (keyboard) keyboard->close();
        if (mouse) mouse->close();
        // CUDA registration 持有 D3D11 资源引用。先销毁 Detector/registration，
        // 再关闭 Capture 的 D3D11 设备，保持跨 API 释放顺序可解释。
        if (detector) detector->reset();
        if (capture) capture->close();
        if (aim) aim->reset();
        keyboard.reset();
        mouse.reset();
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
        if (!impl_->initialize(config)) {
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
    impl_->detector_reload_running.store(false, std::memory_order_release);
    impl_->release_modules();
    // Pipeline 已退出后清除旧预览，但保留三槽大缓冲和用户的启用选择。
    // 下次启动会从新会话首帧重新发布，不把停止前图像误当成实时画面。
    impl_->preview_channel.set_session_active(false);
    const auto preview_stats = impl_->preview_channel.stats();
    {
        std::lock_guard<std::mutex> lock(impl_->snapshot_mutex);
        impl_->current_snapshot.state = RuntimeState::STOPPED;
        impl_->current_snapshot.capture_status = CaptureStatus::CLOSED;
        impl_->current_snapshot.mouse_status = MouseStatus::CLOSED;
        impl_->current_snapshot.detector_reload_state =
            DetectorReloadState::IDLE;
        impl_->current_snapshot.detector_reload_error.clear();
        impl_->current_snapshot.output_armed = false;
        impl_->current_snapshot.aim_hold_active = false;
        impl_->current_snapshot.emergency_stopped = true;
        impl_->current_snapshot.preview_enabled = preview_stats.enabled;
        impl_->current_snapshot.preview_sampled_frames = 0;
        impl_->current_snapshot.preview_dropped_frames = 0;
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

        if (impl_->config.capture.enable_d3d11_cuda_interop) {
            // 互操作预处理器会跨帧缓存三槽 D3D11 资源的 CUDA 注册。
            // 热重载销毁旧 Session 时 Capture 线程仍可能向空闲槽提交 GPU copy，
            // 因此首版契约要求先停止 Runtime，再更换模型并重新启动。
            std::lock_guard<std::mutex> lock(impl_->snapshot_mutex);
            impl_->current_snapshot.detector_reload_state =
                DetectorReloadState::FAILED;
            impl_->current_snapshot.detector_reload_error =
                "D3D11/CUDA 互操作启用时不支持 Detector 热重载；"
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
            // 重载完成会强制解除武装；加载窗口拒绝新的武装请求，避免
            // 指针交换与主线程 ARM 意图竞争后意外恢复输出。
            if (impl_->detector_reload_running.load(
                    std::memory_order_acquire) ||
                !impl_->config.mouse.allow_send_input ||
                !impl_->safety_gate.arm()) {
                return false;
            }
            break;
        case RuntimeIntentType::DISARM_OUTPUT:
            impl_->safety_gate.disarm();
            impl_->aim_reset_requested.store(true, std::memory_order_release);
            break;
        case RuntimeIntentType::AIM_HOLD_CHANGED:
            impl_->safety_gate.set_hold(intent.active);
            if (!intent.active) {
                impl_->aim_reset_requested.store(true,
                                                  std::memory_order_release);
            }
            break;
        case RuntimeIntentType::EMERGENCY_STOP:
            impl_->safety_gate.emergency_stop();
            impl_->aim_reset_requested.store(true, std::memory_order_release);
            break;
        case RuntimeIntentType::RESET_EMERGENCY:
            if (!impl_->safety_gate.reset_emergency()) return false;
            break;
        case RuntimeIntentType::START:
        case RuntimeIntentType::STOP:
            return false;
    }
    try {
        std::lock_guard<std::mutex> lock(impl_->snapshot_mutex);
        impl_->current_snapshot.output_armed = impl_->safety_gate.armed();
        impl_->current_snapshot.aim_hold_active =
            impl_->safety_gate.hold_active();
        impl_->current_snapshot.emergency_stopped =
            impl_->safety_gate.emergency_stopped();
    } catch (...) {
        return false;
    }
    return true;
}

void Runtime::poll_keyboard() noexcept {
    if (!impl_ || !impl_->keyboard) return;
    for (const auto& event : impl_->keyboard->poll()) {
        if (event.type == KeyboardEventType::AIM_HOLD_CHANGED) {
            post_intent({RuntimeIntentType::AIM_HOLD_CHANGED, event.active});
        } else if (event.type == KeyboardEventType::EMERGENCY_STOP) {
            post_intent({RuntimeIntentType::EMERGENCY_STOP, true});
        }
    }
}

RuntimeSnapshot Runtime::snapshot() const noexcept {
    if (!impl_) return {};
    try {
        std::lock_guard<std::mutex> lock(impl_->snapshot_mutex);
        return impl_->current_snapshot;
    } catch (...) {
        return {};
    }
}

bool Runtime::set_preview_enabled(bool enabled) noexcept {
    if (!impl_) return false;
    if (enabled && impl_->config.capture.enable_d3d11_cuda_interop) {
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
