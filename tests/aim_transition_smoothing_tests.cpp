#include "aim/aim.h"

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
    explicit Driver(float smoothing) : aim(config(smoothing)) {}
    AimResult step(bool locked, int axis, bool other = false, bool missing = false) {
        AimFrame f;
        f.sequence = ++index;
        f.captured_at = std::chrono::steady_clock::time_point(std::chrono::seconds(100)) +
            std::chrono::microseconds(index * 4167);
        f.control_at = f.captured_at + std::chrono::milliseconds(3);
        f.roi_width = f.roi_height = 320;
        f.control_center_x = f.control_center_y = 160;
        f.lock_active = locked;
        f.observation_epoch = 1;
        const float sign = other ? -1.0f : 1.0f;
        const float x = 160 + (axis != 1 ? sign * 100 : 0);
        const float y = 160 + (axis != 0 ? sign * 65 : 0);
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
            expect(next.has_command && magnitude(next) < magnitude(reference),
                   "持续控制中更换身份首步必须遵守平滑配置");
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
    expect(changed.has_command && changed.command.dx_counts == changed_direct.command.dx_counts &&
               changed.command.dy_counts == changed_direct.command.dy_counts,
           "完整丢失后的冷启动不继承已废弃控制状态");
}
}

int main() {
    for (int axis : {0,1,2}) for (int preview : {0,60}) test_takeover(axis,preview);
    return failures ? 1 : 0;
}
