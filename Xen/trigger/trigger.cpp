#include "trigger/trigger.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
using Ms = std::chrono::milliseconds;
bool has(const std::vector<int>& ids, int id) noexcept {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}
bool percent(float value) noexcept { return std::isfinite(value) && value >= 1.0f && value <= 100.0f; }
TriggerRegion role(const TriggerConfig& config, int id) noexcept {
    if (has(config.head_class_ids, id)) return TriggerRegion::HEAD;
    if (has(config.person_class_ids, id)) return TriggerRegion::BODY;
    if (has(config.general_class_ids, id)) return TriggerRegion::GENERAL;
    return TriggerRegion::NONE;
}
bool valid_box(const Detection& box, const TriggerObservation& observation, float confidence) noexcept {
    return std::isfinite(box.x1) && std::isfinite(box.y1) && std::isfinite(box.x2) && std::isfinite(box.y2) &&
        std::isfinite(box.confidence) && box.confidence >= confidence && box.confidence <= 1.0f &&
        box.x1 > 0.0f && box.y1 > 0.0f && box.x2 < observation.roi_width && box.y2 < observation.roi_height &&
        box.x2 > box.x1 && box.y2 > box.y1;
}
bool contains(const Detection& body, const Detection& head) noexcept {
    return body.x1 <= head.x1 && body.y1 <= head.y1 && body.x2 >= head.x2 && body.y2 >= head.y2;
}
float iou(const Detection& a, const Detection& b) noexcept {
    const double intersection = std::max(0.0, static_cast<double>(std::min(a.x2, b.x2) - std::max(a.x1, b.x1))) *
        std::max(0.0, static_cast<double>(std::min(a.y2, b.y2) - std::max(a.y1, b.y1)));
    const double area = static_cast<double>(a.x2 - a.x1) * (a.y2 - a.y1) +
        static_cast<double>(b.x2 - b.x1) * (b.y2 - b.y1) - intersection;
    return area > 0.0 ? static_cast<float>(intersection / area) : 0.0f;
}
// 只有唯一双向包含时才把头域身份锚定到人体；几何候选仍分别计算。
std::size_t anchor_index(const TriggerConfig& config, const TriggerObservation& observation, std::size_t index) noexcept {
    if (role(config, observation.detections[index].class_id) != TriggerRegion::HEAD) return index;
    std::size_t match = index;
    unsigned count = 0;
    for (std::size_t i = 0; i < observation.detections.size(); ++i) {
        const auto& box = observation.detections[i];
        if (role(config, box.class_id) == TriggerRegion::BODY && valid_box(box, observation, config.min_confidence) &&
            contains(box, observation.detections[index])) { match = i; ++count; }
    }
    if (count != 1) return index;
    count = 0;
    for (const auto& box : observation.detections) {
        if (role(config, box.class_id) == TriggerRegion::HEAD && valid_box(box, observation, config.min_confidence) &&
            contains(observation.detections[match], box)) ++count;
    }
    return count == 1 ? match : index;
}
}

bool valid_trigger_config(const TriggerConfig& c) noexcept {
    if (c.hold_virtual_key < 0 || c.hold_virtual_key > 255 || c.hold_virtual_key == 1 ||
        !percent(c.head_width_percent) || !percent(c.head_height_percent) ||
        !percent(c.body_width_percent) || !percent(c.body_height_percent) ||
        !percent(c.general_width_percent) || !percent(c.general_height_percent) ||
        !std::isfinite(c.min_confidence) || c.min_confidence < 0.0f || c.min_confidence > 1.0f ||
        c.fire_delay_ms < 0 || c.fire_delay_ms > 1000 || c.shot_interval_ms < 1 || c.shot_interval_ms > 2000 ||
        c.press_duration_ms < 1 || c.press_duration_ms > 500 || c.max_hold_ms < 1 || c.max_hold_ms > 1000 ||
        c.max_observation_age_ms < 1 || c.max_observation_age_ms > 5000 ||
        (c.fire_mode != TriggerFireMode::SINGLE && c.fire_mode != TriggerFireMode::AUTOMATIC) ||
        (c.fire_mode == TriggerFireMode::SINGLE && c.shot_interval_ms < c.press_duration_ms)) return false;
    for (const auto* ids : {&c.person_class_ids, &c.head_class_ids, &c.general_class_ids}) {
        for (std::size_t i = 0; i < ids->size(); ++i) {
            if ((*ids)[i] < 0 || std::find(ids->begin(), ids->begin() + i, (*ids)[i]) != ids->begin() + i) return false;
        }
    }
    for (int id : c.person_class_ids) if (has(c.head_class_ids, id) || has(c.general_class_ids, id)) return false;
    for (int id : c.head_class_ids) if (has(c.general_class_ids, id)) return false;
    return true;
}

const char* TriggerReasonName(TriggerReason reason) noexcept {
    switch (reason) {
#define TRIGGER_REASON(value) case TriggerReason::value: return #value
        TRIGGER_REASON(NONE); TRIGGER_REASON(DISABLED); TRIGGER_REASON(INVALID_CONFIG); TRIGGER_REASON(WAIT_RELEASE);
        TRIGGER_REASON(PERMISSION); TRIGGER_REASON(INVALID_OBSERVATION); TRIGGER_REASON(TIMING_UNAVAILABLE);
        TRIGGER_REASON(STALE); TRIGGER_REASON(NO_CANDIDATE); TRIGGER_REASON(TARGET_CHANGED); TRIGGER_REASON(DELAY);
        TRIGGER_REASON(COOLDOWN); TRIGGER_REASON(WAIT_NEW_FRAME); TRIGGER_REASON(STOP_UNVERIFIED);
        TRIGGER_REASON(STOP_EXPIRED); TRIGGER_REASON(COMMAND_PENDING); TRIGGER_REASON(RELEASED);
        TRIGGER_REASON(UNKNOWN_RECEIPT); TRIGGER_REASON(CANCELED); TRIGGER_REASON(COUNTER_EXHAUSTED);
#undef TRIGGER_REASON
    }
    return "UNKNOWN";
}

bool TriggerController::configure(const TriggerConfig& config) noexcept {
    if (!valid_trigger_config(config) || state_.button_may_be_down || pending_ != TriggerButtonAction::NONE ||
        state_.stop_request_id != 0 || state_.faulted) return false;
    try { config_ = config; } catch (...) { config_valid_ = false; return false; }
    config_valid_ = true;
    candidate_valid_ = false;
    release_seen_ = false;
    state_.phase = config.enabled ? TriggerPhase::WAITING : TriggerPhase::DISABLED;
    state_.reason = config.enabled ? TriggerReason::WAIT_RELEASE : TriggerReason::DISABLED;
    return true;
}

TriggerDecision TriggerController::result(TriggerTime now) const noexcept {
    TriggerDecision decision;
    decision.snapshot = state_;
    auto deadline = [&](TriggerTime value) {
        if (value > now && (decision.next_deadline == TriggerTime{} || value < decision.next_deadline)) decision.next_deadline = value;
    };
    if (candidate_valid_) {
        deadline(observation_expires_);
        deadline(qualified_at_ + Ms(config_.fire_delay_ms));
    }
    if (state_.phase == TriggerPhase::HELD) deadline(held_until_);
    deadline(cooldown_until_);
    if (state_.stop_request_id) { deadline(stop_expires_); deadline(stop_release_deadline_); }
    return decision;
}

TriggerDecision TriggerController::release(TriggerReason reason, TriggerTime now) noexcept {
    candidate_valid_ = false;
    state_.region = TriggerRegion::NONE;
    state_.reason = reason;
    TriggerDecision decision;
    if (state_.button_may_be_down && pending_ != TriggerButtonAction::UP) {
        if (next_command_id_ == std::numeric_limits<std::uint64_t>::max()) {
            state_.faulted = true;
            state_.reason = TriggerReason::COUNTER_EXHAUSTED;
        } else {
            pending_ = TriggerButtonAction::UP;
            pending_at_ = now;
            state_.command_id = ++next_command_id_;
            decision.button_action = TriggerButtonAction::UP;
            decision.command_id = state_.command_id;
        }
    }
    if (state_.stop_request_id) {
        decision.stop_action = TriggerStopAction::CANCEL;
        decision.stop_request_id = state_.stop_request_id;
        state_.stop_request_id = 0;
    }
    state_.phase = state_.faulted ? TriggerPhase::FAULT : pending_ == TriggerButtonAction::UP ? TriggerPhase::UP_PENDING :
        config_.enabled ? TriggerPhase::WAITING : TriggerPhase::DISABLED;
    decision.snapshot = state_;
    return decision;
}

TriggerDecision TriggerController::cancel(TriggerReason reason, TriggerTime now) noexcept {
    release_seen_ = false;
    last_now_ = std::max(last_now_, now);
    return release(reason, now);
}

bool TriggerController::select_candidate(const TriggerObservation& observation, TriggerTime now) noexcept {
    std::size_t selected = observation.detections.size();
    float best_margin = -1.0f, best_confidence = -1.0f;
    for (std::size_t i = 0; i < observation.detections.size(); ++i) {
        const auto& box = observation.detections[i];
        if (!valid_box(box, observation, config_.min_confidence)) continue;
        const auto region = role(config_, box.class_id);
        if (region == TriggerRegion::NONE) continue;
        const float width = region == TriggerRegion::HEAD ? config_.head_width_percent :
            region == TriggerRegion::BODY ? config_.body_width_percent : config_.general_width_percent;
        const float height = region == TriggerRegion::HEAD ? config_.head_height_percent :
            region == TriggerRegion::BODY ? config_.body_height_percent : config_.general_height_percent;
        const double dx = (observation.center_x - (static_cast<double>(box.x1) + box.x2) / 2.0) /
            ((static_cast<double>(box.x2) - box.x1) * width / 200.0);
        const double dy = (observation.center_y - (static_cast<double>(box.y1) + box.y2) / 2.0) /
            ((static_cast<double>(box.y2) - box.y1) * height / 200.0);
        const double square = dx * dx + dy * dy;
        if (square > 1.0) continue;
        const float margin = static_cast<float>(1.0 - square);
        if (margin > best_margin || (margin == best_margin && box.confidence > best_confidence)) {
            selected = i; best_margin = margin; best_confidence = box.confidence;
        }
    }
    if (selected == observation.detections.size()) return false;
    const std::size_t selected_anchor = anchor_index(config_, observation, selected);
    const auto& next_anchor = observation.detections[selected_anchor];
    const auto next_region = role(config_, next_anchor.class_id);
    unsigned matches = 0;
    // 成对头框折叠到人体索引，只计一次；关联检查包含未覆盖准星的有效检测。
    if (candidate_valid_) {
        for (std::size_t i = 0; i < observation.detections.size(); ++i) {
            const auto& box = observation.detections[i];
            if (!valid_box(box, observation, config_.min_confidence) || anchor_index(config_, observation, i) != i) continue;
            if (role(config_, box.class_id) == anchor_region_ && iou(anchor_, box) > 0.5f) ++matches;
        }
    }
    const bool same = candidate_valid_ && matches == 1 && next_region == anchor_region_ && iou(anchor_, next_anchor) > 0.5f;
    if (!same) {
        if (next_candidate_id_ == std::numeric_limits<std::uint64_t>::max()) return false;
        state_.candidate_id = ++next_candidate_id_;
        qualified_at_ = now;
        state_.reason = TriggerReason::TARGET_CHANGED;
    }
    anchor_ = next_anchor;
    anchor_region_ = next_region;
    state_.region = role(config_, observation.detections[selected].class_id);
    state_.normalized_margin = best_margin;
    candidate_valid_ = true;
    return true;
}

TriggerDecision TriggerController::observe(const TriggerObservation& observation, const TriggerPermit& permit, TriggerTime now) noexcept {
    if (now < last_now_) return cancel(TriggerReason::INVALID_OBSERVATION, last_now_);
    last_now_ = now;
    const bool same_epoch = observation.epoch == state_.observation_epoch;
    if (!observation.valid || observation.epoch == 0 || observation.sequence == 0 ||
        (same_epoch && observation.sequence <= state_.observation_sequence) ||
        observation.roi_width <= 0 || observation.roi_height <= 0 ||
        !std::isfinite(observation.center_x) || !std::isfinite(observation.center_y) ||
        observation.center_x < 0 || observation.center_y < 0 || observation.center_x >= observation.roi_width ||
        observation.center_y >= observation.roi_height) return release(TriggerReason::INVALID_OBSERVATION, now);
    if (!observation.timing_valid || observation.observed_at == TriggerTime{} || observation.uncertainty.count() < 0 ||
        observation.observed_at > now) return release(TriggerReason::TIMING_UNAVAILABLE, now);
    const auto max_age = std::chrono::duration_cast<std::chrono::nanoseconds>(Ms(config_.max_observation_age_ms));
    if (observation.uncertainty >= max_age || now - observation.observed_at >= max_age - observation.uncertainty)
        return release(TriggerReason::STALE, now);
    const bool geometry_changed = roi_width_ != observation.roi_width || roi_height_ != observation.roi_height ||
        center_x_ != observation.center_x || center_y_ != observation.center_y;
    if (!same_epoch && state_.observation_epoch != 0) release_seen_ = false;
    const auto previous_id = state_.candidate_id;
    const bool had_candidate = candidate_valid_;
    if (!same_epoch || geometry_changed || now >= observation_expires_) candidate_valid_ = false;
    state_.observation_epoch = observation.epoch;
    state_.observation_sequence = observation.sequence;
    center_x_ = observation.center_x; center_y_ = observation.center_y;
    roi_width_ = observation.roi_width; roi_height_ = observation.roi_height;
    observation_expires_ = observation.observed_at + (max_age - observation.uncertainty);
    if (!select_candidate(observation, now)) return release(TriggerReason::NO_CANDIDATE, now);
    if (had_candidate && state_.candidate_id != previous_id && (state_.button_may_be_down || state_.stop_request_id))
        return release(TriggerReason::TARGET_CHANGED, now);
    return tick(permit, now);
}

TriggerDecision TriggerController::tick(const TriggerPermit& permit, TriggerTime now) noexcept {
    if (now < last_now_) return cancel(TriggerReason::INVALID_OBSERVATION, last_now_);
    last_now_ = now;
    if (!config_valid_) return cancel(TriggerReason::INVALID_CONFIG, now);
    if (!config_.enabled || !permit.enabled) return cancel(TriggerReason::DISABLED, now);
    if (!permit.healthy || !permit.focused || !permit.armed || permit.physical_left_down)
        return cancel(TriggerReason::PERMISSION, now);
    if (!permit.held) {
        release_seen_ = true;
        return release(TriggerReason::RELEASED, now);
    }
    if (state_.faulted) return result(now);
    if (!release_seen_) return release(TriggerReason::WAIT_RELEASE, now);
    if (!candidate_valid_) return release(TriggerReason::NO_CANDIDATE, now);
    if (now >= observation_expires_) return release(TriggerReason::STALE, now);
    if (config_.require_stop && state_.stop_request_id) {
        stop_expires_ = permit.stop_expires_at;
        stop_release_deadline_ = permit.stop_release_deadline;
        const bool qualified = permit.stop_observed_qualified && permit.stop_request_id == state_.stop_request_id &&
            permit.stop_observation_epoch == state_.observation_epoch && stop_expires_ > now && stop_release_deadline_ > now;
        if (!qualified && state_.button_may_be_down) return release(TriggerReason::STOP_EXPIRED, now);
        if (stop_release_deadline_ != TriggerTime{} && stop_release_deadline_ <= now)
            return release(TriggerReason::STOP_EXPIRED, now);
    }
    if (pending_ != TriggerButtonAction::NONE) { state_.reason = TriggerReason::COMMAND_PENDING; return result(now); }
    if (state_.button_may_be_down) {
        if (now >= held_until_) {
            const auto region = state_.region;
            auto decision = release(TriggerReason::RELEASED, now);
            // 正常点射结束不意味着准星离域，连续驻留与冷却互相独立。
            candidate_valid_ = true;
            state_.region = region;
            decision.snapshot = state_;
            return decision;
        }
        return result(now);
    }
    if (config_.require_stop && state_.stop_request_id == 0) {
        if (permit.next_stop_request_id == 0 || permit.next_stop_request_id <= last_stop_id_) {
            state_.phase = TriggerPhase::WAIT_STOP; state_.reason = TriggerReason::STOP_UNVERIFIED; return result(now);
        }
        state_.stop_request_id = permit.next_stop_request_id;
        last_stop_id_ = state_.stop_request_id;
        state_.phase = TriggerPhase::WAIT_STOP; state_.reason = TriggerReason::STOP_UNVERIFIED;
        auto decision = result(now);
        decision.stop_action = TriggerStopAction::REQUEST;
        decision.stop_request_id = state_.stop_request_id;
        return decision;
    }
    if (now < qualified_at_ + Ms(config_.fire_delay_ms)) {
        state_.phase = TriggerPhase::QUALIFYING; state_.reason = TriggerReason::DELAY; return result(now);
    }
    if (config_.require_stop && !(permit.stop_observed_qualified && permit.stop_request_id == state_.stop_request_id &&
        permit.stop_observation_epoch == state_.observation_epoch && permit.stop_expires_at > now && permit.stop_release_deadline > now)) {
        state_.phase = TriggerPhase::WAIT_STOP; state_.reason = TriggerReason::STOP_UNVERIFIED; return result(now);
    }
    if (now < cooldown_until_) { state_.phase = TriggerPhase::COOLDOWN; state_.reason = TriggerReason::COOLDOWN; return result(now); }
    if (state_.observation_epoch == last_down_epoch_ && state_.observation_sequence <= last_down_sequence_) {
        state_.reason = TriggerReason::WAIT_NEW_FRAME; return result(now);
    }
    if (next_command_id_ == std::numeric_limits<std::uint64_t>::max()) {
        state_.faulted = true; state_.phase = TriggerPhase::FAULT; state_.reason = TriggerReason::COUNTER_EXHAUSTED; return result(now);
    }
    pending_ = TriggerButtonAction::DOWN;
    unconfirmed_down_ = true;
    pending_at_ = now;
    state_.command_id = ++next_command_id_;
    state_.button_may_be_down = true;
    state_.phase = TriggerPhase::DOWN_PENDING;
    state_.reason = TriggerReason::NONE;
    last_down_epoch_ = state_.observation_epoch; last_down_sequence_ = state_.observation_sequence;
    auto decision = result(now);
    decision.button_action = TriggerButtonAction::DOWN; decision.command_id = state_.command_id;
    return decision;
}

TriggerDecision TriggerController::acknowledge(const TriggerReceipt& receipt, TriggerTime now) noexcept {
    if (pending_ == TriggerButtonAction::NONE || receipt.command_id != state_.command_id || receipt.action != pending_) return result(now);
    const bool valid_time = now >= last_now_ && receipt.completed_at >= pending_at_ && receipt.completed_at <= now;
    last_now_ = std::max(last_now_, now);
    if (!valid_time || receipt.status == TriggerReceiptStatus::UNKNOWN) {
        state_.faulted = true;
        state_.phase = TriggerPhase::FAULT;
        state_.reason = TriggerReason::UNKNOWN_RECEIPT;
        if (pending_ == TriggerButtonAction::DOWN) return release(TriggerReason::UNKNOWN_RECEIPT, now);
        return result(now);
    }
    const auto action = pending_;
    pending_ = TriggerButtonAction::NONE;
    if (receipt.status == TriggerReceiptStatus::NOT_SENT) {
        if (action == TriggerButtonAction::DOWN) { state_.button_may_be_down = false; unconfirmed_down_ = false; }
        else {
            // UP未发出仍保留同一清理命令，后续有界幂等重试的ACK必须能消债。
            pending_ = TriggerButtonAction::UP;
            state_.faulted = true; state_.phase = TriggerPhase::FAULT;
            state_.reason = TriggerReason::UNKNOWN_RECEIPT; return result(now);
        }
        return release(TriggerReason::CANCELED, now);
    }
    if (action == TriggerButtonAction::DOWN) {
        unconfirmed_down_ = false;
        state_.phase = TriggerPhase::HELD;
        cooldown_until_ = receipt.completed_at + Ms(config_.shot_interval_ms);
        held_until_ = receipt.completed_at + Ms(config_.fire_mode == TriggerFireMode::SINGLE ? config_.press_duration_ms : config_.max_hold_ms);
        if (now >= observation_expires_) return release(TriggerReason::STALE, now);
        if (config_.require_stop && (now >= stop_expires_ || now >= stop_release_deadline_))
            return release(TriggerReason::STOP_EXPIRED, now);
        if (now >= held_until_) return release(TriggerReason::RELEASED, now);
    } else {
        // 取消抢在 down 回执之前时，不能把可能已开火的事务当成没有冷却。
        // up 完成是可证明的保守参考；过期 down 回执不会推进新事务。
        if (unconfirmed_down_) cooldown_until_ = std::max(cooldown_until_, receipt.completed_at + Ms(config_.shot_interval_ms));
        unconfirmed_down_ = false;
        state_.button_may_be_down = false;
        state_.phase = state_.faulted ? TriggerPhase::FAULT : TriggerPhase::COOLDOWN;
    }
    return result(now);
}
