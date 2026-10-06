#ifndef XEN_LINEUP_EXECUTION_INTERNAL_H
#define XEN_LINEUP_EXECUTION_INTERNAL_H
// 纯软件状态机；调用者串行调用。无设备工厂、IPC、后台线程或生产默认标定。
#include "lineup/action_internal.h"
#include <chrono>
#include <set>
#include <vector>
#include <cstdint>
#include <optional>
#include <string>
#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>
namespace lineup { using Clock = std::chrono::steady_clock; }
namespace lineup::detail {
struct ExecutionGeometry {
    int width = 0, height = 0, source_width = 0, source_height = 0, encoded_width = 0, encoded_height = 0;
    double roi_x = 0, roi_y = 0, scale_x = 0, scale_y = 0;
    bool mapping_verified = false;
    bool operator==(const ExecutionGeometry &) const = default;
};
struct ExecutionIdentity {
    std::string recipe_id, reference_id, source_id, session_id;
    std::uint64_t recipe_version = 0, selection_generation = 0;
    ExecutionGeometry geometry;
    bool operator==(const ExecutionIdentity &) const = default;
};
struct ExecutionObservation {
    ExecutionIdentity identity;
    std::uint64_t sequence = 0;
    Clock::time_point captured_at{};
    std::optional<Clock::time_point> source_at;
    std::optional<std::chrono::milliseconds> source_uncertainty;
    double error_x = 0, error_y = 0; // 相对实际准星的局部 ROI 像素误差。
    bool valid = false;
};
struct ExecutionCalibration {
    std::string id, source_id;
    ExecutionGeometry geometry;
    bool validated = false;
    double counts_per_local_pixel_x = 0, counts_per_local_pixel_y = 0;
    std::optional<std::chrono::milliseconds> observation_delay;
};
struct ExecutionPermit {
    bool gate_open = false, focused = false, armed = false, healthy = false, exclusive = false;
    bool input_state_valid = false;
    std::set<std::string> user_held;
    bool allowed() const noexcept { return gate_open && focused && armed && healthy && exclusive; }
};
enum class ExecutionDelivery { ACCEPTED, REJECTED, UNKNOWN };
struct ExecutionReceipt {
    ExecutionDelivery delivery = ExecutionDelivery::REJECTED;
    Clock::time_point completed_at{};
};
struct ExecutionSink {
    std::function<ExecutionReceipt(int, int)> move;
    std::function<ExecutionReceipt(bool)> left_button;
    std::function<ExecutionReceipt(const std::string &, bool)> control;
};
struct ExecutionCapabilities { bool relative_move = false, left_button = false, right_button = false, movement = false, jump = false; };
struct ExecutionLimits {
    int max_step_counts = 0, max_steps = 0;
    std::chrono::milliseconds max_duration{0}, max_frame_age{100};
    double tolerance_pixels = 0, divergence_slack_pixels = 0;
};
// action为完整schema1阶段组合；jump固定Space，不能映射滚轮。旧左键plan保留兼容。
struct ExecutionThrowPlan {
    std::string id, mode = "left_hold_release";
    bool validated = false;
    int hold_ms = 0;
    ExecutionIdentity identity;
    nlohmann::json action;
};
enum class ExecutionState { IDLE, BLOCKED, ALIGNING, ALIGNED, HOLDING, COMPLETED, CANCELLED, UNKNOWN };
struct ExecutionSnapshot {
    ExecutionState state = ExecutionState::IDLE;
    std::string reason, calibration_id, throw_plan_id;
    int steps = 0, aligned_frames = 0;
    bool waiting_feedback = false, cleanup_required = false, release_may_throw = false;
    std::vector<std::string> owned_controls;
    std::size_t scheduled_events = 0, completed_events = 0;
    bool production_available = false; // 核心不决定生产装配；Runtime桥独立报告真实可用性。
};
class ExecutionController {
    ExecutionSink sink_;
    ExecutionCapabilities capabilities_;
    ExecutionLimits limits_;
    ExecutionCalibration calibration_;
    ExecutionIdentity identity_;
    ExecutionSnapshot state_;
    Clock::time_point started_{}, feedback_after_{}, action_started_{}, action_until_{};
    Clock::time_point last_source_at_{}, last_capture_at_{};
    std::uint64_t last_sequence_ = 0;
    double previous_error_ = 0;
    bool throw_consumed_ = false;
    nlohmann::json events_ = nlohmann::json::array();
    std::set<std::string> owned_, cleanup_attempted_, action_controls_;
    std::size_t next_event_ = 0;
    // user_held 必须来自物理监视，不含本会话软件 held。任意相关人工输入都会改变配方语义。
    static bool physical_action_input_held(const ExecutionPermit &permit) {
        return permit.user_held.contains("button:left") || permit.user_held.contains("button:right") ||
               permit.user_held.contains("jump") || permit.user_held.contains("movement:forward") ||
               permit.user_held.contains("movement:back") || permit.user_held.contains("movement:left") ||
               permit.user_held.contains("movement:right");
    }
    bool identity_valid(const ExecutionIdentity &i) const {
        const auto &g = i.geometry;
        return !i.recipe_id.empty() && !i.reference_id.empty() && !i.source_id.empty() && !i.session_id.empty() &&
            i.recipe_version > 0 && i.selection_generation > 0 && g.width > 0 && g.height > 0 &&
            std::isfinite(g.roi_x) && std::isfinite(g.roi_y) && std::isfinite(g.scale_x) && std::isfinite(g.scale_y) &&
            g.scale_x > 0 && g.scale_y > 0;
    }
    bool fresh(const ExecutionObservation &o, Clock::time_point now) const {
        return o.valid && o.sequence > 0 && o.captured_at != Clock::time_point{} && o.source_at &&
            *o.source_at != Clock::time_point{} && o.source_uncertainty && o.source_uncertainty->count() >= 0 && *o.source_uncertainty <= limits_.max_frame_age &&
            o.captured_at <= now && now - o.captured_at <= limits_.max_frame_age &&
            *o.source_at <= now + *o.source_uncertainty &&
            now - *o.source_at + *o.source_uncertainty <= limits_.max_frame_age &&
            std::isfinite(o.error_x) && std::isfinite(o.error_y);
    }
    ExecutionReceipt deliver(const std::string &control, bool down, Clock::time_point now) noexcept {
        try {
            auto receipt = sink_.control ? sink_.control(control, down) :
                control == "button:left" && sink_.left_button ? sink_.left_button(down) : ExecutionReceipt{};
            if (receipt.delivery == ExecutionDelivery::ACCEPTED && (receipt.completed_at < now || receipt.completed_at == Clock::time_point{}))
                receipt.delivery = ExecutionDelivery::UNKNOWN;
            return receipt;
        } catch (...) { return {ExecutionDelivery::UNKNOWN, now}; }
    }
    void cleanup(Clock::time_point now, const std::string &reason, bool uncertain = false) {
        // 只释放本会话已发出/可能已发出的Down；不接管用户原本持有的输入。
        auto controls = owned_;
        for (const auto &control : controls) {
            if (cleanup_attempted_.contains(control)) continue;
            cleanup_attempted_.insert(control);
            if (control.starts_with("button:")) state_.release_may_throw = true;
            auto receipt = deliver(control, false, now);
            if (receipt.delivery == ExecutionDelivery::ACCEPTED) owned_.erase(control);
            else uncertain = true;
        }
        state_.cleanup_required = !owned_.empty();
        state_.state = uncertain || state_.state == ExecutionState::UNKNOWN || state_.cleanup_required
            ? ExecutionState::UNKNOWN : ExecutionState::CANCELLED;
        state_.reason = state_.state == ExecutionState::UNKNOWN ? "action_or_cleanup_unknown" : reason;
        state_.waiting_feedback = false;
    }
    void stop(Clock::time_point now, const std::string &reason) {
        state_.waiting_feedback = false;
        if (!owned_.empty()) cleanup(now, reason);
        else if (state_.state != ExecutionState::UNKNOWN) { state_.state = ExecutionState::CANCELLED; state_.reason = reason; }
    }
    void advance_action(const ExecutionPermit &permit, Clock::time_point now) {
        if (state_.state != ExecutionState::HOLDING) return;
        if (next_event_ >= events_.size()) {
            if (now >= action_until_) { state_.state = ExecutionState::COMPLETED; state_.reason = "sequence_accepted_effect_unknown"; }
            return;
        }
        auto due = action_started_ + std::chrono::milliseconds(events_[next_event_].at("at_ms").get<int>());
        if (now < due) return;
        const auto group_ms = events_[next_event_].at("at_ms").get<int>();
        auto group_end = next_event_;
        while (group_end < events_.size() && events_[group_end].at("at_ms") == group_ms) ++group_end;
        // 不将已错过的多阶段压成同一时刻突发发送；清理拥有项并报告中止。
        if (group_end < events_.size() && now >= action_started_ + std::chrono::milliseconds(events_[group_end].at("at_ms").get<int>())) {
            cleanup(now, "phase_deadline_missed"); return;
        }
        while (next_event_ < group_end) {
            const auto &event = events_[next_event_];
            const auto control = event.at("control").get<std::string>();
            const bool down = event.at("held").get<bool>();
            if (down && physical_action_input_held(permit)) { cleanup(now, "phase_physical_input_changed"); return; }
            if (!down && !owned_.contains(control)) { cleanup(now, "release_not_owned", true); return; }
            auto receipt = deliver(control, down, now);
            ++next_event_; state_.completed_events = next_event_;
            if (down && receipt.delivery != ExecutionDelivery::REJECTED) owned_.insert(control);
            if (!down) {
                if (control.starts_with("button:")) state_.release_may_throw = true;
                if (receipt.delivery == ExecutionDelivery::ACCEPTED) { owned_.erase(control); cleanup_attempted_.erase(control); }
                else cleanup_attempted_.insert(control); // 不重试未知Up，只清理其他拥有项。
            }
            state_.cleanup_required = !owned_.empty();
            if (receipt.delivery != ExecutionDelivery::ACCEPTED) {
                cleanup(now, "action_delivery_rejected", receipt.delivery == ExecutionDelivery::UNKNOWN); return;
            }
            // 同步后端返回可能已跨过下一阶段；不继续发送过时的同组Down。
            now = std::max(now, receipt.completed_at);
            if (now > started_ + limits_.max_duration || (group_end < events_.size() &&
                now >= action_started_ + std::chrono::milliseconds(events_[group_end].at("at_ms").get<int>()))) {
                cleanup(now, "backend_crossed_phase_deadline"); return;
            }
        }
        if (next_event_ == events_.size()) {
            state_.state = now >= action_until_ ? ExecutionState::COMPLETED : ExecutionState::HOLDING;
            state_.reason = now >= action_until_ ? "sequence_accepted_effect_unknown" : "final_released_phase_wait";
            state_.cleanup_required = false;
        }
    }
  public:
    ExecutionController(ExecutionSink sink, ExecutionCapabilities capabilities, ExecutionLimits limits)
        : sink_(std::move(sink)), capabilities_(capabilities), limits_(limits) {}
    ExecutionSnapshot snapshot() const { auto result = state_; result.owned_controls.assign(owned_.begin(), owned_.end()); return result; }
    bool locate_edge(const ExecutionObservation &o, const ExecutionCalibration &calibration,
                     const ExecutionPermit &permit, Clock::time_point now) {
        if (!owned_.empty() || state_.cleanup_required || state_.state == ExecutionState::ALIGNING || state_.state == ExecutionState::ALIGNED || state_.state == ExecutionState::HOLDING) return false;
        state_ = {}; state_.state = ExecutionState::BLOCKED;
        if (!permit.allowed()) { state_.reason = "permit_unavailable"; return false; }
        if (!identity_valid(o.identity) || !fresh(o, now)) { state_.reason = "observation_unavailable"; return false; }
        if (!calibration.validated || calibration.id.empty() || calibration.source_id != o.identity.source_id ||
            calibration.geometry != o.identity.geometry ||
            !std::isfinite(calibration.counts_per_local_pixel_x) || !std::isfinite(calibration.counts_per_local_pixel_y) ||
            calibration.counts_per_local_pixel_x == 0 || calibration.counts_per_local_pixel_y == 0 || !calibration.observation_delay || calibration.observation_delay->count() < 0 || *calibration.observation_delay > limits_.max_duration) {
            state_.reason = "calibration_unavailable_or_mismatch"; return false;
        }
        if (!capabilities_.relative_move || !sink_.move || limits_.max_step_counts <= 0 || limits_.max_steps <= 0 ||
            limits_.max_duration.count() <= 0 || limits_.max_frame_age.count() <= 0 ||
            !std::isfinite(limits_.tolerance_pixels) || limits_.tolerance_pixels <= 0 ||
            !std::isfinite(limits_.divergence_slack_pixels) || limits_.divergence_slack_pixels < 0) {
            state_.reason = "execution_limits_or_capability_unavailable"; return false;
        }
        calibration_ = calibration; identity_ = o.identity; started_ = now;
        last_sequence_ = 0; last_source_at_ = last_capture_at_ = {}; feedback_after_ = {};
        throw_consumed_ = false; previous_error_ = 0;
        events_ = nlohmann::json::array(); next_event_ = 0; cleanup_attempted_.clear(); action_controls_.clear();
        state_.calibration_id = calibration.id; state_.state = ExecutionState::ALIGNING;
        state_.reason = "waiting_fresh_observation";
        return true;
    }
    void observe(const ExecutionObservation &o, const ExecutionPermit &permit, Clock::time_point now) {
        if ((state_.state == ExecutionState::ALIGNING || state_.state == ExecutionState::ALIGNED || state_.state == ExecutionState::HOLDING) && o.identity != identity_) { stop(now, "identity_changed"); return; }
        tick(permit, now);
        if (state_.state != ExecutionState::ALIGNING && state_.state != ExecutionState::ALIGNED && state_.state != ExecutionState::HOLDING) return;
        if (o.identity != identity_) { stop(now, "identity_changed"); return; }
        if (state_.state == ExecutionState::HOLDING) return;
        if (!fresh(o, now)) { stop(now, "observation_lost_or_stale"); return; }
        if (o.sequence <= last_sequence_) return;
        if ((last_source_at_ != Clock::time_point{} && *o.source_at <= last_source_at_) ||
            (last_capture_at_ != Clock::time_point{} && o.captured_at <= last_capture_at_)) { stop(now, "frame_time_not_increasing"); return; }
        last_sequence_ = o.sequence; last_source_at_ = *o.source_at; last_capture_at_ = o.captured_at;
        if (state_.state == ExecutionState::HOLDING) return;
        if (state_.waiting_feedback && *o.source_at - *o.source_uncertainty <= feedback_after_) return;
        const double error = std::hypot(o.error_x, o.error_y);
        if (state_.waiting_feedback && error > previous_error_ + limits_.divergence_slack_pixels) { stop(now, "error_diverged"); return; }
        state_.waiting_feedback = false;
        if (error <= limits_.tolerance_pixels) {
            if (++state_.aligned_frames >= 3) { state_.state = ExecutionState::ALIGNED; state_.reason = "three_fresh_frames_aligned"; }
            return;
        }
        state_.aligned_frames = 0; state_.state = ExecutionState::ALIGNING;
        if (state_.steps >= limits_.max_steps) { stop(now, "step_limit"); return; }
        const auto quantize = [&](double e, double gain) {
            const double desired = e * gain;
            return static_cast<int>(std::lround(std::clamp(desired, -double(limits_.max_step_counts), double(limits_.max_step_counts))));
        };
        const int dx = quantize(o.error_x, calibration_.counts_per_local_pixel_x);
        const int dy = quantize(o.error_y, calibration_.counts_per_local_pixel_y);
        if (dx == 0 && dy == 0) { stop(now, "quantized_zero_outside_tolerance"); return; }
        ++state_.steps;
        ExecutionReceipt receipt;
        try { receipt = sink_.move(dx, dy); } catch (...) { receipt = {ExecutionDelivery::UNKNOWN, now}; }
        if (receipt.delivery != ExecutionDelivery::ACCEPTED || receipt.completed_at < now || receipt.completed_at == Clock::time_point{}) {
            state_.state = receipt.delivery == ExecutionDelivery::REJECTED ? ExecutionState::CANCELLED : ExecutionState::UNKNOWN;
            state_.reason = "move_not_confirmed"; return;
        }
        feedback_after_ = receipt.completed_at + *calibration_.observation_delay;
        previous_error_ = error; state_.waiting_feedback = true; state_.reason = "waiting_post_move_source_frame";
    }
    bool throw_edge(const ExecutionObservation &o, const ExecutionPermit &permit, const ExecutionThrowPlan &plan,
                    Clock::time_point now) {
        if (throw_consumed_) return false;
        tick(permit, now);
        if (state_.state != ExecutionState::ALIGNED || throw_consumed_) return false;
        if (o.identity != identity_ || !fresh(o, now) || o.sequence < last_sequence_ || *o.source_at < last_source_at_ || o.captured_at < last_capture_at_ ||
            std::hypot(o.error_x, o.error_y) > limits_.tolerance_pixels) { stop(now, "throw_observation_changed_or_stale"); return false; }
        if (plan.identity != identity_ || !plan.validated || plan.id.empty() || !permit.input_state_valid) {
            state_.reason = "throw_plan_identity_or_input_unknown"; return false;
        }
        auto action = plan.action;
        if (action.is_null()) {
            if (plan.mode != "left_hold_release" || plan.hold_ms < 1 || plan.hold_ms > 2000) { state_.reason = "throw_plan_unsupported"; return false; }
            action = {{"schema", 1}, {"type", "phases"}, {"phases", nlohmann::json::array({
                {{"buttons", {"left"}}, {"movement", nlohmann::json::array()}, {"jump", false}, {"duration_ms", plan.hold_ms}},
                {{"buttons", nlohmann::json::array()}, {"movement", nlohmann::json::array()}, {"jump", false}, {"duration_ms", 0}}})}};
        }
        auto planned = dry_run_action(action, {capabilities_.left_button, capabilities_.right_button, capabilities_.movement, capabilities_.jump});
        if (!planned.supported || planned.events.empty()) { state_.reason = "throw_plan_unsupported_or_incomplete"; return false; }
        int duration = 0;
        for (const auto &phase : action.at("phases")) duration += phase.at("duration_ms").get<int>();
        if (now + std::chrono::milliseconds(duration) > started_ + limits_.max_duration) { state_.reason = "throw_plan_exceeds_remaining_budget"; return false; }
        if (physical_action_input_held(permit)) { state_.reason = "related_physical_control_already_user_held"; return false; }
        std::set<std::string> controls;
        for (const auto &event : planned.events) {
            auto control = event.at("control").get<std::string>();
            controls.insert(control);
            if (!sink_.control && (control != "button:left" || !sink_.left_button)) { state_.reason = "control_sink_unavailable"; return false; }
        }
        throw_consumed_ = true; state_.throw_plan_id = plan.id; events_ = std::move(planned.events);
        action_controls_ = std::move(controls); action_started_ = now; action_until_ = now + std::chrono::milliseconds(duration); next_event_ = 0;
        state_.scheduled_events = events_.size(); state_.completed_events = 0;
        state_.state = ExecutionState::HOLDING; state_.reason = "action_sequence_active";
        advance_action(permit, now);
        return state_.state == ExecutionState::HOLDING || state_.state == ExecutionState::COMPLETED;
    }
    void tick(const ExecutionPermit &permit, Clock::time_point now) {
        const bool active = state_.state == ExecutionState::ALIGNING || state_.state == ExecutionState::ALIGNED || state_.state == ExecutionState::HOLDING;
        if (!active) return;
        if (!permit.allowed()) { stop(now, "permit_revoked"); return; }
        if (now < started_ || now - started_ > limits_.max_duration) { stop(now, "execution_timeout"); return; }
        if (state_.state == ExecutionState::HOLDING) {
            if (!permit.input_state_valid) { stop(now, "input_state_unknown"); return; }
            advance_action(permit, now); return;
        }
        if (last_capture_at_ != Clock::time_point{} && now - last_capture_at_ > limits_.max_frame_age) stop(now, "feedback_timeout");
    }
    void cancel(Clock::time_point now) { stop(now, "cancelled"); }
};
} // namespace lineup::detail
#endif
