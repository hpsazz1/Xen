#include "recoil/recoil_worker.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <array>
#include <limits>

class RecoilWorker::Impl {
public:
    std::shared_ptr<IMouseController> mouse;
    std::shared_ptr<AutoStopOutputArbiter> arbiter;
    std::shared_ptr<MotionLedger> ledger;
    std::function<RecoilInput()> context;
    std::function<TriggerFiringSignal()> firing;
    RecoilConfig config;
    RecoilController controller;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::thread thread;
    std::atomic<bool> stopping{false}, canceled{false};
    RecoilSnapshot state;
    std::array<RecoilExecutionRecord,2048> records{};
    std::size_t records_begin = 0, records_count = 0;
    std::uint64_t records_dropped = 0;
    bool physical_previous = false, source_blocked = false;
    int last_source = 0;
    std::uint64_t last_synthetic_id = 0;
    std::uint64_t sampled_firing_id = 0;
    std::optional<std::int64_t> sampled_firing_uncertainty_ns;
    RecoilTime physical_started{};
    RecoilInput input() {
        auto result = context();
        InputSnapshot raw;
        result.healthy = mouse->poll_input(raw) && raw.state_valid && raw.status == InputMonitorStatus::READY &&
            !mouse->left_button_faulted();
        const bool physical = result.healthy && raw.virtual_keys[1];
        const auto synthetic = firing();
        if ((mouse->left_button_cleanup_required() && !synthetic.confirmed_down) ||
            (synthetic.confirmed_down && (!synthetic.id || synthetic.started_at==RecoilTime{}))) result.healthy=false;
        if (result.healthy && physical && !physical_previous) physical_started = RecoilClock::now();
        // 不可信输入不能伪造释放或新按下；下一次健康快照需完整释放再建立来源。
        if (result.healthy) physical_previous = physical;
        else source_blocked = true;
        const int source = physical ? 1 : synthetic.confirmed_down ? 2 : 0;
        sampled_firing_id = source == 2 ? synthetic.id : 0;
        sampled_firing_uncertainty_ns.reset();
        if (source == 2 && synthetic.call_started_at != RecoilTime{} &&
            synthetic.backend_completed_at >= synthetic.call_started_at &&
            synthetic.uncertainty.count() >= 0 && synthetic.uncertainty ==
                synthetic.backend_completed_at - synthetic.call_started_at)
            sampled_firing_uncertainty_ns = synthetic.uncertainty.count();
        if ((physical && synthetic.confirmed_down) || (source && last_source && source != last_source) ||
            (source==2 && last_source==2 && synthetic.id!=last_synthetic_id)) source_blocked = true;
        if (result.healthy && !physical && !synthetic.confirmed_down) {
            source_blocked = false; last_source = 0; last_synthetic_id = 0;
        } else if (source) { last_source = source; if(source==2) last_synthetic_id=synthetic.id; }
        result.enabled = config.enabled && !stopping.load();
        result.held = physical || synthetic.confirmed_down;
        result.firing_started_at = physical ? physical_started : synthetic.started_at;
        result.permission = result.permission && !source_blocked && !stopping.load() && !canceled.load() &&
            (config.hold_virtual_key == 0 || raw.virtual_keys[config.hold_virtual_key]);
        return result;
    }
    void record(const RecoilExecutionRecord& value) {
        std::lock_guard lock(mutex);
        if(records_count==records.size()) {
            records[records_begin]=value;records_begin=(records_begin+1)%records.size();
            if(records_dropped!=std::numeric_limits<std::uint64_t>::max())++records_dropped;
        } else {records[(records_begin+records_count)%records.size()]=value;++records_count;}
    }
    void run() noexcept {
        try {
            while (!stopping.load()) {
                if (canceled.exchange(false)) controller.cancel(RecoilReason::CANCELED, RecoilClock::now());
                const auto sampled_at = RecoilClock::now();
                const auto before = input();
                const auto before_source = last_source;
                const auto before_firing_id = sampled_firing_id;
                const auto before_firing_uncertainty_ns = sampled_firing_uncertainty_ns;
                auto decision = controller.advance(before, RecoilClock::now());
                if (decision.has_intent) {
                    const auto arbitration_at = RecoilClock::now();
                    OutputArbiterRejection arbitration_rejection{};
                    auto lock = arbiter->try_enter_aim(OutputArbiterSource::RECOIL, &arbitration_rejection);
                    const auto& intent = decision.intent;
                    RecoilReceipt receipt{intent.command_id, RecoilReceiptStatus::NOT_SENT, RecoilClock::now()};
                    bool backend_called = false;
                    RecoilDispatchRejection rejection = RecoilDispatchRejection::NONE;
                    RecoilTime context_checked_at{}, backend_called_at{}, backend_returned_at{};
                    switch (arbitration_rejection) {
                    case OutputArbiterRejection::LOCK_BUSY: rejection = RecoilDispatchRejection::ARBITER_LOCK_BUSY; break;
                    case OutputArbiterRejection::AUXILIARY_PENDING: rejection = RecoilDispatchRejection::ARBITER_AUXILIARY_PENDING; break;
                    case OutputArbiterRejection::OUTPUT_FAULT: rejection = RecoilDispatchRejection::ARBITER_OUTPUT_FAULT; break;
                    default: break;
                    }
                    if (lock.owns_lock()) {
                        const auto current = input();
                        const auto now = RecoilClock::now();
                        context_checked_at = now;
                        const MouseMoveCommand command{intent.dx_counts, intent.dy_counts};
                        const bool same = current.profile == before.profile && current.weapon_generation == before.weapon_generation &&
                            current.device_epoch == before.device_epoch && current.firing_started_at == before.firing_started_at;
                        if (!same) rejection = RecoilDispatchRejection::CONTEXT_CHANGED;
                        else if (!current.enabled) rejection = RecoilDispatchRejection::DISABLED;
                        else if (!current.held) rejection = RecoilDispatchRejection::NOT_HELD;
                        else if (!current.healthy) rejection = RecoilDispatchRejection::INPUT_UNHEALTHY;
                        else if (!current.permission) rejection = canceled.load() ? RecoilDispatchRejection::CANCELED :
                            stopping.load() ? RecoilDispatchRejection::STOPPING : RecoilDispatchRejection::PERMISSION_DENIED;
                        else if (!current.focused) rejection = RecoilDispatchRejection::NOT_FOCUSED;
                        else if (!current.profile_conditions_match) rejection = RecoilDispatchRejection::PROFILE_MISMATCH;
                        else if (now > intent.expires_at) rejection = RecoilDispatchRejection::EXPIRED;
                        else if (!ledger->permits(command, now)) rejection = RecoilDispatchRejection::BUDGET_EXCEEDED;
                        else if (canceled.load()) rejection = RecoilDispatchRejection::CANCELED;
                        else if (stopping.load()) rejection = RecoilDispatchRejection::STOPPING;
                        if (rejection == RecoilDispatchRejection::NONE) {
                            backend_called = true;
                            backend_called_at = RecoilClock::now();
                            auto output = mouse->move(command);
                            const auto returned_at = RecoilClock::now();
                            backend_returned_at = returned_at;
                            if(output.backend_completed_at==RecoilTime{} || output.backend_completed_at<intent.planned_at ||
                                output.backend_completed_at>returned_at) output.succeeded=false;
                            // 未知不能视作零后重试；同owner后续输出也失去可信账本。
                            const bool recorded=ledger->record(command, output, true);
                            receipt.status = output.succeeded&&recorded ? RecoilReceiptStatus::ACKNOWLEDGED : RecoilReceiptStatus::UNKNOWN;
                            receipt.completed_at = output.backend_completed_at;
                            if (receipt.status==RecoilReceiptStatus::UNKNOWN) arbiter->latch_output_fault();
                        }
                    }
                    record({intent,receipt,before.profile,backend_called,before.firing_started_at,
                        before_source==1?RecoilFiringSource::INPUT_ESTIMATED:
                        before_source==2?RecoilFiringSource::COMMAND_ESTIMATED:RecoilFiringSource::UNKNOWN,
                        rejection,sampled_at,arbitration_at,context_checked_at,backend_called_at,backend_returned_at,
                        before_firing_id,before_firing_uncertainty_ns});
                    controller.acknowledge(receipt, RecoilClock::now());
                }
                {
                    std::unique_lock lock(mutex);
                    state = controller.snapshot();
                    wake.wait_for(lock, std::chrono::milliseconds(2));
                }
            }
            controller.cancel(RecoilReason::CANCELED, RecoilClock::now());
            std::lock_guard lock(mutex); state = controller.snapshot();
        } catch (...) {
            arbiter->latch_output_fault();
            std::lock_guard lock(mutex); state.faulted = true; state.phase = RecoilPhase::FAULT;
            state.reason = RecoilReason::UNKNOWN_RECEIPT;
        }
    }
};
RecoilWorker::RecoilWorker(std::shared_ptr<IMouseController> mouse,
    std::shared_ptr<AutoStopOutputArbiter> arbiter, std::shared_ptr<MotionLedger> ledger,
    std::function<RecoilInput()> context, std::function<TriggerFiringSignal()> firing) : impl_(std::make_unique<Impl>()) {
    impl_->mouse = std::move(mouse); impl_->arbiter = std::move(arbiter); impl_->ledger = std::move(ledger);
    impl_->context = std::move(context); impl_->firing = std::move(firing);
}
RecoilWorker::~RecoilWorker() { stop(); }
bool RecoilWorker::start(const RecoilConfig& config) noexcept {
    try {
        if (impl_->thread.joinable() || !impl_->mouse || !impl_->arbiter || !impl_->ledger ||
            !impl_->context || !impl_->firing || impl_->mouse->left_button_cleanup_required() ||
            impl_->mouse->left_button_faulted() || config.hold_virtual_key < 0 || config.hold_virtual_key > 255) return false;
        impl_->config = config; impl_->stopping.store(false); impl_->canceled.store(false);
        impl_->thread = std::thread([this] { impl_->run(); }); return true;
    } catch (...) { return false; }
}
void RecoilWorker::cancel() noexcept { impl_->canceled.store(true); impl_->wake.notify_one(); }
void RecoilWorker::stop() noexcept {
    impl_->stopping.store(true); impl_->wake.notify_one(); if (impl_->thread.joinable()) impl_->thread.join();
}
RecoilSnapshot RecoilWorker::snapshot() const noexcept { std::lock_guard lock(impl_->mutex); return impl_->state; }
RecoilExecutionLog RecoilWorker::execution_log() const {
    std::lock_guard lock(impl_->mutex);
    RecoilExecutionLog result;result.dropped_count=impl_->records_dropped;
    result.records.reserve(impl_->records_count);
    for(std::size_t i=0;i<impl_->records_count;++i)
        result.records.push_back(impl_->records[(impl_->records_begin+i)%impl_->records.size()]);
    return result;
}
