#include "auto_stop_probe/manual_intervals_internal.h"
#include <iostream>
namespace {
using namespace auto_stop_probe_detail;
void check(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
input_training::Event event(std::uint64_t seq,std::int64_t ms,int mask=0,bool left=false) {
    input_training::Event value; value.epoch=1; value.sequence=seq; value.received_at_ns=ms*1000000;
    value.held_mask=static_cast<std::uint8_t>(mask); value.left_down=left; value.state_valid=true; return value;
}
void tests() {
    ManualIntervalsAccumulator a;
    a.consume(event(1,0)); a.consume(event(2,10,2)); a.consume(event(3,20,2));
    a.consume(event(4,110)); a.consume(event(5,160,8)); a.consume(event(6,165));
    a.consume(event(7,170,0,true)); a.consume(event(8,190));
    a.consume(event(9,270,0,true)); a.consume(event(10,300));
    auto out=a.snapshot(); const auto& m=out["metrics"];
    check(out["operation_count"]==8,"不变报告不重复计操作");
    check(m["hold_A"]["mean_ms"]==100. && m["hold_D"]["mean_ms"]==5.,"每方向保持时间");
    check(m["reverse_interval_A_D"]["mean_ms"]==50. && m["reverse_hold_D"]["mean_ms"]==5.,"反向等待和反向保持");
    check(m["release_to_fire"]["samples"][0]["delta_ms"]==5. && m["fire_down_interval"]["mean_ms"]==100.,
        "最终方向释放至开火以及相邻DOWN");
    check(m["release_to_fire"]["count"]==1 && m["release_to_fire"]["mean_ms"]==5.,
        "原地第二次开火不能重复消费同一次方向释放");
    check(m["fire_hold"]["count"]==2 && m["fire_hold"]["mean_ms"]==25. && m["fire_hold"]["stddev_ms"]==5. &&
        m["fire_hold"]["min_ms"]==20. && m["fire_hold"]["max_ms"]==30.,"保持总体标准差与分母");
    check(m["hold_A"]["samples"][0]["from"]["sequence"]==2 && m["hold_A"]["samples"][0]["to"]["sequence"]==4 &&
        m["hold_A"]["samples"][0]["sample_id"]=="hold_A:1","样本保留精确来源序号和ID");
    ManualIntervalsAccumulator overlap;
    overlap.consume(event(1,0)); overlap.consume(event(2,10,2)); overlap.consume(event(3,20,10));
    overlap.consume(event(4,30,8)); overlap.consume(event(5,50));
    out=overlap.snapshot();
    check(out["metrics"]["reverse_interval_A_D"]["mean_ms"]==-10. &&
        out["metrics"]["reverse_hold_D"]["mean_ms"]==30.,"重叠保持负换向间隔");
    ManualIntervalsAccumulator aborted;
    aborted.consume(event(1,0)); aborted.consume(event(2,10,8)); aborted.consume(event(3,20,10));
    aborted.consume(event(4,30,8)); aborted.consume(event(5,40));
    check(!aborted.snapshot()["metrics"].contains("reverse_interval_A_D"),"取消重叠不能把更早的旧方向DOWN当成新反向DOWN");
    ManualIntervalsAccumulator new_move;
    new_move.consume(event(1,0)); new_move.consume(event(2,10,2)); new_move.consume(event(3,20));
    new_move.consume(event(4,30,8)); new_move.consume(event(5,40,8));
    new_move.consume(event(6,50,8)); new_move.consume(event(7,60)); new_move.consume(event(8,70,0,true));
    auto new_move_out=new_move.snapshot();
    check(new_move_out["metrics"]["release_to_fire"]["count"]==1 &&
        new_move_out["metrics"]["release_to_fire"]["samples"][0]["from"]["sequence"]==7 &&
        new_move_out["metrics"]["release_to_fire"]["mean_ms"]==10.,"新移动DOWN使旧释放失效，只配对当前完整释放");
    ManualIntervalsAccumulator release_gap;
    release_gap.consume(event(1,0)); release_gap.consume(event(2,10,2)); release_gap.consume(event(3,20));
    release_gap.gap(); release_gap.consume(event(4,30)); release_gap.consume(event(5,40,0,true));
    check(!release_gap.snapshot()["metrics"].contains("release_to_fire"),"gap必须清除待配对方向释放");
    ManualIntervalsAccumulator early_fire;
    early_fire.consume(event(1,0)); early_fire.consume(event(2,10,2)); early_fire.consume(event(3,100,2,true));
    early_fire.consume(event(4,105,2)); early_fire.consume(event(5,114));
    early_fire.consume(event(6,150,0,true)); early_fire.consume(event(7,160));
    auto early=early_fire.snapshot();
    check(early["metrics"]["release_to_fire"]["count"]==1 && early["metrics"]["release_to_fire"]["mean_ms"]==-14. &&
        early["metrics"]["release_to_fire"]["samples"][0]["from"]["sequence"]==5 &&
        early["metrics"]["release_to_fire"]["samples"][0]["to"]["sequence"]==3,
        "开火先于最终松键14ms保留负数，之后原地fire不重复消费");
    ManualIntervalsAccumulator replaced_fire;
    replaced_fire.consume(event(1,0)); replaced_fire.consume(event(2,10,2)); replaced_fire.consume(event(3,20,2,true));
    replaced_fire.consume(event(4,25,2)); replaced_fire.consume(event(5,30,2,true)); replaced_fire.consume(event(6,40,0,true));
    check(replaced_fire.snapshot()["metrics"]["release_to_fire"]["mean_ms"]==-10.,"多个未配对fire只取最新一次");
    ManualIntervalsAccumulator canceled_fire;
    canceled_fire.consume(event(1,0)); canceled_fire.consume(event(2,10,2)); canceled_fire.consume(event(3,20,2,true));
    canceled_fire.consume(event(4,30,10,true)); canceled_fire.consume(event(5,40,8,true)); canceled_fire.consume(event(6,50,0,true));
    check(!canceled_fire.snapshot()["metrics"].contains("release_to_fire"),"新移动DOWN取消不明确的早开火配对");
    ManualIntervalsAccumulator fire_gap;
    fire_gap.consume(event(1,0)); fire_gap.consume(event(2,10,2)); fire_gap.consume(event(3,20,2,true));
    fire_gap.gap(); fire_gap.consume(event(4,30,2,true)); fire_gap.consume(event(5,40,0,true));
    check(!fire_gap.snapshot()["metrics"].contains("release_to_fire"),"缺口取消待配对早开火");
    ManualIntervalsAccumulator ws;
    ws.consume(event(1,0)); ws.consume(event(2,10,1)); ws.consume(event(3,20));
    ws.consume(event(4,23,4)); ws.consume(event(5,29));
    out=ws.snapshot();
    check(out["metrics"]["reverse_interval_W_S"]["mean_ms"]==3. && out["metrics"]["reverse_hold_S"]["mean_ms"]==6.,
        "W/S必须按独立同轴配对");
    ManualIntervalsAccumulator atomic;
    atomic.consume(event(1,0)); atomic.consume(event(2,10,2)); atomic.consume(event(3,20,8,true));
    atomic.consume(event(4,30)); out=atomic.snapshot();
    check(out["metrics"]["reverse_interval_A_D"]["count"]==0 &&
        out["metrics"]["reverse_interval_A_D"]["ambiguous_count"]==1 &&
        out["metrics"]["reverse_interval_A_D"]["samples"][0]["delta_ms"]==0.,"同包零差保留但排除平均");
    check(out["metrics"]["fire_hold"]["count"]==0,"同包fire边缘不进入均值");
    ManualIntervalsAccumulator initial;
    initial.consume(event(1,0,2,true)); initial.consume(event(2,10));
    out=initial.snapshot();
    check(out["operation_count"]==2 && !out["metrics"].contains("hold_A") && !out["metrics"].contains("fire_hold"),
        "初始held只留UP不虚构DOWN或保持");
    ManualIntervalsAccumulator broken;
    broken.consume(event(1,0)); broken.consume(event(2,10,2));
    auto missing=event(4,30); broken.consume(missing); broken.consume(event(5,40,8)); broken.consume(event(6,50));
    out=broken.snapshot();
    check(!out["metrics"].contains("hold_A") && !out["metrics"].contains("reverse_interval_A_D") &&
        out["metrics"]["hold_D"]["mean_ms"]==10.,"缺序切段不跨gap配对且新已知边沿可统计");
    auto epoch=event(1,60,2); epoch.epoch=2; broken.consume(epoch);
    auto release=event(2,70); release.epoch=2; broken.consume(release);
    check(!broken.snapshot()["metrics"].contains("hold_A"),"新epoch的初始held不能补起点");
    auto invalid=event(3,80); invalid.epoch=2; invalid.state_valid=false; broken.consume(invalid);
    check(broken.snapshot()["invalid_events"]==3,"缺序、epoch和无效报告分别记录");
    ManualIntervalsAccumulator retained; retained.consume(event(1,0));
    for(std::uint64_t i=0;i<1600;++i) {
        retained.consume(event(i*2+2,static_cast<std::int64_t>(i*10+1),0,true));
        retained.consume(event(i*2+3,static_cast<std::int64_t>(i*10+6)));
    }
    out=retained.snapshot();
    check(out["operation_count"]==3200 && out["operations"].size()==3000 &&
        out["metrics"]["fire_hold"]["count"]==1600 && out["metrics"]["fire_hold"]["samples"].size()==300 &&
        out["metrics"]["fire_hold"]["mean_ms"]==5.,"保留窗口有界但统计分母覆盖全量");
    retained.gap(); auto same=event(3202,17000); retained.consume(same);
    retained.consume(same);
    check(retained.snapshot()["gap_count"]==2,"显式缺口和重复序号各自切段");
}
}
int main() { try { tests(); std::cout<<"人工操作间隔统计专项通过\n"; return 0; }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; } }
