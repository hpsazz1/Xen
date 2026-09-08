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
int failures=0;
void expect(bool ok,const std::string& text) {
    if(!ok) { ++failures;std::cerr<<"FAIL "<<text<<'\n'; }
}
void close(float actual,float expected,const std::string& text) {
    expect(std::isfinite(actual)&&std::isfinite(expected)&&std::fabs(actual-expected)<=.003f,
        text+" actual="+std::to_string(actual)+" expected="+std::to_string(expected));
}
AimConfig config() {
    AimConfig c;c.min_confirmed_hits=1;c.counts_per_pixel_x=.2216375f;
    c.enable_delay_compensation=true;c.control_delay_ms=15;c.max_delay_compensation_ms=44;
    c.enable_prediction=false;c.max_counts_per_frame=14;return c;
}
AimFrame frame(int i,float left,float right,float camera,int mode=0) {
    AimFrame f;f.sequence=100+i;f.observation_epoch=17;
    f.captured_at=std::chrono::steady_clock::time_point{std::chrono::seconds(10)+std::chrono::milliseconds(8*i)};
    f.control_at=f.captured_at+std::chrono::milliseconds(12+(i>16&&i%2?3:0));
    f.roi_width=f.roi_height=320;f.control_center_x=f.control_center_y=160;f.lock_active=true;
    f.detections.push_back({left,140,right,200,.95f,0});
    f.background_motion_x={AimBackgroundMotionStatus::VALID,f.sequence-1,f.sequence,
        f.captured_at-std::chrono::milliseconds(8),f.captured_at,17,camera,.9f,0,2};
    if(mode==1)f.background_motion_x={};
    if(mode==2)f.background_motion_x.status=AimBackgroundMotionStatus::LOW_TEXTURE;
    if(mode==3)f.background_motion_x.captured_at+=std::chrono::nanoseconds(1);
    if(mode==4)f.background_motion_x.dx_roi_pixels=(std::numeric_limits<float>::max)();
    return f;
}
float v(const AimResult& r) {return r.control.observer_target_velocity_x_counts_per_second;}
void valid(const AimResult& r,const std::string& name,AimBackgroundMotionUse use) {
    expect(r.status==AimStatus::SUCCESS&&r.has_target&&r.control.evaluated&&
        r.target.matched_observation_valid&&!r.target.predicted,name+" 有效公共观测");
    expect(r.control.background_motion_use_x==use,name+" use="+AimBackgroundMotionUseName(r.control.background_motion_use_x));
}
void row(const std::string& name,int i,float input_bg,const AimResult& r) {
    const auto& c=r.control;
    std::cout<<name<<','<<i<<','<<input_bg<<','<<AimBackgroundMotionUseName(c.background_motion_use_x)<<','
        <<v(r)<<','<<c.observer_camera_motion_x_source_pixels<<','
        <<c.reverse_translation_raw_left_x_roi_pixels<<','<<c.reverse_translation_raw_right_x_roi_pixels<<','
        <<c.reverse_translation_control_evidence_x<<','<<c.delayed_command_x_counts<<','
        <<c.controller_dt_ms<<','<<r.command.dx_counts<<','<<r.command.dy_counts<<'\n';
}
float seed(Aim& aim,int direction) {
    AimResult r;
    for(int i=0;i<=16;++i) {
        r=aim.process(frame(i,150.0f+direction*i,190.0f+direction*i,0));
        if(i>0) valid(r,"seed",AimBackgroundMotionUse::CONSUMED);
    }
    close(v(r),direction*124.998093f,"已知同一非零前态");
    return v(r);
}
void covariance() {
    for(int direction:{1,-1}) {
        std::array<std::array<float,4>,3> velocities{};
        for(int shift=0;shift<3;++shift) {
            Aim aim(config());const float before=seed(aim,direction);
            const float camera=direction*(shift==0?0:shift==1?-.89f:.5f);
            float left=150.0f+16*direction,right=190.0f+16*direction;
            const float world_left=direction>0?.9f:-1.1f;
            const float world_right=direction>0?1.1f:-.9f;
            const std::string name="covariance_"+std::to_string(direction)+"_"+std::to_string(shift);
            for(int j=0;j<4;++j) {
                left+=world_left+camera;right+=world_right+camera;
                const auto r=aim.process(frame(17+j,left,right,camera));
                valid(r,name,AimBackgroundMotionUse::CONSUMED);row(name,j,camera,r);
                const auto& c=r.control;
                expect(c.reverse_translation_raw_left_x_roi_pixels*c.reverse_translation_raw_right_x_roi_pixels>0,name+" raw严格同号");
                close(c.reverse_translation_raw_left_x_roi_pixels-camera,world_left,name+" 原世界左边区间相同");
                close(c.reverse_translation_raw_right_x_roi_pixels-camera,world_right,name+" 原世界右边区间相同");
                close(c.observer_camera_motion_x_source_pixels,camera,name+" 实测camera不得按raw几何衰减");
                close(v(r),before,name+" 世界先验仍在区间内应保持");
                velocities[shift][j]=v(r);
            }
        }
        for(int shift=1;shift<3;++shift)for(int j=0;j<4;++j)
            close(velocities[shift][j],velocities[0][j],"共同相机平移的世界observer协变性");
    }
}
std::vector<AimResult> fallback(int mode) {
    Aim aim(config());seed(aim,1);
    std::vector<AimResult> rows;
    float left=166,right=206;
    bool low_weight_nonzero_model=false;
    for(int j=0;j<17;++j) {
        left+=.01f;right+=.21f;
        const int actual_mode=j==16?0:mode;
        const auto r=aim.process(frame(17+j,left,right,0,actual_mode));
        const auto expected=actual_mode==0?AimBackgroundMotionUse::CONSUMED:
            mode==1?AimBackgroundMotionUse::MISSING:mode==3?AimBackgroundMotionUse::PAIR_MISMATCH:AimBackgroundMotionUse::INVALID;
        valid(r,"fallback",expected);row("fallback_"+std::to_string(mode),j,0,r);
        if(j<16&&std::fabs(r.control.delayed_command_x_counts)>.1f&&
            std::fabs(r.control.reverse_translation_control_evidence_x)<.5f)low_weight_nonzero_model=true;
        rows.push_back(r);
    }
    expect(low_weight_nonzero_model,"fallback夹具必须实际覆盖非零命令模型和低raw一致性");
    return rows;
}
void fallback_contract() {
    const auto missing=fallback(1);
    for(int mode=2;mode<=4;++mode) {
        const auto invalid=fallback(mode);
        for(int j=0;j<17;++j) {
            close(v(invalid[j]),v(missing[j]),"fallback及恢复VALID后observer状态兼容");
            close(invalid[j].control.observer_camera_motion_x_source_pixels,
                missing[j].control.observer_camera_motion_x_source_pixels,"fallback完整恢复旧camera权重");
            expect(invalid[j].command.dx_counts==missing[j].command.dx_counts&&
                invalid[j].command.dy_counts==missing[j].command.dy_counts,"fallback X/Y命令兼容");
        }
    }
}
void opposed_guard() {
    Aim aim(config());const float before=seed(aim,1);
    const auto r=aim.process(frame(17,165,207,-.89f));
    valid(r,"opposed",AimBackgroundMotionUse::OBSERVATION_UNAVAILABLE);
    close(v(r),before,"raw异向时仍保持observer前态");row("opposed",0,-.89f,r);
}
}
int main() {
    std::cout<<std::setprecision(9)<<"scenario,row,input_bg,use,velocity,effective_camera,raw_left,raw_right,evidence,delayed_q,control_dt_ms,qx,qy\n";
    covariance();fallback_contract();opposed_guard();
    std::cout<<"failures,"<<failures<<'\n';return failures?1:0;
}
