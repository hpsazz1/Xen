#include "trigger/trigger_worker.h"
#include "log/log.h"
#include <condition_variable>
#include <mutex>
#include <thread>

class TriggerWorker::Impl {
public:
    std::shared_ptr<IMouseController> mouse;
    std::shared_ptr<AutoStopOutputArbiter> arbiter;
    std::function<bool()> permission, focused;
    std::function<std::uint64_t()> next_id;
    std::function<bool(std::uint64_t)> request_stop;
    std::function<void(std::uint64_t)> cancel_stop;
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
    TriggerTime cleanup_due{};
    int cleanup_budget_ms = 1000;
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
            if (decision.stop_action == TriggerStopAction::REQUEST) {
                reserved_stop_id = 0;
                if (!request_stop(decision.stop_request_id)) {
                    decision = controller.cancel(TriggerReason::STOP_UNVERIFIED, TriggerClock::now());
                    continue;
                }
            } else if (decision.stop_action == TriggerStopAction::CANCEL) {
                // 先确认左键归零，再通知急停归还；未知释放只留下lease负责兜底。
                if (decision.snapshot.button_may_be_down) deferred_cancel_id = decision.stop_request_id;
                else cancel_stop(decision.stop_request_id);
            }
            if (decision.button_action == TriggerButtonAction::NONE) break;
            const bool down = decision.button_action == TriggerButtonAction::DOWN;
            if (!down) { std::lock_guard state_lock(mutex); firing.confirmed_down = false; }
            auto lock = down ? arbiter->try_enter_aim() : arbiter->try_enter_cleanup();
            TriggerReceipt receipt;
            receipt.command_id = decision.command_id;
            receipt.action = decision.button_action;
            receipt.status = TriggerReceiptStatus::NOT_SENT;
            if (lock.owns_lock()) {
                const auto fresh = permit();
                const auto check_at = TriggerClock::now();
                const bool stop_valid = !config.require_stop || (fresh.stop_observed_qualified &&
                    fresh.stop_request_id == decision.snapshot.stop_request_id &&
                    fresh.stop_observation_epoch == decision.snapshot.observation_epoch &&
                    fresh.stop_expires_at > check_at && fresh.stop_release_deadline > check_at);
                if (!down || (fresh.enabled && fresh.armed && fresh.healthy && fresh.held && fresh.focused &&
                    !fresh.physical_left_down && !canceled.load() && !stopping.load() &&
                    stop_valid && observation_still_current(decision, check_at))) {
                    const auto result = mouse->set_left_button(down);
                    receipt.status = result.disposition == ButtonDisposition::ACKNOWLEDGED
                        ? TriggerReceiptStatus::ACKNOWLEDGED
                        : result.disposition == ButtonDisposition::APPLICATION_UNKNOWN || result.cleanup_required
                            ? TriggerReceiptStatus::UNKNOWN : TriggerReceiptStatus::NOT_SENT;
                    if (receipt.status == TriggerReceiptStatus::UNKNOWN) {
                        arbiter->latch_output_fault();
                    }
                }
                lock.unlock();
            }
            receipt.completed_at = TriggerClock::now();
            if (down && receipt.status == TriggerReceiptStatus::ACKNOWLEDGED) {
                std::lock_guard state_lock(mutex);
                firing = {true, receipt.command_id, receipt.completed_at};
            }
            decision = controller.acknowledge(receipt, receipt.completed_at);
            if (!down) {
                ++cleanup_attempts;
                cleanup_due = receipt.completed_at + std::chrono::milliseconds(2);
                if (receipt.status == TriggerReceiptStatus::ACKNOWLEDGED && deferred_cancel_id) {
                    cancel_stop(deferred_cancel_id); deferred_cancel_id = 0;
                }
            } else cleanup_attempts = 0;
        }
        const auto next = controller.snapshot();
        std::lock_guard lock(mutex);
        if (next.reason != state.reason || next.command_id != state.command_id) {
            LOG_DEBUG("trigger", "trigger.schema=1 phase={} reason={} command={} frame={} stop={} possible_down={}",
                static_cast<int>(next.phase), TriggerReasonName(next.reason), next.command_id,
                next.observation_sequence, next.stop_request_id, next.button_may_be_down);
        }
        state = next;
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
                if (current.faulted && current.button_may_be_down && cleanup_attempts < 3 &&
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
            execute(controller.cancel(TriggerReason::CANCELED, TriggerClock::now()));
            // 未取得锁不消耗发送次数，不能因连续三次争锁失败就遗留共享设备按下态。
            // 截止时间约束新清理调用的准入；已在途调用仍受后端command timeout约束。
            const auto cleanup_deadline = TriggerClock::now() + std::chrono::milliseconds(cleanup_budget_ms);
            int attempts = 0;
            while (attempts < 3 && TriggerClock::now() < cleanup_deadline &&
                (controller.snapshot().button_may_be_down || mouse->left_button_cleanup_required())) {
                auto lock = arbiter->try_enter_cleanup();
                if (!lock.owns_lock()) continue;
                ++attempts;
                const auto result = mouse->set_left_button(false);
                if (result.disposition == ButtonDisposition::ACKNOWLEDGED) {
                    if (deferred_cancel_id) { cancel_stop(deferred_cancel_id); deferred_cancel_id = 0; }
                    // 已有pending up接收同一个清理回执，快照不假留按下。
                    const auto pending = controller.snapshot();
                    TriggerReceipt receipt{pending.command_id, TriggerButtonAction::UP,
                        TriggerReceiptStatus::ACKNOWLEDGED, TriggerClock::now()};
                    lock.unlock();
                    execute(controller.acknowledge(receipt, receipt.completed_at));
                    break;
                }
                arbiter->latch_output_fault();
            }
            if (controller.snapshot().button_may_be_down || mouse->left_button_cleanup_required()) {
                arbiter->latch_output_fault();
                std::lock_guard lock(mutex);
                state.faulted = true;
                state.button_may_be_down = true;
                state.phase = TriggerPhase::FAULT;
                state.reason = TriggerReason::UNKNOWN_RECEIPT;
            }
        } catch (...) {
            arbiter->latch_output_fault();
            auto lock = arbiter->try_enter_cleanup();
            if (lock.owns_lock()) mouse->set_left_button(false);
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
    std::function<bool(std::uint64_t)> request_stop, std::function<void(std::uint64_t)> cancel_stop)
    : impl_(std::make_unique<Impl>()) {
    impl_->mouse = std::move(mouse); impl_->arbiter = std::move(arbiter);
    impl_->permission = std::move(permission); impl_->focused = std::move(focused);
    impl_->next_id = std::move(next_stop_id); impl_->request_stop = std::move(request_stop);
    impl_->cancel_stop = std::move(cancel_stop);
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
