#include "auto_stop/auto_stop.h"

#include <array>
#include <iostream>

namespace {
int failures = 0;
void expect(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << message << '\n'; }
}
}

int main() {
    constexpr std::int64_t base = 1000000000, ms = 1000000;
    auto duration = [&](int hold) {
        AutoStopController controller;
        WasdInputHistory input;
        controller.observe(input.observe(0, 1, 1, base), base);
        controller.observe(input.observe(1, 1, 2, base + ms), base + ms);
        auto pending = controller.request(1, base + (hold + 1) * ms);
        expect(pending.phase == AutoStopPhase::WAITING_ACK && pending.desired_mask == 4,
               "短长按都只请求反向，报告未ACK不能开始计时");
        expect(pending.axis_deadline_ns[0] == 0, "ACK前截止时间必须未知");
        const auto ack_time = base + (hold + 2) * ms;
        auto braking = controller.acknowledge(1, pending.command_id, 4, ack_time);
        expect(braking.phase == AutoStopPhase::BRAKING && !braking.fire_permitted,
               "ACK只开启估计制动，不允许开火");
        const auto deadline = braking.axis_deadline_ns[0];
        expect(controller.acknowledge(1, pending.command_id - 1, 4, ack_time).axis_deadline_ns[0] == deadline,
               "旧报告token不得重设计时");
        auto released = controller.tick(deadline);
        expect(released.desired_mask == 0 && released.phase == AutoStopPhase::WAITING_ACK,
               "预计过零后请求释放，不能未ACK就完成");
        expect(controller.acknowledge(1, released.command_id, 0, deadline).phase == AutoStopPhase::COMPLETE_ESTIMATED,
               "释放ACK后仅完成估计动作");
        expect(!controller.decision().fire_permitted, "估计结束仍不得允许开火");
        return deadline - ack_time;
    };
    const auto short_duration = duration(150), long_duration = duration(400);
    expect(short_duration > 50 * ms && short_duration < 90 * ms &&
           long_duration > short_duration && long_duration < 140 * ms,
           "连续持键库存应区分短长按，不能固定75ms");
    {
        AutoStopController controller;
        WasdInputHistory input;
        controller.observe(input.observe(0, 1, 1, base), base + 100 * ms);
        controller.tick(base + 200 * ms);
        controller.observe(input.observe(1, 1, 2, base + 10 * ms), base + 210 * ms);
        controller.observe(input.observe(3, 1, 3, base + 90 * ms), base + 210 * ms);
        auto requested = controller.request(1, base + 240 * ms);
        expect(requested.phase == AutoStopPhase::WAITING_ACK && requested.desired_mask == 12,
               "空闲轮询不吞掉接收边沿，同批有序双键按事件时间积分");
        auto braking = controller.acknowledge(1, requested.command_id, requested.desired_mask, base + 241 * ms);
        controller.tick(base + 242 * ms);
        auto changed = input.observe(1, 1, 4, base + 241 * ms);
        expect(controller.observe(changed, base + 243 * ms).phase == AutoStopPhase::CANCELLED,
               "活动改向即使排队早于上次tick，也应取消而非伪造边沿");
    }
    for (unsigned first : {1U, 2U, 4U, 8U}) for (unsigned second : {1U, 2U, 4U, 8U}) {
        if (first == second || (first | second) == 5 || (first | second) == 10) continue;
        AutoStopController controller;
        WasdInputHistory input;
        controller.observe(input.observe(0, 1, 1, base), base);
        controller.observe(input.observe(first, 1, 2, base + ms), base + ms);
        controller.observe(input.observe(first | second, 1, 3, base + 81 * ms), base + 81 * ms);
        auto pending = controller.request(1, base + 231 * ms);
        auto braking = controller.acknowledge(1, pending.command_id, pending.desired_mask, base + 231 * ms);
        const unsigned leading = (first == 1 || first == 4) ? 0 : 1;
        const auto leading_deadline = braking.axis_deadline_ns[leading];
        const auto trailing_deadline = braking.axis_deadline_ns[1 - leading];
        expect(leading_deadline > trailing_deadline, "先按轴的库存和截止时间应更大");
        auto transition = controller.tick(trailing_deadline);
        expect(transition.desired_mask != 0 && transition.desired_mask != pending.desired_mask,
               "后按轴先释放，保留另一个轴制动");
        braking = controller.acknowledge(1, transition.command_id, transition.desired_mask, trailing_deadline);
        expect(braking.axis_deadline_ns[leading] < leading_deadline &&
               braking.axis_deadline_ns[leading] > trailing_deadline,
               "双轴改单轴幅值改变必须重算剩余零点");
        controller.cancel(1, trailing_deadline);
        expect(controller.decision().phase == AutoStopPhase::CANCELLED && controller.decision().desired_mask == 0,
               "取消立即撤销期望反向位图");
        expect(controller.acknowledge(1, transition.command_id, transition.desired_mask, trailing_deadline).phase == AutoStopPhase::CANCELLED,
               "取消后旧ACK不得复活请求");
    }
    {
        AutoStopController controller;
        WasdInputHistory input;
        controller.observe(input.observe(0, 1, 1, base), base);
        controller.observe(input.observe(1, 1, 2, base + ms), base + ms);
        controller.request(5, base + 150 * ms);
        expect(controller.request(4, base + 151 * ms).request_id == 5, "旧请求不得替换当前请求");
        expect(controller.observe(input.observe(3, 1, 3, base + 152 * ms), base + 152 * ms).phase == AutoStopPhase::CANCELLED,
               "活动中人工轴变化应取消，不能继续旧计划");
        expect(controller.tick(base).phase == AutoStopPhase::INVALID, "单调时间倒退撤销模型资格");
    }
    for (int kind = 0; kind < 3; ++kind) {
        AutoStopController controller;
        WasdInputHistory input;
        controller.observe(input.observe(0, 1, 1, base), base);
        controller.observe(input.observe(1, 1, 2, base + ms), base + ms);
        controller.request(1, base + 150 * ms);
        auto intent = kind == 0 ? input.observe(5, 1, 3, base + 151 * ms) :
                      kind == 1 ? input.observe(1, 1, 3, base + 151 * ms, true, true) :
                                  input.observe(0, 2, 1, base + 151 * ms);
        expect(controller.observe(intent, base + 151 * ms).phase == AutoStopPhase::INVALID,
               "冲突、缺口、代际切换必须撤销活动请求");
    }
    {
        AutoStopController controller;
        WasdInputHistory input;
        controller.observe(input.observe(0, 1, 1, base), base);
        controller.observe(input.observe(1, 1, 2, base + ms), base + ms);
        controller.observe(input.observe(1, 1, 4, base + 2 * ms, true, true), base + 2 * ms);
        expect(controller.observe(input.observe(0, 1, 5, base + 3 * ms), base + 3 * ms).phase == AutoStopPhase::IDLE,
               "失同步后新的合法全释放可跨序号缺口恢复，不永久锁死");
        controller.observe(input.observe(1, 1, 6, base + 4 * ms), base + 4 * ms);
        auto state = controller.request(1, base + 154 * ms);
        expect(state.phase == AutoStopPhase::WAITING_ACK, "恢复后新按键应能够产生估计请求");
        auto old = input.observe(0, 1, 5, base + 3 * ms);
        expect(controller.observe(old, base + 155 * ms).phase == AutoStopPhase::INVALID,
               "恢复不能放开旧序号旧时间的回退检查");
    }
    constexpr std::array<unsigned, 4> keys{1, 2, 4, 8};
    for (const auto first : keys) {
        for (const auto second : keys) {
            if ((first | second) == 5 || (first | second) == 10) continue;
            WasdInputHistory history;
            history.observe(0, 1, 1, 100);
            const auto one = history.observe(first, 1, 2, 200);
            expect(one.history_valid && !one.conflicting, "合法单键应保留可信输入历史");
            const auto two = history.observe(first | second, 1, 3, 300);
            expect(two.history_valid && !two.conflicting, "所有相邻双键进入顺序应可表示");
            for (unsigned index = 0; index < 4; ++index) {
                if ((1U << index) == first)
                    expect(two.held_since_ns[index] == 200, "追加另一个轴不得重置先按轴的时间");
                else if ((1U << index) == second)
                    expect(two.held_since_ns[index] == 300, "后按轴保留独立边沿时间");
            }
            const auto released = history.observe(second, 1, 4, 400);
            expect(released.history_valid, "斜向改单向应保持剩余轴历史");
            const auto stopped = history.observe(0, 1, 5, 500);
            expect(stopped.history_valid && stopped.held_mask == 0,
                   "明确全松开应归零输入意图，而非输出物理停稳结论");
        }
    }
    WasdInputHistory history;
    expect(!history.observe(1, 1, 1, 100).history_valid,
           "启动时已经按住不能伪造按下时间");
    history.observe(0, 1, 2, 200);
    history.observe(1, 1, 3, 300);
    const auto conflict = history.observe(5, 1, 4, 400);
    expect(conflict.conflicting && !conflict.history_valid,
           "相反键冲突必须使制动历史失去资格");
    expect(!history.observe(4, 1, 5, 500).history_valid,
           "冲突解除不能凭空补出先前运动历史");
    history.observe(0, 1, 6, 600);
    history.observe(3, 1, 7, 700);
    expect(!history.observe(0, 1, 8, 800, false).history_valid,
           "错误报告不能冒充物理全释放");
    history.observe(0, 1, 9, 900);
    expect(!history.observe(1, 1, 10, 1000, true, true).history_valid,
           "输入缺口必须撤销历史资格");
    history.observe(0, 2, 1, 1100);
    expect(!history.observe(1, 1, 50, 1200).history_valid,
           "旧代际迟到不能重建当前历史");
    history.observe(0, 2, 2, 1300);
    history.observe(2, 2, 3, 1400);
    expect(history.observe(2, 2, 3, 1400).held_since_ns[1] == 1400,
           "重复同一事实不能重置持键时间");
    expect(!history.observe(0, 2, 3, 1400).history_valid,
           "同序号不同键态不得被接受为释放");
    history.observe(0, 3, 1, 1500);
    history.observe(1, 3, 2, 1600);
    history.observe(0, 0, 1, 1700);
    expect(!history.observe(0, 2, 10, 1800).history_valid,
           "零代际错误报告不能清除已见代际水位并接纳旧代际释放");
    history.observe(0, 3, 3, 1900);
    history.observe(1, 3, 4, 2000);
    history.observe(1, 3, 6, 2200, false);
    expect(!history.observe(0, 3, 5, 2100).history_valid,
           "无效新报告的序号和时间水位不能被较早释放跨过");
    expect(history.observe(0, 3, 7, 2300).history_valid,
           "错误之后仅新的合法全释放能够重新同步");
    for (const bool enabled : {false, true}) {
        for (const bool supported : {false, true}) {
            for (const bool paused : {false, true}) {
                const auto state = assess_auto_stop_availability(
                    {enabled, 0x05}, supported, true, paused);
                expect(!state.fire_permitted && !state.stop_evidence_available,
                       "所有配置/暂停组合均不得把首批协议能力变成停稳或开火许可");
            }
        }
    }
    if (failures == 0) std::cout << "自动急停输入历史与未验证输出边界通过\n";
    return failures == 0 ? 0 : 1;
}
