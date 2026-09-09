#include "aim/aim.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {
int failures = 0;
void expect(bool ok, const std::string& message) {
    if (!ok) { ++failures; std::cerr << "失败：" << message << '\n'; }
}
bool near(float a, float b) {
    return std::isfinite(a) && std::isfinite(b) && std::fabs(a-b) <= 0.0003f;
}
AimConfig frozen_config(bool delay, bool prediction, float fixed_delay = 15.0f) {
    // c5c4a85d 实际配置；显式赋值避免默认值修改使交叉测试悄然换基准。
    AimConfig c;
    c.person_class_ids = {0, 2}; c.head_class_ids = {1, 3};
    c.high_confidence = .25f; c.low_confidence = .10f;
    c.min_confirmed_hits = 2; c.max_lost_frames = 8;
    c.min_iou = .10f; c.max_center_distance = .25f;
    c.switch_margin = .20f; c.switch_confirm_frames = 3; c.switch_cooldown_frames = 5;
    c.acquisition_range_percent = 90; c.body_aim_height_ratio = .35f;
    c.body_aim_range_percent = 50; c.deadzone_pixels = 1.5f;
    c.smoothing = .475f; c.counts_per_pixel_x = .425f; c.counts_per_pixel_y = .4f;
    c.max_counts_per_frame = 14;
    c.enable_delay_compensation = delay; c.control_delay_ms = fixed_delay;
    c.max_delay_compensation_ms = 44; c.max_delay_compensation_percent = 15;
    c.enable_prediction = prediction; c.max_prediction_lead_percent = 35;
    c.predicted_gain = .5f;
    return c;
}
AimFrame frame_at(int i, int epoch = 1, bool age = true) {
    AimFrame f;
    f.sequence = static_cast<std::uint64_t>(i+1);
    f.observation_epoch = static_cast<std::uint64_t>(epoch);
    f.captured_at = std::chrono::steady_clock::time_point{} +
        std::chrono::seconds(100+epoch) + std::chrono::microseconds(4167*i);
    f.control_at = f.captured_at + std::chrono::milliseconds(age ? 9 : 0);
    f.roi_width = f.roi_height = 320;
    f.control_center_x = f.control_center_y = 160;
    f.source_pixels_per_roi_pixel_x = f.source_pixels_per_roi_pixel_y = 1;
    f.lock_active = i < 160 || i >= 168;
    // 连续平移、换向、短缺测、松键与重新锁定；无需设备且无生产速度门。
    const float travel = i < 120 ? i*.36f : 43.2f-(i-120)*.30f;
    const float x = 104+travel;
    const float y = 155+8*std::sin(i*.045f);
    if (i != 145 && i != 146)
        f.detections.push_back(Detection{x-20, y-28, x+20, y+52, .9f, 0});
    if (i > 0) {
        f.background_motion_x = {AimBackgroundMotionStatus::VALID,
            static_cast<std::uint64_t>(i), f.sequence,
            f.captured_at-std::chrono::microseconds(4167), f.captured_at,
            f.observation_epoch, .0f, .95f, .0f, 4};
    }
    return f;
}
void confirm(Aim& aim, const AimFrame& f, const AimResult& r) {
    if (!r.has_command) return;
    // Aim请求不是实际输出；模拟Runtime松键安全门确认零，不调用Mouse。
    expect(aim.record_backend_completed_command(f.sequence,
        f.control_at+std::chrono::microseconds(100),
        f.lock_active ? r.command.dx_counts : 0,
        f.lock_active ? r.command.dy_counts : 0), "backend账本必须接受本分支执行结果");
}
void contract(const AimConfig& c, const AimFrame& f, const AimResult& r) {
    expect(r.status == AimStatus::SUCCESS, "合法四组合帧不能被拒绝");
    expect(std::hypot(static_cast<float>(r.command.dx_counts),
                      static_cast<float>(r.command.dy_counts)) <= 14.0001f,
           "所有组合的二维请求都受14-count上限约束");
    if (!r.has_target) return;
    const auto& t = r.target;
    for (float value : {t.base_aim_x,t.base_aim_y,t.aim_x,t.aim_y,
         t.prediction_aim_x,t.prediction_aim_y,t.delay_compensated_aim_x,
         t.delay_compensated_aim_y,t.lead_x,t.lead_y,t.delay_compensation_x,
         t.delay_compensation_y,t.delay_compensation_ms,
         r.control.filtered_x_counts,r.control.shaped_x_counts})
        expect(std::isfinite(value), "切换和缺测恢复不得输出非有限状态");
    const float diagonal = std::hypot(t.x2-t.x1,t.y2-t.y1);
    expect(std::hypot(t.delay_compensation_x,t.delay_compensation_y) <=
           diagonal*c.max_delay_compensation_percent/100+.001f,
           "delay向量必须保持框尺度上限");
    expect(std::hypot(t.lead_x,t.lead_y) <=
           diagonal*c.max_prediction_lead_percent/100+.001f,
           "prediction向量必须保持框尺度上限");
    expect(t.delay_compensation_ms >= 0 && t.delay_compensation_ms <= 44.001f,
           "delay时域不能超过配置上限");
    if (!c.enable_prediction) {
        expect(!t.lead_active && near(t.lead_x,0) && near(t.lead_y,0),
               "关prediction必须清除提前向量");
        expect(near(t.aim_x,t.base_aim_x) && near(t.aim_y,t.base_aim_y),
               "关prediction最终瞄点必须等于base");
    }
    if (!c.enable_delay_compensation) {
        expect(!t.delay_compensation_active && near(t.delay_compensation_x,0) &&
               near(t.delay_compensation_y,0) && near(t.delay_compensation_ms,0),
               "关delay不能残留补偿向量或时域");
        expect(near(t.delay_compensated_aim_x,t.base_aim_x) &&
               near(t.delay_compensated_aim_y,t.base_aim_y),
               "关delay补偿点必须还原base");
    }
    if (t.predicted && !c.enable_prediction) expect(!r.has_command, "关闭prediction时短缺测不能请求输出");
    (void)f;
}
std::vector<AimResult> run(const AimConfig& c, bool age = true) {
    Aim aim(c);
    std::vector<AimResult> result;
    int targets = 0, leads = 0, compensations = 0;
    for (int i=0;i<220;++i) {
        const auto f=frame_at(i,1,age); const auto r=aim.process(f);
        contract(c,f,r); confirm(aim,f,r); result.push_back(r);
        targets += r.has_target; leads += r.target.lead_active;
        compensations += r.target.delay_compensation_active;
    }
    expect(targets>200,"交叉测试必须实际进入稳定跟踪而非全部无目标");
    if (c.enable_prediction) expect(leads>0,"prediction开启必须在合法移动输入上实际产生提前");
    if (!age && c.control_delay_ms==0)
        expect(compensations==0,"零观测龄且零固定delay不能凭空补偿");
    std::cout << "delay=" << c.enable_delay_compensation << " prediction=" << c.enable_prediction
              << " fixed=" << c.control_delay_ms << " age=" << age << " targets=" << targets
              << " leads=" << leads << " compensation=" << compensations << '\n';
    return result;
}
void same_public(const AimResult& a,const AimResult& b) {
    expect(a.status==b.status && a.has_target==b.has_target &&
           a.has_command==b.has_command && a.target.track_id==b.target.track_id &&
           a.command.dx_counts==b.command.dx_counts && a.command.dy_counts==b.command.dy_counts,
           "新配置重建后不能泄漏前一配置目标身份或命令库存");
    expect(near(a.target.aim_x,b.target.aim_x) && near(a.target.aim_y,b.target.aim_y) &&
           near(a.control.pending_net_x_counts,b.control.pending_net_x_counts) &&
           near(a.control.residual_before_quantization_x_counts,b.control.residual_before_quantization_x_counts),
           "重建后最终点及库存须与同配置fresh实例一致");
}
void disabled_delay_parameter_independence() {
    for (const bool prediction : {false,true}) for (int scenario=0;scenario<3;++scenario) {
        std::array<Aim,3> aims{Aim(frozen_config(false,prediction,0)),
            Aim(frozen_config(false,prediction,15)),Aim(frozen_config(false,prediction,44))};
        int different_frames=0;
        for(int i=0;i<220;++i) {
                        auto f=frame_at(i,20);
            if (scenario>0) {
                // 沿用旧hidden-inventory输入，独立改变关闭功能的参数。
                const int pose_sample=scenario==1 ? std::min(i,99) : i;
                const int phase_index=pose_sample%34;
                const float pose=phase_index<=17 ? -1+phase_index*(2.0f/17) : 1-(phase_index-17)*(2.0f/17);
                float x=scenario==1 ? 60.0f+.8f*std::min(i,99) : 80.0f+.8f*std::min(i,99);
                if (scenario==1 && i>=160) x-=(i-159)*.8f;
                if (scenario==2 && i>=100) x-=std::min(i-99,20)*5.5f;
                if (scenario==2 && i>=120) x+=(i-119)*.55f;
                x+=pose*(scenario==1 ? 3.0f : .21f);
                const float width=scenario==1 ? 16+pose*.5f : 17+pose*.6f;
                const float height=70+pose*1.8f;
                f.detections={Detection{x-width*.5f,175-height*.5f,x+width*.5f,175+height*.5f,.9f,0}};
                f.background_motion_x={}; f.lock_active=false;
                f.control_at=f.captured_at;
            }
            std::array<AimResult,3> r;
            for(int j=0;j<3;++j) {r[j]=aims[j].process(f);confirm(aims[j],f,r[j]);}
            for(int j=1;j<3;++j) {
                const auto& a=r[0];const auto& b=r[j];
                const bool same = a.status==b.status && a.has_target==b.has_target &&
                    a.command.dx_counts==b.command.dx_counts && a.command.dy_counts==b.command.dy_counts &&
                    near(a.target.base_aim_x,b.target.base_aim_x) && near(a.target.base_aim_y,b.target.base_aim_y) &&
                    near(a.target.aim_x,b.target.aim_x) && near(a.target.aim_y,b.target.aim_y);
                if (!same && different_frames++<4)
                    std::cout << "disabled-delay difference prediction=" << prediction << " scenario=" << scenario << " seq=" << f.sequence
                              << " fixed=" << (j==1?15:44) << " q0=" << a.command.dx_counts
                              << " qOther=" << b.command.dx_counts << " base0=" << a.target.base_aim_x << " baseOther=" << b.target.base_aim_x << '\n';
            }
        }
        expect(different_frames==0,"关闭delay后修改其固定延迟0/15/44不得改变公开几何或请求，差异="+std::to_string(different_frames));
    }
}
void restart_switches() {
    Aim reused(frozen_config(true,true));
    // Runtime启动使用新Aim实例；GUI应用配置不是不存在的Aim热更新API。
    const std::array<std::array<bool,2>,7> modes{{{true,true},{false,true},{true,true},
        {true,false},{true,true},{false,false},{true,true}}};
    int epoch=2;
    for (const auto mode:modes) {
        const auto c=frozen_config(mode[0],mode[1]);
        reused=Aim(c); Aim fresh(c);
        for(int i=0;i<180;++i) {
            const auto f=frame_at(i,epoch); const auto a=reused.process(f);const auto b=fresh.process(f);
            contract(c,f,a);same_public(a,b);confirm(reused,f,a);confirm(fresh,f,b);
        }
        ++epoch;
    }
}
}
int main() {
    std::array<std::vector<AimResult>,4> runs;
    for(int d=0;d<2;++d) for(int p=0;p<2;++p) runs[d*2+p]=run(frozen_config(d!=0,p!=0));
    for(std::size_t i=0;i<runs[0].size();++i) for(int m=1;m<4;++m) {
        const auto& a=runs[0][i].target;const auto& b=runs[m][i].target;
        expect(near(a.base_aim_x,b.base_aim_x) && near(a.base_aim_y,b.base_aim_y),
               "delay和prediction开关不可回写公共基础几何");
    }
    run(frozen_config(true,false,0));run(frozen_config(true,true,0));
    run(frozen_config(true,false,0),false);run(frozen_config(true,true,0),false);
    restart_switches();disabled_delay_parameter_independence();
    std::cout << "失败总数=" << failures << '\n';
    return failures==0 ? 0 : 1;
}




