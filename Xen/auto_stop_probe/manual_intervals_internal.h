#ifndef AUTO_STOP_MANUAL_INTERVALS_INTERNAL_H
#define AUTO_STOP_MANUAL_INTERVALS_INTERNAL_H
#include "auto_stop_probe/probe_internal.h"
#include "input_training/input_training.h"
#include <array>
#include <cmath>
#include <deque>
#include <map>
#include <optional>

namespace auto_stop_probe_detail {
// 只描述已接收边沿的操作间隔；不把间隔升级成停稳时刻或输出计划。
class ManualIntervalsAccumulator {
public:
    void gap() {
        ++gaps_; ++segment_; known_=false;
        keys_={}; previous_operation_.reset(); previous_fire_down_.reset(); last_direction_up_.reset(); pending_fire_.reset();
    }
    void consume(const input_training::Event& event) {
        ++received_;
        const bool invalid = !event.state_valid || !event.epoch || !event.sequence || event.received_at_ns<0 || event.held_mask>15;
        const bool disorder = seen_ && (event.epoch!=epoch_ || event.sequence!=sequence_+1 || event.received_at_ns<time_);
        if(event.gap || invalid || disorder) gap();
        if(invalid) { ++invalid_; return; }
        if(disorder) ++invalid_;
        const std::uint8_t state=static_cast<std::uint8_t>(event.held_mask | (event.left_down ? 16 : 0));
        if(!known_) {
            // 当前已按住的键仅作基线，不能生成缺失的DOWN。
            mask_=state; known_=true;
        } else {
            const auto changed=static_cast<std::uint8_t>(state^mask_);
            const bool atomic=changed && (changed&(changed-1));
            std::array<std::optional<Edge>,5> edges;
            for(std::size_t k=0;k<5;++k) {
                if(!(changed&(1<<k))) continue;
                Edge edge{++operation_count_,segment_,event.epoch,event.sequence,event.received_at_ns,k,
                    (state&(1<<k))!=0,atomic};
                edges[k]=edge;
                auto row=edge_json(edge);
                row["previous_interval_ms"]=previous_operation_ ? Json((edge.time-previous_operation_->time)/1e6) : Json(nullptr);
                if(previous_operation_) add("operation_interval",*previous_operation_,edge);
                previous_operation_=edge;
                operations_.push_back(std::move(row));
                if(operations_.size()>3000) operations_.pop_front();
            }
            // 先处理单键保持。各边沿携atomic标记，因此遍历顺序不冒充同包先后。
            for(std::size_t k=0;k<5;++k) {
                if(!edges[k]) continue;
                const auto edge=*edges[k];
                auto& key=keys_[k];
                if(edge.down) {
                    key.down=edge; key.up.reset(); key.up_origin.reset(); key.reverse=false;
                    // 新移动开始后，旧释放不再属于当前移动周期。
                    if(k<4) { last_direction_up_.reset(); pending_fire_.reset(); }
                } else {
                    if(key.down) {
                        add(k==4 ? "fire_hold" : std::string("hold_")+names_[k],*key.down,edge);
                        if(k<4 && key.reverse) add(std::string("reverse_hold_")+names_[k],*key.down,edge);
                    }
                    // 未观测DOWN的UP可以保留为原始操作，但不能参与后续完整操作配对。
                    key.up=key.down ? std::optional<Edge>(edge) : std::nullopt;
                    key.up_origin=key.down;
                    key.down.reset(); key.reverse=false;
                    if(k<4) last_direction_up_=key.up;
                }
            }
            // 每个轴最多配一次有符号换向：松开后按下为正，先按反向再松旧键为负。
            for(std::size_t axis=0;axis<2;++axis) {
                const std::size_t first=axis==0 ? 1 : 0, second=axis==0 ? 3 : 2;
                for(std::size_t from : {first,second}) {
                    const std::size_t to=from==first ? second : first;
                    const bool new_press=edges[to] && edges[to]->down;
                    const bool old_release=edges[from] && !edges[from]->down;
                    if(!(new_press || old_release) || !keys_[from].up || !keys_[to].down) continue;
                    // 反向键按下必须晚于旧键的已知起始；up/down的配对本轮仅消费一次。
                    if(new_press && (state&(1<<from))) continue;
                    if(old_release && !(state&(1<<to))) continue;
                    const auto release=*keys_[from].up, press=*keys_[to].down;
                    if(!keys_[from].up_origin || press.sequence<keys_[from].up_origin->sequence) continue;
                    add(std::string("reverse_interval_")+names_[from]+"_"+names_[to],release,press);
                    keys_[from].up.reset(); keys_[from].up_origin.reset(); keys_[to].reverse=true;
                }
            }
            if(edges[4] && edges[4]->down) {
                const auto fire=*edges[4];
                if(previous_fire_down_) add("fire_down_interval",*previous_fire_down_,fire);
                previous_fire_down_=fire;
                // 新fire替换尚未配对的旧fire，不把多个原地点击归给同一次释放。
                pending_fire_.reset();
                // 只有本报告已全松WASD，才能把最近方向UP描述成最终方向UP。
                if(!(state&15) && last_direction_up_) {
                    add("release_to_fire",*last_direction_up_,fire);
                    // 一次移动释放只归属下一次开火，原地连续开火不能反复贡献旧间隔。
                    last_direction_up_.reset();
                } else if(state&15) {
                    bool movement_known=true;
                    for(std::size_t k=0;k<4;++k)
                        if((state&(1<<k)) && !keys_[k].down) movement_known=false;
                    if(movement_known) pending_fire_=fire;
                }
            }
            // 开火先于最终UP时保留负间隔；首次全松即消费，后续fire不能重用此UP。
            if(!(state&15) && (changed&15) && pending_fire_) {
                if(last_direction_up_) add("release_to_fire",*last_direction_up_,*pending_fire_);
                pending_fire_.reset(); last_direction_up_.reset();
            }
            mask_=state;
        }
        seen_=true; epoch_=event.epoch; sequence_=event.sequence; time_=event.received_at_ns;
    }
    Json snapshot() const {
        Json out{{"source","KMBOX_MONITOR"},{"time_domain","LOCAL_RECEIVE_STEADY"},
            {"meaning","OBSERVED_OPERATION_INTERVALS_NOT_GAME_SETTLING_OR_OUTPUT_PLAN"},
            {"received_events",received_},{"operation_count",operation_count_},{"gap_count",gaps_},
            {"invalid_events",invalid_},{"operations",operations_},{"metrics",Json::object()},
            {"retained_operation_limit",3000},{"retained_samples_per_metric",300},
            {"statistics_scope","ALL_ACCEPTED_NON_AMBIGUOUS_SAMPLES"},
            {"stddev_definition","POPULATION"},{"physical_validation_passed",false}};
        for(const auto& [name,metric]:metrics_) {
            out["metrics"][name]={{"label",metric_label(name)},{"count",metric.count},{"ambiguous_count",metric.ambiguous_count},
                {"mean_ms",metric.count ? Json(metric.mean) : Json(nullptr)},
                {"stddev_ms",metric.count ? Json(std::sqrt(std::max(0.0,metric.m2/static_cast<double>(metric.count)))) : Json(nullptr)},
                {"min_ms",metric.count ? Json(metric.minimum) : Json(nullptr)},
                {"max_ms",metric.count ? Json(metric.maximum) : Json(nullptr)},
                {"sample_count",metric.total},{"samples",metric.samples}};
        }
        return out;
    }
private:
    struct Edge {
        std::uint64_t id=0,segment=0,epoch=0,sequence=0;
        std::int64_t time=0; std::size_t key=0; bool down=false,ambiguous=false;
    };
    struct Key { std::optional<Edge> down,up,up_origin; bool reverse=false; };
    struct Metric {
        std::uint64_t count=0,ambiguous_count=0,total=0;
        double mean=0,m2=0,minimum=0,maximum=0;
        std::deque<Json> samples;
    };
    // 输入位图保持W/A/S/D；上面的同轴配对显式使用1/3与0/2索引。
    inline static constexpr std::array<const char*,5> names_{"W","A","S","D","LEFT"};
    static std::string metric_label(const std::string& name) {
        if(name=="operation_interval") return "相邻操作间隔";
        if(name=="fire_hold") return "左键保持时间";
        if(name=="fire_down_interval") return "相邻左键按下间隔";
        if(name=="release_to_fire") return "最终方向松开至左键按下（负值为提前按下）";
        if(name.starts_with("reverse_interval_")) return "反向切换间隔 "+name.substr(17)+"（负值表示重叠）";
        if(name.starts_with("reverse_hold_")) return "反向键保持 "+name.substr(13);
        if(name.starts_with("hold_")) return "方向键保持 "+name.substr(5);
        return name;
    }
    static Json edge_json(const Edge& edge) {
        return {{"operation_id",edge.id},{"segment",edge.segment},{"key",names_[edge.key]},
            {"edge",edge.down ? "DOWN":"UP"},{"time_ns",edge.time},{"epoch",edge.epoch},
            {"sequence",edge.sequence},{"atomic_ambiguous",edge.ambiguous}};
    }
    void add(const std::string& name,const Edge& from,const Edge& to) {
        if(from.segment!=to.segment) return;
        auto& metric=metrics_[name];
        const double delta=(to.time-from.time)/1e6;
        const bool ambiguous=from.ambiguous || to.ambiguous || from.sequence==to.sequence;
        metric.samples.push_back({{"sample_id",name+":"+std::to_string(++metric.total)},
            {"from",edge_json(from)},{"to",edge_json(to)},{"delta_ms",delta},{"ambiguous",ambiguous}});
        if(metric.samples.size()>300) metric.samples.pop_front();
        if(ambiguous) { ++metric.ambiguous_count; return; }
        ++metric.count;
        const double difference=delta-metric.mean;
        metric.mean+=difference/static_cast<double>(metric.count);
        metric.m2+=difference*(delta-metric.mean);
        if(metric.count==1) metric.minimum=metric.maximum=delta;
        else { metric.minimum=std::min(metric.minimum,delta); metric.maximum=std::max(metric.maximum,delta); }
    }
    std::array<Key,5> keys_{};
    std::map<std::string,Metric> metrics_;
    std::deque<Json> operations_;
    std::optional<Edge> previous_operation_,previous_fire_down_,last_direction_up_,pending_fire_;
    std::uint64_t received_=0,operation_count_=0,gaps_=0,invalid_=0,segment_=1,epoch_=0,sequence_=0;
    std::int64_t time_=0;
    std::uint8_t mask_=0;
    bool known_=false,seen_=false;
};
}
#endif
