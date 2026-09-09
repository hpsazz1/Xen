#include "aim/aim.h"
#include <array>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {
int failures = 0;
constexpr float tolerance = 0.003f;
void expect(bool ok, const std::string& label) {
    if (!ok) { ++failures; std::cerr << "FAIL " << label << '\n'; }
}
void close(float a, float b, const std::string& label) {
    expect(std::isfinite(a) && std::isfinite(b) && std::fabs(a-b) <= tolerance,
           label + " actual=" + std::to_string(a) + " expected=" + std::to_string(b));
}
AimConfig config() {
    AimConfig c;
    c.min_confirmed_hits=1;
    c.counts_per_pixel_x=0.2216375f;
    c.enable_delay_compensation=true;
    c.control_delay_ms=15;
    c.max_delay_compensation_ms=44;
    c.enable_prediction=false;
    c.max_counts_per_frame=14;
    return c;
}
AimFrame make_frame(int index, int source_ms, int previous_ms, int delay_ms,
                    float left, float right, float bg, int mode=0) {
    AimFrame f;
    f.sequence=100+index;
    f.captured_at=std::chrono::steady_clock::time_point{std::chrono::seconds(10)+std::chrono::milliseconds(source_ms)};
    f.control_at=f.captured_at+std::chrono::milliseconds(delay_ms);
    f.roi_width=f.roi_height=320;
    f.control_center_x=f.control_center_y=160;
    f.lock_active=true;
    f.observation_epoch=17;
    f.detections.push_back({left,140,right,200,0.95f,0});
    f.background_motion_x={AimBackgroundMotionStatus::VALID, f.sequence-1,f.sequence,
        std::chrono::steady_clock::time_point{std::chrono::seconds(10)+std::chrono::milliseconds(previous_ms)},
        f.captured_at,17,bg,0.9f,0,2};
    if (mode==1) f.background_motion_x={};
    if (mode==2) f.background_motion_x.status=AimBackgroundMotionStatus::LOW_TEXTURE;
    if (mode==3) f.background_motion_x.captured_at+=std::chrono::nanoseconds(1);
    if (mode==4) f.background_motion_x.dx_roi_pixels=(std::numeric_limits<float>::max)();
    return f;
}
float velocity(const AimResult& r) { return r.control.observer_target_velocity_x_counts_per_second; }
void usable(const AimResult& r, const std::string& label, bool consumed) {
    expect(r.status==AimStatus::SUCCESS && r.has_target && r.control.evaluated &&
        r.target.matched_observation_valid && !r.target.predicted,label+" 公共观测有效");
    if (consumed) expect(r.control.background_motion_use_x==AimBackgroundMotionUse::CONSUMED,label+" CONSUMED");
}
void trace(const std::string& name, int i, double source_dt, const AimResult& r) {
    std::cout << name << ',' << i << ',' << source_dt << ',' << r.control.controller_dt_ms << ','
        << AimBackgroundMotionUseName(r.control.background_motion_use_x) << ',' << velocity(r) << ','
        << r.control.observer_camera_motion_x_source_pixels << ','
        << r.control.reverse_translation_raw_left_x_roi_pixels << ','
        << r.control.reverse_translation_raw_right_x_roi_pixels << ','
        << r.control.modelled_response_x_counts << ',' << r.command.dx_counts << ',' << r.command.dy_counts << '\n';
}
std::vector<AimResult> rigid(const std::string& name, bool variable_source, bool jitter, int mode=0) {
    Aim aim(config());
    std::vector<AimResult> out;
    int source=0, last_delay=12;
    float left=160,right=200;
    out.push_back(aim.process(make_frame(0,0,-8,12,left,right,0,mode)));
    usable(out.back(),name+" seed",false);
    close(velocity(out.back()),0,name+" seed observer");
    constexpr std::array<int,4> intervals{4,12,8,16};
    for (int i=1;i<=16;++i) {
        const int dt=variable_source?intervals[(i-1)%4]:8;
        const int previous=source;
        source+=dt;
        const int delay=12+(jitter && i%2 ? (variable_source?2:3):0);
        const float motion=0.09375f*dt;
        left+=motion; right+=motion;
        const auto r=aim.process(make_frame(i,source,previous,delay,left,right,-0.03125f*dt,mode));
        usable(r,name+" row"+std::to_string(i),mode==0);
        close(r.control.controller_dt_ms,static_cast<float>(dt+delay-last_delay),name+" control dt 保持控制时间");
        trace(name,i,dt,r);
        out.push_back(r); last_delay=delay;
    }
    return out;
}
void rigid_contract(bool variable) {
    const std::string name=variable?"variable_source":"fixed_source";
    const auto constant=rigid(name+"_constant",variable,false);
    const auto jitter=rigid(name+"_jitter",variable,true);
    for (int i=1;i<=16;++i)
        close(velocity(jitter[i]),velocity(constant[i]),name+" observer 不受调度延迟污染 row"+std::to_string(i));
    // 两条输入只有到达时刻不同；同一源位移的斜率不能跟随调度抖动。
    // 控制间隔、积分和命令仍按各自控制时刻推进，不要求完整输出相同。
    for (int i=1;i<=16;++i)
        close(jitter[i].control.error_derivative_x_source_pixels_per_second,
              constant[i].control.error_derivative_x_source_pixels_per_second,
              name+" 同源位移的误差斜率不受到达抖动污染 row"+std::to_string(i));
    const std::array<float,4> golden=variable
        ?std::array<float,4>{125.0f/3,275.0f/3,325.0f/3,1075.0f/9}
        :std::array<float,4>{62.5f,93.75f,109.375f,117.1875f};
    for (int i=1;i<=4;++i) {
        close(velocity(constant[i]),golden[i-1],name+" 手算观测合同 constant row"+std::to_string(i));
        close(velocity(jitter[i]),golden[i-1],name+" 手算观测合同 jitter row"+std::to_string(i));
    }
}
void prior_contract() {
    std::array<float,2> after{};
    for (int variant=0;variant<2;++variant) {
        Aim aim(config());
        float left=160,right=200,before=0;
        for (int i=0;i<=16;++i) {
            const auto r=aim.process(make_frame(i,8*i,8*(i-1),12,left,right,0));
            if(i>0) usable(r,"projection seed",true);
            before=velocity(r); left+=1;right+=1;
        }
        // 上次原框为176/216，下一帧只提供0.5/1.5像素的同向区间。
        left-=0.5f; right+=0.5f;
        const auto r=aim.process(make_frame(17,136,128,variant?18:6,left,right,0));
        usable(r,"projection",true);
        close(r.control.controller_dt_ms,variant?14.0f:2.0f,"projection control dt");
        trace(variant?"prior_long":"prior_short",17,8,r);
        close(velocity(r),before,"先验在观测位移区间内时不得因control dt投影到端点");
        after[variant]=velocity(r);
    }
    close(after[0],after[1],"先验投影只依赖原始观测pair时间");
}
void fallback_contract() {
    const auto missing=rigid("missing",true,true,1);
    for (int mode=2;mode<=4;++mode) {
        const auto other=rigid(mode==2?"low_texture":mode==3?"pair_mismatch":"finite_overflow",true,true,mode);
        for(int i=1;i<=16;++i) {
            expect(other[i].control.background_motion_use_x==(mode==3?AimBackgroundMotionUse::PAIR_MISMATCH:AimBackgroundMotionUse::INVALID),"fallback 状态明确");
            close(velocity(other[i]),velocity(missing[i]),"fallback observer 与MISSING一致");
            close(other[i].control.observer_camera_motion_x_source_pixels,missing[i].control.observer_camera_motion_x_source_pixels,"fallback camera一致");
            expect(other[i].command.dx_counts==missing[i].command.dx_counts && other[i].command.dy_counts==missing[i].command.dy_counts,"fallback X/Y命令一致");
        }
    }
    for(int i=1;i<=16;++i) {
        const auto& c=missing[i].control;
        expect(c.background_motion_use_x==AimBackgroundMotionUse::MISSING,"MISSING 状态明确");
        const float dt=c.controller_dt_ms/1000;
        const float measured=(c.reverse_translation_raw_left_x_roi_pixels-c.observer_camera_motion_x_source_pixels)/dt;
        const float expected=velocity(missing[i-1])+dt/(0.008f+dt)*(measured-velocity(missing[i-1]));
        close(velocity(missing[i]),expected,"MISSING保持控制dt的既有更新合同");
    }
}
void step_and_full_interval_contract() {
    // 前16次都是1 px/8 ms，用已知非零前态检出只换分母却遗漏alpha。
    for (int variant=0;variant<4;++variant) {
        Aim aim(config());
        float before=0;
        for (int i=0;i<=16;++i) {
            const auto r=aim.process(make_frame(i,8*i,8*(i-1),12,160.0f+i,200.0f+i,0));
            if(i>0) usable(r,"step seed",true);
            before=velocity(r);
        }
        const double source_dt_ms=variant<2?8:variant==2?0.5:60;
        const int control_dt_ms=variant==0?2:variant==1?14:variant==2?2:66;
        const float displacement=variant<2?2:variant==2?0.0625f:7.5f;
        auto f=make_frame(17,136,128,12,176+displacement,216+displacement,0);
        f.captured_at=f.background_motion_x.previous_captured_at+
            std::chrono::microseconds(static_cast<long long>(source_dt_ms*1000));
        f.control_at=f.background_motion_x.previous_captured_at+
            std::chrono::milliseconds(12+control_dt_ms);
        f.background_motion_x.captured_at=f.captured_at;
        // 跳号不改变同一个原始观测端点；不得以帧号差重建dt。
        f.sequence=200;
        f.background_motion_x.sequence=200;
        const auto r=aim.process(f);
        usable(r,"step/full interval",true);
        close(r.control.controller_dt_ms,variant==3?50.0f:static_cast<float>(control_dt_ms),"controller dt 仍按原合同clamp");
        const float expected=variant<2?(before+250.0f)*0.5f:
            variant==2?(8.0f*before+0.5f*125.0f)/8.5f:(8.0f*before+60.0f*125.0f)/68.0f;
        close(velocity(r),expected,variant<2?"刚体速度阶跃的8ms alpha使用观测间隔":"完整原始观测间隔不得套用控制dt clamp");
        trace("step_or_unclamped_"+std::to_string(variant),17,source_dt_ms,r);
    }
}
}
int main() {
    std::cout<<std::setprecision(9)<<"scenario,row,source_dt_ms,control_dt_ms,use,velocity,camera,raw_left,raw_right,M,qx,qy\n";
    rigid_contract(false); rigid_contract(true); prior_contract(); step_and_full_interval_contract(); fallback_contract();
    std::cout<<"failures,"<<failures<<'\n';
    return failures?1:0;
}
