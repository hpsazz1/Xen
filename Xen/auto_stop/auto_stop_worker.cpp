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

std::unique_lock<std::timed_mutex> AutoStopOutputArbiter::enter_aim_until(Clock::time_point deadline,
    OutputArbiterRejection* rejection) noexcept {
    std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
    auto& counts = counters_[static_cast<std::size_t>(OutputArbiterSource::AIM)];
    OutputArbiterRejection reason = OutputArbiterRejection::LOCK_BUSY;
    for (;;) {
        if (faulted_.load(std::memory_order_acquire)) { reason = OutputArbiterRejection::OUTPUT_FAULT; break; }
        if (Clock::now() >= deadline) break;
        // 已申请的键盘短事务先完成；不能跨配置持有时长设置pending。
        if (!auxiliary_pending_.load(std::memory_order_acquire)) {
            if (!lock.try_lock_until(deadline)) break;
            if (faulted_.load(std::memory_order_acquire)) {
                lock.unlock(); reason = OutputArbiterRejection::OUTPUT_FAULT; break;
            }
            if (Clock::now() >= deadline) { lock.unlock(); break; }
            if (!auxiliary_pending_.load(std::memory_order_acquire)) {
                reason = OutputArbiterRejection::NONE; break;
            }
            lock.unlock();
        }
        // 此时没有持有后端锁；醒来先释放状态锁，再尝试后端事务。
        std::unique_lock pending_lock(pending_mutex_);
        if (!pending_changed_.wait_until(pending_lock, deadline, [&] {
                return !auxiliary_pending_.load(std::memory_order_acquire) ||
                    faulted_.load(std::memory_order_acquire);
            })) break;
    }
    if (reason != OutputArbiterRejection::NONE && faulted_.load(std::memory_order_acquire))
        reason = OutputArbiterRejection::OUTPUT_FAULT;
    if (rejection) *rejection = reason;
    if (reason == OutputArbiterRejection::NONE) counts.acquired.fetch_add(1, std::memory_order_relaxed);
    else {
        if (reason == OutputArbiterRejection::OUTPUT_FAULT) counts.output_fault.fetch_add(1, std::memory_order_relaxed);
        else counts.lock_busy.fetch_add(1, std::memory_order_relaxed);
        aim_skips_.fetch_add(1, std::memory_order_relaxed);
    }
    return lock;
}

void AutoStopOutputArbiter::set_auxiliary_pending(bool pending) noexcept {
    {
        std::lock_guard lock(pending_mutex_);
        auxiliary_pending_.store(pending, std::memory_order_release);
    }
    pending_changed_.notify_all();
}

void AutoStopOutputArbiter::latch_output_fault() noexcept {
    {
        std::lock_guard lock(pending_mutex_);
        faulted_.store(true, std::memory_order_release);
    }
    pending_changed_.notify_all();
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
    std::function<AutoStopWeaponContext()> weapon_context;
    bool weapon_context_seen = false;
    std::uint64_t normal_weapon_generation = 0;
    Clock::time_point target_until{}, tracking_target_until{};
    std::atomic<std::uint64_t> manual_fire_id{0};
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
    std::uint64_t estimated_id = 0, estimated_generation = 0;
    std::uint64_t resume_id = 0, resume_generation = 0;
    Clock::time_point resume_not_before{}, movement_not_before{};
    bool trigger_idle = false;
    WasdEventCursor trigger_idle_cursor;
    std::uint64_t trigger_idle_generation = 0;

    void publish(AutoStopStatus status) {
        std::lock_guard<std::mutex> lock(mutex);
        state.status = status;
    }
    bool release_key_held(const InputSnapshot& input) const noexcept {
        for (const int key : config.release_virtual_keys)
            if (key > 0 && key < 256 && input.virtual_keys[key]) return true;
        return false;
    }
    bool weapon_permission() noexcept {
        AutoStopWeaponContext incoming;
        try { if (weapon_context) incoming = weapon_context(); }
        catch (...) { incoming.required = true; }
        if (incoming.required && incoming.canonical_id.empty()) incoming.valid = false;
        std::lock_guard<std::mutex> lock(mutex);
        const auto previous = state.weapon_context;
        const bool changed = weapon_context_seen && (previous.required || incoming.required) &&
            (previous.required != incoming.required || previous.valid != incoming.valid ||
                previous.generation != incoming.generation || previous.canonical_id != incoming.canonical_id ||
                previous.trust_generation != incoming.trust_generation || previous.session_trusted != incoming.session_trusted);
        const bool trusted = previous.required && incoming.required && previous.session_trusted &&
            incoming.session_trusted && incoming.trust_generation != 0 &&
            previous.trust_generation == incoming.trust_generation;
        if (changed) {
            const auto generation = cancel_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
            normal_weapon_generation = trusted ? generation : 0;
            estimated_id = 0; pending_id = 0;
            trigger_idle = false; ++trigger_idle_generation;
            target_until = {}; tracking_target_until = {};
        }
        if ((changed && !trusted) || (incoming.required && !incoming.valid && !incoming.session_trusted))
            state.release_required = true;
        state.weapon_context = incoming;
        weapon_context_seen = true;
        return !incoming.required || incoming.valid;
    }
    bool permission(const InputSnapshot& input) noexcept {
        try {
            if (!weapon_permission()) return false;
            { std::lock_guard<std::mutex> lock(mutex); if (state.release_required) return false; }
            return allowed && allowed() && !arbiter->faulted_.load(std::memory_order_acquire) &&
                input.state_valid && input.status == InputMonitorStatus::READY &&
                !release_key_held(input) &&
                !input.virtual_keys[0x23] && config.activation_virtual_key > 0 &&
                config.activation_virtual_key < 256 &&
                (input.virtual_keys[config.activation_virtual_key] || manual_fire_id.load() != 0) &&
                !paused.load(std::memory_order_acquire) && !stopping.load(std::memory_order_acquire);
        } catch (...) { return false; }
    }
    bool manual_permission(const InputSnapshot& input) noexcept {
        try {
            // release_required属于快捷键循环的重新武装条件，不能使人工松键依赖快捷键松开。
            return config.use_counterpulse_timing && allocate_request && focused && focused() &&
                weapon_permission() && allowed && allowed() && !arbiter->faulted_.load() &&
                input.state_valid && input.status == InputMonitorStatus::READY &&
                !release_key_held(input) && !input.virtual_keys[0x23] && !paused.load() && !stopping.load();
        } catch (...) { return false; }
    }
    bool session_permission(bool require_target) noexcept {
        try {
            const bool source_focused = focused && focused();
            std::lock_guard<std::mutex> lock(mutex);
            state.source_focused = source_focused;
            if (!source_focused) state.release_required = true;
            state.target_available = Clock::now() < target_until;
            return source_focused && !state.release_required &&
                (manual_fire_id.load() != 0 ? Clock::now() < tracking_target_until :
                    (!require_target || state.target_available));
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
        AutoStopController controller(config);
        InputSnapshot input;
        std::uint64_t active_id = 0, active_generation = 0, active_input_epoch = 0;
        std::uint8_t software_mask = 0, original_mask = 0;
        bool debt = false, estimated = false, independent = false, target_consumed = false;
        bool independent_acquired = false;
        bool manual_release = false;
        std::uint8_t release_armed_mask = 0;
        bool mask_only = false, masked_hold = false;
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
            arbiter->set_auxiliary_pending(false);
        };
        const auto mark_estimated = [&]() {
            if (estimated) return;
            estimated = true;
            { std::lock_guard<std::mutex> lock(mutex);
              estimated_id = independent ? active_id : 0;
              estimated_generation = active_generation;
              ++state.completed; state.status = AutoStopStatus::ESTIMATED; }
            LOG_INFO("auto_stop", "请求{}反向释放及配置等待已完成，仅为估计资格", active_id);
            release_reservation();
        };
        auto reserve = [&]() {
            if (output.owns_lock()) return true;
            arbiter->set_auxiliary_pending(true);
            const auto started = now_ns();
            const bool acquired = output.try_lock_until(Clock::now() + std::chrono::milliseconds(command_timeout_ms + 5));
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++state.arbiter_wait_samples;
                state.max_arbiter_wait_ns = std::max(state.max_arbiter_wait_ns, now_ns() - started);
            }
            if (!acquired) arbiter->set_auxiliary_pending(false);
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
            if (!success) arbiter->latch_output_fault();
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
        auto cancel_active = [&](bool force_fault, const char* reason, bool normal_activation_release = false,
                                 bool cycle_resume = false, bool manual_finished = false, bool weapon_transition = false) {
            { std::lock_guard<std::mutex> lock(mutex); estimated_id = 0; manual_fire_id = 0; }
            AutoStopBlockReason block_reason;
            { std::lock_guard<std::mutex> lock(mutex); block_reason = state.block_reason; }
            LOG_INFO("auto_stop", "取消请求{}，原因={}，阻断={}", active_id, reason,
                AutoStopBlockReasonName(block_reason));
            // 正常松键或点射完成后的循环归还才承接受控输出模型；原始物理历史不补零事件。
            const bool can_resume = (normal_activation_release || cycle_resume) && independent && estimated && software_mask == 0 &&
                !force_fault && intent.history_valid && !intent.conflicting && intent.held_mask == held_wasd(input);
            const auto before_cleanup_cursor = cursor;
            if (active_id && !can_resume) controller.cancel(active_id, now_ns());
            const bool clean_ok = clean();
            const auto released_at = now_ns();
            bool resumed = false;
            if ((can_resume || weapon_transition) && clean_ok) {
                InputSnapshot after_cleanup;
                WasdEventBatch after_events;
                auto after_cursor = cursor;
                bool continuous = mouse->poll_input(after_cleanup) && after_cleanup.state_valid &&
                    after_cleanup.status == InputMonitorStatus::READY &&
                    mouse->read_wasd_events(after_cursor, after_events) && after_events.subscribed && !after_events.gap &&
                    after_cursor.epoch == before_cleanup_cursor.epoch && held_wasd(after_cleanup) == intent.held_mask;
                // 监听报告可重复同一键态；逐条验证连续性，不把新报告误当松键或改向。
                // 临时检查不推进正式历史，下一轮仍按原事件时间消费这些报告。
                auto checked_history = history;
                auto checked_sequence = before_cleanup_cursor.sequence;
                for (std::size_t i = 0; continuous && i < after_events.count; ++i) {
                    const auto& event = after_events.events[i];
                    continuous = event.state_valid && event.epoch == before_cleanup_cursor.epoch &&
                        event.sequence > checked_sequence && event.sequence - checked_sequence == 1 &&
                        event.held_mask == intent.held_mask;
                    if (!continuous) break;
                    const auto checked = checked_history.observe(event.held_mask, event.epoch, event.sequence,
                        event.received_at_steady_ns, event.state_valid);
                    continuous = checked.input_continuous && checked.history_valid && !checked.conflicting;
                    checked_sequence = event.sequence;
                }
                continuous = continuous && after_cursor.sequence == checked_sequence;
                if (weapon_transition) {
                    if (continuous && allowed && allowed() && focused && focused() &&
                        !after_cleanup.virtual_keys[0x23] && !paused.load() && !stopping.load() && !arbiter->faulted_.load())
                        resumed = controller.restart_after_cleanup(intent, released_at);
                    if (!resumed) {
                        std::lock_guard<std::mutex> lock(mutex);
                        state.release_required = true;
                    }
                } else if (continuous && (cycle_resume ? permission(after_cleanup) : !after_cleanup.virtual_keys[config.activation_virtual_key]) &&
                    !after_cleanup.virtual_keys[0x23] && !release_key_held(after_cleanup) && allowed && allowed() &&
                    !paused.load() && !stopping.load() && !arbiter->faulted_.load() &&
                    active_generation == cancel_generation.load() && session_permission(false))
                    resumed = controller.resume_after_masked_hold(intent, released_at);
                if (!resumed && active_id) controller.cancel(active_id, now_ns());
            } else if (can_resume && active_id) controller.cancel(active_id, now_ns());
            const bool retain_release = clean_ok && !force_fault && intent.history_valid && !intent.conflicting &&
                intent.held_mask == 0 && input.state_valid && input.status == InputMonitorStatus::READY && held_wasd(input) == 0;
            if (retain_release && !resumed) controller.observe(intent, now_ns());
            if (resumed) LOG_INFO("auto_stop", "键盘归还已确认；保留连续输入以支持再次急停");
            // 正常归还不破坏真实事件连续性；无法承接模型时，下次仅屏蔽，不伪造运动历史。
            // 清理期间的事件留给正式游标，下一轮先验证再准入。
            const bool retain_input = (normal_activation_release || manual_release || weapon_transition) && clean_ok && !force_fault && intent.input_continuous;
            if (manual_release && !weapon_transition) {
                // 松键动作结束后仅用真实当前输入建立下一计划，不承接软件反向期间的运动估算。
                controller = AutoStopController(config);
                if (intent.history_valid) controller.observe(intent, now_ns());
            }
            std::lock_guard<std::mutex> lock(mutex);
            if (active_id && !(cycle_resume && resumed) && !manual_finished) ++state.canceled;
            if (!manual_release && !weapon_transition && ((cycle_resume && !resumed) || (config.cycle_enabled && !normal_activation_release && !cycle_resume)))
                state.release_required = true;
            if (cycle_resume && resumed) ++state.cycle_count;
            state.cycle_moving = cycle_resume && resumed;
            if (fault || force_fault || !clean_ok) { fault = true; state.status = AutoStopStatus::FAULT; }
            else state.status = paused.load() ? AutoStopStatus::PAUSED : AutoStopStatus::CANCELED;
            active_id = 0;
            manual_release = false;
            release_armed_mask = 0;
            independent = false;
            independent_acquired = false;
            estimated = false;
            mask_only = masked_hold = false;
            if (!retain_release && !resumed && !retain_input) { history.reset(); intent = {}; }
            return resumed;
        };
        try {
            while (!stopping.load(std::memory_order_acquire)) {
                bool input_ok = mouse->poll_input(input) && input.state_valid && input.status == InputMonitorStatus::READY;
                std::uint8_t released_axes = 0;
                std::int64_t release_event_ns = 0;
                bool manual_input_changed = false;
                const bool observe_manual_edges = !active_id && !debt && input_ok && manual_permission(input);
                if (!observe_manual_edges) release_armed_mask = 0;
                WasdEventBatch batch;
                bool events_ok = mouse->read_wasd_events(cursor, batch) && batch.subscribed && !batch.gap;
                if (!events_ok) { history.reset(); intent = {}; release_armed_mask = 0; }
                if (events_ok) for (std::size_t i = 0; i < batch.count; ++i) {
                    const auto& event = batch.events[i];
                    if (!event.state_valid) { events_ok = false; history.reset(); intent = {}; break; }
                    const auto previous_intent = intent;
                    intent = history.observe(event.held_mask, event.epoch, event.sequence, event.received_at_steady_ns, event.state_valid);
                    if (manual_release && event.held_mask != original_mask) manual_input_changed = true;
                    // 只接纳无人接管期间真实的新按下和释放。重复报告不重新武装，清理后不补发旧释放。
                    if (observe_manual_edges && intent.history_valid && previous_intent.history_valid &&
                        intent.input_continuous && intent.epoch == previous_intent.epoch) {
                        const auto released = WasdReleasedAxes(previous_intent, intent) & release_armed_mask;
                        // 只保留最后一次全松边沿，不累计组合键中较早释放的方向。
                        if (released) released_axes = static_cast<std::uint8_t>(released);
                        if (released) release_event_ns = intent.received_at_ns;
                        release_armed_mask = static_cast<std::uint8_t>((release_armed_mask |
                            (intent.held_mask & ~previous_intent.held_mask)) & intent.held_mask);
                    } else { release_armed_mask = 0; released_axes = 0; }
                    // 活动会话逐事件锁定缺口/代际/时间故障，同批后续全松不能洗掉撤销。
                    if (active_id && (independent || manual_release) && (!intent.input_continuous || event.epoch != active_input_epoch)) {
                        events_ok = false;
                        history.reset(); intent = {};
                        break;
                    }
                    // 全部屏蔽确认后，物理改向不再是施加给游戏的输入；制动沿原ACK模型推进。
                    if (!estimated && !independent_acquired && !manual_release) controller.observe(intent, event.received_at_steady_ns);
                }
                // 事件读取晚于快照：真实UP可能刚进入事件流，旧DOWN快照不能吃掉唯一全松边沿。
                // 仅在该错配发生时刷新；后续许可与发送前事件复核仍拒绝新按下/缺口。
                if (released_axes && events_ok && intent.held_mask == 0 && held_wasd(input) != 0)
                    input_ok = mouse->poll_input(input) && input.state_valid && input.status == InputMonitorStatus::READY;
                // 救援只信任本设备的新按键边沿；失联缓存不产生救援动作。
                bool rescue_pressed = false, only_weapon_keys = true;
                if (input_ok) for (const int key : config.release_virtual_keys) {
                    if (key <= 0 || key >= 256) continue;
                    const bool pressed = input.virtual_keys[key] && !previous_release_keys[key];
                    rescue_pressed |= pressed;
                    if (pressed && !((key >= 0x31 && key <= 0x35) || key == 0x51)) only_weapon_keys = false;
                    previous_release_keys[key] = input.virtual_keys[key];
                }
                if (rescue_pressed) {
                    weapon_permission();
                    bool normal_switch = false;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        normal_switch = only_weapon_keys && state.weapon_context.required &&
                            state.weapon_context.session_trusted && state.weapon_context.trust_generation != 0 &&
                            !state.release_required && !fault && manual_fire_id.load() == 0;
                    }
                    normal_switch = normal_switch && input_ok && events_ok && intent.input_continuous &&
                        allowed && allowed() && focused && focused() && !input.virtual_keys[0x23] &&
                        !paused.load() && !arbiter->faulted_.load();
                    cancel_generation.fetch_add(1, std::memory_order_acq_rel);
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        pending_id = 0;
                        if (!normal_switch) state.release_required = true;
                        target_until = {}; tracking_target_until = {};
                        state.block_reason = AutoStopBlockReason::RELEASE_REQUIRED;
                        ++state.rescue_attempts;
                    }
                    // 即使普通许可/焦点/暂停/FAULT已阻断，也只走键盘债务清理，不发新DOWN。
                    debt = true;
                    cancel_active(false, normal_switch ? "weapon_switch_hotkey" : "release_hotkey",
                        false, false, false, normal_switch);
                    target_consumed = false;
                    bool acknowledged = false, fault_remains = false;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        acknowledged = !state.cleanup_unknown;
                        if (acknowledged) ++state.rescue_succeeded;
                        else ++state.rescue_failed;
                        fault_remains = fault;
                    }
                    LOG_INFO("auto_stop", "按键归还：急停键盘债务={}，共享故障锁存={}，恢复方式={}",
                        acknowledged ? "归还已确认" : "清理未确认，释放救援键再按可重试",
                        fault_remains ? "保留，需重启" : "未由本模块锁存",
                        normal_switch && acknowledged && !fault_remains ? "切枪键释放后重新核验" : "松开允许键后重新触发");
                    report_block();
                    std::unique_lock<std::mutex> lock(mutex);
                    wake.wait_for(lock, std::chrono::milliseconds(1));
                    continue;
                }
                bool latched_fault;
                { std::lock_guard<std::mutex> lock(mutex); latched_fault = fault; }
                if (!latched_fault && arbiter->faulted_.load(std::memory_order_acquire)) {
                    if (active_id || debt) cancel_active(true, "shared_output_fault");
                    std::lock_guard<std::mutex> lock(mutex);
                    fault = latched_fault = true;
                    state.status = AutoStopStatus::FAULT;
                }
                const bool weapon_ready = weapon_permission();
                const bool session_ready = allocate_request && session_permission(!independent);
                bool release_required = false;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (weapon_ready && (!allocate_request || state.source_focused) && input_ok && !release_key_held(input) &&
                        (config.activation_virtual_key <= 0 || config.activation_virtual_key >= 256 ||
                        !input.virtual_keys[config.activation_virtual_key]))
                        state.release_required = false;
                    release_required = state.release_required;
                }
                const bool request_eligible = session_ready && !release_required && input_ok && permission(input) &&
                    Clock::now() >= movement_not_before;
                if (!request_eligible) target_consumed = false;
                // 松键制动与快捷键制动共享active_id和软件键盘债务，不能同时拥有输出。
                bool queued_request = false;
                { std::lock_guard<std::mutex> lock(mutex); queued_request = pending_id != 0; }
                const bool hotkey_priority = queued_request || (request_eligible && !target_consumed && intent.held_mask != 0);
                if (!active_id && !debt && released_axes && !hotkey_priority && !latched_fault &&
                    input_ok && events_ok && intent.history_valid && intent.held_mask == 0 && held_wasd(input) == 0 &&
                    release_event_ns > 0 && now_ns() >= release_event_ns &&
                    now_ns() - release_event_ns < 500'000'000 && manual_permission(input)) {
                    const auto id = allocate_request();
                    std::lock_guard<std::mutex> lock(mutex);
                    if (id > last_request_id && !pending_id) {
                        // 全松是本次真实事件；独立建立固定时序计划，无需沿用上一次运动模型。
                        controller = AutoStopController(config);
                        controller.observe(intent, now_ns());
                        const auto decision = controller.request_manual_release(id, released_axes, now_ns());
                        if (decision.phase == AutoStopPhase::WAITING_ACK && decision.request_id == id) {
                            active_id = last_request_id = id;
                            active_generation = cancel_generation.load(); active_input_epoch = intent.epoch;
                            original_mask = intent.held_mask;
                            lease_end = Clock::now() + std::chrono::milliseconds(500);
                            manual_release = true; release_armed_mask = 0;
                            state.request_id = id; ++state.requests; state.status = AutoStopStatus::BRAKING;
                            state.block_reason = AutoStopBlockReason::NONE;
                            state.counter_release_ack_ns = state.completion_ready_ns = 0;
                            state.cycle_moving = false;
                            LOG_INFO("auto_stop", "开始松键急停{}，释放方向mask={}，保留物理方向mask={}", id, released_axes, original_mask);
                        }
                    }
                }
                if (manual_release) {
                    const auto valid_manual = [&]() {
                        return manual_permission(input) && input_ok && events_ok && !manual_input_changed && intent.input_continuous && intent.epoch == active_input_epoch &&
                            held_wasd(input) == original_mask && intent.held_mask == original_mask &&
                            active_generation == cancel_generation.load() && Clock::now() < lease_end;
                    };
                    const auto reports_still_released = [&]() {
                        // 等待输出事务后复核整个积压窗口；最终快照为零不能掩盖按下再松开。
                        WasdEventBatch pending;
                        if (!mouse->read_wasd_events(cursor, pending) || !pending.subscribed || pending.gap ||
                            cursor.epoch != active_input_epoch || pending.count >= pending.events.size()) {
                            events_ok = false; history.reset(); intent = {}; return false;
                        }
                        bool still_released = true;
                        for (std::size_t i = 0; i < pending.count; ++i) {
                            const auto& event = pending.events[i];
                            intent = history.observe(event.held_mask, event.epoch, event.sequence,
                                event.received_at_steady_ns, event.state_valid);
                            if (event.held_mask != 0) { still_released = false; manual_input_changed = true; }
                            if (!event.state_valid || event.epoch != active_input_epoch || !intent.input_continuous || !intent.history_valid)
                                events_ok = false;
                        }
                        // 正式消费动作拥有期间的事件，撤销后不能把该窗口重新解释为新一轮松键。
                        return still_released && events_ok;
                    };
                    if (hotkey_priority || !valid_manual()) {
                        const auto new_press = intent.held_mask;
                        const bool may_rearm = manual_permission(input) && input_ok && events_ok && intent.history_valid &&
                            intent.epoch == active_input_epoch && new_press != 0 && new_press == held_wasd(input) &&
                            active_generation == cancel_generation.load();
                        cancel_active(false, "manual_release_revoked");
                        // 人工新按下已取消无屏蔽的反向输出；正常清理后允许其下一次真实全松。
                        if (may_rearm && !debt && !fault) release_armed_mask = new_press;
                    }
                    else {
                        auto decision = controller.tick(now_ns());
                        if (decision.phase == AutoStopPhase::WAITING_ACK) {
                            if (!reserve() || !(input_ok = mouse->poll_input(input)) || !reports_still_released() || !valid_manual())
                                cancel_active(false, "manual_permission_before_report");
                            else {
                                const auto started = now_ns();
                                if (software_mask && decision.desired_mask == 0) {
                                    std::lock_guard<std::mutex> lock(mutex);
                                    ++state.release_commands;
                                }
                                const auto result = mouse->set_wasd_keyboard(decision.desired_mask);
                                debt |= result.datagram_sent; receipt(result, started);
                                const auto returned = now_ns();
                                const auto ack = std::chrono::duration_cast<std::chrono::nanoseconds>(result.protocol_ack_received_at.time_since_epoch()).count();
                                const auto completed = std::chrono::duration_cast<std::chrono::nanoseconds>(result.backend_completed_at.time_since_epoch()).count();
                                if (result.disposition != KeyboardDisposition::ACKNOWLEDGED || ack < started || ack > completed || completed > returned)
                                    cancel_active(true, "manual_keyboard_not_acknowledged");
                                else {
                                    software_mask = decision.desired_mask;
                                    decision = controller.acknowledge(active_id, decision.command_id, software_mask, ack);
                                    if (decision.completion_ready_ns) {
                                        std::lock_guard<std::mutex> lock(mutex);
                                        state.counter_release_ack_ns = ack;
                                        state.completion_ready_ns = decision.completion_ready_ns;
                                    }
                                }
                            }
                        }
                        if (manual_release && decision.phase == AutoStopPhase::COMPLETE_ESTIMATED) {
                            { std::lock_guard<std::mutex> lock(mutex); ++state.completed; }
                            cancel_active(false, "manual_release_completed", false, false, true);
                        } else if (manual_release && (decision.phase == AutoStopPhase::INVALID || decision.phase == AutoStopPhase::CANCELLED))
                            cancel_active(false, "manual_controller_canceled");
                    }
                    release_reservation();
                    std::unique_lock<std::mutex> lock(mutex);
                    wake.wait_for(lock, std::chrono::milliseconds(1));
                    continue;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (latched_fault || !weapon_ready || release_required || !input_ok || !events_ok ||
                        paused.load() || config.activation_virtual_key <= 0 || config.activation_virtual_key >= 256 ||
                        !input.virtual_keys[config.activation_virtual_key] ||
                        !state.source_focused || !allowed || !allowed()) state.cycle_moving = false;
                    state.block_reason = latched_fault ? AutoStopBlockReason::OUTPUT_FAULT :
                        allocate_request && !state.source_focused ? AutoStopBlockReason::SOURCE_FOCUS :
                        !weapon_ready ? AutoStopBlockReason::WEAPON_CONTEXT :
                        release_required ? AutoStopBlockReason::RELEASE_REQUIRED :
                        !input_ok ? AutoStopBlockReason::INPUT_UNAVAILABLE :
                        !events_ok || (allocate_request ? !intent.input_continuous : !intent.history_valid) ? AutoStopBlockReason::INPUT_HISTORY :
                        paused.load() ? AutoStopBlockReason::PAUSED :
                        config.activation_virtual_key <= 0 || config.activation_virtual_key >= 256 ||
                            !input.virtual_keys[config.activation_virtual_key] ? AutoStopBlockReason::ACTIVATION_NOT_HELD :
                        !allowed || !allowed() || input.virtual_keys[0x23] ? AutoStopBlockReason::SAFETY_PERMISSION :
                        active_id && independent_acquired ? AutoStopBlockReason::NONE :
                        allocate_request && !state.target_available ?
                            (target_until != Clock::time_point{} ? AutoStopBlockReason::TARGET_STALE : target_reason) :
                        !intent.held_mask || (!allocate_request && intent.conflicting) ? AutoStopBlockReason::MOTION_UNAVAILABLE :
                        !active_id && target_consumed ? AutoStopBlockReason::CONTINUOUS_REQUEST_CONSUMED : AutoStopBlockReason::NONE;
                }
                bool normal_weapon_change = false;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    normal_weapon_change = active_id && manual_fire_id.load() == 0 && active_generation != cancel_generation.load() &&
                        normal_weapon_generation == cancel_generation.load() && !state.release_required &&
                        state.weapon_context.session_trusted;
                }
                if (normal_weapon_change && input_ok && events_ok && intent.input_continuous &&
                    intent.epoch == active_input_epoch && allowed && allowed() && focused && focused() &&
                    !input.virtual_keys[0x23] && !release_key_held(input) && !paused.load() && !latched_fault) {
                    cancel_active(false, "normal_weapon_transition", false, false, false, true);
                    target_consumed = false;
                    continue;
                }
                if (active_id && independent && !session_ready)
                    cancel_active(false, "source_focus_revoked");
                std::uint64_t requested_resume = 0, requested_resume_generation = 0;
                Clock::time_point requested_not_before;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    requested_resume = resume_id; requested_resume_generation = resume_generation;
                    requested_not_before = resume_not_before; resume_id = 0;
                }
                if (requested_resume && requested_resume == active_id && manual_fire_id.load() == 0) {
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        // 与人工接管线性化：接管先成功则丢弃归还，归还先认领则拒绝后来的接管。
                        if (manual_fire_id.load() != 0) continue;
                        estimated_id = 0;
                    }
                    const bool may_resume = manual_fire_id.load() == 0 && config.cycle_enabled && independent && estimated && software_mask == 0 &&
                        requested_resume_generation == active_generation && active_generation == cancel_generation.load() &&
                        input_ok && events_ok && intent.input_continuous && intent.epoch == active_input_epoch &&
                        permission(input) && session_permission(false);
                    const bool resumed = cancel_active(false, "cycle_resume", false, may_resume);
                    if (may_resume && resumed) {
                        target_consumed = false;
                        movement_not_before = std::max(requested_not_before, Clock::now() + std::chrono::milliseconds(1));
                    } else {
                        std::lock_guard<std::mutex> lock(mutex);
                        state.release_required = true;
                    }
                    // 本轮准入条件在归还前取得，下一轮重新读取输入和期限。
                    continue;
                }
                if (active_id && (!input_ok || !events_ok ||
                    (independent && (!intent.input_continuous || intent.epoch != active_input_epoch)) ||
                    (!independent_acquired && !mask_only && !intent.history_valid) || !permission(input) ||
                    (!independent_acquired && !mask_only && held_wasd(input) != original_mask) || active_generation != cancel_generation.load() ||
                    ((!independent || (!estimated && !masked_hold) || software_mask != 0) && Clock::now() >= lease_end))) {
                    const bool normal_release = manual_fire_id.load() == 0 && independent && (estimated || masked_hold) && input_ok && events_ok &&
                        intent.input_continuous && intent.epoch == active_input_epoch &&
                        !input.virtual_keys[config.activation_virtual_key] && !input.virtual_keys[0x23] &&
                        !release_key_held(input) && !paused.load() && !stopping.load() &&
                        active_generation == cancel_generation.load() && allowed && allowed() &&
                        !release_required && session_permission(false);
                    const char* reason = !input_ok ? "input_invalid" : !events_ok ? "input_gap" :
                        independent && (!intent.input_continuous || intent.epoch != active_input_epoch) ? "input_history_discontinuous" :
                        !independent_acquired && !mask_only && !intent.history_valid ? "history_invalid" : paused.load() ? "paused" :
                        !permission(input) ? (normal_release ? "activation_released" : "permission_revoked") :
                        !independent_acquired && held_wasd(input) != original_mask ? "direction_changed" :
                        active_generation != cancel_generation.load() ? "caller_canceled" : "lease_expired";
                    cancel_active(false, reason, normal_release);
                }
                // 仅屏蔽没有制动完成资格；真实方向键全松后清理屏蔽，交回原地几何扳机。
                // 已完成反向制动或人工接管仍沿用原保持契约，不补发接管期间的松键反向。
                if (active_id && independent && masked_hold && manual_fire_id.load() == 0 &&
                    input_ok && events_ok && intent.input_continuous && intent.epoch == active_input_epoch &&
                    intent.held_mask == 0 && held_wasd(input) == 0) {
                    cancel_active(false, "masked_directions_released", true);
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
                        // 目标只用于这次准入；接收后不再因目标变化撤销制动。
                        if (!pending_id && !target_consumed && request_eligible && events_ok &&
                            intent.input_continuous && intent.held_mask != 0 &&
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
                        { std::lock_guard<std::mutex> lock(mutex); state.cycle_moving = false; }
                        if (config.use_counterpulse_timing) {
                            std::lock_guard<std::mutex> lock(mutex);
                            state.counter_release_ack_ns = state.completion_ready_ns = 0;
                        }
                        active_input_epoch = intent.epoch;
                        original_mask = intent.held_mask;
                        estimated = false;
                        masked_hold = false;
                        mask_only = independent && (!intent.history_valid || intent.conflicting);
                        if (!input_ok || !events_ok || (independent ? !intent.input_continuous : !intent.history_valid) || !permission(input) ||
                            (independent && !session_permission(false)) ||
                            active_generation != cancel_generation.load() || Clock::now() >= lease_end) {
                            cancel_active(false, "request_not_eligible");
                        } else {
                            const auto decision = mask_only ? AutoStopDecision{} : controller.request(active_id, now_ns());
                            const bool plan_valid = decision.phase == AutoStopPhase::WAITING_ACK && decision.request_id == active_id;
                            // 归还窗口等情况也可能只保留原始历史，没有可继续使用的运动模型。
                            if (independent && !plan_valid) mask_only = true;
                            if ((!mask_only && !plan_valid) || !reserve()) {
                                cancel_active(false, "controller_or_arbiter_unavailable");
                            } else {
                                publish(AutoStopStatus::BRAKING);
                                LOG_INFO("auto_stop", "开始请求{}，来源={}，物理方向mask={}", active_id,
                                    independent ? "目标识别" : "显式调用", original_mask);
                                // 独立锁存覆盖全部WASD，避免完成后换方向键绕过屏蔽。
                                const std::uint8_t mask_to_install = independent ? 15 : original_mask;
                                for (std::uint8_t key = 1; key <= 8; key <<= 1) if (mask_to_install & key) {
                                    if (!mouse->poll_input(input) || !permission(input) ||
                                        (independent && !session_permission(false)) || (!mask_only && held_wasd(input) != original_mask) ||
                                        active_generation != cancel_generation.load() || Clock::now() >= lease_end) { cancel_active(false, "permission_changed_before_mask"); break; }
                                    const auto started = now_ns();
                                    const auto result = mouse->set_wasd_mask(key, true);
                                    debt |= result.datagram_sent;
                                    receipt(result, started);
                                    if (result.disposition != KeyboardDisposition::ACKNOWLEDGED) { cancel_active(true, "mask_not_acknowledged"); break; }
                                }
                                if (active_id && independent) {
                                    // 逐键安装不是原子事务；先复核安装窗口，不能把积压的改向/缺口
                                    // 当成接管后的健康输入。临时游标不消费正式事件。
                                    const auto acquired_at = now_ns();
                                    auto acquisition_cursor = cursor;
                                    WasdEventBatch acquisition_events;
                                    auto acquisition_history = history;
                                    const bool acquisition_valid = mouse->read_wasd_events(acquisition_cursor, acquisition_events) &&
                                        acquisition_events.subscribed && !acquisition_events.gap &&
                                        acquisition_cursor.epoch == active_input_epoch &&
                                        acquisition_events.count < acquisition_events.events.size() &&
                                        std::all_of(acquisition_events.events.begin(),
                                            acquisition_events.events.begin() + acquisition_events.count, [&](const auto& event) {
                                                const auto observed = acquisition_history.observe(event.held_mask, event.epoch,
                                                    event.sequence, event.received_at_steady_ns, event.state_valid);
                                                return event.state_valid && event.epoch == active_input_epoch && observed.input_continuous &&
                                                    (mask_only || event.received_at_steady_ns > acquired_at || event.held_mask == original_mask);
                                            });
                                    if (!acquisition_valid) cancel_active(false, "input_changed_during_acquisition");
                                    else {
                                        independent_acquired = true;
                                        LOG_INFO("auto_stop", "请求{}全部WASD屏蔽已确认，模式={}", active_id,
                                            mask_only ? "仅零报告保持，运动模型不可用" : "有限制动后保持至松键");
                                    }
                                }
                            }
                        }
                    } else if (!fault) {
                        publish(config.activation_virtual_key == 0 ? AutoStopStatus::UNBOUND : paused.load() ? AutoStopStatus::PAUSED :
                            (input_ok && events_ok && (allocate_request ? intent.input_continuous : intent.history_valid) ? AutoStopStatus::READY : AutoStopStatus::WAITING_INPUT));
                    }
                }
                if (active_id && mask_only) {
                    if (!masked_hold) {
                        if (!reserve() || !mouse->poll_input(input) || !permission(input) || !session_permission(false) ||
                            active_generation != cancel_generation.load() || Clock::now() >= lease_end) {
                            cancel_active(false, "permission_changed_before_zero");
                        } else {
                            const auto started = now_ns();
                            const auto result = mouse->set_wasd_keyboard(0);
                            debt |= result.datagram_sent;
                            receipt(result, started);
                            if (result.disposition != KeyboardDisposition::ACKNOWLEDGED) cancel_active(true, "zero_not_acknowledged");
                            else {
                                software_mask = 0;
                                masked_hold = true;
                                publish(AutoStopStatus::MASKED);
                                LOG_INFO("auto_stop", "请求{}零软件报告已确认，仅屏蔽保持至松键；未估算停稳，未授予开火", active_id);
                                release_reservation();
                            }
                        }
                    }
                } else if (active_id) {
                    const auto previous = controller.decision();
                    const auto decision = controller.tick(now_ns());
                    if (decision.phase == AutoStopPhase::INVALID || decision.phase == AutoStopPhase::CANCELLED) cancel_active(false, "controller_canceled");
                    else if (decision.phase == AutoStopPhase::WAITING_ACK) {
                        if (!reserve() || !mouse->poll_input(input) || !permission(input) ||
                            (independent && !session_permission(false)) ||
                            (!independent_acquired && held_wasd(input) != original_mask) ||
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
                            const auto returned = now_ns();
                            const auto ack_time = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                result.protocol_ack_received_at.time_since_epoch()).count();
                            const auto backend_time = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                result.backend_completed_at.time_since_epoch()).count();
                            const bool ack_valid = !config.use_counterpulse_timing ||
                                (ack_time >= started && ack_time <= backend_time && backend_time <= returned);
                            if (result.disposition != KeyboardDisposition::ACKNOWLEDGED || !ack_valid)
                                cancel_active(true, "keyboard_not_acknowledged");
                            else {
                                software_mask = decision.desired_mask;
                                const auto completed = controller.acknowledge(active_id, decision.command_id, software_mask,
                                    config.use_counterpulse_timing ? ack_time : returned);
                                if (config.use_counterpulse_timing && completed.completion_ready_ns != 0) {
                                    std::lock_guard<std::mutex> lock(mutex);
                                    state.counter_release_ack_ns = ack_time;
                                    state.completion_ready_ns = completed.completion_ready_ns;
                                }
                                if (completed.phase == AutoStopPhase::COMPLETE_ESTIMATED) mark_estimated();
                            }
                        }
                    } else if (decision.phase == AutoStopPhase::COMPLETE_ESTIMATED) {
                        mark_estimated();
                        publish(AutoStopStatus::ESTIMATED);
                    }
                }
                // 只串行本轮实际后端事务；反向持有与释放后等待期间允许Aim进入。
                release_reservation();
                report_block();
                std::unique_lock<std::mutex> lock(mutex);
                const bool idle = input_ok && events_ok && intent.input_continuous && intent.held_mask == 0 &&
                    held_wasd(input) == 0 && !active_id && !pending_id && !debt && !fault && !state.cleanup_unknown;
                if (trigger_idle != idle) ++trigger_idle_generation;
                trigger_idle = idle;
                if (trigger_idle) trigger_idle_cursor = cursor;
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
        std::function<std::uint64_t()> allocate_request, std::function<bool()> focused,
        std::function<AutoStopWeaponContext()> weapon_context)
    : impl_(std::make_unique<Impl>(std::move(mouse), std::move(arbiter), std::move(permission))) {
    impl_->allocate_request = std::move(allocate_request);
    impl_->focused = std::move(focused);
    impl_->weapon_context = std::move(weapon_context);
}
void AutoStopWorker::publish_target(std::chrono::steady_clock::time_point valid_until, AutoStopBlockReason reason) noexcept {
    if (!impl_) return;
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->target_until = valid_until;
        impl_->target_reason = valid_until == Clock::time_point{} ? reason : AutoStopBlockReason::NONE;
        impl_->wake.notify_all();
    } catch (...) {}
}
void AutoStopWorker::publish_tracking_target(Clock::time_point valid_until) noexcept {
    if (!impl_) return;
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto now = Clock::now();
        // 先检查旧期限：worker尚未处理过期时，新帧也不能复活已中断的人工保持。
        if (impl_->manual_fire_id.load() != 0 &&
            (impl_->tracking_target_until <= now || valid_until <= now))
            impl_->cancel_generation.fetch_add(1, std::memory_order_acq_rel);
        impl_->tracking_target_until = valid_until;
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
        impl_->trigger_idle = false;
        impl_->trigger_idle_cursor = {};
        ++impl_->trigger_idle_generation;
        impl_->weapon_context_seen = false;
        impl_->resume_id = 0;
        impl_->manual_fire_id = 0;
        impl_->tracking_target_until = {};
        impl_->movement_not_before = {};
        impl_->state.use_counterpulse_timing = config.use_counterpulse_timing;
        if (config.use_counterpulse_timing) {
            if (config.counter_hold_ms < 1 || config.counter_hold_ms > 200 ||
                config.shot_after_release_ms < 0 || config.shot_after_release_ms > 200) return false;
            impl_->state.counter_hold_ms = config.counter_hold_ms;
            impl_->state.shot_after_release_ms = config.shot_after_release_ms;
        }
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
            impl_->arbiter->latch_output_fault();
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
bool AutoStopWorker::retain_for_manual_fire(std::uint64_t request_id) noexcept {
    if (!impl_ || request_id == 0) return false;
    try {
        InputSnapshot input;
        if (!impl_->mouse->poll_input(input) || !input.state_valid || input.status != InputMonitorStatus::READY ||
            !input.virtual_keys[1] || input.virtual_keys[0x23] || impl_->release_key_held(input)) return false;
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->running || !impl_->allocate_request || impl_->fault || impl_->state.cleanup_unknown ||
            impl_->paused.load() || impl_->stopping.load() || impl_->arbiter->faulted_.load() ||
            impl_->state.release_required || !impl_->state.source_focused ||
            impl_->state.status != AutoStopStatus::ESTIMATED || impl_->estimated_id != request_id ||
            impl_->estimated_generation != impl_->cancel_generation.load() ||
            Clock::now() >= impl_->tracking_target_until ||
            (impl_->state.weapon_context.required && !impl_->state.weapon_context.valid)) return false;
        impl_->manual_fire_id = request_id;
        impl_->resume_id = 0;
        impl_->wake.notify_all();
        return true;
    } catch (...) { return false; }
}
bool AutoStopWorker::resume_movement(std::uint64_t request_id, Clock::time_point not_before) noexcept {
    if (!impl_ || request_id == 0) return false;
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto now = Clock::now();
        if (!impl_->running || impl_->manual_fire_id.load() != 0 || !impl_->config.cycle_enabled || !impl_->allocate_request || impl_->fault ||
            impl_->paused.load() || impl_->stopping.load() || impl_->state.cleanup_unknown ||
            impl_->state.release_required || impl_->state.status != AutoStopStatus::ESTIMATED ||
            impl_->estimated_id != request_id || impl_->resume_id != 0 ||
            impl_->estimated_generation != impl_->cancel_generation.load() ||
            not_before > now + std::chrono::seconds(10)) return false;
        impl_->resume_id = request_id;
        impl_->resume_generation = impl_->estimated_generation;
        impl_->resume_not_before = not_before;
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
        impl_->weapon_permission();
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->running || impl_->state.request_id != request_id) return;
        // 武器已撤销该请求时，Trigger的对应UP清理不再升级成独立信任中断。
        if (impl_->normal_weapon_generation != impl_->cancel_generation.load() ||
            impl_->submitted_generation == impl_->cancel_generation.load())
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

bool AutoStopWorker::idle_for_trigger(const InputSnapshot& input) const noexcept {
    if (!impl_ || !input.state_valid || input.status != InputMonitorStatus::READY || held_wasd(input) != 0 ||
        input.virtual_keys[0x23] || impl_->release_key_held(input)) return false;
    try {
        if (!impl_->weapon_permission()) return false;
        WasdEventCursor cursor;
        std::uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (!impl_->running || !impl_->trigger_idle || impl_->pending_id || impl_->fault ||
                impl_->state.status == AutoStopStatus::BRAKING || impl_->state.status == AutoStopStatus::ESTIMATED ||
                impl_->state.status == AutoStopStatus::MASKED || impl_->state.status == AutoStopStatus::FAULT ||
                impl_->state.cleanup_unknown || impl_->state.release_required || impl_->paused.load() ||
                impl_->stopping.load()) return false;
            cursor = impl_->trigger_idle_cursor;
            generation = impl_->trigger_idle_generation;
        }
        const auto previous = cursor;
        WasdEventBatch tail;
        if (!impl_->mouse->read_wasd_events(cursor, tail) || !tail.subscribed || tail.gap ||
            cursor.epoch != previous.epoch || tail.count == tail.events.size()) return false;
        auto sequence = previous.sequence;
        for (std::size_t i = 0; i < tail.count; ++i) {
            const auto& event = tail.events[i];
            if (!event.state_valid || event.epoch != previous.epoch || event.held_mask != 0 ||
                event.sequence <= sequence || event.sequence - sequence != 1) return false;
            sequence = event.sequence;
        }
        if (cursor.sequence != sequence) return false;
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->running && impl_->trigger_idle && impl_->trigger_idle_generation == generation &&
            !impl_->pending_id && !impl_->fault && !impl_->state.cleanup_unknown && !impl_->state.release_required &&
            !impl_->paused.load() && !impl_->stopping.load() &&
            impl_->state.status != AutoStopStatus::BRAKING && impl_->state.status != AutoStopStatus::ESTIMATED &&
            impl_->state.status != AutoStopStatus::MASKED && impl_->state.status != AutoStopStatus::FAULT;
    } catch (...) { return false; }
}

std::uint64_t AutoStopWorker::estimated_completion_id() const noexcept {
    if (!impl_) return 0;
    try {
        InputSnapshot input;
        if (!impl_->mouse->poll_input(input) || !impl_->permission(input)) return 0;
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->running && !impl_->fault && !impl_->state.cleanup_unknown &&
            impl_->state.status == AutoStopStatus::ESTIMATED && impl_->state.source_focused &&
            impl_->estimated_generation == impl_->cancel_generation.load() &&
            !impl_->paused.load() && !impl_->stopping.load() ? impl_->estimated_id : 0;
    } catch (...) { return 0; }
}
