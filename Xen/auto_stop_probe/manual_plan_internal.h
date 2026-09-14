#ifndef AUTO_STOP_MANUAL_PLAN_INTERNAL_H
#define AUTO_STOP_MANUAL_PLAN_INTERNAL_H
#include "auto_stop_probe/counterpulse_internal.h"
#include "input_training/input_training.h"
#include <array>
#include <cmath>
#include <map>
#include <optional>

namespace auto_stop_probe_detail {
// 纯接收域分析；候选只供已有人工入口复测，不调用任何输出接口。
class ManualPlanBuilder {
    struct Edge { std::uint8_t mask; std::int64_t time; std::uint64_t sequence; };
    struct Sample {
        double move = 0, counter_delay = 0, counter_hold = 0, after = 0, hold = 0;
        std::optional<double> delay;
        std::string unsupported_reason;
        Json source;
    };
    struct Group { std::string mode; unsigned direction = 2; std::vector<Sample> samples; };
    std::map<std::string, Group> groups_;
    std::map<std::string, std::uint64_t> rejected_;
    std::vector<Edge> edges_;
    input_training::Event previous_{};
    bool have_ = false, active_ = false, finished_ = false;
    std::optional<std::int64_t> down_, last_up_;
    std::uint64_t down_sequence_ = 0, last_up_sequence_ = 0;
    std::string issue_;
    std::uint64_t events_ = 0, holds_ = 0, unsupported_ = 0;
    static double ms(std::int64_t a, std::int64_t b) { return static_cast<double>(a-b)/1000000.0; }
    void reject(const std::string& reason) { ++unsupported_; ++rejected_[reason]; }
    void reset_cycle() { edges_.clear(); issue_.clear(); down_.reset(); active_ = false; }
    void complete(const input_training::Event& event) {
        const auto up = event.received_at_ns;
        ++holds_;
        if (issue_ == "ATOMIC_EDGE_AMBIGUOUS" || issue_ == "MISSING_INITIAL_NEUTRAL" || issue_ == "WS_OR_DIAGONAL_UNSUPPORTED") {
            reject(issue_); return;
        }
        if (!down_) { reject("MISSING_FIRE_DOWN"); return; }
        Sample s; s.hold = ms(up, *down_); s.unsupported_reason = issue_;
        std::string mode; unsigned direction = 2;
        if (edges_.empty()) {
            mode = "stationary";
            if (last_up_) s.delay = ms(*down_, *last_up_);
        } else if ((edges_.size() == 2 || edges_.size() == 4) &&
            (edges_[0].mask == input_training::kA || edges_[0].mask == input_training::kD)) {
            direction = edges_[0].mask;
            if (last_up_) s.delay = ms(edges_[0].time, *last_up_);
            if (edges_.size() == 4) {
                mode = "counter";
                const auto opposite = direction == input_training::kA ? input_training::kD : input_training::kA;
                const bool overlap = edges_[1].mask == (input_training::kA | input_training::kD) && edges_[2].mask == opposite;
                if ((!overlap && (edges_[1].mask != 0 || edges_[2].mask != opposite)) || edges_[3].mask != 0) {
                    reject("UNSUPPORTED_DIRECTION_SEQUENCE"); return;
                }
                const auto original_up = edges_[overlap ? 2 : 1].time;
                const auto counter_down = edges_[overlap ? 1 : 2].time;
                s.move = ms(original_up, edges_[0].time);
                s.counter_delay = ms(counter_down, original_up);
                s.counter_hold = ms(edges_[3].time, counter_down);
                if (overlap) s.unsupported_reason = "SIGNED_OVERLAP_UNSUPPORTED";
            } else {
                if (edges_[1].mask != 0) { reject("INCOMPLETE_MOVEMENT_RELEASE"); return; }
                mode = "no_counter"; s.move = ms(edges_[1].time, edges_[0].time);
            }
            s.after = ms(*down_, edges_.back().time);
        } else { reject("UNSUPPORTED_MOVEMENT_SEQUENCE"); return; }
        // 逐样本先核对原始范围，不能让取平均或毫秒舍入掩盖不可表达动作。
        if (s.hold < 1 || s.hold > 2000 || (s.delay && (*s.delay < 0 || *s.delay > 2000)) ||
            (mode != "stationary" && (s.move < 1 || s.move > 500 || s.after < 0 || s.after > 20)) ||
            (mode == "counter" && (s.counter_delay < 0 || s.counter_delay > 200 || s.counter_hold < 1 || s.counter_hold > 200))) {
            if (s.unsupported_reason.empty()) s.unsupported_reason = "CYCLE_OUTSIDE_SCHEMA2_BOUNDS";
        }
        const auto key = mode + (mode == "stationary" ? ":none" : direction == input_training::kA ? ":A" : ":D");
        auto& group = groups_[key]; group.mode = mode; group.direction = direction;
        if (group.samples.size() >= 300) { reject("GROUP_SAMPLE_BUDGET"); return; }
        if (!s.unsupported_reason.empty()) reject(s.unsupported_reason);
        Json movement = Json::array();
        for (const auto& edge : edges_) movement.push_back(Json{{"held_mask",edge.mask},{"sequence",edge.sequence},{"received_at_ns",edge.time}});
        s.source = Json{{"hold_id",holds_},{"epoch",event.epoch},{"fire_down_sequence",down_sequence_},
            {"fire_up_sequence",event.sequence},{"fire_down_ns",*down_},{"fire_up_ns",up},
            {"previous_fire_up_sequence",last_up_ ? Json(last_up_sequence_) : Json(nullptr)},
            {"previous_fire_up_ns",last_up_ ? Json(*last_up_) : Json(nullptr)},
            {"movement_edges",std::move(movement)},{"cadence_included",s.delay.has_value()},
            {"unsupported_reason",s.unsupported_reason},
            {"raw_ms",Json{{"move_ms",s.move},{"counter_delay_ms",s.counter_delay},{"counter_hold_ms",s.counter_hold},
                {"shot_after_release_ms",s.after},{"shot_hold_ms",s.hold},{"fire_delay_ms",s.delay ? Json(*s.delay) : Json(nullptr)}}}};
        group.samples.push_back(s);
    }
public:
    void consume(const input_training::Event& event) {
        if (finished_) throw std::runtime_error("动作计划分析已结束");
        if (++events_ > 1048576) throw std::runtime_error("动作计划事件预算超限");
        if (!event.state_valid || event.gap || !event.epoch || !event.sequence || event.received_at_ns < 0 ||
            event.held_mask > 15 || (have_ && (event.epoch != previous_.epoch ||
            event.sequence != previous_.sequence + 1 || event.received_at_ns < previous_.received_at_ns))) {
            reject("INPUT_GAP_OR_INVALID_ORDER"); reset_cycle(); last_up_.reset(); have_ = false; return;
        }
        if (!have_) {
            have_ = true; previous_ = event;
            if (event.held_mask || event.left_down) issue_ = "MISSING_INITIAL_NEUTRAL";
            active_ = event.left_down;
            return;
        }
        const auto changed = static_cast<unsigned>(event.held_mask ^ previous_.held_mask);
        const bool button_edge = event.left_down != previous_.left_down;
        if ((changed && (changed & (changed-1))) || (changed && button_edge)) issue_ = "ATOMIC_EDGE_AMBIGUOUS";
        bool exclude = issue_ == "ATOMIC_EDGE_AMBIGUOUS" || issue_ == "MISSING_INITIAL_NEUTRAL" || issue_ == "WS_OR_DIAGONAL_UNSUPPORTED";
        if (!exclude && (event.held_mask & (input_training::kW | input_training::kS))) {
            issue_ = "WS_OR_DIAGONAL_UNSUPPORTED"; exclude = true;
        }
        if (!exclude && (event.held_mask & (input_training::kA | input_training::kD)) == (input_training::kA | input_training::kD))
            issue_ = "SIGNED_OVERLAP_UNSUPPORTED";
        if (changed) {
            if (active_ && !exclude) issue_ = "MOVEMENT_DURING_FIRE_UNSUPPORTED";
            if (edges_.size() < 5) edges_.push_back({event.held_mask, event.received_at_ns, event.sequence});
            else if (!exclude) issue_ = "EXTRA_MOVEMENT_EDGES";
        }
        if (button_edge && event.left_down) {
            active_ = true; down_ = event.received_at_ns; down_sequence_ = event.sequence;
            if (event.held_mask && !exclude) issue_ = "FIRE_BEFORE_MOVEMENT_RELEASE";
        } else if (button_edge) {
            complete(event);
            reset_cycle();
            if (!event.held_mask) { last_up_ = event.received_at_ns; last_up_sequence_ = event.sequence; }
            else { last_up_.reset(); issue_ = "MISSING_INITIAL_NEUTRAL"; }
        }
        previous_ = event;
    }
    Json finish(int candidate_shots = 8) {
        if (finished_) throw std::runtime_error("动作计划分析不能重复结束");
        finished_ = true;
        if (active_ || !edges_.empty() || !issue_.empty()) reject("UNFINISHED_CYCLE");
        Json out{{"schema_version",1},{"source","LOCAL_STEADY_MONITOR_RECEIPT"},
            {"meaning","CANDIDATE_RETEST_NOT_EXACT_REPLAY"},{"first_shot_semantics","SEED_SHOT_BEFORE_MOVEMENT"},
            {"rounding","NEAREST_INTEGER_MS_NO_CLAMP"},{"observed_events",events_},{"completed_holds",holds_},
            {"sample_window","FIRST_300_COMPLETE_PAIRED_CYCLES_PER_GROUP"},
            {"inactive_field_defaults","stationary move_ms=1; non-counter counter_hold_ms=1; unused waits=0"},
            {"unsupported",unsupported_},{"reasons",rejected_},{"groups",Json::array()}};
        for (const auto& [key, g] : groups_) {
            Sample sum; double delay_sum = 0; std::size_t cadence = 0, unsupported = 0;
            Json sources = Json::array();
            std::map<std::string, std::size_t> group_reasons;
            for (const auto& s : g.samples) {
                sources.push_back(s.source);
                sum.move += s.move; sum.counter_delay += s.counter_delay; sum.counter_hold += s.counter_hold;
                sum.after += s.after; sum.hold += s.hold;
                if (s.delay) { delay_sum += *s.delay; ++cadence; }
                if (!s.unsupported_reason.empty()) { ++unsupported; ++group_reasons[s.unsupported_reason]; }
            }
            const double n = static_cast<double>(g.samples.size());
            Json means{{"move_ms",sum.move/n},{"counter_delay_ms",sum.counter_delay/n},
                {"counter_hold_ms",sum.counter_hold/n},{"shot_after_release_ms",sum.after/n},{"shot_hold_ms",sum.hold/n},
                {"fire_delay_ms",cadence ? Json(delay_sum/cadence) : Json(nullptr)}};
            Json item{{"group",key},{"baseline",g.mode},{"direction",g.mode == "stationary" ? Json(nullptr) : Json(g.direction)},
                {"samples",g.samples.size()},{"cadence_samples",cadence},{"mean_ms",means},
                {"source_samples",std::move(sources)},
                {"supported_samples",g.samples.size()-unsupported},{"unsupported",unsupported},
                {"unsupported_reasons",group_reasons},{"candidate_plan",nullptr},{"proposed_plan",nullptr},
                {"validation_errors",Json::array()},{"reasons",Json::array()}};
            if (!cadence) item["validation_errors"].push_back("PREVIOUS_FIRE_UP_INTERVAL_UNAVAILABLE");
            if (unsupported) item["validation_errors"].push_back("SOURCE_CYCLES_NOT_EXPRESSIBLE_OR_AMBIGUOUS");
            {
                Json plan{{"schema_version",2},{"baseline",g.mode},{"capture_enabled",false},{"shots",candidate_shots},
                    {"direction",g.direction},{"move_during_fire_delay",false},{"fire_interval_ms",0},{"late_tolerance_ms",5}};
                for (const auto* field : {"move_ms","counter_delay_ms","counter_hold_ms","shot_after_release_ms","shot_hold_ms","fire_delay_ms"})
                    plan[field] = means[field].is_null() ? Json(nullptr) : Json(std::llround(means[field].get<double>()));
                if (g.mode == "stationary") { plan["move_ms"] = 1; plan["shot_after_release_ms"] = 0; }
                if (g.mode != "counter") { plan["counter_hold_ms"] = 1; plan["counter_delay_ms"] = 0; }
                item["proposed_plan"] = plan;
                try {
                    const auto parsed = counterpulse_plan_json(parse_counterpulse_plan(plan));
                    if (item["validation_errors"].empty()) item["candidate_plan"] = parsed;
                } catch (const std::exception&) { item["validation_errors"].push_back("MEAN_PLAN_REJECTED_BY_SCHEMA2_PARSER"); }
            }
            item["reasons"] = item["validation_errors"];
            out["groups"].push_back(std::move(item));
        }
        return out;
    }
};
}
#endif
