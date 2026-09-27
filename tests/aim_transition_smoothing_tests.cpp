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
    AimResult step(bool locked, int axis, bool other = false, bool missing = false) {
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
        const float x = 160 + (axis != 1 ? sign * 100 : 0);
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
    if (axis==2) expect(b.command.dx_counts!=0 && b.command.dy_counts!=0,
                        "双轴夹具必须实际产生两轴命令，不能被Y限幅退化为单轴");
    std::cout << "接管 axis=" << axis << " preview=" << preview_frames
              << " smooth=" << a.command.dx_counts << ',' << a.command.dy_counts
              << " direct=" << b.command.dx_counts << ',' << b.command.dy_counts << '\n';
    expect(a.has_target && a.has_command, "接管立即可用，不增加等待门禁");
    if (preview_frames > 0) {
        expect(magnitude(a) < magnitude(b), "预计算后接管首步必须响应既有平滑配置");
    } else {
        // 无前序控制区间的冷启动保持原合同；本轮否决了同时改冷启动的
        // 候选，因为既有半身恢复闭环出现额外反转，不以换golden掩盖。
        expect(a.command.dx_counts == b.command.dx_counts &&
                   a.command.dy_counts == b.command.dy_counts,
               "冷启动保留原首步，不冒充已验证的接管过渡");
    }
    // 不改变目标的持续锁定不能每帧重新起步；误差持续时必须保留追赶能力。
    bool grew = false;
    for (int i=0;i<12;++i) {
        grew |= magnitude(smooth.step(true,axis)) > magnitude(a);
        direct.step(true,axis);
    }
    if (preview_frames > 0) expect(grew, "同身份持续追踪不能每帧重启接管滤波");
    for (int i=0;i<12;++i) { smooth.step(false,axis); direct.step(false,axis); }
    const auto resumed = smooth.step(true,axis);
    const auto resumed_direct = direct.step(true,axis);
    expect(resumed.target.track_id == a.target.track_id, "重锁不重建观测身份");
    expect(resumed.has_command && magnitude(resumed) < magnitude(resumed_direct),
           "松键再接管立即输出且首步遵守平滑配置");
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
        expect(actual.command.dx_counts==expected.command.dx_counts &&
                   actual.command.dy_counts==expected.command.dy_counts,
               "松键、显式reset或源epoch变化后不继承旧会话过渡");
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
}

int main() {
    for (int axis : {0,1,2}) for (int preview : {0,60}) test_takeover(axis,preview);
    for (int axis : {0,1,2}) test_reacquisition_interval(axis);
    test_session_boundaries();
    test_transition_clock();
    return failures ? 1 : 0;
}
