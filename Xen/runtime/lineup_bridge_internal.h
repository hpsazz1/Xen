#ifndef XEN_RUNTIME_LINEUP_BRIDGE_INTERNAL_H
#define XEN_RUNTIME_LINEUP_BRIDGE_INTERNAL_H
#include "lineup/execution_internal.h"
#include "mouse/mouse.h"
#include "auto_stop/auto_stop_worker.h"
#include <functional>
#include <memory>

namespace runtime::detail {
// 不创建设备。调用者提供唯一 owner、同一仲裁器及实时公共许可；方法由会话线程串行调用。
class LineupExecutionSession {
    using Clock = lineup::Clock;
    using Permit = lineup::detail::ExecutionPermit;
    using Receipt = lineup::detail::ExecutionReceipt;
    using Delivery = lineup::detail::ExecutionDelivery;
    using State = lineup::detail::ExecutionState;
    std::shared_ptr<IMouseController> mouse_;
    std::shared_ptr<AutoStopOutputArbiter> arbiter_;
    std::function<Permit()> permit_;
    std::function<void(bool)> pause_;
    std::function<bool()> quiescent_;
    std::function<void()> fault_;
    std::unique_lock<std::timed_mutex> owner_lock_;
    std::unique_ptr<lineup::detail::ExecutionController> controller_;
    std::uint8_t movement_mask_ = 0;
    bool active_ = false;

    Receipt button_receipt(const ButtonReceipt &r) const {
        return {r.disposition == ButtonDisposition::ACKNOWLEDGED ? Delivery::ACCEPTED :
                (r.disposition == ButtonDisposition::APPLICATION_UNKNOWN || r.datagram_sent ? Delivery::UNKNOWN : Delivery::REJECTED),
                r.backend_completed_at};
    }
    Receipt keyboard_receipt(const KeyboardReceipt &r) const {
        return {r.disposition == KeyboardDisposition::ACKNOWLEDGED ? Delivery::ACCEPTED :
                (r.disposition == KeyboardDisposition::APPLICATION_UNKNOWN || r.datagram_sent ? Delivery::UNKNOWN : Delivery::REJECTED),
                r.backend_completed_at};
    }
    Receipt move(int dx, int dy) {
        if (!owner_lock_.owns_lock() || !permit().allowed()) return {};
        const auto r = mouse_->move({dx, dy});
        return {r.succeeded && r.protocol_ack_received ? Delivery::ACCEPTED :
                (r.protocol_ack_received || r.backend_completed_at != Clock::time_point{} ? Delivery::UNKNOWN : Delivery::REJECTED),
                r.backend_completed_at};
    }
    Receipt control(const std::string &name, bool down) {
        // UP 只由 core 的 owned 集合清理；失焦/End 后仍必须允许归还本会话债务。
        if (!owner_lock_.owns_lock() || (down && !permit().allowed())) return {};
        if (name == "button:left") return button_receipt(mouse_->set_left_button(down));
        if (name == "button:right") return button_receipt(mouse_->set_right_button(down));
        if (name == "jump") return keyboard_receipt(mouse_->set_space_key(down));
        std::uint8_t bit = name == "movement:forward" ? 1 : name == "movement:left" ? 2 :
                           name == "movement:back" ? 4 : name == "movement:right" ? 8 : 0;
        if (!bit) return {};
        const auto next = static_cast<std::uint8_t>(down ? movement_mask_ | bit : movement_mask_ & ~bit);
        const auto result = keyboard_receipt(mouse_->set_wasd_keyboard(next));
        if (result.delivery != Delivery::REJECTED) movement_mask_ = next;
        return result;
    }
    void settle() {
        const auto snapshot = controller_->snapshot();
        const bool running = snapshot.state == State::ALIGNING || snapshot.state == State::ALIGNED || snapshot.state == State::HOLDING;
        if (snapshot.state == State::UNKNOWN || (!running && snapshot.cleanup_required)) {
            // 未知回执不能让竞争输出自动恢复；公共急停和本活动锁存直到会话结束。
            fault_();
            return;
        }
        if (snapshot.state == State::IDLE || snapshot.state == State::BLOCKED ||
            snapshot.state == State::COMPLETED || snapshot.state == State::CANCELLED) release();
    }
    void release() {
        if (owner_lock_.owns_lock()) owner_lock_.unlock();
        if (active_) { active_ = false; pause_(false); }
    }
public:
    LineupExecutionSession(std::shared_ptr<IMouseController> mouse,
            std::shared_ptr<AutoStopOutputArbiter> arbiter,
            std::function<Permit()> permit, std::function<void(bool)> pause,
            std::function<bool()> quiescent, std::function<void()> fault,
            lineup::detail::ExecutionLimits limits)
        : mouse_(std::move(mouse)), arbiter_(std::move(arbiter)), permit_(std::move(permit)),
          pause_(std::move(pause)), quiescent_(std::move(quiescent)), fault_(std::move(fault)) {
        lineup::detail::ExecutionSink sink;
        sink.move = [this](int x, int y) { return move(x,y); };
        sink.left_button = [this](bool down) { return control("button:left", down); };
        sink.control = [this](const std::string &name, bool down) { return control(name,down); };
        const bool combo = mouse_ && mouse_->supports_lineup_inputs();
        controller_ = std::make_unique<lineup::detail::ExecutionController>(std::move(sink),
            lineup::detail::ExecutionCapabilities{bool(mouse_), mouse_ && mouse_->supports_left_button(), combo,
                                                 mouse_ && mouse_->supports_wasd_keyboard(), combo}, limits);
    }
    ~LineupExecutionSession() { try { cancel(Clock::now()); } catch (...) {} }
    Permit permit() const {
        auto result = permit_();
        result.exclusive = result.exclusive && owner_lock_.owns_lock();
        return result;
    }
    bool reserve() {
        if (!active_) { active_ = true; pause_(true); }
        if (owner_lock_.owns_lock()) return true;
        if (!quiescent_()) return false;
        if (!owner_lock_.owns_lock()) owner_lock_ = arbiter_->try_enter_lineup();
        return owner_lock_.owns_lock();
    }
    bool locate(const lineup::detail::ExecutionObservation &observation,
                const lineup::detail::ExecutionCalibration &calibration, Clock::time_point now) {
        if (!reserve()) return false;
        const bool ok = controller_->locate_edge(observation, calibration, permit(), now);
        settle(); return ok;
    }
    void observe(const lineup::detail::ExecutionObservation &observation, Clock::time_point now) {
        controller_->observe(observation, permit(), now); settle();
    }
    bool throw_edge(const lineup::detail::ExecutionObservation &observation,
                    const lineup::detail::ExecutionThrowPlan &plan, Clock::time_point now) {
        const bool ok = controller_->throw_edge(observation, permit(), plan, now);
        settle(); return ok;
    }
    void tick(Clock::time_point now) { controller_->tick(permit(), now); settle(); }
    void cancel(Clock::time_point now) { controller_->cancel(now); settle(); }
    auto snapshot() const { return controller_->snapshot(); }
    bool active() const noexcept { return active_; }
};
}
#endif
