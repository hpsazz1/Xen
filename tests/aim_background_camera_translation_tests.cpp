#include "aim/aim.h"
#include "aim_camera_domain_fixture.h"
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
// 同一世界反向运动只因共同相机平移而让raw两边跨零，不能丢掉观测。
void reversed_world_covariance() {
    for (int direction : {1, -1}) {
        std::array<std::array<float, 4>, 2> velocities{};
        for (int shifted = 0; shifted < 2; ++shifted) {
            Aim aim(config());
            seed(aim, direction);
            const float camera = static_cast<float>(direction * shifted);
            float left = 150.0f + 16 * direction;
            float right = 190.0f + 16 * direction;
            const float world_left = direction > 0 ? -1.1f : .9f;
            const float world_right = direction > 0 ? -.9f : 1.1f;
            for (int j = 0; j < 4; ++j) {
                left += world_left + camera;
                right += world_right + camera;
                const auto r = aim.process(frame(17 + j, left, right, camera));
                valid(r, "世界反向协变", AimBackgroundMotionUse::CONSUMED);
                const auto& c = r.control;
                if (shifted)
                    expect(c.reverse_translation_raw_left_x_roi_pixels *
                               c.reverse_translation_raw_right_x_roi_pixels < 0,
                           "相机平移分支必须实际覆盖raw异向");
                close(c.reverse_translation_raw_left_x_roi_pixels - camera,
                      world_left, "相同世界左边位移");
                close(c.reverse_translation_raw_right_x_roi_pixels - camera,
                      world_right, "相同世界右边位移");
                close(c.observer_camera_motion_x_source_pixels, camera,
                      "同源相机位移必须完整消费");
                velocities[shifted][j] = v(r);
                if (j == 3)
                    expect(direction * v(r) < 0,
                           "持续反向观测必须使运动估计换向");
            }
        }
        for (int j = 0; j < 4; ++j)
            close(velocities[1][j], velocities[0][j],
                  "共同相机平移不能改变世界反向响应");
    }
}
auto at(std::int64_t ns) {
    return std::chrono::steady_clock::time_point{
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::nanoseconds(ns))};
}
void actual_camera_domain(bool mirror) {
    AimConfig config;
    config.person_class_ids = {0, 2};
    config.head_class_ids = {1, 3};
    config.high_confidence = 0.25f;
    config.low_confidence = 0.1f;
    config.min_confirmed_hits = 2;
    config.max_lost_frames = 8;
    config.min_iou = 0.1f;
    config.max_center_distance = 0.25f;
    config.switch_margin = 0.2f;
    config.switch_confirm_frames = 3;
    config.switch_cooldown_frames = 5;
    config.acquisition_range_percent = 90.0f;
    config.body_aim_height_ratio = 0.35f;
    config.body_aim_range_percent = 50.0f;
    config.deadzone_pixels = 1.5f;
    config.smoothing = 0.475f;
    config.counts_per_pixel_x = 0.425f;
    config.counts_per_pixel_y = 0.4f;
    config.max_counts_per_frame = 14.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 44.0f;
    config.max_delay_compensation_percent = 15.0f;
    config.enable_prediction = false;
    config.max_prediction_lead_percent = 35.0f;
    config.predicted_gain = 0.5f;
    Aim aim(config);
    const int direction = mirror ? -1 : 1;
    int checked = 0;
    float previous_velocity = 0.0f;
    for (const auto& s : aim_camera_domain_fixture::kSamples) {
        if (s.observation_clock_reset) aim.reset();
        AimFrame f;
        f.sequence = s.sequence;
        f.captured_at = at(s.source_ns);
        f.control_at = at(s.control_ns);
        f.roi_width = s.roi_width;
        f.roi_height = s.roi_height;
        f.control_center_x = s.center_x;
        f.control_center_y = s.center_y;
        f.source_pixels_per_roi_pixel_x = s.scale_x;
        f.source_pixels_per_roi_pixel_y = s.scale_y;
        f.lock_active = s.lock_active;
        f.observation_epoch = s.epoch;
        for (int i = 0; i < s.count; ++i) {
            auto d = s.detections[static_cast<std::size_t>(i)];
            if (mirror) {
                const float left = d.x1;
                d.x1 = 2.0f * s.center_x - d.x2;
                d.x2 = 2.0f * s.center_x - left;
            }
            f.detections.push_back(d);
        }
        f.background_motion_x = {s.status, s.previous_sequence, s.background_sequence,
            at(s.previous_ns), at(s.background_ns), s.background_epoch,
            mirror ? -s.bg_dx : s.bg_dx, s.response, s.disagreement, s.patches};
        const auto r = aim.process(f);
        const auto& c = r.control;
        expect(r.status == AimStatus::SUCCESS, "实际9帧最小输入须正常处理");
        const float scalars[] = {c.proportional_x_counts, c.feedforward_x_counts,
            c.desired_before_reverse_x_counts, c.filtered_x_counts,
            c.modelled_response_x_counts, c.shaped_x_counts,
            c.pending_net_x_counts, c.pending_absolute_x_counts,
            c.residual_before_quantization_x_counts,
            c.history_adjusted_x_counts, c.pre_eligibility_filtered_x_counts,
            c.filtered_integral_x_counts, c.target_motion_maintenance_x_counts,
            c.observer_target_velocity_x_counts_per_second,
            c.error_derivative_x_source_pixels_per_second, c.opening_weight_x};
        for (const float value : scalars)
            expect(std::isfinite(value), "原字段和分阶段维护诊断必须有限");
        expect(c.opening_weight_x >= 0.0f && c.opening_weight_x <= 1.0f,
               "opening权重保持有效范围");
        expect(r.command.dy_counts == s.expected_y, "原向与镜像都必须保持原Y输出");
        expect(std::hypot(static_cast<float>(r.command.dx_counts),
                          static_cast<float>(r.command.dy_counts)) <= 14.0f,
               "维护更新不得突破二维14上限");
        if (r.command.dx_counts != 0)
            expect(r.command.dx_counts * (r.target.base_aim_x - f.control_center_x) > 0.0f,
                   "非零X请求必须朝当前固定base误差方向");
        if (r.has_command)
            expect(aim.record_backend_completed_command(f.sequence, at(s.backend_ns),
                       r.command.dx_counts, r.command.dy_counts),
                   "每个分支只能确认自身产生的命令");
        if (s.sequence == 2052) {
            previous_velocity = direction * c.observer_target_velocity_x_counts_per_second;
            expect(previous_velocity > 0.0f, "实际前缀必须形成旧向运动估计");
            ++checked;
        }
        if (s.sequence == 2053) {
            expect(c.background_motion_use_x == AimBackgroundMotionUse::CONSUMED,
                   "camera校正后同向的实际运动证据必须被消费");
            expect(direction * c.observer_target_velocity_x_counts_per_second < previous_velocity,
                   "实际反向观测必须纠正旧向运动估计");
            ++checked;
        }
    }
    expect(checked == 2, "必须覆盖实际旧先验和反向证据帧");
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
    covariance();fallback_contract();opposed_guard();reversed_world_covariance();actual_camera_domain(false);actual_camera_domain(true);
    std::cout<<"failures,"<<failures<<'\n';return failures?1:0;
}
