// SPDX-License-Identifier: GPL-2.0-or-later
#include "marker_internal.h"
#include <iostream>
#include <string>

int main() {
    using namespace xen::recorder::detail;
    int failures=0;
    const auto check=[&](bool success,const char* reason) {
        if (!success) { std::cerr<<reason<<'\n'; ++failures; }
    };
    {
        MainRenderClock clock;
        auto timing=clock.candidate(100,true);
        check(!timing.encoder_cts && timing.status==FrameTimingStatus::NO_COMPLETED_FRAME,
            "GPU首帧没有已完成主合成样本，必须未知");
        clock.completed(100);
        timing=clock.candidate(137,true);
        check(timing.encoder_cts==100 && timing.status==FrameTimingStatus::VALID,
            "GPU当前内容对应真实上一轮完成CTS");
        clock.completed(137);
        timing=clock.candidate(901,true);
        check(timing.encoder_cts==137,"不等间隔使用实际完成CTS，不能减固定一帧");
        timing=clock.candidate(901,true);
        check(!timing.encoder_cts && timing.status==FrameTimingStatus::DUPLICATE_CURRENT,
            "同内容CTS重复source渲染不能猜新编码CTS");
        timing=clock.candidate(950,true);
        check(!timing.encoder_cts && timing.status==FrameTimingStatus::COMPLETION_GAP,
            "上一内容已渲染但主合成完成回调缺失时映射未知");
        clock.completed(950);
        timing=clock.candidate(1100,true);
        check(timing.encoder_cts==950,"缺口后取得新实际完成基准可以恢复");
        clock.completed(1100); clock.completed(1100);
        timing=clock.candidate(1200,true);
        check(!timing.encoder_cts && timing.status==FrameTimingStatus::INVALID_COMPLETION,
            "重复完成CTS不能静默充当可信上一轮");
        clock.completed(1200);
        timing=clock.candidate(1000,true);
        check(!timing.encoder_cts && timing.status==FrameTimingStatus::ROLLBACK,
            "内容时钟倒退切断旧代际");
        clock.completed(1000);
        timing=clock.candidate(1050,true);
        check(timing.encoder_cts==1000,"倒退后只承接新的真实完成基准");
        clock.reset();
        check(!clock.candidate(2000,true).encoder_cts,"重置/重新录制不能继承旧previous CTS");
        clock.reset();
        check(clock.candidate(3000,false).encoder_cts==3000,"raw编码模式保留当前内容CTS");
        clock.completed(3000);
        check(!clock.candidate(0,true).encoder_cts,"零内容时戳明确未知且清空代际");
        check(!clock.candidate(4000,true).encoder_cts,"零时戳后不复用旧完成帧");
    }
    {
        MarkerLifecycle state;
        check(!state.request("",100,2000) && !state.request("SYNC newline\n",100,2000) &&
            !state.request(std::string(97,'A'),100,2000) && !state.request("SYNC-1",0,2000),
            "无效编号和未知请求时钟不能成为可渲染标记");
        check(!state.request("SYNC-1",100,0) && !state.request("SYNC-1",100,10001),
            "可见时长必须有界且非零");
        check(state.request("SYNC-1",1000000000LL,2000),"接受首个标记");
        check(!state.request("SYNC-1",1100000000LL,2000) && !state.request("SYNC-2",1100000000LL,2000),
            "等待和可见期间不替换编号，也不接受重复请求");
        auto frame=state.frame(1500000000LL,77);
        check(frame.draw && frame.record && frame.first,"第一次实际帧同时产生首个候选");
        check(std::string(state.id().data())=="SYNC-1","候选保留请求原编号");
        frame=state.frame(1500001000LL,77);
        check(frame.draw && !frame.record && !frame.first,"同CTS重复source render不重复写候选");
        frame=state.frame(1500002000LL,78);
        check(frame.draw && frame.record && !frame.first,"之后实际帧仍保留编码匹配候选，允许首帧编码缺失");
        check(state.frame(3499999999LL,79).draw,"两秒可见时长从首次实际渲染起算");
        frame=state.frame(3500000000LL,80);
        check(!frame.draw && !frame.record && !state.busy(),"精确到可见结束边界不再渲染编号");
        check(!state.request("SYNC-1",3500000001LL,2000),"相邻重复编号在结束后仍拒绝");
        check(state.request("SYNC-2",3500000001LL,2000),"结束后新编号可接受");
        frame=state.frame(5500000001LL,81);
        check(frame.timed_out && !frame.draw && !frame.record && !state.busy(),
            "请求后两秒还没有实际帧则超时，不能伪造渲染或PTS");
        frame=state.frame(5500000002LL,82);
        check(!frame.timed_out && !frame.draw,"超时只发布一次，迟到帧不能复活旧标记");
    }
    {
        MarkerLifecycle state;
        check(state.request(std::string(96,'X'),1000,1),"最长96字符安全编号可接受");
        check(!state.frame(1001,0).draw && state.busy(),"无当前视频CTS时不提前消费首次显示");
        check(state.frame(999,1).timed_out && !state.busy(),"本机时间倒退明确失败，不延长等待");
    }
    {
        RenderQueue queue;
        RawRenderedMarker event; event.id[0]='S';
        for (std::uint64_t frame=1;frame<=700;++frame) {
            event.frame_time=frame; event.first=frame==1; queue.push(event);
        }
        check(queue.size()==512 && queue.dropped()==188,"生产者超出有限环时覆盖数准确可见");
        std::uint64_t expected=189;
        while (queue.pop(event)) check(event.frame_time==expected++,"覆盖后保留候选顺序连续且不伪造首帧");
        check(expected==701 && queue.size()==0 && queue.dropped()==188,"drain不清累计缺口，末尾不漏候选");
        check(!queue.pop(event),"空队列不重复交付旧事件");
        event.frame_time=701; queue.push(event);
        check(queue.pop(event) && event.frame_time==701 && queue.dropped()==188,"冷路径清空后继续录制仍保留累计缺口");
    }
    return failures ? 1 : 0;
}
