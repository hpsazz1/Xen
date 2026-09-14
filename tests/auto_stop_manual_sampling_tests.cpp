#include "auto_stop_probe/manual_sampling_internal.h"
#include <iostream>
namespace {
using namespace auto_stop_probe_detail;
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
input_training::Event event(std::uint64_t sequence, std::int64_t ms, int mask=0, bool left=false) {
    input_training::Event e; e.epoch=1; e.sequence=sequence; e.received_at_ns=ms*1000000;
    e.held_mask=static_cast<std::uint8_t>(mask); e.left_down=left; e.state_valid=true; return e;
}
void tests() {
    TwoAxisSamplingModel xy;
    xy.observe_wasd(0,0); xy.observe_wasd(1000000,3);
    auto value=xy.query(1001000000);
    check(value["valid"]==true && std::abs(value["estimated_speed"].get<double>()-std::sqrt(2.))<1e-9,
        "W+A每轴上限独立，hypot合成不斜向归一");
    TwoAxisSamplingModel ws; ws.observe_wasd(0,0); ws.observe_wasd(1000000,1);
    check(ws.query(101000000)["estimated_speed"].get<double>()>.5,"W轴独立加速");
    ws.observe_wasd(101000000,4);
    check(ws.query(111000000)["estimated_speed"].get<double>()<.5,"S反向制动");
    ManualSamplingAccumulator manual;
    manual.consume(event(1,0)); manual.consume(event(2,10,2)); manual.consume(event(3,110));
    manual.consume(event(4,115,8)); manual.consume(event(5,120));
    manual.consume(event(6,125,0,true)); manual.consume(event(7,130));
    auto snap=manual.snapshot(142000000,true);
    check(snap["timings"].size()==1 && snap["timings"][0]["delta_ms"]==5.0,"AD松按间隔接收域5ms");
    check(snap["shots"].size()==1 && snap["shots"][0]["samples"].empty(),"短按首样本到期之前不生成");
    snap=manual.snapshot(143000000,true);
    check(snap["first_sample_count"]==1 && snap["shots"][0]["samples"][0]["sample_id"]=="1:1",
        "短按仍按down+18ms采样，ID稳定");
    check(!snap["shots"][0].contains("down_ack_ns") && snap["source"]=="KMBOX_MONITOR" &&
        snap["samples_are_bullets"]==false,"不伪造ACK或子弹");
    // UI提前查询不推进模型；UP后首样本前新方向仍影响最终模型。
    manual.consume(event(8,135,2)); manual.consume(event(9,143,2));
    auto corrected=manual.snapshot(143000000,true);
    check(corrected["shots"][0]["samples"][0]["estimated_speed"]!=snap["shots"][0]["samples"][0]["estimated_speed"],
        "提前UI外推不固化，后续原始事件参与到期采样");
    ManualSamplingAccumulator long_hold;
    long_hold.consume(event(1,0)); long_hold.consume(event(2,1,0,true));
    auto live=long_hold.snapshot(1001000000,true);
    check(live["total_sample_count"]==10,"无新包长按持续模型采样");
    auto stopped=long_hold.snapshot(1001000000,false);
    check(stopped["shots"][0]["down_model"]["valid"]==false,"停止时缺UP不可当完整");
    check(long_hold.snapshot(9000000000LL,false)["total_sample_count"]==10,"停止后冻结时刻不继续采样");
    ManualSamplingAccumulator high_rate;
    high_rate.consume(event(1,0)); high_rate.consume(event(2,1,0,true));
    for (std::uint64_t i=3;i<3000;++i) high_rate.consume(event(i,static_cast<std::int64_t>(i),0,true));
    high_rate.consume(event(3000,3000));
    auto rate=high_rate.snapshot(3000000000LL,true);
    check(rate["shots"][0]["down_model"]["valid"]==true && rate["shots"][0]["samples"][0]["valid"]==true,
        "高频相同状态不耗尽模型历史或丢早期已到期样本");
    ManualSamplingAccumulator initial;
    initial.consume(event(1,0,2,true)); initial.consume(event(2,10)); initial.consume(event(3,20,0,true));
    auto start=initial.snapshot(40000000,true);
    check(start["missing_start_count"]==1 && start["shot_count"]==1 && start["current_model"]["valid"]==false,
        "初始held不伪造down，初始速度不可信不能全松恢复");
    ManualSamplingAccumulator broken;
    broken.consume(event(1,0)); broken.consume(event(2,10,0,true)); broken.gap();
    broken.consume(event(3,30)); broken.consume(event(4,40,0,true));
    auto gap=broken.snapshot(60000000,true);
    check(gap["shots"][0]["complete_hold"]==false && gap["current_model"]["valid"]==false,"gap损坏hold与速度可信度");
    auto duplicate=event(4,40,0,true); broken.consume(duplicate);
    check(broken.snapshot(60000000,true)["invalid_events"]==1,"重复序号不可当新down");
    auto epoch=event(1,70); epoch.epoch=2; broken.consume(epoch);
    check(broken.snapshot(70000000,true)["invalid_events"]==2,"epoch切换显式断流");
    ManualSamplingAccumulator atomic;
    atomic.consume(event(1,0)); atomic.consume(event(2,10,1)); atomic.consume(event(3,20,4,true));
    auto ambiguous=atomic.snapshot(40000000,true);
    check(ambiguous["timings"][0]["atomic_ambiguous"]==true && ambiguous["timings"][0]["grade"]=="UNCLASSIFIED" &&
        ambiguous["shots"][0]["atomic_ambiguous"]==true,"WS同包切换与fire均标歧义");
    ManualSamplingAccumulator retained;
    retained.consume(event(1,0));
    std::uint64_t seq=2; std::int64_t ms=1;
    for (int i=0;i<305;++i) {
        retained.consume(event(seq++,ms++,2)); retained.consume(event(seq++,ms++));
        retained.consume(event(seq++,ms++,8,true)); retained.consume(event(seq++,ms++));
    }
    auto bounded=retained.snapshot(ms*1000000,true);
    check(bounded["shot_count"]==305 && bounded["shots"].size()==300 && bounded["timings"].size()==300 &&
        bounded["shots"][0]["ordinal"]==6,"最近300条retention保留总数和稳定ID");
    SamplingSettings tuned; tuned.fire_sample_delay_ms=30; tuned.auto_fire_interval_ms=200;
    ManualSamplingAccumulator custom(tuned); custom.consume(event(1,0)); custom.consume(event(2,1,0,true));
    check(custom.snapshot(1001000000,true)["total_sample_count"]==5,"可调采样间隔生效");
}
}
int main() { try { tests(); std::cout<<"人工输入模型专项通过\n"; return 0; }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; } }
