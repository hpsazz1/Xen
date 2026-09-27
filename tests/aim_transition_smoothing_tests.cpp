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
    const auto baseline=run(0,.2f,12);
    const auto softened=run(30,.2f,12);
    std::cout<<"软区累计 baseline="<<baseline<<" softened="<<softened<<'\n';
    expect(baseline>0 && softened>0 && softened<baseline*.7,
           "近中心仍有少量跟随，持续输出须明显弱于关闭软区");
    expect(run(30,1,12)==baseline,"保留强度1精确兼容原输出");
    expect(run(30,.2f,80)==run(0,.2f,80),"软区外保持原输出");

    const auto soft_config=[](float radius,float strength) {
        auto c=config(.475f);
        c.soft_zone_radius_percent=radius;
        c.soft_zone_min_strength=strength;
        c.deadzone_pixels=0;
        c.body_aim_range_percent=1; // 最小合法框内范围，保持缩放对照几何一致。
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
        f.detections={{center+(x-16)*scale,center+(y-16)*scale,
                       center+(x+16)*scale,center+(y+84)*scale,.9f,0}};
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
    // 框内1%安全范围可能把基础点向中心移0.16 ROI像素；外侧样本跨过该余量。
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
}

int main() {
    for (int axis : {0,1,2}) for (int preview : {0,60}) test_takeover(axis,preview);
    for (int axis : {0,1,2}) test_reacquisition_interval(axis);
    test_session_boundaries();
    test_transition_clock();
    test_activation_gap();
    test_moving_activation_budget();
    test_soft_zone();
    return failures ? 1 : 0;
}
