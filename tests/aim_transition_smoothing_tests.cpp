#include "aim/aim.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>

namespace {
int failures = 0;
void expect(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "失败：" << message << '\n'; }
}

// Run6572的240Hz、14-count、双轴比例与平滑配置；几何为合成输入，
// 不冒充缺少完整候选检测集合的原Run重放，不连接Mouse。
AimConfig config(float smoothing) {
    AimConfig c;
    c.min_confirmed_hits = 1;
    c.body_aim_height_ratio = .16f;
    c.smoothing = smoothing;
    c.counts_per_pixel_x = .425f;
    c.counts_per_pixel_y = .4f;
    c.max_counts_per_frame = 14;
    c.enable_delay_compensation = true;
    c.control_delay_ms = 15;
    c.max_delay_compensation_ms = 44;
    return c;
}

struct Driver {
    Aim aim;
    int index = 0;
    std::int64_t elapsed_us = 0;
    int step_us = 4167;
    std::uint64_t epoch = 1;
    explicit Driver(float smoothing) : aim(config(smoothing)) {}
    AimResult step(bool locked, int axis, bool other = false, bool missing = false,
                   float motion_x_offset = 0.0f) {
        AimFrame f;
        f.sequence = ++index;
        elapsed_us += step_us;
        f.captured_at = std::chrono::steady_clock::time_point(std::chrono::seconds(100)) +
            std::chrono::microseconds(elapsed_us);
        f.control_at = f.captured_at + std::chrono::milliseconds(3);
        f.roi_width = f.roi_height = 320;
        f.control_center_x = f.control_center_y = 160;
        f.lock_active = locked;
        f.observation_epoch = epoch;
        const float sign = other ? -1.0f : 1.0f;
        const float x = 160 + (axis != 1 ? sign * (100 + motion_x_offset) : 0);
        const float y = 160 + (axis != 0 ? sign * (axis == 2 ? 20 : 65) : 0);
        if (!missing) f.detections = {{x-16,y-16,x+16,y+84,.9f,0}};
        const auto r = aim.process(f);
        expect(r.status == AimStatus::SUCCESS, "合成帧必须通过真实Aim接口");
        expect(std::hypot(r.command.dx_counts,r.command.dy_counts) <= 14,
               "所有阶段保留二维步长上限");
        if (r.has_command) expect(aim.record_backend_completed_command(
            f.sequence, f.control_at + std::chrono::microseconds(100),
            locked ? r.command.dx_counts : 0, locked ? r.command.dy_counts : 0),
            "公开接口必须接受已执行或松键零回执");
        return r;
    }
};

double magnitude(const AimResult& r) {
    return std::hypot(r.command.dx_counts,r.command.dy_counts);
}

void test_takeover(int axis, int preview_frames) {
    Driver smooth(.475f), direct(1.0f);
    for (int i=0;i<preview_frames;++i) {
        expect(smooth.step(false,axis).has_target, "松键仍保留目标预计算");
        direct.step(false,axis);
    }
    const auto a = smooth.step(true,axis);
    const auto b = direct.step(true,axis);
    if (axis==2 && preview_frames==0) expect(b.command.dx_counts!=0 && b.command.dy_counts!=0,
                        "双轴夹具必须实际产生两轴命令，不能被Y限幅退化为单轴");
    std::cout << "接管 axis=" << axis << " preview=" << preview_frames
              << " smooth=" << a.command.dx_counts << ',' << a.command.dy_counts
              << " direct=" << b.command.dx_counts << ',' << b.command.dy_counts << '\n';
    expect(a.has_target && a.control.evaluated, "接管首样本立即计算当前目标");
    if (preview_frames > 0) {
        expect(!a.has_command && !b.has_command && magnitude(a)==0 && magnitude(b)==0,
               "预计算后激活建立零预算起点，普通平滑开关不能绕过过渡");
    } else {
        // 无前序控制区间的冷启动保持原合同；本轮否决了同时改冷启动的
        // 候选，因为既有半身恢复闭环出现额外反转，不以换golden掩盖。
        expect(a.has_command && a.command.dx_counts == b.command.dx_counts &&
                   a.command.dy_counts == b.command.dy_counts,
               "冷启动保留原首步，不冒充已验证的接管过渡");
    }
    // 不改变目标的持续锁定不能每帧重新起步；误差持续时必须保留追赶能力。
    bool grew = false;
    bool both_axes_observed = false;
    for (int i=0;i<12;++i) {
        const auto next = smooth.step(true,axis);
        const auto reference = direct.step(true,axis);
        grew |= magnitude(next) > magnitude(a);
        both_axes_observed |= reference.command.dx_counts!=0 && reference.command.dy_counts!=0;
        if (preview_frames > 0) {
            const float allowance=14.0f*(i+1)*.004167f/.1f;
            expect(std::fabs(next.control.pre_eligibility_filtered_x_counts)<=allowance+.001f &&
                       std::fabs(reference.control.pre_eligibility_filtered_x_counts)<=allowance+.001f,
                   "激活前段持续受真实时间额度约束，不能第二帧恢复满幅");
        }
    }
    if (preview_frames > 0) expect(grew, "同身份持续追踪不能每帧重启接管滤波");
    if (axis==2) expect(both_axes_observed, "双轴夹具过渡期间必须实际产生两轴命令");
    for (int i=0;i<12;++i) { smooth.step(false,axis); direct.step(false,axis); }
    const auto resumed = smooth.step(true,axis);
    const auto resumed_direct = direct.step(true,axis);
    expect(resumed.target.track_id == a.target.track_id, "重锁不重建观测身份");
    expect(resumed.control.evaluated && !resumed.has_command && !resumed_direct.has_command &&
               magnitude(resumed)==0 && magnitude(resumed_direct)==0,
           "同身份松键再接管也建立完整过渡零起点");
    bool switched = false;
    std::uint64_t switched_id = 0;
    for (int i=0;i<12;++i) {
        const auto next = smooth.step(true,axis,true);
        const auto reference = direct.step(true,axis,true);
        if (!switched && next.has_target && next.target.track_id != a.target.track_id) {
            switched = true;
            switched_id = next.target.track_id;
            expect(magnitude(next) <= 2.0 && magnitude(reference) <= 2.0,
                   "不同身份接管从小额度开始，不因关闭普通平滑而直通满幅");
        }
    }
    expect(switched, "切换回归必须实际到达不同的生产track身份");
    for (int i=0;i<10;++i) {
        expect(!smooth.step(true,axis,false,true).has_command, "丢失观测不延续旧命令");
        direct.step(true,axis,false,true);
    }
    const auto changed = smooth.step(true,axis);
    const auto changed_direct = direct.step(true,axis);
    expect(changed.has_target && changed.target.track_id != switched_id,
           "新目标必须通过生产关联取得独立身份");
    expect(magnitude(changed) <= 2.0 && magnitude(changed_direct) <= 2.0,
           "持锁完全丢失后新身份必须从小额度接管，不冒充首次冷启动");
}

void test_reacquisition_interval(int axis) {
    Driver switched(.475f), cold(.475f);
    switched.step(true,axis,true);
    for (int i=0;i<12;++i) switched.step(true,axis,true);
    for (int i=0;i<12;++i)
        expect(!switched.step(true,axis,false,true).has_command,
               "空窗必须立即停发，不能平滑续发旧目标");
    double early_switched = 0, early_cold = 0;
    bool started = false, progressed = false;
    for (int i=0;i<36;++i) {
        const auto a=switched.step(true,axis);
        const auto b=cold.step(true,axis);
        if (i<8) { early_switched += magnitude(a); early_cold += magnitude(b); }
        if (i<3) started |= a.has_command;
        if (i>=24) progressed |= magnitude(a)>2;
        if (i<8) expect(magnitude(a)<8,
            "重获后前段需持续缓入，不能只改变第一个控制样本");
    }
    expect(started,"接管立即计算并在量化可见后输出，不插入反应等待");
    expect(early_switched<early_cold*.5,
           "整段接管前缀的输出须明显低于直接冷启动");
    expect(progressed,"同身份持续控制须完成过渡，不能每帧重新起步");
    std::cout << "接管前8样本 axis=" << axis << " transition=" << early_switched
              << " cold=" << early_cold << '\n';
}

void test_session_boundaries() {
    for (int boundary=0;boundary<3;++boundary) {
        Driver d(.475f), fresh(.475f);
        d.step(true,0,true);
        for (int i=0;i<12;++i) d.step(true,0,false,true);
        if (boundary==0) d.step(false,0,false,true);
        if (boundary==1) d.aim.reset();
        if (boundary==2) ++d.epoch;
        const auto actual=d.step(true,0);
        const auto expected=fresh.step(true,0);
        if (boundary==0) {
            expect(actual.has_target && actual.control.evaluated && !actual.has_command &&
                       magnitude(actual)==0 && expected.has_command,
                   "松键空帧后的再次激活建立新过渡，不能退化成冷启动满幅");
        } else {
            expect(actual.command.dx_counts==expected.command.dx_counts &&
                   actual.command.dy_counts==expected.command.dy_counts,
                   "显式reset或源epoch变化后保留清洁实例冷启动合同");
        }
    }
    Driver gap(.475f);
    gap.step(true,0,true);
    for (int i=0;i<12;++i) gap.step(true,0,false,true);
    gap.step(true,0);
    gap.step(true,0,false,true);
    gap.step_us=200000;
    const auto resumed=gap.step(true,0);
    std::cout << "空窗恢复 track=" << resumed.target.track_id
              << " command=" << resumed.command.dx_counts << ',' << resumed.command.dy_counts
              << " filtered=" << resumed.control.filtered_x_counts
              << " shaped=" << resumed.control.shaped_x_counts << '\n';
    expect(magnitude(resumed)<3,
           "接管中长观测空窗不能在恢复首样本一次释放整段额度");
}

void test_transition_clock() {
    for (bool irregular : {false,true}) {
        Driver d(1.0f);
        d.step(true,0,true);
        for (int i=0;i<12;++i) d.step(true,0,false,true);
        const auto entered=d.step(true,0);
        expect(entered.has_target && !entered.has_command,
               "新身份进入时刻建立零起点，不消费此前空窗时间");
        int elapsed=0;
        for (int i=0;elapsed<100000;++i) {
            d.step_us=irregular ? (i%2==0 ? 3000 : 7000) : 4000;
            elapsed+=d.step_us;
            const auto r=d.step(true,0);
            const float allowance=14.0f*std::min(1.0f,elapsed/100000.0f);
            expect(std::fabs(r.control.pre_eligibility_filtered_x_counts)<=allowance+0.001f,
                   "不同采样间隔均按有效真实时间约束PI额度，不能按帧数释放");
        }
    }
}

void test_activation_gap() {
    for (int scenario=0;scenario<3;++scenario) {
        for (bool irregular : {false,true}) {
            Driver d(.475f);
            for (int i=0;i<4;++i) d.step(false,0,false,scenario==2);
            // 分别覆盖直接激活、预计算后激活空帧、从未有目标的激活空窗。
            if (scenario>0) {
                expect(!d.step(true,0,false,true).has_command,
                       "激活边沿无观测立即停发");
                if (scenario==2) {
                    d.step_us=200000;
                    expect(!d.step(true,0,false,true).has_command,
                           "激活后长空窗不得产生请求或提前完成过渡");
                    d.step_us=4167;
                }
            }
            const auto first=d.step(true,0);
            expect(first.has_target && first.control.evaluated && !first.has_command &&
                       magnitude(first)==0,
                   "激活边沿经空帧后首次真实目标仍建立零预算起点");
            int elapsed=0;
            bool sent=false, completed=false;
            for (int i=0;elapsed<120000;++i) {
                d.step_us=irregular ? (i%2==0 ? 3000 : 7000) : 4000;
                elapsed+=d.step_us;
                const auto r=d.step(true,0);
                const float allowance=14.0f*std::min(1.0f,elapsed/100000.0f);
                expect(std::fabs(r.control.pre_eligibility_filtered_x_counts)<=allowance+.001f,
                       "激活整段按有效时间限额，不由帧数或普通平滑代替");
                if (elapsed<20000) sent|=r.has_command;
                if (elapsed>=100000) completed|=magnitude(r)>7;
                if (i==2) {
                    expect(!d.step(true,0,false,true).has_command,
                           "过渡中的观测空帧必须停发");
                    const auto resumed=d.step(true,0);
                    expect(resumed.has_target &&
                               std::fabs(resumed.control.pre_eligibility_filtered_x_counts)<=allowance+.001f,
                           "观测恢复首样本不消费空窗及恢复间隔的预算");
                    // 这两步没有连续真实观测，故 elapsed 不增加。
                }
            }
            expect(sent,"过渡在量化可见后及时发送，不插入100ms等待门禁");
            expect(completed,"连续有效控制达到100ms后必须完成过渡，不能每帧重启");
        }
    }
}

void test_moving_activation_budget() {
    for (bool opposite : {false,true}) {
        Driver d(.475f);
        std::uint64_t preview_id=0;
        for (int i=0;i<12;++i) {
            const auto preview=d.step(false,0,opposite,false,-70.0f+3.0f*i);
            expect(preview.has_target,"移动预计算必须持续保有真实生产目标");
            preview_id=preview.target.track_id;
        }
        const auto first=d.step(true,0,opposite,false,-34.0f);
        expect(first.has_target && first.target.track_id==preview_id,
               "移动激活必须保持预计算身份，不能借身份重置清除维护来源");
        expect(std::fabs(first.control.observer_target_velocity_x_counts_per_second)>.01f,
               "移动激活夹具必须在首帧实际形成非零运动维护估计");
        std::cout<<"移动激活 opposite="<<opposite
                 <<" estimate="<<first.control.observer_target_velocity_x_counts_per_second
                 <<" maintenance="<<first.control.modelled_response_x_counts
                 <<" shaped="<<first.control.shaped_x_counts<<'\n';
        expect(!first.has_command && std::fabs(first.control.shaped_x_counts)<.001f &&
                   std::fabs(first.control.modelled_response_x_counts)<.001f,
               "激活零预算必须同时约束PI与运动维护，维护不能绕过过渡");
        bool sent=false;
        for (int i=1;i<=12;++i) {
            const auto r=d.step(true,0,opposite,false,-34.0f+3.0f*i);
            expect(r.has_target && r.target.track_id==preview_id,
                   "移动接管前段维持同一身份，避免反复重启掩盖预算泄漏");
            const float allowance=14.0f*i*.004167f/.1f;
            expect(std::fabs(r.control.shaped_x_counts)<=allowance+.001f,
                   "移动接管的最终浮点总量必须遵守有效时间额度，而非仅PI滤波量");
            sent|=r.has_command;
        }
        expect(sent,"移动接管仍在过渡前段开始跟随，不以持续零输出来通过限额");
    }
}

void test_far_y_activation_budget() {
    for (int direction : {-1,1}) for (bool irregular : {false,true}) {
        for (int setting=0;setting<4;++setting) {
            auto c=config(.475f);
            c.soft_zone_radius_percent=setting==1?0.0f:30.0f;
            c.soft_zone_min_strength=setting==2?1.0f:.2f;
            c.body_aim_height_ratio=.5f;
            const float initial_error=setting==3?24.0f:80.0f;
            const float radius=160*c.soft_zone_radius_percent/100;
            const float duration_us=100000*(1+(radius>0 ?
                (1-c.soft_zone_min_strength)*std::clamp(initial_error/radius-1,0.0f,1.0f):0));
            Aim aim(c);
            int wall_us=0; std::uint64_t sequence=0,epoch=91;
            const auto step=[&](bool missing) {
                AimFrame f; f.sequence=++sequence; f.observation_epoch=epoch;
                f.captured_at=std::chrono::steady_clock::time_point(std::chrono::seconds(800))+
                    std::chrono::microseconds(wall_us); f.control_at=f.captured_at;
                f.roi_width=f.roi_height=320; f.control_center_x=f.control_center_y=160;
                f.lock_active=true; f.ease_first_activation=true;
                const float y=160+direction*initial_error;
                if(!missing) f.detections={{144,y-16,176,y+16,.95f,0}};
                const auto r=aim.process(f);
                if(r.has_command) expect(aim.record_backend_completed_command(f.sequence,f.control_at,
                    r.command.dx_counts,r.command.dy_counts),"远Y预算只回执本分支命令");
                return r;
            };
            // 首次激活、显式reset、source epoch变化均应重新建立真实零起点。
            for(int phase=0;phase<3;++phase) {
                if(phase==1) aim.reset();
                if(phase==2) ++epoch;
                wall_us+=4000;
                int effective_us=0,sent=0; bool full_legacy_seen=false;
                for(int i=0;effective_us<=220000;++i) {
                    if(i) {
                        const int dt=irregular?(i%2?3000:7000):4000;
                        wall_us+=dt;
                        if(i==8) {
                            expect(!step(true).has_command,"远Y过渡观测空帧必须停发");
                            wall_us+=dt;
                            // 恢复帧不把丢失及恢复间隔计入有效预算。
                        } else effective_us+=dt;
                    }
                    const auto r=step(false);
                    const float allowance=14*std::min(1.0f,effective_us/duration_us);
                    expect(r.status==AimStatus::SUCCESS && r.has_target && r.control.evaluated,
                           "远Y激活预算必须有真实生产目标与有效控制");
                    expect(std::fabs(static_cast<float>(r.command.dy_counts))<=allowance+1.0f,
                           "Y预算须按初始距离和软区强度延长有效接管，空帧不得蓄时");
                    if(!i) expect(!r.has_command,"首次激活及reset或epoch重建必须从零命令开始");
                    expect(r.command.dx_counts==0,"纯Y夹具不得生成无关X命令");
                    sent+=r.has_command?1:0;
                    if(effective_us>=100000 && std::abs(r.command.dy_counts)>=13) full_legacy_seen=true;
                }
                expect(sent>0,"延长Y接管仍在200ms内有非零跟随，不能靠停发通过");
                if(setting==1 || setting==2)
                    expect(full_legacy_seen,"软区关闭或强度1继续原100ms路径，不能全局减增益");
            }
        }
    }
}

void test_far_y_recoil_pause_budget() {
    auto c=config(.475f); c.soft_zone_radius_percent=30;
    c.soft_zone_min_strength=.2f; c.body_aim_height_ratio=.5f;
    Aim aim(c); bool previous_owned=true; int effective_us=0,sent=0;
    for(int i=0;i<80;++i) {
        const bool owned=i<10 || (i>=18 && i<28);
        if(!owned && !previous_owned) effective_us+=4000;
        AimFrame f; f.sequence=i+1; f.observation_epoch=93;
        f.captured_at=std::chrono::steady_clock::time_point(std::chrono::seconds(850))+
            std::chrono::microseconds(4000*i); f.control_at=f.captured_at;
        f.roi_width=f.roi_height=320; f.control_center_x=f.control_center_y=160;
        f.lock_active=true; f.ease_first_activation=true; f.recoil_y_owned=owned;
        f.detections={{144,224,176,256,.95f,0}};
        const auto r=aim.process(f);
        expect(r.status==AimStatus::SUCCESS && r.has_target,
               "压枪交接测试仍保持真实远Y目标");
        const float allowance=14*std::min(1.0f,effective_us/(100000*(1+.8f*(80.0f/48-1))));
        expect(owned ? r.command.dy_counts==0 : std::abs(r.command.dy_counts)<=allowance+1,
               "压枪占用不得消费Y接管时间或用屏蔽后零误差初始化短周期");
        if(i==10) expect(!r.has_command,"首次占用结束须按真实80ROI建立零起点");
        if(r.has_command) { ++sent; expect(aim.record_backend_completed_command(
            f.sequence,f.control_at,r.command.dx_counts,r.command.dy_counts),"压枪交接回执有效"); }
        previous_owned=owned;
    }
    expect(sent>0,"压枪释放后Y必须恢复有效跟随，不能永久停发");
}

void test_legacy_y_recoil_release_bypasses_extended_cap() {
    for(int setting=0;setting<3;++setting) {
        auto c=config(.475f);
        c.soft_zone_radius_percent=setting==0?0.0f:30.0f;
        c.soft_zone_min_strength=setting==1?1.0f:.2f;
        c.body_aim_height_ratio=.5f;
        const float error=setting==2?24.0f:80.0f;
        Aim eased(c),legacy(c);
        for(int i=0;i<=31;++i) {
            AimFrame f; f.sequence=i+1; f.observation_epoch=95;
            f.captured_at=std::chrono::steady_clock::time_point(std::chrono::seconds(950))+
                std::chrono::milliseconds(4*i); f.control_at=f.captured_at;
            f.roi_width=f.roi_height=320; f.control_center_x=f.control_center_y=160;
            f.lock_active=true; f.recoil_y_owned=i<30;
            const float y=160+error;
            f.detections={{144,y-16,176,y+16,.95f,0}};
            // 两分支占用期间Y状态均为零；共同100ms过程已完成，
            // 不需要扩展的配置释放时应与不请求首次缓入的分支一致。
            f.ease_first_activation=true;
            const auto actual=eased.process(f);
            f.ease_first_activation=false;
            const auto reference=legacy.process(f);
            expect(actual.has_target && reference.has_target,
                   "旧Y释放语义对照始终有当前目标");
            if(i<30) expect(actual.command.dy_counts==0 && reference.command.dy_counts==0,
                            "两个Y释放对照在recoil占用期间均不得输出");
            else expect(reference.has_command && reference.command.dy_counts!=0 &&
                            actual.command.dy_counts==reference.command.dy_counts &&
                            actual.command.dx_counts==reference.command.dx_counts,
                        "关闭软区、强度1、近区在120ms压枪占用后应旁路新Ycap，不得重新缓入100ms");
            if(actual.has_command) expect(eased.record_backend_completed_command(f.sequence,f.control_at,
                actual.command.dx_counts,actual.command.dy_counts),"兼容分支回执有效");
            if(reference.has_command) expect(legacy.record_backend_completed_command(f.sequence,f.control_at,
                reference.command.dx_counts,reference.command.dy_counts),"旧语义对照回执有效");
        }
    }
}

void test_far_y_discrete_feedback_settles() {
    // 合成单步离散相机仅验证软件预算后段，不拟合或声明真实设备plant。
    constexpr double response=.55;
    for(float initial:{-88.0f,-65.0f,65.0f,88.0f}) {
        auto c=config(.475f); c.soft_zone_radius_percent=30;
        c.soft_zone_min_strength=.2f; c.body_aim_height_ratio=.5f;
        Aim aim(c); double error=initial,tail_error=0; int previous=0,sent=0,tail_samples=0,tail_peak=0;
        const float duration_us=100000*(1+.8f*std::clamp(std::fabs(initial)/48-1,0.0f,1.0f));
        for(int i=0;i<=150;++i) {
            if(i) error-=response*previous;
            AimFrame f; f.sequence=i+1; f.observation_epoch=94;
            f.captured_at=std::chrono::steady_clock::time_point(std::chrono::seconds(900))+
                std::chrono::microseconds(4000*i); f.control_at=f.captured_at;
            f.roi_width=f.roi_height=320; f.control_center_x=f.control_center_y=160;
            f.lock_active=true; f.ease_first_activation=true;
            const float y=160+static_cast<float>(error);
            f.detections={{144,y-16,176,y+16,.95f,0}};
            const auto r=aim.process(f);
            expect(r.status==AimStatus::SUCCESS && r.has_target,
                   "远Y离散闭环始终保有当前目标");
            previous=r.has_command?r.command.dy_counts:0;
            const float allowance=14*std::min(1.0f,4000*i/duration_us);
            expect(std::abs(previous)<=allowance+1,
                   "误差接近目标时不能缩短冻结的Y接管周期而突释");
            if(r.has_command) { ++sent; expect(aim.record_backend_completed_command(
                f.sequence,f.control_at,r.command.dx_counts,r.command.dy_counts),"离散Y闭环只反馈自身回执"); }
            if(i>=75) { tail_error+=std::fabs(error); ++tail_samples; tail_peak=std::max(tail_peak,std::abs(previous)); }
        }
        expect(sent>0 && tail_samples>0 && tail_error/tail_samples<=4 && tail_peak<=3,
               "远Y软件闭环300至600ms尾段须收敛且没有延迟满幅释放");
        std::cout<<"远Y软件闭环 initial="<<initial<<" tail="<<tail_error/tail_samples<<" peak="<<tail_peak<<'\n';
    }
}

void test_soft_zone() {
    const auto run=[](float radius,float strength,float offset) {
        auto c=config(.475f); c.soft_zone_radius_percent=radius;
        c.soft_zone_min_strength=strength; c.deadzone_pixels=0;
        c.enable_delay_compensation=false;
        Aim a(c); double sum=0;
        for (int i=0;i<120;++i) {
            AimFrame f; f.sequence=i+1; f.roi_width=f.roi_height=320;
            f.control_center_x=f.control_center_y=160; f.lock_active=true;
            f.captured_at=std::chrono::steady_clock::time_point(std::chrono::seconds(200))+std::chrono::milliseconds(4*i);
            f.control_at=f.captured_at;
            f.detections={{160+offset-16,144,160+offset+16,244,.9f,0}};
            const auto r=a.process(f);
            expect(r.status==AimStatus::SUCCESS,"软区测试通过生产Aim接口");
            if(i>20) sum+=std::fabs(r.command.dx_counts);
            if(r.has_command) a.record_backend_completed_command(f.sequence,f.control_at+std::chrono::microseconds(100),r.command.dx_counts,r.command.dy_counts);
        }
        return sum;
    };
    const auto baseline=run(0,.2f,4);
    const auto softened=run(30,.2f,4);
    std::cout<<"软区累计 baseline="<<baseline<<" softened="<<softened<<'\n';
    expect(baseline>0 && softened>0 && softened<baseline*.7,
           "近中心仍有少量跟随，持续输出须明显弱于关闭软区");
    expect(run(30,1,4)==baseline,"保留强度1精确兼容原输出");
    expect(run(30,.2f,12)==run(0,.2f,12),"准星在50%内窗外时不得提前弱化追赶");
    expect(run(30,.2f,80)==run(0,.2f,80),"软区外保持原输出");

    const auto soft_config=[](float radius,float strength) {
        auto c=config(.475f);
        c.soft_zone_radius_percent=radius;
        c.soft_zone_min_strength=strength;
        c.deadzone_pixels=0;
        c.body_aim_range_percent=100; // 宽内窗覆盖ROI软区边缘，独立检验原软区连续边界。
        c.enable_delay_compensation=false;
        return c;
    };
    const auto geometry_frame=[](int roi,float source_scale,float x,float y,int sequence) {
        AimFrame f;
        const float scale=roi/320.0f;
        const float center=roi*.5f;
        f.sequence=sequence;
        f.roi_width=f.roi_height=roi;
        f.control_center_x=f.control_center_y=center;
        f.source_pixels_per_roi_pixel_x=f.source_pixels_per_roi_pixel_y=source_scale;
        f.lock_active=true;
        f.captured_at=std::chrono::steady_clock::time_point(std::chrono::seconds(300))+
            std::chrono::milliseconds(4*sequence);
        f.control_at=f.captured_at;
        f.detections={{center+(x-64)*scale,center+(y-16)*scale,
                       center+(x+64)*scale,center+(y+84)*scale,.9f,0}};
        return f;
    };
    const auto first_ratio=[&](int roi,float source_scale,float x,float y,bool vertical) {
        Aim off(soft_config(0,.2f)), on(soft_config(30,.2f));
        const auto f=geometry_frame(roi,source_scale,x,y,1);
        const auto a=off.process(f), b=on.process(f);
        expect(a.status==AimStatus::SUCCESS && b.status==AimStatus::SUCCESS &&
                   a.has_target && b.has_target,"软区几何对照必须由生产接口取得目标");
        // Y 没有单独 shaped 诊断，单轴冷启动无余数时直接检查其整数请求。
        if (vertical) {
            expect(a.command.dy_counts!=0 && b.command.dy_counts!=0 &&
                       std::abs(b.command.dy_counts)<std::abs(a.command.dy_counts),
                   "Y轴近中心保持非零弱跟随，不能仅弱化X轴");
        }
        const float before=a.control.shaped_x_counts;
        const float after=b.control.shaped_x_counts;
        if (!vertical) expect(std::fabs(before)>.001f,"X权重对照必须有非零浮点请求");
        return std::fabs(before)>.001f ? after/before : 0.0f;
    };
    first_ratio(320,1,0,12,true);
    const float roi_weight=first_ratio(320,1,12,0,false);
    expect(roi_weight>0 && roi_weight<1 &&
               std::fabs(first_ratio(640,.5f,12,0,false)-roi_weight)<.00001f &&
               std::fabs(first_ratio(320,2,12,0,false)-roi_weight)<.00001f,
           "相同ROI归一化几何的软化权重不随ROI分辨率或source比例改变");
    const float inside_weight=first_ratio(320,1,47.9f,0,false);
    // 宽内窗覆盖ROI软区外沿，仍单独验证原软区外缘连续性。
    const float edge_weight=first_ratio(320,1,48.2f,0,false);
    const float outside_weight=first_ratio(320,1,48.3f,0,false);
    expect(inside_weight<=edge_weight && std::fabs(inside_weight-edge_weight)<.0001f &&
               edge_weight==1.0f && outside_weight==1.0f,
           "软区外缘两侧权重连续，边界及区外精确恢复原请求");

    Aim disabled(soft_config(0,.2f)), full_strength(soft_config(30,1));
    for (int i=1;i<=60;++i) {
        const auto f=geometry_frame(320,1,12+8*std::sin(i*.15f),
                                   10+4*std::cos(i*.15f),i);
        const auto a=disabled.process(f), b=full_strength.process(f);
        expect(a.status==AimStatus::SUCCESS && b.status==a.status &&
                   a.has_target && b.has_target && a.has_command==b.has_command &&
                   a.command.dx_counts==b.command.dx_counts &&
                   a.command.dy_counts==b.command.dy_counts &&
                   a.control.shaped_x_counts==b.control.shaped_x_counts &&
                   a.control.filtered_x_counts==b.control.filtered_x_counts &&
                   a.control.modelled_response_x_counts==b.control.modelled_response_x_counts,
               "strength1必须逐帧保持双轴命令及浮点账本等价，不能仅累计量相等");
        if (a.has_command) expect(disabled.record_backend_completed_command(
            f.sequence,f.control_at+std::chrono::microseconds(100),
            a.command.dx_counts,a.command.dy_counts),"禁用软区对照完成回执必须被接受");
        if (b.has_command) expect(full_strength.record_backend_completed_command(
            f.sequence,f.control_at+std::chrono::microseconds(100),
            b.command.dx_counts,b.command.dy_counts),"strength1对照完成回执必须被接受");
    }
}

void test_soft_zone_requires_target_window() {
    const auto sample = [](float offset, float radius, int roi, bool head_only) {
        auto c = config(.475f);
        c.soft_zone_radius_percent=radius; c.soft_zone_min_strength=.2f;
        c.body_aim_range_percent=50; c.body_aim_height_ratio=.5f;
        c.deadzone_pixels=0; c.enable_delay_compensation=false;
        Aim aim(c); AimFrame f;
        f.sequence=1; f.roi_width=f.roi_height=roi;
        f.source_pixels_per_roi_pixel_x=f.source_pixels_per_roi_pixel_y=320.0f/roi;
        f.control_center_x=f.control_center_y=roi*.5f; f.lock_active=true;
        f.captured_at=std::chrono::steady_clock::time_point(std::chrono::seconds(600));
        f.control_at=f.captured_at;
        const float scale=roi/320.0f, x=f.control_center_x+offset*scale;
        f.detections={{x-16*scale,f.control_center_y-16*scale,
                       x+16*scale,f.control_center_y+16*scale,.95f,head_only?1:0}};
        const auto r=aim.process(f);
        expect(r.status==AimStatus::SUCCESS && r.has_target &&
                   r.target.matched_observation_valid && r.control.evaluated,
               "捕获内窗必须由实际matched目标及生产控制覆盖");
        return r;
    };
    for(bool head_only:{false,true}) for(int roi:{320,640}) {
        for(float offset:{-24.0f,-12.0f,-8.0f,8.0f,12.0f,24.0f}) {
            const auto a=sample(offset,0,roi,head_only),b=sample(offset,30,roi,head_only);
            expect(std::fabs(a.control.shaped_x_counts)>.001f &&
                       a.control.shaped_x_counts==b.control.shaped_x_counts &&
                       a.command.dx_counts==b.command.dx_counts,
                   "matched身体或头部50%内窗外和边界不得因ROI软区提前削弱追赶");
        }
        const auto a=sample(4,0,roi,head_only),b=sample(4,30,roi,head_only);
        expect(std::fabs(b.control.shaped_x_counts)>.001f &&
                   std::fabs(b.control.shaped_x_counts)<std::fabs(a.control.shaped_x_counts),
               "进入当前目标内窗后仍保留非零弱跟随");
        const auto edge=sample(7.99f,30,roi,head_only),edge_off=sample(7.99f,0,roi,head_only);
        expect(std::fabs(edge.control.shaped_x_counts-edge_off.control.shaped_x_counts)<.001f,
               "从内窗内趋近边界必须连续恢复追赶而非硬开关");
    }
}

void test_soft_zone_leaves_current_window_without_latching() {
    auto c=config(.475f);
    c.soft_zone_radius_percent=30; c.soft_zone_min_strength=.2f;
    c.body_aim_height_ratio=.5f; c.body_aim_range_percent=50;
    c.deadzone_pixels=0; c.enable_delay_compensation=false;
    auto off_c=c; off_c.soft_zone_radius_percent=0;
    Aim soft(c),off(off_c);
    AimFrame f;
    f.sequence=1; f.observation_epoch=82;
    f.captured_at=std::chrono::steady_clock::time_point(std::chrono::seconds(650));
    f.control_at=f.captured_at;
    f.roi_width=f.roi_height=320; f.control_center_x=f.control_center_y=160;
    f.lock_active=true; f.detections={{144,144,176,176,.95f,0}};
    const auto inside=soft.process(f),inside_off=off.process(f);
    expect(inside.has_target && inside_off.has_target &&
               inside.target.matched_observation_valid && inside.control.evaluated &&
               !inside.has_command && !inside_off.has_command &&
               inside.control.shaped_x_counts==0 && inside_off.control.shaped_x_counts==0,
           "出窗对照先在中心建立同轨迹零误差，避免历史控制状态造成假差异");
    f.sequence=2; f.captured_at+=std::chrono::milliseconds(4); f.control_at=f.captured_at;
    f.detections={{156,144,188,176,.95f,0}};
    const auto outside=soft.process(f),outside_off=off.process(f);
    const float current_left=outside.target.matched_observation_x1+
        (outside.target.matched_observation_x2-outside.target.matched_observation_x1)*.25f;
    expect(outside.has_target && outside_off.has_target &&
               outside.target.track_id==inside.target.track_id &&
               outside_off.target.track_id==inside_off.target.track_id &&
               outside.target.matched_observation_valid &&
               outside.target.matched_observation_x1==156 && current_left>160,
           "次帧必须是同身份的当前matched内窗出界，不能靠新身份或旧框通过");
    expect(std::fabs(outside_off.control.shaped_x_counts)>.001f &&
               outside.control.shaped_x_counts==outside_off.control.shaped_x_counts &&
               outside.command.dx_counts==outside_off.command.dx_counts,
           "曾在窗内不能锁存弱化资格，当前matched出窗必须立即恢复完整追赶");
}

void test_soft_zone_missing_background_chase() {
    auto c=config(.475f); c.soft_zone_radius_percent=30; c.soft_zone_min_strength=.2f;
    c.body_aim_height_ratio=.5f; c.body_aim_range_percent=50;
    Aim aim(c); double error=28;
    int previous_command=0,outside=0,inside=0,moving_inside=0,sent=0,audits=0;
    constexpr double plant=.2216375/.425;
    for(int i=0;i<960;++i) {
        // 无背景且源观测有周期扰动；相机只反馈本分支已完成命令。
        if(i) error+=(i<600?.5:0)-plant*previous_command;
        const double observed=error+.35*std::sin(i*.37);
        AimFrame f; f.sequence=i+1; f.observation_epoch=81;
        f.captured_at=std::chrono::steady_clock::time_point(std::chrono::seconds(700))+
            std::chrono::microseconds(4167*i); f.control_at=f.captured_at;
        f.roi_width=f.roi_height=320; f.control_center_x=f.control_center_y=160; f.lock_active=true;
        f.detections={{static_cast<float>(144+observed),144,static_cast<float>(176+observed),176,.95f,0}};
        const auto r=aim.process(f);
        expect(r.status==AimStatus::SUCCESS && r.has_target &&
                   r.control.background_motion_use_x!=AimBackgroundMotionUse::CONSUMED,
               "扰动追赶明确覆盖缺BG，不能依赖维护豁免");
        previous_command=r.has_command?r.command.dx_counts:0;
        if(r.has_command) { ++sent; expect(aim.record_backend_completed_command(
            f.sequence,f.control_at,previous_command,r.command.dy_counts),"缺BG反馈回执有效"); }
        if(i<600 && std::fabs(observed)>8 && std::fabs(observed)<48) {
            ++outside;
            {
                ++audits; auto off_c=c; off_c.soft_zone_radius_percent=0;
                Aim off(off_c),soft(c); const auto a=off.process(f),b=soft.process(f);
                expect(a.has_target && b.has_target && std::fabs(a.control.shaped_x_counts)>.001f &&
                           a.control.shaped_x_counts==b.control.shaped_x_counts,
                       "实际缺BG扰动反馈产生的框外追赶观测不得被大软区提前减力");
            }
        }
        if(i<600 && std::fabs(observed)<8) ++moving_inside;
        if(i>=600 && std::fabs(observed)<8) ++inside;
    }
    expect(outside>0 && inside>20 && sent>20 && audits>0,
           "缺BG扰动追赶与停止后框内跟随均须非空覆盖");
    expect(moving_inside>outside,
           "缺BG有扰动时也须在持续移动阶段大部分时间追入安全窗，不能等目标停下才进入");
    std::cout<<"缺BG扰动追赶 outside="<<outside<<" moving_inside="<<moving_inside
             <<" inside="<<inside<<" sent="<<sent<<" audits="<<audits<<'\n';
}

struct SoftMotionSummary {
    double moving_error = 0, stopped_error = 0, turned_error = 0;
    double moving_maintenance = 0, stopped_maintenance = 0;
    int moving_samples = 0, stopped_samples = 0, turned_samples = 0;
};

SoftMotionSummary soft_motion_feedback(int direction, bool background_available,
                                     bool world_stationary) {
    auto c = config(.475f);
    c.soft_zone_radius_percent = 30;
    c.soft_zone_min_strength = .2f;
    c.body_aim_height_ratio = 1.0f / 3.0f;
    Aim aim(c);
    constexpr std::int64_t interval_ns = 4166667;
    constexpr double dt = interval_ns * 1e-9;
    constexpr double plant = .2216375 / .425;
    double error = direction * 8.0;
    int previous_command = 0;
    SoftMotionSummary summary;
    // 沿用observed_feedback已知离散相机，仅验证软件职责，不拟合物理plant。
    // 三阶段使用相同时间轴；相机反馈始终只来自当前分支已完成的真实请求。
    for (int i = 0; i < 1440; ++i) {
        const double background = i ? -plant * previous_command : 0.0;
        const double world_delta = world_stationary ? 0.0 :
            (i < 480 ? direction * 120.0 * dt : i < 960 ? 0.0 : -direction * 120.0 * dt);
        if (i) error += background + world_delta;
        AimFrame f;
        f.sequence = 100 + i;
        f.observation_epoch = 71;
        f.captured_at = std::chrono::steady_clock::time_point(std::chrono::seconds(400)) +
            std::chrono::nanoseconds(interval_ns * i);
        f.control_at = f.captured_at;
        f.roi_width = f.roi_height = 320;
        f.control_center_x = f.control_center_y = 160;
        f.lock_active = true;
        f.detections = {{static_cast<float>(136 + error),120,
                         static_cast<float>(184 + error),240,.95f,0}};
        f.background_motion_x = {
            background_available ? AimBackgroundMotionStatus::VALID : AimBackgroundMotionStatus::MISSING,
            f.sequence - 1, f.sequence, f.captured_at - std::chrono::nanoseconds(interval_ns),
            f.captured_at, f.observation_epoch, static_cast<float>(background), .9f, 0,
            background_available ? 2 : 0};
        const auto r = aim.process(f);
        previous_command = r.has_command ? r.command.dx_counts : 0;
        expect(r.status == AimStatus::SUCCESS && r.has_target &&
                   std::abs(previous_command) <= 14 && r.command.dy_counts == 0,
               "软区反馈各阶段保持目标、有限输出及原Y和向量上限");
        if (r.has_command) expect(aim.record_backend_completed_command(
            f.sequence, f.control_at, previous_command, 0),
            "闭环只确认当前分支自身命令，不能复制另一分支输出");
        if (i >= 360 && i < 480) {
            summary.moving_error += std::fabs(error);
            summary.moving_maintenance += std::fabs(r.control.modelled_response_x_counts);
            ++summary.moving_samples;
        }
        if (i >= 840 && i < 960) {
            summary.stopped_error += std::fabs(error);
            summary.stopped_maintenance += std::fabs(r.control.modelled_response_x_counts);
            ++summary.stopped_samples;
        }
        if (i >= 1320) { summary.turned_error += std::fabs(error); ++summary.turned_samples; }
        if (!world_stationary && background_available && i >= 1320)
            expect(r.control.modelled_response_x_counts * direction <= .001f,
                   "反向连续真实观测后不得继续保留旧方向维护份额");
    }
    summary.moving_error /= summary.moving_samples;
    summary.moving_maintenance /= summary.moving_samples;
    summary.stopped_error /= summary.stopped_samples;
    summary.stopped_maintenance /= summary.stopped_samples;
    summary.turned_error /= summary.turned_samples;
    return summary;
}

void test_soft_zone_observed_motion() {
    for (int direction : {-1,1}) {
        const auto motion = soft_motion_feedback(direction,true,false);
        const auto missing = soft_motion_feedback(direction,false,false);
        const auto stationary = soft_motion_feedback(direction,true,true);
        std::cout << "软区观测闭环 direction=" << direction
                  << " moving=" << motion.moving_error << " stopped=" << motion.stopped_error
                  << " turned=" << motion.turned_error << " maintenance=" << motion.moving_maintenance
                  << " stopped_maintenance=" << motion.stopped_maintenance
                  << " missing=" << missing.moving_error << " static=" << stationary.moving_error << '\n';
        expect(motion.moving_maintenance > .01,
               "可信背景闭环须真正覆盖非零运动维护，不能靠空覆盖通过");
        expect(motion.moving_error <= 1.5 && motion.turned_error <= 1.5,
               "可信匹配的持续横移与转向在软区内也应回到既有死区，不能长期欠跟随");
        expect(motion.stopped_error <= 1.5 && motion.stopped_maintenance < .05,
               "目标停止后运动维护应退出，不能以持续拖动换取移动误差改善");
        expect(stationary.moving_error <= 1.5 && stationary.moving_maintenance < .05,
               "静态目标与背景同动不得被识别成独立运动维护");
        expect(missing.stopped_error <= 1.5,
               "缺失背景仍能完成静态位置收敛，不能将未知背景当真零外推");
    }
}

void test_soft_zone_background_activation_budget() {
    auto c = config(.475f);
    c.soft_zone_radius_percent = 30;
    c.soft_zone_min_strength = .2f;
    c.body_aim_height_ratio = 1.0f / 3.0f;
    Aim aim(c);
    bool consumed_moving_background = false;
    bool resumed_moving_background = false;
    for (int i = 0; i < 32; ++i) {
        AimFrame f;
        f.sequence = i + 1;
        f.observation_epoch = 72;
        f.captured_at = std::chrono::steady_clock::time_point(std::chrono::seconds(500)) +
            std::chrono::microseconds(4167 * i);
        f.control_at = f.captured_at;
        f.roi_width = f.roi_height = 320;
        f.control_center_x = f.control_center_y = 160;
        f.lock_active = i >= 12;
        f.ease_first_activation = true;
        const float x = 160 + .5f * i;
        f.detections = {{x-24,120,x+24,240,.95f,0}};
        // 激活时已有可信世界平移；中途短缺BG不能清掉或提前消费接管额度。
        const bool available = i < 18 || i >= 22;
        f.background_motion_x = {
            available ? AimBackgroundMotionStatus::VALID : AimBackgroundMotionStatus::MISSING,
            f.sequence-1,f.sequence,f.captured_at-std::chrono::microseconds(4167),
            f.captured_at,f.observation_epoch,0,.9f,0,available ? 2 : 0};
        const auto r = aim.process(f);
        expect(r.status == AimStatus::SUCCESS && r.has_target,
               "软区与背景接管夹具必须保持真实目标");
        if (i >= 12) {
            const float allowance = 14 * std::min(1.0f,(i-12)*.004167f/.1f);
            expect(std::fabs(r.control.shaped_x_counts) <= allowance + .001f,
                   "软区保护运动维护不能绕过激活额度，BG失效恢复也不得重置为满额");
            if (i == 12) expect(!r.has_command && std::fabs(r.control.shaped_x_counts)<.001f,
                                "已有BG运动支持的软区激活首帧仍为零预算");
            if (r.control.background_motion_use_x == AimBackgroundMotionUse::CONSUMED &&
                r.control.modelled_response_x_counts > .001f) {
                consumed_moving_background |= i < 18;
                resumed_moving_background |= i >= 22;
            }
        }
        if (r.has_command) expect(aim.record_backend_completed_command(
            f.sequence,f.control_at,f.lock_active?r.command.dx_counts:0,0),
            "软区激活只回执许可后的本分支命令");
    }
    expect(consumed_moving_background && resumed_moving_background,
           "接管额度回归必须在背景失效前后实际消费非零运动维护，不能以空覆盖通过");
}
}

int main() {
    for (int axis : {0,1,2}) for (int preview : {0,60}) test_takeover(axis,preview);
    for (int axis : {0,1,2}) test_reacquisition_interval(axis);
    test_session_boundaries();
    test_transition_clock();
    test_activation_gap();
    test_moving_activation_budget();
    test_far_y_activation_budget();
    test_far_y_recoil_pause_budget();
    test_legacy_y_recoil_release_bypasses_extended_cap();
    test_far_y_discrete_feedback_settles();
    test_soft_zone();
    test_soft_zone_requires_target_window();
    test_soft_zone_leaves_current_window_without_latching();
    test_soft_zone_missing_background_chase();
    test_soft_zone_observed_motion();
    test_soft_zone_background_activation_budget();
    return failures ? 1 : 0;
}
