#ifndef AUTO_STOP_MANUAL_SAMPLING_INTERNAL_H
#define AUTO_STOP_MANUAL_SAMPLING_INTERNAL_H
#include "auto_stop_probe/sampling_analysis_internal.h"
#include "input_training/input_training.h"
#include <deque>

namespace auto_stop_probe_detail {
// 两轴分别复用原有输入模型；轴上限独立，合成量不做斜向归一化。
class TwoAxisSamplingModel {
public:
    explicit TwoAxisSamplingModel(SamplingSettings settings = {})
        : settings_(settings), x_(settings), y_(settings) {}
    void observe_wasd(std::int64_t time, std::uint8_t mask) {
        if (mask > 15) { invalidate(time); return; }
        x_.observe_wasd(time, mask & 10);
        y_.observe_wasd(time, ((mask & 1) ? 2 : 0) | ((mask & 4) ? 8 : 0));
    }
    void invalidate(std::int64_t time) { x_.invalidate(time); y_.invalidate(time); }
    void observe_time(std::int64_t time) { x_.observe_time(time); y_.observe_time(time); }
    Json query(std::int64_t time) const {
        const auto x = x_.query(time), y = y_.query(time);
        auto value = x;
        value["valid"] = x["valid"].get<bool>() && y["valid"].get<bool>();
        value["axis_x"] = x; value["axis_y"] = y;
        value["estimated_speed"] = nullptr; value["speed_ratio"] = nullptr;
        value["within_model_threshold"] = nullptr; value["classification"] = "UNAVAILABLE";
        if (value["valid"] == true) {
            const double speed = std::hypot(x["estimated_speed"].get<double>(), y["estimated_speed"].get<double>());
            const double ratio = speed / (settings_.max_move_speed * settings_.clean_shot_speed_ratio);
            value["estimated_speed"] = speed; value["speed_ratio"] = ratio;
            value["within_model_threshold"] = ratio <= 1;
            value["classification"] = ratio <= 1 ? "WITHIN_MODEL_THRESHOLD" : ratio <= 1.5 ? "MICRO" : "RUNNING";
        }
        return value;
    }
private:
    SamplingSettings settings_;
    sampling_detail::SamplingModel x_, y_;
};

// 单一采集线程持有；UI应读取其发布的JSON快照，不能并发调用本对象。
class ManualSamplingAccumulator {
public:
    explicit ManualSamplingAccumulator(SamplingSettings settings = {})
        : settings_(parse_sampling_settings(sampling_settings_json(settings))), model_(settings_) {}
    void gap() {
        ++gaps_;
        model_.invalidate(last_time_);
        for (auto& shot : shots_) if (!shot.up && !shot.ended) {
            shot.ended = true; shot.valid = false; shot.reason = "INPUT_GAP";
        }
        // 短按松开后的首样本尚未到期，也不能跨缺口猜出速度。
        for (auto& shot : shots_) if (shot.samples.empty()) shot.valid = false;
        active_.reset(); known_ = false;
        axes_[0] = {}; axes_[1] = {};
    }
    void consume(const input_training::Event& event) {
        if (stopped_at_) return;
        ++received_;
        const bool bad = !event.state_valid || event.epoch == 0 || event.sequence == 0 ||
            event.received_at_ns < 0 || event.received_at_ns > INT64_MAX - 100000000000LL || event.held_mask > 15;
        const bool discontinuity = seen_ && (event.epoch != epoch_ || event.sequence != sequence_ + 1 ||
            event.received_at_ns < last_time_);
        if (event.gap || discontinuity || bad) gap();
        if (bad) { ++invalid_; return; }
        if (discontinuity) ++invalid_;
        // 到期时间之前的事实已完整接收，再固化样本。高频鼠标包不保存模型状态。
        finalize_samples(event.received_at_ns, false);
        const bool baseline = !known_;
        const bool movement_changed = baseline || event.held_mask != mask_;
        const bool fire_changed = !baseline && event.left_down != left_;
        if (movement_changed) model_.observe_wasd(event.received_at_ns, event.held_mask);
        else model_.observe_time(event.received_at_ns);
        if (!baseline && movement_changed) {
            assess_axis(axes_[0], event, 2, 8, 'A', 'D');
            assess_axis(axes_[1], event, 1, 4, 'W', 'S');
        }
        if (baseline && event.left_down) ++missing_starts_;
        if (fire_changed && event.left_down) {
            Shot shot;
            shot.ordinal = ++total_shots_; shot.down = event.received_at_ns;
            shot.epoch = event.epoch; shot.sequence = event.sequence;
            shot.down_model = model_.query(shot.down);
            shot.schedule = sampling_detail::sample_due_times(shot.down, {},
                INT64_MAX, settings_);
            shot.ambiguous = movement_changed;
            shot.valid = shot.down_model["valid"].get<bool>();
            shots_.push_back(std::move(shot)); active_ = total_shots_;
            if (shots_.size() > 300) shots_.pop_front();
        } else if (fire_changed && !event.left_down && active_) {
            for (auto& shot : shots_) if (shot.ordinal == *active_) {
                shot.up = event.received_at_ns; shot.ambiguous |= movement_changed;
                shot.schedule = sampling_detail::sample_due_times(shot.down, shot.up, *shot.up, settings_);
            }
            active_.reset();
        }
        mask_ = event.held_mask; left_ = event.left_down; known_ = true; seen_ = true;
        epoch_ = event.epoch; sequence_ = event.sequence; last_time_ = event.received_at_ns;
        finalize_samples(last_time_, true);
    }
    Json snapshot(std::int64_t now_ns, bool recording) {
        if (!recording && !stopped_at_) stopped_at_ = std::max(now_ns, last_time_);
        const auto now = stopped_at_.value_or(now_ns);
        const bool horizon_valid = now >= last_time_ && now >= 0 && now <= INT64_MAX - 100000000000LL;
        Json out{{"source","KMBOX_MONITOR"},{"analysis_mode","MANUAL_RECEIVE_INPUT_MODEL"},
            {"model_scope","TWO_AXIS_INPUT_MODEL_WITHOUT_DIAGONAL_NORMALIZATION"},
            {"quality_note","接收时序输入模型，可调参数不是游戏速度实测；初始静止为假设，缺口后需重新录制"},
            {"initial_velocity_assumption","ZERO_NOT_PHYSICALLY_VERIFIED"},
            {"physical_validation_passed",false},{"samples_are_bullets",false},
            {"game_stability",nullptr},{"game_shot_stability",nullptr},
            {"recording",recording && !stopped_at_},{"time_ns",now},{"settings",sampling_settings_json(settings_)},
            {"shots",Json::array()},{"timings",timings_},{"received_events",received_},
            {"invalid_events",invalid_},{"gap_count",gaps_},{"missing_start_count",missing_starts_},
            {"shot_count",total_shots_},{"total_timing_count",total_timings_},
            {"source_loss_verifiable",false},{"current_model",model_.query(now)},
            {"model_summary",Json::object()}};
        if (!horizon_valid) make_unavailable(out["current_model"]);
        for (const auto* name : {"down","first","held"}) out["model_summary"][name] =
            {{"observed",0},{"valid",0},{"within_model_threshold",0},{"micro",0},{"running",0}};
        std::size_t first = 0, held = 0;
        for (const auto& shot : shots_) {
            auto samples = shot.samples;
            const auto schedule = sampling_detail::sample_due_times(shot.down, shot.up, now, settings_);
            if (!shot.ended && horizon_valid) {
                for (std::size_t i = samples.size(); i < schedule.times.size(); ++i) {
                    if (schedule.times[i] > now) break;
                    samples.push_back(sample(shot, schedule.times[i], i));
                }
            }
            const bool incomplete = !shot.up && stopped_at_.has_value();
            auto down = shot.down_model;
            if (!shot.valid || incomplete) make_unavailable(down);
            for (auto& value : samples) if (!shot.valid || incomplete) make_unavailable(value);
            Json item{{"ordinal",shot.ordinal},{"epoch",shot.epoch},{"down_sequence",shot.sequence},
                {"down_received_ns",shot.down},{"up_received_ns",shot.up ? Json(*shot.up) : Json(nullptr)},
                {"complete_hold",shot.up.has_value() && !shot.ended},{"active_hold",!shot.up && !shot.ended && !stopped_at_},
                {"valid_input_hold",shot.valid && !incomplete},{"atomic_ambiguous",shot.ambiguous},
                {"observed_hold_ms",shot.up ? Json((*shot.up-shot.down)/1e6) : Json(nullptr)},
                {"end_reason",incomplete ? "MISSING_UP" : shot.reason},
                {"down_model",down},{"samples",samples},{"sample_budget_exceeded",schedule.budget_exceeded}};
            tally(out,"down",down);
            for (const auto& value : samples) {
                const bool is_first = value["kind"] == "FIRST_MODEL_SAMPLE";
                tally(out,is_first ? "first" : "held",value);
                if (is_first) ++first; else ++held;
            }
            out["shots"].push_back(std::move(item));
        }
        out["first_sample_count"] = first; out["held_sample_count"] = held;
        out["total_sample_count"] = first + held;
        out["retained_shot_count"] = shots_.size();
        out["summary_scope"] = "RETAINED_300_HOLDS";
        return out;
    }
private:
    struct Shot {
        std::uint64_t ordinal = 0, epoch = 0, sequence = 0;
        std::int64_t down = 0;
        std::optional<std::int64_t> up;
        bool valid = false, ambiguous = false, ended = false;
        sampling_detail::SamplingSchedule schedule;
        std::string reason = "RECEIVED_INPUT";
        Json down_model, samples = Json::array();
    };
    struct Axis { std::optional<std::int64_t> release[2], overlap[2]; bool pressed_known[2]{}; };
    static void make_unavailable(Json& model) {
        model["valid"] = false; model["classification"] = "UNAVAILABLE";
        model["within_model_threshold"] = nullptr;
    }
    static void tally(Json& out, const char* key, const Json& sample) {
        auto& row = out["model_summary"][key]; row["observed"] = row["observed"].get<int>() + 1;
        if (!sample.value("valid",false)) return;
        row["valid"] = row["valid"].get<int>() + 1;
        const auto grade = sample.value("classification","");
        const char* field = grade == "WITHIN_MODEL_THRESHOLD" ? "within_model_threshold" : grade == "MICRO" ? "micro" : "running";
        row[field] = row[field].get<int>() + 1;
    }
    Json sample(const Shot& shot, std::int64_t due, std::size_t index) const {
        auto value = model_.query(due);
        value["sample_id"] = std::to_string(shot.ordinal) + ":" + std::to_string(index + 1);
        value["kind"] = index ? "HELD_MODEL_SAMPLE" : "FIRST_MODEL_SAMPLE";
        value["atomic_ambiguous"] = shot.ambiguous;
        value["time_domain"] = "LOCAL_RECEIVE_STEADY";
        if (!shot.valid) make_unavailable(value);
        return value;
    }
    void finalize_samples(std::int64_t horizon, bool inclusive) {
        for (auto& shot : shots_) {
            if (shot.ended) continue;
            for (std::size_t i = shot.samples.size(); i < shot.schedule.times.size(); ++i) {
                const auto due = shot.schedule.times[i];
                if (due > horizon || (!inclusive && due == horizon)) break;
                shot.samples.push_back(sample(shot,due,i));
                shot.samples.back()["extrapolated"] = false;
            }
        }
    }
    void timing(std::int64_t release, std::int64_t press, char from, char to, bool ambiguous) {
        const auto delta = press-release;
        const auto magnitude = std::abs(delta);
        const char* grade = ambiguous || magnitude > 120000000 ? "UNCLASSIFIED" : magnitude <= 2000000 ? "PERFECT" :
            magnitude <= 10000000 ? "EXCELLENT" : delta < 0 ? "EARLY" : "LATE";
        timings_.push_back({{"ordinal",++total_timings_},{"from",std::string(1,from)},{"to",std::string(1,to)},
            {"delta_ms",delta/1e6},{"grade",grade},{"atomic_ambiguous",ambiguous},{"timing_uncertainty_known",false},
            {"meaning","RECEIVED_KEY_SWITCH_INTERVAL_NOT_STOP_ERROR"}});
        if (timings_.size() > 300) timings_.erase(timings_.begin());
    }
    void assess_axis(Axis& axis, const input_training::Event& event, std::uint8_t first, std::uint8_t second, char a, char b) {
        const std::uint8_t bits[2]{first,second}; const char names[2]{a,b};
        const auto changed = (mask_ ^ event.held_mask) & (first|second);
        if (changed == (first|second)) {
            for (int k=0;k<2;++k) if ((mask_&bits[k]) && !(event.held_mask&bits[k]) &&
                (event.held_mask&bits[1-k]) && axis.pressed_known[k])
                timing(event.received_at_ns,event.received_at_ns,names[k],names[1-k],true);
            axis = {};
            for (int k=0;k<2;++k) axis.pressed_known[k] = (event.held_mask&bits[k]) != 0;
            return;
        }
        for (int k=0;k<2;++k) {
            if (!(changed&bits[k])) continue;
            const int other=1-k;
            if (event.held_mask&bits[k]) {
                axis.release[k].reset();
                if ((mask_&bits[other]) && axis.pressed_known[other]) axis.overlap[other]=event.received_at_ns;
                else if (axis.release[other]) {
                    timing(*axis.release[other],event.received_at_ns,names[other],names[k],false);
                    axis.release[other].reset();
                }
                axis.pressed_known[k]=true;
            } else {
                axis.overlap[other].reset();
                if (axis.pressed_known[k]) {
                    if (axis.overlap[k]) { timing(event.received_at_ns,*axis.overlap[k],names[k],names[other],false); axis.overlap[k].reset(); }
                    else axis.release[k]=event.received_at_ns;
                }
                axis.pressed_known[k]=false;
            }
        }
    }
    SamplingSettings settings_;
    TwoAxisSamplingModel model_;
    std::deque<Shot> shots_;
    Json timings_ = Json::array();
    Axis axes_[2];
    std::optional<std::uint64_t> active_;
    std::optional<std::int64_t> stopped_at_;
    std::uint64_t received_=0, invalid_=0, gaps_=0, missing_starts_=0, total_shots_=0, total_timings_=0, epoch_=0, sequence_=0;
    std::int64_t last_time_=0;
    std::uint8_t mask_=0;
    bool left_=false, known_=false, seen_=false;
};
}
#endif
