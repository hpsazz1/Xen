#include "aim/aim.h"
#include "log/log.h"
#include <chrono>
#include <cmath>
#include <iostream>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
int failures = 0;
void expect(bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << "[失败] " << message << '\n'; }
}
AimFrame frame(std::uint64_t id, Clock::time_point at, bool external = true) {
    AimFrame f;
    f.sequence = id; f.captured_at = at; f.control_at = at + 2ms;
    f.roi_width = f.roi_height = 320;
    f.control_center_x = f.control_center_y = 160;
    f.observation_epoch = 1; f.lock_active = true;
    f.detections.push_back({190, 160, 250, 260, 0.95f, 0});
    f.external_motion = {external, true, 0, at - 1s, f.control_at, {}};
    return f;
}
void zero_receipt(Aim& aim, const AimFrame& f) {
    expect(aim.record_backend_completed_command(f.sequence, f.control_at, 0, 0), "Aim模拟零回执已记录");
}
void background(AimFrame& f, const AimFrame& previous, float dx = 0) {
    auto& b = f.background_motion_x;
    b.status = AimBackgroundMotionStatus::VALID;
    b.previous_sequence = previous.sequence; b.sequence = f.sequence;
    b.previous_captured_at = previous.captured_at; b.captured_at = f.captured_at;
    b.observation_epoch = 1; b.dx_roi_pixels = dx; b.min_response = 1; b.usable_patch_count = 2;
}
void test_window() {
    const auto t = Clock::time_point{} + 10s;
    ExternalMotionWindow w{true, true, 2, t, t + 10ms, {{1, t + 2ms, 9, 4}, {2, t + 3ms, -9, -1}}};
    const auto sum = external_motion_sum(w, t, t + 10ms);
    expect(sum && sum->first == 0 && sum->second == 3, "有符号总位移守恒");
    expect(!external_motion_proven_zero_x(w, t, t + 10ms), "正反命令净零不是未运动证据");
    expect(external_motion_proven_zero_x(w, t + 3ms, t + 10ms), "左开右闭查询排除已见事件");
    expect(!external_motion_sum(w, t - 1ms, t + 1ms), "查询早于覆盖不能当零");
    expect(!external_motion_sum(w, t, t + 11ms), "查询晚于快照拒绝");
    auto broken = w; broken.complete = false;
    expect(!external_motion_sum(broken, t, t + 1ms), "故障账本不可消费");
    broken = w; broken.events[1].id = 1;
    expect(!external_motion_sum(broken, t, t + 10ms), "重复id拒绝");
    broken = w; broken.revision = broken.events[1].id = 3;
    expect(!external_motion_sum(broken, t, t + 10ms), "中间缺失事件拒绝");
    broken = w; broken.revision = 3;
    expect(!external_motion_sum(broken, t, t + 10ms), "尾revision未覆盖拒绝");
    broken = w; broken.events.clear();
    expect(!external_motion_sum(broken, t, t + 10ms), "非零revision不能伪造空账本");
    broken = w; broken.events[1].completed_at = t + 1ms;
    expect(!external_motion_sum(broken, t, t + 10ms), "完成时间逆序拒绝");
    // FIFO已淘汰前缀允许从声明边界继续查询，不要求首id从1重新开始。
    w.events.erase(w.events.begin()); w.covered_from = t + 2ms;
    expect(external_motion_sum(w, t + 2ms, t + 10ms)->first == -9, "合法保留后缀可用");
}
void test_aim_only_equivalence() {
    const auto t = Clock::time_point{} + 10s;
    for (bool prediction : {false, true}) for (bool delay : {false, true}) {
        AimConfig c; c.min_confirmed_hits = 1; c.control_delay_ms = 15;
        c.enable_prediction = prediction; c.enable_delay_compensation = delay;
        Aim baseline(c), observed(c);
        AimFrame prior;
        for (std::uint64_t id = 1; id <= 24; ++id) {
            auto a = frame(id, t + std::chrono::milliseconds(id * 4), false);
            if (id > 1) background(a, prior);
            auto b = a; b.external_motion.enabled = true;
            const auto x = baseline.process(a), y = observed.process(b);
            expect(x.status == AimStatus::SUCCESS && y.status == x.status &&
                x.has_command == y.has_command && x.command.dx_counts == y.command.dx_counts &&
                x.command.dy_counts == y.command.dy_counts && x.target.aim_x == y.target.aim_x &&
                x.target.aim_y == y.target.aim_y && x.control.shaped_x_counts == y.control.shaped_x_counts &&
                x.control.feedforward_x_counts == y.control.feedforward_x_counts,
                "完整空外部账本逐帧保持Aim-only数值，四种预测/延迟组合");
            zero_receipt(baseline, a); zero_receipt(observed, b); prior = a;
        }
    }
}
void test_coverage_and_accounting() {
    const auto t = Clock::time_point{} + 10s;
    AimConfig c; c.min_confirmed_hits = 1; c.control_delay_ms = 15;
    Aim baseline(c), observed(c), truncated(c);
    auto first = frame(1, t);
    baseline.process(first); observed.process(first); truncated.process(first);
    zero_receipt(baseline, first); zero_receipt(observed, first); zero_receipt(truncated, first);
    auto second = frame(2, t + 4ms); background(second, first);
    auto moved = second;
    moved.external_motion.revision = 1;
    moved.external_motion.events.push_back({1, second.control_at - 1ms, 6, 1000});
    const auto a = baseline.process(second), b = observed.process(moved);
    expect(a.status == AimStatus::SUCCESS && b.status == AimStatus::SUCCESS && a.control.evaluated && b.control.evaluated,
           "有完整账本的控制实际完成");
    expect(b.control.execution_unseen_command_x_counts - a.control.execution_unseen_command_x_counts == 6,
           "外部未见位移只扣一次X执行库存");
    expect(b.control.pending_net_x_counts - a.control.pending_net_x_counts == 6 &&
           b.control.pending_absolute_x_counts - a.control.pending_absolute_x_counts == 6,
           "总执行库存包含外部完成，未混入Aim自身issued环");
    expect(a.command.dy_counts > 0 && b.command.dy_counts == 0, "外部Y库存支付剩余同向请求");
    zero_receipt(observed, moved);
    auto third = frame(3, t + 8ms); background(third, second);
    third.external_motion = moved.external_motion; third.external_motion.covered_until = third.control_at;
    expect(observed.process(third).control.execution_unseen_command_x_counts == 6,
           "Aim零回执不删除外部库存也不重复记录外部命令");
    auto missing = second;
    missing.external_motion.covered_from = first.captured_at - 14ms;
    const auto denied = truncated.process(missing);
    expect(denied.status == AimStatus::INVALID_INPUT && !denied.has_command,
           "仅当前库存完整但raw帧对历史缺失禁止旧模型输出");
}
void test_background_not_double_counted() {
    const auto t = Clock::time_point{} + 10s;
    AimConfig c; c.min_confirmed_hits = 1; c.control_delay_ms = 15;
    Aim a(c);
    auto first = frame(1, t); a.process(first); zero_receipt(a, first);
    auto second = frame(2, t + 4ms); background(second, first, -3);
    second.external_motion.revision = 1;
    second.external_motion.events.push_back({1, second.captured_at - 16ms, 10, 0});
    const auto result = a.process(second);
    expect(result.status == AimStatus::SUCCESS && result.control.background_motion_use_x == AimBackgroundMotionUse::CONSUMED &&
           result.control.observer_camera_motion_x_source_pixels == -3,
           "同源背景已包含全部相机运动，外部模型不得叠加");
}
}
int main() {
    LogConfig log; log.enable_console = false; log.enable_file = false; log.enable_ringbuf = false;
    Log::init(log);
    test_window(); test_aim_only_equivalence(); test_coverage_and_accounting(); test_background_not_double_counted();
    Log::shutdown();
    return failures == 0 ? 0 : 1;
}
