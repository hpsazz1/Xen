#include "trigger/trigger_worker.h"
#include "log/log.h"
#include <condition_variable>
#include <mutex>
#include <thread>
#include <algorithm>
#include <array>
#include <limits>

class TriggerWorker::Impl {
public:
    std::shared_ptr<IMouseController> mouse;
    std::shared_ptr<AutoStopOutputArbiter> arbiter;
    std::function<bool()> permission, focused;
    std::function<std::uint64_t()> next_id;
    std::function<bool(std::uint64_t)> request_stop;
    std::function<void(std::uint64_t)> cancel_stop;
    std::function<TriggerContext()> context;
    TriggerConfig config;
    TriggerController controller;
    std::atomic<std::shared_ptr<const TriggerObservation>> latest;
    std::shared_ptr<const TriggerObservation> evaluated_observation;
    std::atomic<bool> stopping{false}, canceled{false};
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::thread thread;
    TriggerSnapshot state;
    TriggerFiringSignal firing;
    std::uint64_t reserved_stop_id = 0, deferred_cancel_id = 0;
    unsigned cleanup_attempts = 0;
    TriggerTime cleanup_due{}, cleanup_deadline{};
    bool cleanup_active = false, cleanup_exhausted = false;
    std::array<TriggerExecutionEvent, 2048> events{};
    std::size_t event_begin = 0, event_count = 0;
    std::uint64_t event_sequence = 0, dropped_count = 0;
    int cleanup_budget_ms = 1000;
    void record(TriggerExecutionEvent event) {
        std::lock_guard lock(mutex);
        if (event_sequence == std::numeric_limits<std::uint64_t>::max()) {
            if (dropped_count != std::numeric_limits<std::uint64_t>::max()) ++dropped_count;
            return;
        }
        event.sequence = ++event_sequence;
        if (event_count == events.size()) {
            event_begin = (event_begin + 1) % events.size();
            --event_count;
            if (dropped_count != std::numeric_limits<std::uint64_t>::max()) ++dropped_count;
        }
        events[(event_begin + event_count++) % events.size()] = event;
    }
    void begin_cleanup(TriggerTime now) {
        if (cleanup_active) return;
        cleanup_active = true;
        cleanup_exhausted = false;
        cleanup_attempts = 0;
        cleanup_due = now;
        cleanup_deadline = now + std::chrono::milliseconds(cleanup_budget_ms);
    }
    void check_cleanup_budget(TriggerTime now) {
        if (!cleanup_active || cleanup_exhausted ||
            (cleanup_attempts < 3 && now < cleanup_deadline)) return;
        cleanup_exhausted = true;
        arbiter->latch_output_fault();
        TriggerExecutionEvent event;
        event.snapshot = controller.snapshot();
        event.snapshot.faulted = true;
        event.snapshot.phase = TriggerPhase::FAULT;
        event.snapshot.reason = TriggerReason::UNKNOWN_RECEIPT;
        event.rejection_reason = cleanup_attempts >= 3 ? "cleanup_attempts_exhausted" : "cleanup_deadline_expired";
        event.observed_at = now;
        record(event);
    }
    TriggerPermit permit(bool allocate_stop_id = false) {
        TriggerPermit p;
        InputSnapshot input;
        const bool polled = mouse->poll_input(input);
        p.enabled = config.enabled && !stopping.load();
        p.healthy = polled && input.state_valid && input.status == InputMonitorStatus::READY;
        p.held = config.hold_virtual_key > 0 && input.virtual_keys[config.hold_virtual_key];
        p.physical_left_down = input.virtual_keys[1];
        p.armed = permission() && !stopping.load();
        p.focused = focused();
        if (context) p.context = context();
        const auto current = controller.snapshot();
        if (allocate_stop_id && config.require_stop && current.region != TriggerRegion::NONE &&
            current.stop_request_id == 0 && reserved_stop_id == 0 && p.enabled && p.healthy &&
            p.held && p.armed && p.focused && !p.physical_left_down && !current.faulted)
            reserved_stop_id = next_id();
        p.next_stop_request_id = reserved_stop_id;
        // 当前AutoStop只有ESTIMATED，不能构造观察停稳许可。
        return p;
    }
    bool observation_still_current(const TriggerDecision& decision, TriggerTime now) {
        const auto value = latest.load();
        if (!value || value != evaluated_observation || !value->valid || !value->timing_valid || value->observed_at == TriggerTime{} ||
            value->observed_at > now || value->uncertainty.count() < 0 ||
            value->epoch != decision.snapshot.observation_epoch ||
            value->sequence != decision.snapshot.observation_sequence) return false;
        const auto age_limit = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::milliseconds(config.max_observation_age_ms));
        return value->uncertainty < age_limit && now - value->observed_at < age_limit - value->uncertainty;
    }
    void execute(TriggerDecision decision) {
        for (int chain = 0; chain < 4; ++chain) {
            TriggerExecutionEvent event;
            event.snapshot = decision.snapshot;
            event.button_action = decision.button_action;
            event.stop_action = decision.stop_action;
            event.stop_request_id = decision.stop_request_id;
            event.planned_at = TriggerClock::now();
            if (evaluated_observation) event.source_observed_at = evaluated_observation->observed_at;
            if (decision.stop_action == TriggerStopAction::REQUEST) {
                reserved_stop_id = 0;
                if (!request_stop(decision.stop_request_id)) {
                    event.rejection_reason = "stop_request_rejected";
                    event.observed_at = TriggerClock::now(); record(event);
                    decision = controller.cancel(TriggerReason::STOP_UNVERIFIED, event.observed_at);
                    continue;
                }
            } else if (decision.stop_action == TriggerStopAction::CANCEL) {
                // 先确认左键归零，再通知急停归还；未知释放只留下lease负责兜底。
                if (decision.snapshot.button_may_be_down) deferred_cancel_id = decision.stop_request_id;
                else cancel_stop(decision.stop_request_id);
            }
            if (decision.button_action == TriggerButtonAction::NONE) {
                bool changed;
                { std::lock_guard lock(mutex); changed = decision.snapshot.reason != state.reason ||
                    decision.snapshot.phase != state.phase || decision.snapshot.command_id != state.command_id; }
                if (changed || decision.stop_action != TriggerStopAction::NONE) {
                    event.observed_at = TriggerClock::now(); record(event);
                }
                break;
            }
            const bool down = decision.button_action == TriggerButtonAction::DOWN;
            if (!down) {
                { std::lock_guard state_lock(mutex); firing.confirmed_down = false; }
                begin_cleanup(event.planned_at);
                check_cleanup_budget(event.planned_at);
                if (cleanup_exhausted || event.planned_at < cleanup_due) break;
            }
            OutputArbiterRejection rejection = OutputArbiterRejection::NONE;
            auto lock = down ? arbiter->try_enter_aim(OutputArbiterSource::TRIGGER, &rejection) : arbiter->try_enter_cleanup();
            TriggerReceipt receipt;
            receipt.command_id = decision.command_id;
            receipt.action = decision.button_action;
            receipt.status = TriggerReceiptStatus::NOT_SENT;
            bool context_changed = false;
            event.rejection_reason = "arbiter_unavailable";
            if (rejection == OutputArbiterRejection::LOCK_BUSY) event.rejection_reason = "arbiter_lock_busy";
            else if (rejection == OutputArbiterRejection::AUXILIARY_PENDING) event.rejection_reason = "arbiter_auxiliary_pending";
            else if (rejection == OutputArbiterRejection::OUTPUT_FAULT) event.rejection_reason = "arbiter_output_fault";
            if (lock.owns_lock()) {
                const auto check_at = TriggerClock::now();
                bool eligible = !down && check_at < cleanup_deadline;
                if (down) {
                    const auto fresh = permit();
                    const auto revalidated_at = TriggerClock::now();
                    const auto& decided_context = decision.snapshot.context;
                    const bool context_valid = fresh.context.required == decided_context.required &&
                        (!fresh.context.required || (fresh.context.valid && decided_context.valid &&
                            fresh.context.generation != 0 && fresh.context.generation == decided_context.generation));
                    context_changed = !context_valid;
                    const bool stop_valid = !config.require_stop || (fresh.stop_observed_qualified &&
                        fresh.stop_request_id == decision.snapshot.stop_request_id &&
                        fresh.stop_observation_epoch == decision.snapshot.observation_epoch &&
                        fresh.stop_expires_at > revalidated_at && fresh.stop_release_deadline > revalidated_at);
                    eligible = fresh.enabled && fresh.armed && fresh.healthy && fresh.held && fresh.focused &&
                        !fresh.physical_left_down && !canceled.load() && !stopping.load() &&
                        context_valid && stop_valid && observation_still_current(decision, revalidated_at);
                    event.rejection_reason = !context_valid ? "context_changed" :
                        !stop_valid ? "stop_unverified" : "permission_or_observation_changed";
                } else event.rejection_reason = "cleanup_deadline_expired";
                if (eligible) {
                    event.backend_called = true;
                    event.call_started_at = TriggerClock::now();
                    if (!down) ++cleanup_attempts;
                    const auto result = mouse->set_left_button(down);
                    event.observed_at = TriggerClock::now();
                    event.backend_completed_at = result.backend_completed_at;
                    event.protocol_ack_received_at = result.protocol_ack_received_at;
                    const bool valid_time = result.backend_completed_at != TriggerTime{} &&
                        result.backend_completed_at >= event.call_started_at && result.backend_completed_at <= event.observed_at &&
                        result.protocol_ack_received_at != TriggerTime{} &&
                        result.protocol_ack_received_at >= event.call_started_at &&
                        result.protocol_ack_received_at <= result.backend_completed_at;
                    receipt.status = result.disposition == ButtonDisposition::ACKNOWLEDGED && valid_time
                        ? TriggerReceiptStatus::ACKNOWLEDGED
                        : result.disposition == ButtonDisposition::ACKNOWLEDGED ||
                          result.disposition == ButtonDisposition::APPLICATION_UNKNOWN || result.cleanup_required
                            ? TriggerReceiptStatus::UNKNOWN : TriggerReceiptStatus::NOT_SENT;
                    event.rejection_reason = receipt.status == TriggerReceiptStatus::ACKNOWLEDGED ? "none" :
                        result.disposition == ButtonDisposition::ACKNOWLEDGED && !valid_time ? "invalid_receipt_time" :
                        receipt.status == TriggerReceiptStatus::UNKNOWN ? "unknown_receipt" : "backend_not_sent";
                    receipt.completed_at = result.backend_completed_at;
                    if (receipt.status == TriggerReceiptStatus::UNKNOWN) arbiter->latch_output_fault();
                }
                lock.unlock();
            }
            if (event.observed_at == TriggerTime{}) event.observed_at = TriggerClock::now();
            // 未调用/明确未发送只有本机拒绝时刻；绝不补造成功后端时间。
            if (receipt.status == TriggerReceiptStatus::NOT_SENT) receipt.completed_at = event.observed_at;
            event.receipt_status = receipt.status;
            record(event);
            if (down && receipt.status == TriggerReceiptStatus::ACKNOWLEDGED && !stopping.load() && !canceled.load()) {
                std::lock_guard state_lock(mutex);
                firing = {true, receipt.command_id, receipt.completed_at, event.call_started_at,
                    event.backend_completed_at, event.protocol_ack_received_at, event.observed_at,
                    std::chrono::duration_cast<std::chrono::nanoseconds>(event.backend_completed_at - event.call_started_at)};
            }
            decision = controller.acknowledge(receipt, event.observed_at);
            if (context_changed) {
                reserved_stop_id = 0;
                auto cancellation = controller.cancel(TriggerReason::CONTEXT_CHANGED, event.observed_at);
                // NOT_SENT 已清按钮责任，但其返回的急停 CANCEL 仍须执行，不能被新取消覆盖。
                if (cancellation.stop_action == TriggerStopAction::NONE) {
                    cancellation.stop_action = decision.stop_action;
                    cancellation.stop_request_id = decision.stop_request_id;
                }
                decision = cancellation;
            }
            if (!down) {
                cleanup_due = event.observed_at + std::chrono::milliseconds(2);
                if (receipt.status == TriggerReceiptStatus::ACKNOWLEDGED) {
                    cleanup_active = false;
                    if (deferred_cancel_id) { cancel_stop(deferred_cancel_id); deferred_cancel_id = 0; }
                } else check_cleanup_budget(event.observed_at);
            }
        }
        auto next = controller.snapshot();
        if (cleanup_exhausted) { next.faulted = true; next.phase = TriggerPhase::FAULT; next.reason = TriggerReason::UNKNOWN_RECEIPT; }
        std::lock_guard lock(mutex);
        state = next;
    }
    void finish_cleanup() {
        // 停止/异常是一次独立的最终清理阶段，允许恢复运行阶段已耗尽的债务。
        // 三条路径共用execute；阶段内只有真实后端调用计数，截止时间不续期。
        cleanup_active = false;
        begin_cleanup(TriggerClock::now());
        execute(controller.cancel(TriggerReason::CANCELED, TriggerClock::now()));
        if (!controller.snapshot().button_may_be_down && !mouse->left_button_cleanup_required()) {
            cleanup_active = false;
            return;
        }
        while (controller.snapshot().button_may_be_down || mouse->left_button_cleanup_required()) {
            check_cleanup_budget(TriggerClock::now());
            if (cleanup_exhausted) break;
            const auto pending = controller.snapshot();
            TriggerDecision retry;
            retry.button_action = TriggerButtonAction::UP;
            retry.command_id = pending.command_id;
            retry.snapshot = pending;
            execute(retry);
            if (controller.snapshot().button_may_be_down || mouse->left_button_cleanup_required()) {
                std::unique_lock lock(mutex);
                wake.wait_until(lock, std::min(cleanup_due, cleanup_deadline));
            }
        }
        if (controller.snapshot().button_may_be_down || mouse->left_button_cleanup_required()) {
            arbiter->latch_output_fault();
            std::lock_guard lock(mutex);
            state.faulted = true; state.button_may_be_down = true;
            state.phase = TriggerPhase::FAULT; state.reason = TriggerReason::UNKNOWN_RECEIPT;
        }
    }
    void run() noexcept {
        try {
            std::shared_ptr<const TriggerObservation> consumed;
            while (!stopping.load()) {
                const auto now = TriggerClock::now();
                if (canceled.exchange(false)) {
                    reserved_stop_id = 0;
                    execute(controller.cancel(TriggerReason::CANCELED, now));
                }
                auto observation = latest.load();
                const auto p = permit(true);
                if (observation && observation != consumed) {
                    evaluated_observation = observation;
                    execute(controller.observe(*observation, p, now));
                    consumed = std::move(observation);
                } else execute(controller.tick(p, now));
                const auto current = controller.snapshot();
                check_cleanup_budget(TriggerClock::now());
                if (current.faulted && current.button_may_be_down && !cleanup_exhausted &&
                    TriggerClock::now() >= cleanup_due) {
                    TriggerDecision retry;
                    retry.button_action = TriggerButtonAction::UP;
                    retry.command_id = current.command_id;
                    retry.snapshot = current;
                    execute(retry);
                }
                std::unique_lock lock(mutex);
                wake.wait_for(lock, std::chrono::milliseconds(2));
            }
            finish_cleanup();
        } catch (...) {
            arbiter->latch_output_fault();
            try { finish_cleanup(); } catch (...) {}
            std::lock_guard state_lock(mutex);
            firing.confirmed_down = false;
            state.faulted = true;
            state.phase = TriggerPhase::FAULT;
            state.reason = TriggerReason::UNKNOWN_RECEIPT;
        }
    }
};

TriggerWorker::TriggerWorker(std::shared_ptr<IMouseController> mouse,
    std::shared_ptr<AutoStopOutputArbiter> arbiter, std::function<bool()> permission,
    std::function<bool()> focused, std::function<std::uint64_t()> next_stop_id,
    std::function<bool(std::uint64_t)> request_stop, std::function<void(std::uint64_t)> cancel_stop,
    std::function<TriggerContext()> context)
    : impl_(std::make_unique<Impl>()) {
    impl_->mouse = std::move(mouse); impl_->arbiter = std::move(arbiter);
    impl_->permission = std::move(permission); impl_->focused = std::move(focused);
    impl_->next_id = std::move(next_stop_id); impl_->request_stop = std::move(request_stop);
    impl_->cancel_stop = std::move(cancel_stop);
    impl_->context = std::move(context);
}
TriggerWorker::~TriggerWorker() { stop(); }
bool TriggerWorker::start(const TriggerConfig& config, int cleanup_budget_ms) noexcept {
    try {
        if (impl_->thread.joinable() || !impl_->mouse || !impl_->arbiter ||
            !impl_->permission || !impl_->focused || !impl_->next_id || !impl_->request_stop || !impl_->cancel_stop ||
            !impl_->mouse->supports_left_button() || impl_->mouse->left_button_faulted() ||
            impl_->mouse->left_button_cleanup_required() || cleanup_budget_ms <= 0 || cleanup_budget_ms > 5000 ||
            !impl_->controller.configure(config)) return false;
        impl_->config = config;
        impl_->cleanup_budget_ms = cleanup_budget_ms;
        impl_->stopping.store(false);
        impl_->latest.store({}); impl_->canceled.store(false);
        impl_->evaluated_observation.reset();
        impl_->reserved_stop_id = 0; impl_->cleanup_attempts = 0;
        impl_->cleanup_active = false; impl_->cleanup_exhausted = false;
        impl_->thread = std::thread([this] { impl_->run(); });
        return true;
    } catch (...) { return false; }
}
void TriggerWorker::publish(std::shared_ptr<const TriggerObservation> value) noexcept {
    impl_->latest.store(std::move(value)); impl_->wake.notify_one();
}
void TriggerWorker::cancel() noexcept {
    impl_->canceled.store(true);
    { std::lock_guard lock(impl_->mutex); impl_->firing.confirmed_down = false; }
    impl_->wake.notify_one();
}
void TriggerWorker::stop() noexcept {
    impl_->stopping.store(true); impl_->wake.notify_one();
    if (impl_->thread.joinable()) impl_->thread.join();
}
TriggerFiringSignal TriggerWorker::firing_signal() const noexcept {
    std::lock_guard lock(impl_->mutex);
    auto value = impl_->firing;
    if (impl_->stopping.load() || impl_->canceled.load()) value.confirmed_down = false;
    return value;
}
TriggerSnapshot TriggerWorker::snapshot() const noexcept {
    std::lock_guard lock(impl_->mutex); return impl_->state;
}
TriggerExecutionLog TriggerWorker::execution_log() const {
    std::lock_guard lock(impl_->mutex);
    TriggerExecutionLog result;
    result.dropped_count = impl_->dropped_count;
    result.events.reserve(impl_->event_count);
    for (std::size_t i = 0; i < impl_->event_count; ++i)
        result.events.push_back(impl_->events[(impl_->event_begin + i) % impl_->events.size()]);
    if (!result.events.empty()) {
        result.first_sequence = result.events.front().sequence;
        result.last_sequence = result.events.back().sequence;
    }
    return result;
}
