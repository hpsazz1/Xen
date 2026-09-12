#include "recoil/recoil_worker.h"
#include "recoil/recoil_execution_events.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <array>
#include <limits>
#include <algorithm>

class RecoilWorker::Impl {
public:
    std::shared_ptr<IMouseController> mouse;
    std::shared_ptr<AutoStopOutputArbiter> arbiter;
    std::shared_ptr<MotionLedger> ledger;
    std::function<RecoilInput()> context;
    std::function<TriggerFiringSignal()> firing;
    RecoilConfig config;
    RecoilController controller;
    std::shared_ptr<const RecoilCalibrationPermit> calibration_permit;
    std::unique_ptr<RecoilCalibrationBudget> calibration_budget;
    RecoilCalibrationBudgetSnapshot calibration_state;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::thread thread;
    std::atomic<bool> stopping{false}, canceled{false};
    RecoilSnapshot state;
    std::array<RecoilExecutionRecord,2048> records{};
    std::size_t records_begin = 0, records_count = 0;
    std::uint64_t records_dropped = 0;
    std::array<RecoilExecutionEvent,2048> events{};
    std::size_t events_begin = 0, events_count = 0;
    std::uint64_t event_sequence = 0, events_dropped = 0, last_begun_session = 0;
    bool event_sequence_exhausted = false;
    std::optional<RecoilExecutionEvent> active_batch;
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
        if(calibration_budget) {
            if(result.healthy&&raw.virtual_keys[calibration_permit->manifest().cancel_virtual_key])
                calibration_budget->finish(RecoilCalibrationEnd::CANCELED);
            const bool within_budget=calibration_budget->check_time(RecoilClock::now());
            result.permission=result.permission&&!synthetic.confirmed_down&&calibration_permit->matches(result.profile)&&
                within_budget;
        }
        return result;
    }
    void publish_event(RecoilExecutionEvent event) {
        std::lock_guard lock(mutex);
        if(event_sequence==std::numeric_limits<std::uint64_t>::max()) {event_sequence_exhausted=true;return;}
        event.sequence=++event_sequence;
        if(events_count==events.size()) {
            events[events_begin]=std::move(event);events_begin=(events_begin+1)%events.size();
            if(events_dropped!=std::numeric_limits<std::uint64_t>::max())++events_dropped;
        } else {events[(events_begin+events_count)%events.size()]=std::move(event);++events_count;}
    }
    void end_batch(RecoilBatchEndReason reason) {
        if(!active_batch)return;
        auto event=*active_batch;event.kind=RecoilExecutionEventKind::END;
        event.event_at=RecoilClock::now();event.final=controller.snapshot();event.end_reason=reason;
        publish_event(std::move(event));active_batch.reset();
        if(calibration_budget) {
            const auto end=reason==RecoilBatchEndReason::EXHAUSTED ? RecoilCalibrationEnd::COMPLETED :
                (reason==RecoilBatchEndReason::UNKNOWN||reason==RecoilBatchEndReason::EXCEPTION) ? RecoilCalibrationEnd::UNKNOWN_RECEIPT :
                reason==RecoilBatchEndReason::NOT_SENT ? RecoilCalibrationEnd::NOT_SENT :
                reason==RecoilBatchEndReason::CONTEXT ? RecoilCalibrationEnd::CONTEXT : RecoilCalibrationEnd::CANCELED;
            calibration_budget->finish(end);
        }
    }
    void finish_if_ended() {
        if(!active_batch)return;
        const auto snapshot=controller.snapshot();
        if(snapshot.phase==RecoilPhase::FIRING||snapshot.phase==RecoilPhase::PENDING)return;
        auto reason=RecoilBatchEndReason::CONTEXT;
        switch(snapshot.reason) {
        case RecoilReason::EXHAUSTED: reason=RecoilBatchEndReason::EXHAUSTED;break;
        case RecoilReason::RELEASED: reason=RecoilBatchEndReason::RELEASED;break;
        case RecoilReason::CANCELED: reason=RecoilBatchEndReason::CANCELED;break;
        case RecoilReason::NOT_SENT: reason=RecoilBatchEndReason::NOT_SENT;break;
        case RecoilReason::UNKNOWN_RECEIPT: reason=RecoilBatchEndReason::UNKNOWN;break;
        case RecoilReason::LATE: reason=RecoilBatchEndReason::LATE;break;
        case RecoilReason::LIMIT: reason=RecoilBatchEndReason::LIMIT;break;
        default:break;
        }
        end_batch(reason);
    }
    bool begin_if_started(const RecoilInput& input) {
        const auto snapshot=controller.snapshot();
        if(!snapshot.session_id||snapshot.session_id==last_begun_session)return true;
        if(active_batch)end_batch(RecoilBatchEndReason::CONTEXT);
        last_begun_session=snapshot.session_id;
        RecoilExecutionEvent event;event.firing_id=snapshot.session_id;event.event_at=RecoilClock::now();
        event.firing_started_at=input.firing_started_at;event.profile=input.profile;
        event.weapon_generation=input.weapon_generation;event.device_epoch=input.device_epoch;
        event.firing_source=last_source==1 ? RecoilFiringSource::INPUT_ESTIMATED :
            last_source==2 ? RecoilFiringSource::COMMAND_ESTIMATED : RecoilFiringSource::UNKNOWN;
        active_batch=event;publish_event(std::move(event));
        if(calibration_budget&&!calibration_budget->begin_firing(RecoilClock::now())) {
            controller.cancel(RecoilReason::CANCELED,RecoilClock::now());finish_if_ended();return false;
        }
        return true;
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
                if (canceled.exchange(false)) {
                    if(calibration_budget)calibration_budget->finish(RecoilCalibrationEnd::CANCELED);
                    controller.cancel(RecoilReason::CANCELED, RecoilClock::now());finish_if_ended();
                }
                const auto sampled_at = RecoilClock::now();
                const auto before = input();
                const auto before_source = last_source;
                const auto before_firing_id = sampled_firing_id;
                const auto before_firing_uncertainty_ns = sampled_firing_uncertainty_ns;
                auto decision = controller.advance(before, RecoilClock::now());
                if(!begin_if_started(before))decision.has_intent=false;
                finish_if_ended();
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
                        else if (calibration_budget&&!calibration_budget->reserve(intent.dx_counts,intent.dy_counts,RecoilClock::now()))
                            rejection=RecoilDispatchRejection::CALIBRATION_BUDGET;
                        if (rejection == RecoilDispatchRejection::NONE) {
                            backend_called = true;
                            backend_called_at = RecoilClock::now();
                            auto output = mouse->move(command);
                            const auto returned_at = RecoilClock::now();
                            backend_returned_at = returned_at;
                            if(output.backend_completed_at==RecoilTime{} || output.backend_completed_at<backend_called_at ||
                                output.backend_completed_at>returned_at) output.succeeded=false;
                            // 未知不能视作零后重试；同owner后续输出也失去可信账本。
                            const bool recorded=ledger->record(command, output, true);
                            receipt.status = output.succeeded&&recorded ? RecoilReceiptStatus::ACKNOWLEDGED : RecoilReceiptStatus::UNKNOWN;
                            receipt.completed_at = output.backend_completed_at;
                            if (receipt.status==RecoilReceiptStatus::UNKNOWN) arbiter->latch_output_fault();
                        }
                    }
                    if(lock.owns_lock())lock.unlock();
                    RecoilExecutionRecord execution{intent,receipt,before.profile,backend_called,before.firing_started_at,
                        before_source==1?RecoilFiringSource::INPUT_ESTIMATED:
                        before_source==2?RecoilFiringSource::COMMAND_ESTIMATED:RecoilFiringSource::UNKNOWN,
                        rejection,sampled_at,arbitration_at,context_checked_at,backend_called_at,backend_returned_at,
                        before_firing_id,before_firing_uncertainty_ns};
                    record(execution);
                    if(active_batch) {
                        auto event=*active_batch;event.kind=RecoilExecutionEventKind::COMMAND;
                        event.event_at=RecoilClock::now();event.command=std::move(execution);publish_event(std::move(event));
                    }
                    controller.acknowledge(receipt, RecoilClock::now());
                    finish_if_ended();
                }
                {
                    std::unique_lock lock(mutex);
                    state = controller.snapshot();
                    if(calibration_budget)calibration_state=calibration_budget->snapshot();
                    wake.wait_for(lock, std::chrono::milliseconds(2));
                }
            }
            controller.cancel(RecoilReason::CANCELED, RecoilClock::now());
            end_batch(RecoilBatchEndReason::STOPPED);
            if(calibration_budget)calibration_budget->finish(RecoilCalibrationEnd::CANCELED);
            std::lock_guard lock(mutex); state = controller.snapshot();
            if(calibration_budget)calibration_state=calibration_budget->snapshot();
        } catch (...) {
            arbiter->latch_output_fault();
            try {end_batch(RecoilBatchEndReason::EXCEPTION);}catch(...){}
            if(calibration_budget)calibration_budget->finish(RecoilCalibrationEnd::UNKNOWN_RECEIPT);
            std::lock_guard lock(mutex); state.faulted = true; state.phase = RecoilPhase::FAULT;
            state.reason = RecoilReason::UNKNOWN_RECEIPT;
            if(calibration_budget)calibration_state=calibration_budget->snapshot();
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
    // 生产入口不能复用一次校准构造的Controller或permit。
    if(impl_->calibration_permit)return false;
    return start_impl(config);
}
bool RecoilWorker::start_impl(const RecoilConfig& config) noexcept {
    try {
        if (impl_->thread.joinable() || !impl_->mouse || !impl_->arbiter || !impl_->ledger ||
            !impl_->context || !impl_->firing || impl_->mouse->left_button_cleanup_required() ||
            impl_->mouse->left_button_faulted() || config.hold_virtual_key < 0 || config.hold_virtual_key > 255) return false;
        impl_->config = config; impl_->stopping.store(false); impl_->canceled.store(false);
        impl_->thread = std::thread([this] { impl_->run(); }); return true;
    } catch (...) { return false; }
}
bool RecoilWorker::start_calibration(const RecoilConfig& config,std::shared_ptr<const RecoilCalibrationPermit> permit) noexcept {
    try {
        if(impl_->thread.joinable()||impl_->calibration_permit||!permit||!config.enabled||config.mixed_aim||
            config.hold_virtual_key!=permit->manifest().hold_virtual_key||!impl_->ledger)return false;
        const auto& environment=permit->manifest().environment;
        if(config.game_build!=environment.game_build||config.conditions!=environment.conditions||
            config.input_path!=environment.input_path||config.sensitivity!=environment.sensitivity)return false;
        impl_->calibration_permit=std::move(permit);
        impl_->calibration_budget=std::make_unique<RecoilCalibrationBudget>(impl_->calibration_permit);
        if(!impl_->calibration_budget->check_time(RecoilClock::now()))return false;
        impl_->controller=RecoilController(impl_->calibration_permit);
        impl_->ledger->reset(impl_->calibration_permit->limits().rolling_window_counts,
            impl_->calibration_permit->limits().rolling_window_ms,RecoilClock::now());
        return start_impl(config);
    }catch(...){return false;}
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
RecoilEventSlice RecoilWorker::read_execution_events(std::uint64_t after,std::size_t maximum) const {
    std::lock_guard lock(impl_->mutex);RecoilEventSlice result;
    result.latest_sequence=impl_->event_sequence;result.dropped_total=impl_->events_dropped;
    result.sequence_exhausted=impl_->event_sequence_exhausted;
    if(!impl_->events_count)return result;
    result.oldest_available_sequence=impl_->events[impl_->events_begin].sequence;
    result.cursor_gap=after<result.oldest_available_sequence-1;
    maximum=std::min<std::size_t>(maximum,256);result.events.reserve(maximum);
    for(std::size_t i=0;i<impl_->events_count&&result.events.size()<maximum;++i) {
        const auto& event=impl_->events[(impl_->events_begin+i)%impl_->events.size()];
        if(event.sequence>after)result.events.push_back(event);
    }
    return result;
}
RecoilCalibrationBudgetSnapshot RecoilWorker::calibration_snapshot() const noexcept {
    std::lock_guard lock(impl_->mutex);return impl_->calibration_state;
}
