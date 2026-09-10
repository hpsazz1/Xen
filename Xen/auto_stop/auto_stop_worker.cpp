#include "auto_stop/auto_stop_worker.h"
#include "log/log.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
std::int64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}
std::uint8_t held_wasd(const InputSnapshot& input) noexcept {
    return static_cast<std::uint8_t>((input.virtual_keys['W'] ? 1 : 0) |
        (input.virtual_keys['A'] ? 2 : 0) | (input.virtual_keys['S'] ? 4 : 0) | (input.virtual_keys['D'] ? 8 : 0));
}
}

std::unique_lock<std::timed_mutex> AutoStopOutputArbiter::try_enter_aim() noexcept {
    std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
    if (!faulted_.load(std::memory_order_acquire) && !auxiliary_pending_.load(std::memory_order_acquire) && lock.try_lock()) {
        if (!faulted_.load(std::memory_order_acquire) && !auxiliary_pending_.load(std::memory_order_acquire)) return lock;
        lock.unlock();
    }
    aim_skips_.fetch_add(1, std::memory_order_relaxed);
    return lock;
}

std::unique_lock<std::timed_mutex> AutoStopOutputArbiter::try_enter_cleanup() noexcept {
    std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
    (void)lock.try_lock_for(std::chrono::milliseconds(20));
    return lock;
}

class AutoStopWorker::Impl {
public:
    Impl(std::shared_ptr<IMouseController> device, std::shared_ptr<AutoStopOutputArbiter> gate,
        std::function<bool()> permission) : mouse(std::move(device)), arbiter(std::move(gate)), allowed(std::move(permission)) {}
    std::shared_ptr<IMouseController> mouse;
    std::shared_ptr<AutoStopOutputArbiter> arbiter;
    std::function<bool()> allowed;
    AutoStopConfig config;
    int command_timeout_ms = 300;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::thread thread;
    std::atomic<bool> stopping{false}, paused{false};
    std::atomic<std::uint64_t> cancel_generation{0};
    bool running = false, fault = false;
    std::uint64_t pending_id = 0, last_request_id = 0, submitted_generation = 0;
    Clock::time_point submitted_at;
    AutoStopSnapshot state;

    void publish(AutoStopStatus status) {
        std::lock_guard<std::mutex> lock(mutex);
        state.status = status;
    }
    bool permission(const InputSnapshot& input) noexcept {
        try {
            return allowed && allowed() && input.state_valid && input.status == InputMonitorStatus::READY &&
                !input.virtual_keys[0x23] && config.activation_virtual_key > 0 &&
                config.activation_virtual_key < 256 && input.virtual_keys[config.activation_virtual_key] &&
                !paused.load(std::memory_order_acquire) && !stopping.load(std::memory_order_acquire);
        } catch (...) { return false; }
    }
    void receipt(const KeyboardReceipt& result, std::int64_t started) {
        if (result.disposition != KeyboardDisposition::ACKNOWLEDGED ||
            (!result.datagram_sent && result.protocol_ack_received_at == Clock::time_point{})) return;
        std::lock_guard<std::mutex> lock(mutex);
        ++state.acknowledged_commands;
        state.max_ack_wait_ns = std::max(state.max_ack_wait_ns, now_ns() - started);
    }
    void run() noexcept {
        std::unique_lock<std::timed_mutex> output(arbiter->mutex_, std::defer_lock);
        WasdInputHistory history;
        WasdMotionIntent intent;
        WasdEventCursor cursor;
        AutoStopController controller;
        InputSnapshot input;
        std::uint64_t active_id = 0, active_generation = 0;
        std::uint8_t software_mask = 0, original_mask = 0;
        bool debt = false, estimated = false;
        Clock::time_point lease_end;
        auto release_reservation = [&]() {
            if (output.owns_lock()) output.unlock();
            arbiter->auxiliary_pending_.store(false, std::memory_order_release);
        };
        auto reserve = [&]() {
            if (output.owns_lock()) return true;
            arbiter->auxiliary_pending_.store(true, std::memory_order_release);
            const auto started = now_ns();
            const bool acquired = output.try_lock_until(Clock::now() + std::chrono::milliseconds(command_timeout_ms + 5));
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++state.arbiter_wait_samples;
                state.max_arbiter_wait_ns = std::max(state.max_arbiter_wait_ns, now_ns() - started);
            }
            if (!acquired) arbiter->auxiliary_pending_.store(false, std::memory_order_release);
            return acquired;
        };
        auto clean = [&]() {
            bool success = true;
            if (debt) {
                success = reserve();
                if (success) {
                    const auto started = now_ns();
                    const auto result = mouse->cleanup_wasd_keyboard();
                    receipt(result, started);
                    success = result.disposition == KeyboardDisposition::ACKNOWLEDGED;
                    std::lock_guard<std::mutex> lock(mutex);
                    ++state.cleanup_attempts;
                    if (!success) ++state.cleanup_failures;
                }
                if (success) {
                    debt = false; software_mask = 0;
                    std::lock_guard<std::mutex> lock(mutex);
                    state.cleanup_unknown = false;
                }
            }
            if (!success) arbiter->faulted_.store(true, std::memory_order_release);
            release_reservation();
            if (!success) {
                LOG_WARN("auto_stop", "请求{}清理未确认，保留FAULT并阻止Aim发送", active_id);
                std::lock_guard<std::mutex> lock(mutex);
                fault = true;
                state.cleanup_unknown = true;
                state.status = AutoStopStatus::FAULT;
            }
            return success;
        };
        auto cancel_active = [&](bool force_fault, const char* reason) {
            LOG_DEBUG("auto_stop", "取消请求{}，原因={}", active_id, reason);
            (void)reason;
            if (active_id) controller.cancel(active_id, now_ns());
            const bool clean_ok = clean();
            const bool retain_release = clean_ok && !force_fault && intent.history_valid && !intent.conflicting &&
                intent.held_mask == 0 && input.state_valid && input.status == InputMonitorStatus::READY && held_wasd(input) == 0;
            if (retain_release) controller.observe(intent, now_ns());
            std::lock_guard<std::mutex> lock(mutex);
            if (active_id) ++state.canceled;
            if (fault || force_fault || !clean_ok) { fault = true; state.status = AutoStopStatus::FAULT; }
            else state.status = paused.load() ? AutoStopStatus::PAUSED : AutoStopStatus::CANCELED;
            active_id = 0;
            estimated = false;
            if (!retain_release) { history.reset(); intent = {}; }
        };
        try {
            while (!stopping.load(std::memory_order_acquire)) {
                bool input_ok = mouse->poll_input(input) && input.state_valid && input.status == InputMonitorStatus::READY;
                WasdEventBatch batch;
                const bool events_ok = mouse->read_wasd_events(cursor, batch) && batch.subscribed && !batch.gap;
                if (!events_ok) { history.reset(); intent = {}; }
                if (events_ok) for (std::size_t i = 0; i < batch.count; ++i) {
                    const auto& event = batch.events[i];
                    intent = history.observe(event.held_mask, event.epoch, event.sequence, event.received_at_steady_ns, event.state_valid);
                    if (!estimated) controller.observe(intent, event.received_at_steady_ns);
                }
                bool latched_fault;
                { std::lock_guard<std::mutex> lock(mutex); latched_fault = fault; }
                if (active_id && (!input_ok || !events_ok || !intent.history_valid || !permission(input) ||
                    held_wasd(input) != original_mask || active_generation != cancel_generation.load() || Clock::now() >= lease_end)) {
                    const char* reason = !input_ok ? "input_invalid" : !events_ok ? "input_gap" :
                        !intent.history_valid ? "history_invalid" : paused.load() ? "paused" :
                        !permission(input) ? "permission_revoked" : held_wasd(input) != original_mask ? "direction_changed" :
                        active_generation != cancel_generation.load() ? "caller_canceled" : "lease_expired";
                    cancel_active(false, reason);
                }
                if (latched_fault) {
                    std::unique_lock<std::mutex> lock(mutex);
                    wake.wait_for(lock, std::chrono::milliseconds(1));
                    continue;
                }
                if (!active_id) {
                    std::uint64_t requested = 0;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        requested = pending_id;
                        if (requested) {
                            pending_id = 0;
                            active_generation = submitted_generation;
                            lease_end = submitted_at + std::chrono::milliseconds(500);
                        }
                    }
                    if (requested) {
                        active_id = requested;
                        original_mask = intent.held_mask;
                        estimated = false;
                        if (!input_ok || !events_ok || !intent.history_valid || !permission(input) ||
                            active_generation != cancel_generation.load() || Clock::now() >= lease_end) {
                            cancel_active(false, "request_not_eligible");
                        } else {
                            const auto decision = controller.request(active_id, now_ns());
                            if (decision.phase != AutoStopPhase::WAITING_ACK || decision.request_id != active_id || !reserve()) {
                                cancel_active(false, "controller_or_arbiter_unavailable");
                            } else {
                                publish(AutoStopStatus::BRAKING);
                                LOG_DEBUG("auto_stop", "开始请求{}，物理方向mask={}", active_id, original_mask);
                                for (std::uint8_t key = 1; key <= 8; key <<= 1) if (original_mask & key) {
                                    if (!mouse->poll_input(input) || !permission(input) || held_wasd(input) != original_mask ||
                                        active_generation != cancel_generation.load() || Clock::now() >= lease_end) { cancel_active(false, "permission_changed_before_mask"); break; }
                                    const auto started = now_ns();
                                    const auto result = mouse->set_wasd_mask(key, true);
                                    debt |= result.datagram_sent;
                                    receipt(result, started);
                                    if (result.disposition != KeyboardDisposition::ACKNOWLEDGED) { cancel_active(true, "mask_not_acknowledged"); break; }
                                }
                            }
                        }
                    } else if (!fault) {
                        publish(config.activation_virtual_key == 0 ? AutoStopStatus::UNBOUND : paused.load() ? AutoStopStatus::PAUSED :
                            (input_ok && events_ok && intent.history_valid ? AutoStopStatus::READY : AutoStopStatus::WAITING_INPUT));
                    }
                }
                if (active_id) {
                    const auto previous = controller.decision();
                    const auto decision = controller.tick(now_ns());
                    if (decision.phase == AutoStopPhase::INVALID || decision.phase == AutoStopPhase::CANCELLED) cancel_active(false, "controller_canceled");
                    else if (decision.phase == AutoStopPhase::WAITING_ACK) {
                        if (!mouse->poll_input(input) || !permission(input) || held_wasd(input) != original_mask ||
                            active_generation != cancel_generation.load() || Clock::now() >= lease_end) cancel_active(false, "permission_changed_before_report");
                        else {
                            const auto started = now_ns();
                            if ((software_mask & ~decision.desired_mask) != 0) {
                                std::int64_t due = started;
                                for (const auto deadline : previous.axis_deadline_ns) if (deadline > 0 && deadline <= started) due = std::min(due, deadline);
                                std::lock_guard<std::mutex> lock(mutex);
                                ++state.release_commands;
                                // 这里只测释放调用开始相对模型deadline的迟到，绝非设备已释放时刻。
                                state.max_release_overshoot_ns = std::max(state.max_release_overshoot_ns, started - due);
                            }
                            const auto result = mouse->set_wasd_keyboard(decision.desired_mask);
                            debt |= result.datagram_sent;
                            receipt(result, started);
                            if (result.disposition != KeyboardDisposition::ACKNOWLEDGED) cancel_active(true, "keyboard_not_acknowledged");
                            else {
                                software_mask = decision.desired_mask;
                                const auto completed = controller.acknowledge(active_id, decision.command_id, software_mask, now_ns());
                                if (completed.phase == AutoStopPhase::COMPLETE_ESTIMATED) {
                                    estimated = true;
                                    LOG_DEBUG("auto_stop", "请求{}反向软件键释放已确认，进入估算完成，未授予开火", active_id);
                                    { std::lock_guard<std::mutex> lock(mutex); ++state.completed; state.status = AutoStopStatus::ESTIMATED; }
                                    // 软件键已经释放，物理屏蔽的有界租期内允许Aim继续发送。
                                    release_reservation();
                                }
                            }
                        }
                    } else if (estimated) publish(AutoStopStatus::ESTIMATED);
                }
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait_for(lock, std::chrono::milliseconds(1));
            }
            if (active_id || debt) cancel_active(false, "worker_stopped");
        } catch (...) {
            cancel_active(true, "worker_exception");
        }
        mouse->set_wasd_event_subscription(false);
        release_reservation();
        std::lock_guard<std::mutex> lock(mutex);
        running = false;
        pending_id = 0;
        if (!fault) state.status = AutoStopStatus::DISABLED;
    }
};

AutoStopWorker::AutoStopWorker(std::shared_ptr<IMouseController> mouse,
        std::shared_ptr<AutoStopOutputArbiter> arbiter, std::function<bool()> permission)
    : impl_(std::make_unique<Impl>(std::move(mouse), std::move(arbiter), std::move(permission))) {}
AutoStopWorker::~AutoStopWorker() { stop(); }
bool AutoStopWorker::start(const AutoStopConfig& config, int command_timeout_ms) noexcept {
    if (!impl_) return false;
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->running || impl_->thread.joinable() || impl_->fault) return false;
        impl_->config = config;
        impl_->state = {};
        if (!config.enabled) return true;
        if (!impl_->mouse || !impl_->arbiter || impl_->arbiter->faulted_.load() || !impl_->mouse->output_owner_exclusive() ||
            !impl_->mouse->supports_wasd_keyboard() || command_timeout_ms < 1 || command_timeout_ms > 1000) return false;
        // App共享owner可跨Runtime存活；新worker不能假定上个会话已清掉设备债务。
        impl_->state.telemetry_available = true;
        impl_->state.device_protocol_available = true;
        const auto cleanup_started = now_ns();
        const auto initial_cleanup = impl_->mouse->cleanup_wasd_keyboard();
        ++impl_->state.cleanup_attempts;
        if (initial_cleanup.disposition != KeyboardDisposition::ACKNOWLEDGED) {
            ++impl_->state.cleanup_failures;
            impl_->state.cleanup_unknown = true;
            impl_->state.status = AutoStopStatus::FAULT;
            impl_->fault = true;
            impl_->arbiter->faulted_.store(true, std::memory_order_release);
            LOG_WARN("auto_stop", "共享设备的历史键盘债务清理未确认，拒绝启动新会话");
            return false;
        }
        if (initial_cleanup.datagram_sent || initial_cleanup.protocol_ack_received_at != Clock::time_point{}) {
            ++impl_->state.acknowledged_commands;
            impl_->state.max_ack_wait_ns = now_ns() - cleanup_started;
        }
        if (!impl_->mouse->set_wasd_event_subscription(true)) return false;
        impl_->command_timeout_ms = command_timeout_ms;
        impl_->stopping.store(false);
        impl_->paused.store(false);
        impl_->state.status = config.activation_virtual_key == 0 ? AutoStopStatus::UNBOUND : AutoStopStatus::WAITING_INPUT;
        impl_->state.device_protocol_available = true;
        impl_->state.telemetry_available = true;
        impl_->running = true;
        Log::register_module("auto_stop", LogLevel::INFO);
        impl_->thread = std::thread([this]() { impl_->run(); });
        return true;
    } catch (...) {
        if (impl_->mouse) impl_->mouse->set_wasd_event_subscription(false);
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->running = false;
        impl_->state.status = AutoStopStatus::FAULT;
        return false;
    }
}
void AutoStopWorker::stop() noexcept {
    if (!impl_) return;
    impl_->stopping.store(true, std::memory_order_release);
    impl_->wake.notify_all();
    try { if (impl_->thread.joinable()) impl_->thread.join(); } catch (...) {}
}
bool AutoStopWorker::request(std::uint64_t id) noexcept {
    if (!impl_ || id == 0) return false;
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->running || impl_->fault || impl_->paused.load() || impl_->stopping.load() ||
            impl_->pending_id || id <= impl_->last_request_id || impl_->state.status != AutoStopStatus::READY) return false;
        impl_->last_request_id = id;
        impl_->pending_id = id;
        impl_->submitted_at = Clock::now();
        impl_->submitted_generation = impl_->cancel_generation.load();
        impl_->state.request_id = id;
        ++impl_->state.requests;
        impl_->state.status = AutoStopStatus::BRAKING;
        impl_->wake.notify_all();
        return true;
    } catch (...) { return false; }
}
void AutoStopWorker::cancel() noexcept {
    if (!impl_) return;
    impl_->cancel_generation.fetch_add(1, std::memory_order_acq_rel);
    impl_->wake.notify_all();
}
void AutoStopWorker::cancel(std::uint64_t request_id) noexcept {
    if (!impl_ || request_id == 0) return;
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->running || impl_->state.request_id != request_id) return;
        impl_->cancel_generation.fetch_add(1, std::memory_order_acq_rel);
        impl_->wake.notify_all();
    } catch (...) {}
}
void AutoStopWorker::set_paused(bool paused) noexcept {
    if (!impl_) return;
    impl_->paused.store(paused, std::memory_order_release);
    if (paused) impl_->cancel_generation.fetch_add(1, std::memory_order_acq_rel);
    impl_->wake.notify_all();
}
AutoStopSnapshot AutoStopWorker::snapshot() const noexcept {
    if (!impl_) return {};
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto result = impl_->state;
        if (impl_->arbiter) result.aim_skips = impl_->arbiter->aim_skips();
        return result;
    } catch (...) { return {}; }
}
