#include "movement/recording.h"
#include <algorithm>
#include <array>
#include <map>
#include <stdexcept>

namespace movement::recording {
namespace {
using Kind = MovementTraceEvent::Kind;
int physical_keys(const InputReportEvent& input) {
    int keys = input.wasd_mask;
    if (std::find(input.keyboard_usages.begin(), input.keyboard_usages.end(), 0x2c) != input.keyboard_usages.end()) keys |= 16;
    if (input.keyboard_modifiers & 1) keys |= 32;
    if (input.keyboard_modifiers & 2) keys |= 64;
    return keys;
}
int software_keys(const MovementTraceEvent& event) {
    return event.wasd | (event.space ? 16 : 0) | (event.ctrl ? 32 : 0) | (event.shift ? 64 : 0);
}
const char* kind_name(Kind kind) {
    switch (kind) {
    case Kind::INPUT: return "physical_report";
    case Kind::MOVE: return "software_move";
    case Kind::KEYBOARD: return "software_keyboard";
    case Kind::MASK: return "physical_mask";
    case Kind::WHEEL: return "software_wheel";
    }
    return "unknown";
}
int delta16(int next, int previous) {
    const unsigned value = static_cast<unsigned>(next - previous) & 65535;
    return value >= 32768 ? static_cast<int>(value) - 65536 : static_cast<int>(value);
}
}
nlohmann::json trace_json(const std::vector<MovementTraceEvent>& events, std::int64_t begin_ns,
        std::int64_t end_ns, bool gap, int motion_mode) {
    auto list = nlohmann::json::array();
    for (const auto& event : events) {
        nlohmann::json item{{"type", kind_name(event.kind)}, {"request_ns", event.at_ns - begin_ns},
            {"completed_ns", event.completed_ns ? event.completed_ns - begin_ns : 0}};
        if (event.kind == Kind::INPUT) {
            item["keys"] = physical_keys(event.input);
            item["valid"] = event.input.state_valid;
            item["raw_x"] = event.input.raw_x; item["raw_y"] = event.input.raw_y;
            item["raw_wheel"] = event.input.raw_wheel;
            item["baseline"] = event.baseline;
            item["physical_source_verified"] = event.input.physical_source_verified;
        } else {
            item["acknowledged"] = event.acknowledged;
            if (event.kind == Kind::KEYBOARD) item["keys"] = software_keys(event);
            if (event.kind == Kind::MASK) { item["wasd"] = event.wasd; item["masked"] = event.masked; }
            if (event.kind == Kind::MOVE) { item["dx"] = event.dx; item["dy"] = event.dy; }
            if (event.kind == Kind::WHEEL) item["wheel"] = event.wheel;
        }
        list.push_back(std::move(item));
    }
    return {{"schema_version", 1}, {"duration_ns", end_ns - begin_ns}, {"gap", gap},
        {"source_loss_verifiable", false},
        {"motion_interpretation", motion_mode == 1 ? "relative" : motion_mode == 2 ? "cumulative" : "raw_only"},
        {"timing_domain", "host_receive_and_command_receipt"}, {"game_validation_passed", false},
        {"events", std::move(list)}};
}
Result derive(const std::vector<MovementTraceEvent>& source, std::int64_t begin_ns,
        std::int64_t end_ns, bool gap, int motion_mode) {
    Result result;
    try {
        if (gap) throw std::runtime_error("录制存在缺口或超过容量，原始记录保留，未生成不完整回放");
        if (motion_mode < 1 || motion_mode > 2) throw std::runtime_error("已保存原始录制；仅原始模式不生成回放方案");
        if (end_ns <= begin_ns || end_ns - begin_ns > 60000000000LL)
            throw std::runtime_error("录制时长无效或超过60秒");
        const int end_ms = static_cast<int>((end_ns - begin_ns + 999999) / 1000000);
        auto events = source;
        const auto stamp = [](const MovementTraceEvent& e) { return e.kind == Kind::INPUT ? e.at_ns : std::max(e.at_ns,e.completed_ns); };
        std::stable_sort(events.begin(),events.end(),[&](const auto& a,const auto& b) {return stamp(a)<stamp(b);});
        auto& plan = result.plan;
        plan.name = "录制身法"; plan.mask.enabled = false;
        plan.notes = "按所选鼠标报告模式派生的接收域候选；鼠标每10ms合并，完整请求/回执及原始报告另存。按monitor为物理输入解释；若固件混入软件输出会重复计数，来源与游戏效果未实机标定。";
        std::array<int,7> physical_start{},software_start{};
        std::array<int,4> mask_start{};
        physical_start.fill(-1); software_start.fill(-1); mask_start.fill(-1);
        int physical = 0,software = 0,masks = 0;
        bool baseline = false;
        int previous_x=0,previous_y=0,previous_wheel=0;
        std::map<int,std::array<int,2>> motion;
        const auto key_edges = [&](int next,int& current,auto& starts,int at,bool is_physical,bool is_mask) {
            for (std::size_t i=0;i<starts.size();++i) {
                const int bit=1<<i;
                if ((current&bit) && !(next&bit)) {
                    timeline::Action action;
                    action.kind=is_mask?timeline::ActionKind::MASK:timeline::ActionKind::KEY;
                    action.key=static_cast<timeline::Key>(i); action.start_ms=starts[i];
                    action.duration_ms=std::max(1,at-starts[i]); action.physical=is_physical;
                    if(action.start_ms+action.duration_ms<=60000)plan.actions.push_back(action);
                }
                if (!(current&bit) && (next&bit)) starts[i]=at;
            }
            current=next;
        };
        const auto wheel_action = [&](int wheel,int at) {
            if (!wheel) return;
            timeline::Action action; action.kind=timeline::ActionKind::WHEEL;
            action.start_ms=at; action.duration_ms=1; action.wheel=wheel;
            plan.actions.push_back(action);
        };
        for(const auto& event:events) {
            if(event.kind!=Kind::INPUT && event.at_ns>=begin_ns && event.at_ns<=end_ns && event.completed_ns>end_ns)
                throw std::runtime_error("录制末尾仍有未结束命令，原始轨迹保留");
            const auto timestamp=stamp(event);
            if(timestamp<begin_ns || timestamp>end_ns) continue;
            const int at=static_cast<int>((timestamp-begin_ns)/1000000);
            if(event.kind==Kind::INPUT) {
                if(!event.input.state_valid) throw std::runtime_error("物理输入状态无效，未生成回放");
                key_edges(physical_keys(event.input),physical,physical_start,at,true,false);
                if(baseline && !event.baseline) {
                    auto& bucket=motion[(at/10)*10];
                    bucket[0]+=motion_mode==1?event.input.raw_x:delta16(event.input.raw_x,previous_x);
                    bucket[1]+=motion_mode==1?event.input.raw_y:delta16(event.input.raw_y,previous_y);
                    wheel_action(motion_mode==1?event.input.raw_wheel:delta16(event.input.raw_wheel,previous_wheel),at);
                } else if(!event.baseline && (event.input.raw_x || event.input.raw_y || event.input.raw_wheel))
                    throw std::runtime_error("录制缺少起点基线，首包包含位移或滚轮，原始数据保留");
                baseline=true;previous_x=event.input.raw_x;previous_y=event.input.raw_y;previous_wheel=event.input.raw_wheel;
            } else {
                if(!event.acknowledged) throw std::runtime_error("软件命令未确认，原始轨迹保留，未生成回放");
                if(event.kind==Kind::KEYBOARD)key_edges(software_keys(event),software,software_start,at,false,false);
                if(event.kind==Kind::MASK)key_edges(event.masked?(masks|event.wasd):(masks&~event.wasd),masks,mask_start,at,false,true);
                if(event.kind==Kind::MOVE) {auto& bucket=motion[(at/10)*10];bucket[0]+=event.dx;bucket[1]+=event.dy;}
                if(event.kind==Kind::WHEEL)wheel_action(event.wheel,at);
            }
        }
        if(!baseline)throw std::runtime_error("没有有效物理输入基线，原始轨迹保留");
        if(software || masks)throw std::runtime_error("录制结束时身法尚未释放或恢复屏蔽，请在动作完成后停止录制");
        key_edges(0,physical,physical_start,end_ms,true,false);
        for(const auto& [at,counts]:motion) {
            if(!counts[0]&&!counts[1])continue;
            timeline::Action action;action.kind=timeline::ActionKind::MOVE;
            action.start_ms=at;action.duration_ms=std::max(1,std::min(10,end_ms-at));
            action.dx=counts[0];action.dy=counts[1];plan.actions.push_back(action);
        }
        std::stable_sort(plan.actions.begin(),plan.actions.end(),[](const auto& a,const auto& b){return a.start_ms<b.start_ms;});
        if(!timeline::validate_plan(plan,result.error))return result;
        result.complete=true;
    }catch(const std::exception& e){result.error=e.what();}
    return result;
}
}
