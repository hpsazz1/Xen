#include "recoil/recoil.h"
#include <cmath>
#include <iostream>
#include <limits>
namespace {
int failures=0;
void expect(bool b,const char* message){if(!b){++failures;std::cerr<<message<<'\n';}}
auto time(double ms){return RecoilTime{}+std::chrono::duration_cast<RecoilClock::duration>(std::chrono::duration<double,std::milli>(1000+ms));}
auto profile(){
    auto p=std::make_shared<RecoilProfile>();p->id="synthetic";p->weapon_id="synthetic_weapon";
    p->state=RecoilProfileState::CALIBRATED;p->phase_tolerance_ms=20;p->recovery_ms=50;
    p->source.sha256=std::string(64,'a');p->source.source_unit="synthetic_device_counts";
    p->calibration={"synthetic","fake","synthetic","synthetic:test_only",1.0};
    p->points={{0,0,0},{10,0.4,0.6},{20,0.8,1.2},{30,1.2,1.8},{40,1.6,2.4}};return p;
}
RecoilInput input(){RecoilInput i;i.enabled=i.healthy=i.focused=i.permission=i.profile_conditions_match=true;
    i.device_epoch=i.weapon_generation=1;i.profile=profile();return i;}
void acknowledge(RecoilController& c,const RecoilDecision& d,double ms){
    if(d.has_intent)c.acknowledge({d.intent.command_id,RecoilReceiptStatus::ACKNOWLEDGED,time(ms)},time(ms));
}
void schema_and_compile(){
    auto p=*profile();std::string error;expect(validate_recoil_profile(p,error),"合成profile合法");
    auto text=serialize_recoil_profile(p);RecoilProfile roundtrip;
    expect(load_recoil_profile(text,roundtrip,error)&&serialize_recoil_profile(roundtrip)==text,"schema往返确定性");
    p.points[1].time_ms=0;expect(!validate_recoil_profile(p,error),"重复时间拒绝");
    p=*profile();p.unit="pixels";expect(!validate_recoil_profile(p,error),"错误单位拒绝");
    p=*profile();p.points[1].x_counts=std::numeric_limits<double>::quiet_NaN();expect(!validate_recoil_profile(p,error),"NaN拒绝");
    p=*profile();p.calibration.evidence.clear();expect(!validate_recoil_profile(p,error),"伪校准缺证据拒绝");
    p=*profile();RecoilProfile tuned;
    expect(compile_recoil_profile(p,{2,1,5,2},tuned,error),"时间与单轴增益编译");
    expect(tuned.points[1].time_ms==5&&tuned.points[2].time_ms==15&&tuned.points.back().time_ms==75,"时间缩放固定首非零节点起点");
    expect(sample_recoil_profile(tuned,4).x_counts==0,"开始偏移前必须保持零而不是提前慢拉");
    expect(tuned.points.back().x_counts==3.2&&tuned.points.back().y_counts==2.4,"增益只影响本轴且时间不改总位移");
    expect(tuned.state==RecoilProfileState::SCHEMA_VALID&&!tuned.phase_tolerance_ms&&tuned.calibration.evidence.empty(),"调参降级候选");
    expect(!compile_recoil_profile(p,{1,1,-20,1},tuned,error),"禁止射击前补偿");
    expect(std::abs(sample_recoil_profile(p,15).x_counts-0.6)<1e-12,"分段线性求值");
    expect(sample_recoil_profile(p,999).x_counts==1.6,"尾部不外推");
}
void counts_and_phase(){
    auto i=input();RecoilController c;i.held=true;
    expect(!c.advance(i,time(0)).has_intent,"启动已持键不开始");
    i.held=false;c.advance(i,time(1));i.held=true;c.advance(i,time(2));
    for(int t=12;t<=42;t+=10){auto d=c.advance(i,time(t));acknowledge(c,d,t);}
    auto s=c.snapshot();expect(s.confirmed_x==1&&s.confirmed_y==2,"唯一量化保留fraction累计");
    expect(std::abs(s.discarded_x-0.6)<1e-12&&std::abs(s.discarded_y-0.4)<1e-12,"耗尽仅丢亚count余数");
    expect(!c.advance(i,time(52)).has_intent&&c.snapshot().session_id==1,"耗尽长按不循环");
    i.held=false;c.advance(i,time(53));i.held=true;
    expect(c.advance(i,time(54)).snapshot.reason==RecoilReason::RESET_UNVERIFIED,"恢复不足不重启");
    i.held=false;c.advance(i,time(104));i.held=true;c.advance(i,time(105));
    expect(c.snapshot().session_id==2,"恢复已知且新边沿可重启");

    auto j=input();RecoilController late;late.advance(j,time(0));j.held=true;late.advance(j,time(1));
    expect(late.advance(j,time(25)).snapshot.reason==RecoilReason::LATE,"超相位容差取消而非追赶");
    expect(!late.advance(j,time(26)).has_intent,"迟到不压缩补发");
}
void receipts_and_context(){
    auto i=input();RecoilController c;c.advance(i,time(0));i.held=true;c.advance(i,time(1));
    auto d=c.advance(i,time(21));expect(d.has_intent,"形成有界意图");
    expect(!c.advance(i,time(22)).has_intent,"未决不重复发送");
    expect(c.snapshot().confirmed_y==0,"ACK前无执行记账");
    c.cancel(RecoilReason::CANCELED,time(22));acknowledge(c,d,23);
    expect(c.snapshot().confirmed_y==1&&!c.advance(i,time(24)).has_intent,"取消后迟到ACK只记实际账不复活");
    auto j=input();RecoilController unknown;unknown.advance(j,time(0));j.held=true;unknown.advance(j,time(1));
    auto u=unknown.advance(j,time(21));unknown.acknowledge({u.intent.command_id,RecoilReceiptStatus::UNKNOWN,time(22)},time(22));
    expect(unknown.snapshot().faulted&&unknown.snapshot().confirmed_y==0&&unknown.snapshot().unknown_y==1,"未知回执单列不记成功");
    j.held=false;unknown.advance(j,time(23));j.held=true;expect(!unknown.advance(j,time(24)).has_intent,"未知fault不因按键复活");
    auto k=input();RecoilController change;change.advance(k,time(0));k.held=true;change.advance(k,time(1));
    k.weapon_generation=2;expect(!change.advance(k,time(11)).has_intent,"换枪长按不从头开始");
    k.focused=false;change.advance(k,time(12));k.focused=true;expect(!change.advance(k,time(13)).has_intent,"失焦恢复需新边沿");
    auto imported=input();auto candidate=profile();candidate->state=RecoilProfileState::IMPORTED;imported.profile=candidate;
    RecoilController blocked;expect(blocked.advance(imported,time(0)).snapshot.reason==RecoilReason::UNCALIBRATED,"导入不等于可执行");
    auto timed=input();RecoilController start;start.advance(timed,time(0));timed.held=true;timed.firing_started_at=time(1);
    expect(start.advance(timed,time(21)).has_intent,"明确down时刻不能延迟为worker当前时刻");
    auto expired=input();RecoilController rejected;rejected.advance(expired,time(0));expired.held=true;expired.firing_started_at=time(1);
    expect(rejected.advance(expired,time(22)).snapshot.reason==RecoilReason::LATE,"起始证据过期不追赶");
}
void candidate_validation_keeps_production_closed(){
    RecoilProfile candidate;
    candidate.id="offline_candidate";candidate.weapon_id="offline_weapon";
    candidate.state=RecoilProfileState::SCHEMA_VALID;
    candidate.points={{0,0,0},{10,0.4,-0.6},{20,0.8,-1.2},{30,1.2,-1.8},{40,1.6,-2.4}};
    const auto before=serialize_recoil_profile(candidate);
    RecoilCandidateReplayReport report;std::string error;
    const bool valid=validate_recoil_candidate_execution(candidate,{},report,error);
    expect(valid,"未校准候选必须通过真实执行器的软件回放入口验证");
    if(valid){
        expect(report.terminal.confirmed_x==1&&report.terminal.confirmed_y==-2,"候选回放报告来自实际整数ACK账本");
        expect(std::abs(report.terminal.discarded_x-0.6)<1e-9&&std::abs(report.terminal.discarded_y+0.4)<1e-9,"候选尾部仅丢弃正负亚count余数");
    }
    expect(serialize_recoil_profile(candidate)==before,"软件回放不能补写相位或校准证据");
    auto production=input();production.profile=std::make_shared<RecoilProfile>(candidate);
    RecoilController controller;
    expect(controller.advance(production,time(0)).snapshot.reason==RecoilReason::UNCALIBRATED,"软件验证后生产入口仍拒绝未校准候选");
    production.held=true;
    expect(!controller.advance(production,time(1)).has_intent,"软件验证结果不能变成生产开火许可");
}
void candidate_execution_rejects_compiler_only_limits(){
    RecoilProfile candidate;candidate.id="limit_candidate";candidate.weapon_id="offline_weapon";
    candidate.state=RecoilProfileState::SCHEMA_VALID;
    candidate.points={{0,0,0},{1,32767.5,0},{40,32767.5,0}};
    RecoilProfile compiled;RecoilCandidateReplayReport report;std::string error;
    expect(compile_recoil_profile(candidate,{},compiled,error),"结构合法的超单命令候选能通过旧compiler-only入口");
    expect(!validate_recoil_candidate_execution(candidate,{},report,error)&&error.find("limit")!=std::string::npos,
        "候选执行回放必须拒绝超过生产单轴命令上限的实际增量");
    expect(!report.validated,"执行被拒绝不能留软件验证成功状态");
    candidate.points[1].x_counts=candidate.points[2].x_counts=32767;
    expect(validate_recoil_candidate_execution(candidate,{},report,error),"现有单命令上限等号保持可执行");
    expect(report.max_abs_command_axis_counts==32767&&report.command_axis_limit_counts==32767,
        "回放记录实际最大命令及现有上限");
}
void candidate_replay_checks_faults_and_bounds(){
    RecoilProfile candidate;candidate.id="replay_candidate";candidate.weapon_id="offline_weapon";
    candidate.state=RecoilProfileState::SCHEMA_VALID;
    candidate.points={{0,0,0},{1.00000005,1.25,-2.75}};
    RecoilCandidateReplayReport report;std::string error;
    const bool valid=validate_recoil_candidate_execution(candidate,{},report,error);
    expect(valid,"非整数tick尾部应完整回放结束");
    if(valid){
        expect(report.validated&&report.step_ms==1&&report.phase_budget_ms==20,"软件调度条件必须显式报告且不取代实测相位");
        expect(report.terminal.phase==RecoilPhase::EXHAUSTED&&report.terminal.confirmed_x==1&&report.terminal.confirmed_y==-2,
            "最后一个可表示tick必须覆盖完整尾部");
        expect(report.tail_checked&&report.phase_edge_checked&&report.deadline_checked&&report.cancellation_checked&&
            report.unknown_receipt_checked&&report.not_sent_checked,"非零候选必须实际完成边界和回执故障回放");
        expect(report.advance_calls>0&&report.advance_calls<=report.advance_limit,"候选回放报告有界实际工作量");
    }
    expect(!validate_recoil_candidate_execution(candidate,{0,20},report,error),"零步长拒绝而不是进入无限回放");
    expect(!validate_recoil_candidate_execution(candidate,{21,20},report,error),"步长不能超过软件相位预算");
    candidate.points.back().time_ms=40;
    expect(!validate_recoil_candidate_execution(candidate,{0.0001,20},report,error)&&error.find("工作量")!=std::string::npos&&
        report.advance_calls==0,"可表示但工作量过大的步长须在循环前拒绝");
    candidate.points.back().time_ms=60000.01;
    expect(!validate_recoil_candidate_execution(candidate,{},report,error),"超过既有有限时长的候选拒绝");
}
void candidate_large_zero_curve_is_bounded(){
    RecoilProfile candidate;candidate.id="large_zero_candidate";candidate.weapon_id="offline_weapon";
    candidate.state=RecoilProfileState::SCHEMA_VALID;
    for(int i=0;i<100000;++i)candidate.points.push_back({60000.0*i/99999,0,0});
    RecoilCandidateReplayReport report;std::string error;
    const bool valid=validate_recoil_candidate_execution(candidate,{},report,error);
    expect(valid,"schema上界的零曲线在固定工作量内完成");
    if(valid){
        expect(report.acknowledged_commands==0&&report.acknowledged_l1_counts==0,"零曲线不能捏造模拟命令");
        expect(report.tail_checked&&report.deadline_checked&&report.cancellation_checked,"零曲线仍检查尾部时限和取消");
        expect(!report.unknown_receipt_checked&&!report.not_sent_checked,"零意图候选不能冒称检查了回执故障");
        expect(report.advance_calls<70000,"大曲线不对每个节点重新遍历整条弹序");
    }
}
}
int main(){schema_and_compile();counts_and_phase();receipts_and_context();candidate_validation_keeps_production_closed();
    candidate_execution_rejects_compiler_only_limits();candidate_replay_checks_faults_and_bounds();candidate_large_zero_curve_is_bounded();return failures?1:0;}
