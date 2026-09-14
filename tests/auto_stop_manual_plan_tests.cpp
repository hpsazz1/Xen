#include "auto_stop_probe/manual_plan_internal.h"
#include <iostream>
namespace {
using namespace auto_stop_probe_detail;
void require(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
struct Fixture {
    ManualPlanBuilder builder;
    std::uint64_t sequence = 0;
    void add(int ms, unsigned mask = 0, bool fire = false, bool gap = false) {
        input_training::Event e; e.epoch = 1; e.sequence = ++sequence; e.received_at_ns = ms*1000000LL;
        e.held_mask = static_cast<std::uint8_t>(mask); e.left_down = fire; e.state_valid = true; e.gap = gap;
        builder.consume(e);
    }
    void seed() { add(0); add(1,0,true); add(6); }
    void counter(int start, unsigned dir = 2, bool overlap = false) {
        const auto other = dir == 2 ? 8 : 2;
        add(start,dir);
        if (overlap) { add(start+90,10); add(start+100,other); }
        else { add(start+100); add(start+110,other); }
        add(start+120); add(start+130,0,true); add(start+135);
    }
};
const Json& group(const Json& report, const std::string& key) {
    for (const auto& value : report.at("groups")) if (value.at("group") == key) return value;
    throw std::runtime_error("缺少预期方向组");
}
}
int main() {
    try {
        Fixture f; f.seed(); f.counter(20); f.counter(200,8);
        const auto report = f.builder.finish();
        const auto& a = group(report,"counter:A"); const auto& d = group(report,"counter:D");
        require(a["samples"] == 1 && d["samples"] == 1, "方向不能混平均");
        require(a["source_samples"][0]["hold_id"] == 2 && a["source_samples"][0]["fire_down_sequence"] == 8 &&
            a["source_samples"][0]["fire_up_sequence"] == 9 && a["source_samples"][0]["previous_fire_up_sequence"] == 3 &&
            a["source_samples"][0]["movement_edges"].size() == 4 && a["source_samples"][0]["raw_ms"]["fire_delay_ms"] == 14,
            "均值可追溯到原始周期与各事件端点");
        const auto p = parse_counterpulse_plan(a.at("candidate_plan"));
        require(p.schema_version == 2 && p.move_ms == 100 && p.counter_delay_ms == 10 && p.counter_hold_ms == 10 &&
            p.shot_after_release_ms == 10 && p.shot_hold_ms == 5 && p.fire_delay_ms == 14 && !p.move_during_fire_delay,
            "完整周期映射正式schema2及ACK等待候选");
        Fixture overlap; overlap.seed(); overlap.counter(20,8,true);
        const auto og = group(overlap.builder.finish(),"counter:D");
        require(og["candidate_plan"].is_null() && og["proposed_plan"]["counter_delay_ms"] == -10 &&
            og["mean_ms"]["counter_hold_ms"] == 30 && !og["validation_errors"].empty(), "重叠均值保留但不能clamp成合法计划");
        Fixture excessive; excessive.seed(); excessive.add(20,2); excessive.add(620); excessive.add(630,0,true); excessive.add(635);
        const auto eg = group(excessive.builder.finish(),"no_counter:A");
        require(eg["proposed_plan"]["move_ms"] == 600 && eg["candidate_plan"].is_null(), "超范围均值可编辑但不能静默裁剪");
        Fixture stationary; stationary.seed(); stationary.add(100,0,true); stationary.add(110);
        const auto sg = group(stationary.builder.finish(),"stationary:none");
        require(!sg["candidate_plan"].is_null() && sg["mean_ms"]["fire_delay_ms"] == 94, "原地前UP至DOWN独立统计");
        Fixture atomic; atomic.seed(); atomic.add(20,2); atomic.add(120,8); atomic.add(130); atomic.add(140,0,true); atomic.add(145);
        require(atomic.builder.finish()["reasons"].contains("ATOMIC_EDGE_AMBIGUOUS"), "同包双边沿不可分解时序");
        Fixture gap; gap.seed(); gap.add(20,2); gap.add(30,0,false,true); gap.add(40); gap.add(50,0,true); gap.add(60);
        const auto gr = gap.builder.finish();
        require(gr["reasons"].contains("INPUT_GAP_OR_INVALID_ORDER") && group(gr,"stationary:none")["candidate_plan"].is_null(), "缺口后不沿用前UP水位");
        Fixture unfinished; unfinished.seed(); unfinished.add(20,2);
        require(unfinished.builder.finish()["reasons"].contains("UNFINISHED_CYCLE"), "未完成周期排除");
        Fixture too_many; too_many.seed(); too_many.counter(20);
        require(group(too_many.builder.finish(31),"counter:A")["candidate_plan"].is_null(), "候选仍受正式shots上限约束");
        std::cout << "人工周期候选专项通过；未调用设备\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
