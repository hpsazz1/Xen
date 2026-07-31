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
    mutable std::mutex lifecycle_mutex;
    mutable std::mutex snapshot_mutex;
    AppConfig config;
    RuntimeSnapshot current_snapshot;
    runtime::detail::LatestFrameQueue frame_queue;
    runtime::detail::SafetyGate safety_gate;
    std::unique_ptr<ICapture> capture;
    std::unique_ptr<Detector> detector;
    std::unique_ptr<Aim> aim;
    std::unique_ptr<IMouseController> mouse;
    std::unique_ptr<KeyboardListener> keyboard;
    std::thread capture_thread;
    std::thread pipeline_thread;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> aim_reset_requested{false};
    std::deque<double> pipeline_samples;
    std::chrono::steady_clock::time_point fps_started{};
    std::uint64_t fps_frame_count = 0;

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
        safety_gate.emergency_stop();
        stop_requested.store(true, std::memory_order_release);
        frame_queue.stop();
        try {
            std::lock_guard<std::mutex> lock(snapshot_mutex);
            current_snapshot.state = RuntimeState::FAILED;
            current_snapshot.last_error = error;
            current_snapshot.output_armed = false;
            current_snapshot.emergency_stopped = true;
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
        aim = std::make_unique<Aim>(config.aim);
        capture = create_capture(config.capture);
        if (!capture || !capture->open()) {
            set_error(capture ? capture->last_error() : "创建 Capture 失败");
            return false;
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

        {
            std::lock_guard<std::mutex> lock(snapshot_mutex);
            current_snapshot = {};
            current_snapshot.state = RuntimeState::STARTING;
            current_snapshot.capture_status = capture->status();
            current_snapshot.mouse_status = mouse->status();
            current_snapshot.provider = detector->backend_name();
            current_snapshot.output_allowed_by_config =
                config.mouse.allow_send_input;
        }
        pipeline_samples.clear();
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

            if (aim_reset_requested.exchange(false,
                    std::memory_order_acq_rel)) {
                aim->reset();
            }

            auto detections = detector->detect(frame->bgr);
            profile.detector = detector->profile();
            AimResult aim_result;
            bool mouse_sent = false;
            double mouse_elapsed_ms = 0.0;
            if (profile.detector.status == DetectionStatus::SUCCESS) {
                AimFrame aim_frame;
                aim_frame.sequence = frame->timing.sequence;
                aim_frame.captured_at = frame->timing.captured_at;
                aim_frame.roi_width = frame->bgr.cols;
                aim_frame.roi_height = frame->bgr.rows;
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
            update_pipeline_snapshot(*frame, profile, aim_result,
                                     mouse->status(), mouse_sent);
        }
    }

    void release_modules() noexcept {
        if (keyboard) keyboard->close();
        if (mouse) mouse->close();
        if (capture) capture->close();
        if (detector) detector->reset();
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
    if (impl_->capture_thread.joinable() || impl_->pipeline_thread.joinable()) {
        impl_->stop_requested.store(true, std::memory_order_release);
        impl_->frame_queue.stop();
        if (impl_->capture_thread.joinable()) impl_->capture_thread.join();
        if (impl_->pipeline_thread.joinable()) impl_->pipeline_thread.join();
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
    } catch (...) {
    }
    impl_->release_modules();
    {
        std::lock_guard<std::mutex> lock(impl_->snapshot_mutex);
        impl_->current_snapshot.state = RuntimeState::STOPPED;
        impl_->current_snapshot.capture_status = CaptureStatus::CLOSED;
        impl_->current_snapshot.mouse_status = MouseStatus::CLOSED;
        impl_->current_snapshot.output_armed = false;
        impl_->current_snapshot.aim_hold_active = false;
        impl_->current_snapshot.emergency_stopped = true;
    }
}

bool Runtime::post_intent(const RuntimeIntent& intent) noexcept {
    if (!impl_) return false;
    switch (intent.type) {
        case RuntimeIntentType::ARM_OUTPUT:
            if (!impl_->config.mouse.allow_send_input ||
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
