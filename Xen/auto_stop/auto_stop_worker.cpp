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

std::unique_lock<std::timed_mutex> AutoStopOutputArbiter::try_enter_aim(OutputArbiterSource source,
    OutputArbiterRejection* rejection) noexcept {
    std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
    const auto index = static_cast<std::size_t>(source);
    auto& counts = counters_[index < counters_.size() ? index : 0];
    OutputArbiterRejection reason = OutputArbiterRejection::NONE;
    if (faulted_.load(std::memory_order_acquire)) reason = OutputArbiterRejection::OUTPUT_FAULT;
    else if (auxiliary_pending_.load(std::memory_order_acquire)) reason = OutputArbiterRejection::AUXILIARY_PENDING;
    else if (!lock.try_lock()) reason = OutputArbiterRejection::LOCK_BUSY;
    else {
        if (faulted_.load(std::memory_order_acquire)) reason = OutputArbiterRejection::OUTPUT_FAULT;
        else if (auxiliary_pending_.load(std::memory_order_acquire)) reason = OutputArbiterRejection::AUXILIARY_PENDING;
        if (reason != OutputArbiterRejection::NONE) lock.unlock();
    }
    if (rejection) *rejection = reason;
    switch (reason) {
    case OutputArbiterRejection::NONE: counts.acquired.fetch_add(1, std::memory_order_relaxed); break;
    case OutputArbiterRejection::LOCK_BUSY: counts.lock_busy.fetch_add(1, std::memory_order_relaxed); break;
    case OutputArbiterRejection::AUXILIARY_PENDING: counts.auxiliary_pending.fetch_add(1, std::memory_order_relaxed); break;
    case OutputArbiterRejection::OUTPUT_FAULT: counts.output_fault.fetch_add(1, std::memory_order_relaxed); break;
    }
    if (source == OutputArbiterSource::AIM && reason != OutputArbiterRejection::NONE)
        aim_skips_.fetch_add(1, std::memory_order_relaxed);
    return lock;
}

OutputArbiterSnapshot AutoStopOutputArbiter::snapshot() const noexcept {
    OutputArbiterSnapshot result;
    for (std::size_t i = 0; i < counters_.size(); ++i) {
        const auto& source = counters_[i];
        result.sources[i] = {source.acquired.load(std::memory_order_relaxed), source.lock_busy.load(std::memory_order_relaxed),
            source.auxiliary_pending.load(std::memory_order_relaxed), source.output_fault.load(std::memory_order_relaxed)};
    }
    return result;
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
    std::function<std::uint64_t()> allocate_request;
    std::function<bool()> focused;
    Clock::time_point target_until{};
    std::uint64_t target_generation = 0;
    AutoStopBlockReason target_reason = AutoStopBlockReason::NO_TARGET;
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
    bool release_key_held(const InputSnapshot& input) const noexcept {
        for (const int key : config.release_virtual_keys)
            if (key > 0 && key < 256 && input.virtual_keys[key]) return true;
        return false;
    }
    bool permission(const InputSnapshot& input) noexcept {
        try {
            { std::lock_guard<std::mutex> lock(mutex); if (state.release_required) return false; }
            return allowed && allowed() && input.state_valid && input.status == InputMonitorStatus::READY &&
                !release_key_held(input) &&
                !input.virtual_keys[0x23] && config.activation_virtual_key > 0 &&
                config.activation_virtual_key < 256 && input.virtual_keys[config.activation_virtual_key] &&
                !paused.load(std::memory_order_acquire) && !stopping.load(std::memory_order_acquire);
        } catch (...) { return false; }
    }
    bool target_permission(std::uint64_t generation = UINT64_MAX) noexcept {
        try {
            const bool source_focused = focused && focused();
            std::lock_guard<std::mutex> lock(mutex);
            state.source_focused = source_focused;
            if (!source_focused) state.release_required = true;
            state.target_available = Clock::now() < target_until;
            return source_focused && !state.release_required && state.target_available &&
                (generation == UINT64_MAX || generation == target_generation);
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
        bool debt = false, estimated = false, independent = false, target_consumed = false;
        std::uint64_t active_target_generation = 0;
        Clock::time_point lease_end;
        Clock::time_point last_block_log{};
        AutoStopBlockReason logged_reason = AutoStopBlockReason::NONE;
        bool have_block_log = false;
        std::array<bool, 256> previous_release_keys{};
        const auto report_block = [&]() {
            AutoStopSnapshot current;
            { std::lock_guard<std::mutex> lock(mutex); current = state; }
            const auto now = Clock::now();
            if ((!have_block_log || current.block_reason != logged_reason) &&
                (!have_block_log || now - last_block_log >= std::chrono::milliseconds(500))) {
                LOG_INFO("auto_stop", "状态={}，阻断={}，请求={}", AutoStopStatusName(current.status),
                    AutoStopBlockReasonName(current.block_reason), current.request_id);
                logged_reason = current.block_reason; last_block_log = now; have_block_log = true;
            }
        };
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
            LOG_INFO("auto_stop", "取消请求{}，原因={}", active_id, reason);
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
            independent = false;
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
                // 救援只信任本设备的新按键边沿；失联缓存不产生救援动作。
                bool rescue_pressed = false;
                if (input_ok) for (const int key : config.release_virtual_keys) {
                    if (key <= 0 || key >= 256) continue;
                    rescue_pressed |= input.virtual_keys[key] && !previous_release_keys[key];
                    previous_release_keys[key] = input.virtual_keys[key];
                }
                if (rescue_pressed) {
                    cancel_generation.fetch_add(1, std::memory_order_acq_rel);
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        pending_id = 0;
                        state.release_required = true;
                        state.block_reason = AutoStopBlockReason::RELEASE_REQUIRED;
                        ++state.rescue_attempts;
                    }
                    // 即使普通许可/焦点/暂停/FAULT已阻断，也只走键盘债务清理，不发新DOWN。
                    debt = true;
                    cancel_active(false, "release_hotkey");
                    bool acknowledged = false, fault_remains = false;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        acknowledged = !state.cleanup_unknown;
                        if (acknowledged) ++state.rescue_succeeded;
                        else ++state.rescue_failed;
                        fault_remains = fault;
                    }
                    LOG_INFO("auto_stop", "释放热键救援：急停键盘债务={}，共享故障锁存={}；需松开允许键后重新触发",
                        acknowledged ? "归还已确认" : "清理未确认，释放救援键再按可重试",
                        fault_remains ? "保留，需重启" : "未由本模块锁存");
                    report_block();
                    std::unique_lock<std::mutex> lock(mutex);
                    wake.wait_for(lock, std::chrono::milliseconds(1));
                    continue;
                }
                bool latched_fault;
                { std::lock_guard<std::mutex> lock(mutex); latched_fault = fault; }
                const bool target_ready = allocate_request && target_permission();
                bool release_required = false;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if ((!allocate_request || state.source_focused) && input_ok && !release_key_held(input) &&
                        config.activation_virtual_key > 0 && config.activation_virtual_key < 256 &&
                        !input.virtual_keys[config.activation_virtual_key])
                        state.release_required = false;
                    release_required = state.release_required;
                }
                const bool target_eligible = target_ready && !release_required && input_ok && permission(input);
                std::uint64_t current_target_generation;
                { std::lock_guard<std::mutex> lock(mutex); current_target_generation = target_generation; }
                if (!target_eligible) target_consumed = false;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    state.block_reason = latched_fault ? AutoStopBlockReason::OUTPUT_FAULT :
                        allocate_request && !state.source_focused ? AutoStopBlockReason::SOURCE_FOCUS :
                        release_required ? AutoStopBlockReason::RELEASE_REQUIRED :
                        !input_ok ? AutoStopBlockReason::INPUT_UNAVAILABLE :
                        !events_ok || !intent.history_valid ? AutoStopBlockReason::INPUT_HISTORY :
                        paused.load() ? AutoStopBlockReason::PAUSED :
                        config.activation_virtual_key <= 0 || config.activation_virtual_key >= 256 ||
                            !input.virtual_keys[config.activation_virtual_key] ? AutoStopBlockReason::ACTIVATION_NOT_HELD :
                        !allowed || !allowed() || input.virtual_keys[0x23] ? AutoStopBlockReason::SAFETY_PERMISSION :
                        allocate_request && !state.target_available ?
                            (target_until != Clock::time_point{} ? AutoStopBlockReason::TARGET_STALE : target_reason) :
                        !intent.held_mask || intent.conflicting ? AutoStopBlockReason::MOTION_UNAVAILABLE :
                        !active_id && target_consumed ? AutoStopBlockReason::CONTINUOUS_REQUEST_CONSUMED : AutoStopBlockReason::NONE;
                }
                if (active_id && independent && (!target_eligible || active_target_generation != current_target_generation))
                    cancel_active(false, "target_or_focus_revoked");
                if (active_id && (!input_ok || !events_ok || !intent.history_valid || !permission(input) ||
                    held_wasd(input) != original_mask || active_generation != cancel_generation.load() || Clock::now() >= lease_end)) {
                    const char* reason = !input_ok ? "input_invalid" : !events_ok ? "input_gap" :
                        !intent.history_valid ? "history_invalid" : paused.load() ? "paused" :
                        !permission(input) ? "permission_revoked" : held_wasd(input) != original_mask ? "direction_changed" :
                        active_generation != cancel_generation.load() ? "caller_canceled" : "lease_expired";
                    cancel_active(false, reason);
                }
                if (latched_fault) {
                    report_block();
                    std::unique_lock<std::mutex> lock(mutex);
                    wake.wait_for(lock, std::chrono::milliseconds(1));
                    continue;
                }
                if (!active_id) {
                    std::uint64_t requested = 0;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        // 连续目标与按住会话只消费一次，观察刷新不生成新租期。
                        if (!pending_id && !target_consumed && target_eligible && events_ok &&
                            intent.history_valid && intent.held_mask != 0 && !intent.conflicting &&
                            state.status == AutoStopStatus::READY) {
                            const auto id = allocate_request();
                            if (id > last_request_id) {
                                pending_id = last_request_id = id;
                                submitted_at = Clock::now();
                                submitted_generation = cancel_generation.load();
                                state.request_id = id;
                                ++state.requests;
                                state.status = AutoStopStatus::BRAKING;
                                independent = true;
                                active_target_generation = target_generation;
                                target_consumed = true;
                            }
                        }
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
                            (independent && !target_permission(active_target_generation)) ||
                            active_generation != cancel_generation.load() || Clock::now() >= lease_end) {
                            cancel_active(false, "request_not_eligible");
                        } else {
                            const auto decision = controller.request(active_id, now_ns());
                            if (decision.phase != AutoStopPhase::WAITING_ACK || decision.request_id != active_id || !reserve()) {
                                cancel_active(false, "controller_or_arbiter_unavailable");
                            } else {
                                publish(AutoStopStatus::BRAKING);
                                LOG_INFO("auto_stop", "开始请求{}，来源={}，物理方向mask={}", active_id,
                                    independent ? "目标识别" : "显式调用", original_mask);
                                for (std::uint8_t key = 1; key <= 8; key <<= 1) if (original_mask & key) {
                                    if (!mouse->poll_input(input) || !permission(input) ||
                                        (independent && !target_permission(active_target_generation)) || held_wasd(input) != original_mask ||
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
                        if (!mouse->poll_input(input) || !permission(input) ||
                            (independent && !target_permission(active_target_generation)) || held_wasd(input) != original_mask ||
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
                                    LOG_INFO("auto_stop", "请求{}反向软件键释放已确认，进入估算完成，未授予开火", active_id);
                                    { std::lock_guard<std::mutex> lock(mutex); ++state.completed; state.status = AutoStopStatus::ESTIMATED; }
                                    // 软件键已经释放，物理屏蔽的有界租期内允许Aim继续发送。
                                    release_reservation();
                                }
                            }
                        }
                    } else if (estimated) publish(AutoStopStatus::ESTIMATED);
                }
                report_block();
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
        std::shared_ptr<AutoStopOutputArbiter> arbiter, std::function<bool()> permission,
        std::function<std::uint64_t()> allocate_request, std::function<bool()> focused)
    : impl_(std::make_unique<Impl>(std::move(mouse), std::move(arbiter), std::move(permission))) {
    impl_->allocate_request = std::move(allocate_request);
    impl_->focused = std::move(focused);
}
void AutoStopWorker::publish_target(std::chrono::steady_clock::time_point valid_until, AutoStopBlockReason reason) noexcept {
    if (!impl_) return;
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (valid_until == Clock::time_point{}) ++impl_->target_generation;
        impl_->target_until = valid_until;
        impl_->target_reason = valid_until == Clock::time_point{} ? reason : AutoStopBlockReason::NONE;
        impl_->wake.notify_all();
    } catch (...) {}
}
AutoStopWorker::~AutoStopWorker() { stop(); }
bool AutoStopWorker::start(const AutoStopConfig& config, int command_timeout_ms) noexcept {
    if (!impl_) return false;
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->running || impl_->thread.joinable() || impl_->fault) return false;
        impl_->config = config;
        impl_->state = {};
        impl_->state.independent_trigger_enabled = static_cast<bool>(impl_->allocate_request);
        impl_->state.focus_required = impl_->state.independent_trigger_enabled;
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
