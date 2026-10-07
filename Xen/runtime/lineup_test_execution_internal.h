#ifndef XEN_RUNTIME_LINEUP_TEST_EXECUTION_INTERNAL_H
#define XEN_RUNTIME_LINEUP_TEST_EXECUTION_INTERNAL_H
#include "lineup/control_ipc.h"

namespace runtime::detail {
// 独立测试入口的单次意图。调用者串行访问；设备输出仍全部经过现有 ExecutionSession。
class LineupTestExecution {
    using Clock = lineup::Clock;
    using State = lineup::detail::ExecutionState;
    enum class Phase { IDLE, AWAITING_LOCATE, BOUND, ALIGNING, DISPATCHED };
    Phase phase_ = Phase::IDLE;
    std::uint64_t trigger_sequence_ = 0, epoch_ = 0, baseline_message_ = 0;
    std::uint64_t baseline_locate_ = 0, locate_sequence_ = 0, reference_version_ = 0;
    bool baseline_available_ = false;
    Clock::time_point started_{}, deadline_{};
    lineup::detail::ExecutionIdentity identity_;
    nlohmann::json action_;
    const char *reason_ = "";

    bool same_binding(const lineup::control::Snapshot &s) const {
        return s.locate_sequence == locate_sequence_ && s.request.trigger_sequence == trigger_sequence_ &&
            s.request.observation.identity == identity_ && s.request.reference_version == reference_version_ &&
            s.request.throw_action == action_;
    }
public:
    static constexpr auto kLifetime = std::chrono::milliseconds(4000);
    bool pending() const noexcept {
        return phase_ == Phase::AWAITING_LOCATE || phase_ == Phase::BOUND || phase_ == Phase::ALIGNING;
    }
    bool owns_trigger(std::uint64_t sequence) const noexcept {
        return sequence != 0 && sequence == trigger_sequence_;
    }
    bool rejects_trigger(std::uint64_t sequence) const noexcept {
        return phase_ == Phase::IDLE && owns_trigger(sequence);
    }
    bool locate_only_aligned(std::uint64_t sequence, State state) const noexcept {
        return state == State::ALIGNED && !pending() && sequence != 0 && !owns_trigger(sequence);
    }
    const char *reason() const noexcept { return reason_; }
    void cancel(const char *reason) noexcept {
        // 保留已见序号，拒绝取消后迟到的同一定位；下一次新按键必须取得新序号。
        phase_ = Phase::IDLE;
        reason_ = reason;
    }
    bool locate_only(State state) noexcept {
        // F8 只取消尚未开始的待投意图；执行持键时拒绝，不提前释放出道具。
        if (phase_ == Phase::DISPATCHED || state == State::HOLDING || state == State::UNKNOWN) return false;
        cancel("test_locate_only_requested");
        return true;
    }
    bool begin(std::uint64_t sequence, const lineup::control::Snapshot &s, State state, Clock::time_point now) noexcept {
        if (phase_ != Phase::IDLE || state == State::HOLDING || state == State::UNKNOWN ||
            sequence == 0 || sequence <= trigger_sequence_ || !s.connected || !s.connection_epoch) return false;
        trigger_sequence_ = sequence; epoch_ = s.connection_epoch;
        baseline_message_ = s.message_sequence; baseline_locate_ = s.locate_sequence;
        baseline_available_ = s.available;
        started_ = now; deadline_ = now + kLifetime;
        phase_ = Phase::AWAITING_LOCATE; reason_ = "test_waiting_matching_locate";
        return true;
    }
    void track(const lineup::control::Snapshot &s, Clock::time_point now) {
        if (!pending()) return;
        if (now < started_ || now >= deadline_) { cancel("test_execution_timeout"); return; }
        if (!s.connected || s.connection_epoch != epoch_) { cancel("test_control_reconnected_or_disconnected"); return; }
        if (phase_ == Phase::AWAITING_LOCATE && s.message_sequence <= baseline_message_) {
            if (baseline_available_ && (!s.available || s.valid_until <= now)) cancel("test_previous_control_revoked");
            return;
        }
        if (!s.available || s.valid_until <= now || s.request.mode == lineup::control::Mode::CANCEL) {
            cancel("test_locate_cancelled_or_expired"); return;
        }
        if (phase_ == Phase::AWAITING_LOCATE) {
            // F9 发出前已在途的旧观察不能绑定新意图，也不能延长其四秒有效期。
            if (!s.locate_sequence || s.locate_sequence == baseline_locate_) return;
            if (s.request.trigger_sequence != trigger_sequence_) { cancel("test_locate_trigger_mismatch"); return; }
            const auto design = lineup::detail::inspect_action(s.request.throw_action);
            if (!design.valid || !design.configured || !design.timing_complete) {
                cancel("test_throw_action_incomplete"); return;
            }
            identity_ = s.request.observation.identity; reference_version_ = s.request.reference_version;
            action_ = s.request.throw_action; locate_sequence_ = s.locate_sequence;
            phase_ = Phase::BOUND; reason_ = "test_matching_locate_bound";
        } else if (!same_binding(s)) { cancel("test_locate_binding_changed"); return; }
        if (!s.request.observation.valid) cancel("test_observation_invalid");
    }
    void locate_started(const lineup::control::Snapshot &s, bool accepted, Clock::time_point now) {
        track(s, now);
        if (!owns_trigger(s.request.trigger_sequence) || !pending()) return;
        if (!accepted || phase_ != Phase::BOUND || !same_binding(s)) { cancel("test_locate_rejected"); return; }
        phase_ = Phase::ALIGNING; reason_ = "test_waiting_alignment";
    }
    bool take_throw(const lineup::control::Snapshot &s, State state, Clock::time_point now) {
        track(s, now);
        if (phase_ != Phase::ALIGNING) return false;
        if (state != State::ALIGNING && state != State::ALIGNED) { cancel("test_alignment_failed"); return false; }
        if (state != State::ALIGNED) return false;
        phase_ = Phase::DISPATCHED; reason_ = "test_throw_dispatched_once";
        return true;
    }
    void settle(State state) {
        if (phase_ == Phase::DISPATCHED && state != State::HOLDING) cancel("test_throw_attempt_finished");
        else if (phase_ == Phase::ALIGNING && state != State::ALIGNING && state != State::ALIGNED)
            cancel("test_alignment_failed");
    }
};
}
#endif
