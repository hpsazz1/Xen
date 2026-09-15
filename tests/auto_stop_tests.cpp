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
    AutoStopConfig legacy;
    legacy.use_counterpulse_timing = false;
    for (const std::uint8_t before_mask : {1, 2, 4, 8, 3, 9, 6, 12}) {
        for (const std::uint8_t after_mask : {0, 1, 2, 4, 8, 3, 9, 6, 12}) {
            WasdInputHistory history;
            history.observe(0, 1, 1, base);
            const auto before = history.observe(before_mask, 1, 2, base + ms);
            const auto after = history.observe(after_mask, 1, 3, base + 2 * ms);
            const auto expected = static_cast<std::uint8_t>(after_mask == 0 ? before_mask : 0);
            expect(WasdReleasedAxes(before, after) == expected,
                   "人工释放须WASD全部松开，剩余持键、重复状态及直接换向不得误触发");
        }
    }
    {
        WasdInputHistory history;
        history.observe(0, 1, 1, base);
        const auto before = history.observe(9, 1, 2, base + ms);
        const auto after = history.observe(8, 1, 3, base + 2 * ms);
        expect(WasdReleasedAxes(before, after) == 0, "W+D松W仍持D，不得制动");
        const auto all_released = history.observe(0, 1, 4, base + 3 * ms);
        expect(WasdReleasedAxes(after, all_released) == 8,
               "D最后松开只制动D，不累计此前已松开的W");
        for (int invalid_case = 0; invalid_case < 9; ++invalid_case) {
            auto invalid = after;
            invalid.held_mask = 0;
            switch (invalid_case) {
                case 0: invalid.sequence = before.sequence; break;
                case 1: ++invalid.sequence; break;
                case 2: ++invalid.epoch; break;
                case 3: invalid.received_at_ns = before.received_at_ns - 1; break;
                case 4: invalid.input_continuous = false; break;
                case 5: invalid.history_valid = false; break;
                case 6: invalid.conflicting = true; break;
                case 7: invalid.held_mask = 10; break;
                case 8: invalid.held_mask = 16; break;
            }
            expect(WasdReleasedAxes(before, invalid) == 0,
                   "释放检测拒绝重复游标、缺口、换代、倒时及无效历史");
        }
        auto invalid_before = after;
        invalid_before.history_valid = false;
        expect(WasdReleasedAxes(invalid_before, all_released) == 0, "不得由无效前态推导释放");
        expect(WasdReleasedAxes(all_released, history.observe(0, 1, 5, base + 4 * ms)) == 0,
               "持续全松的新序号报告不得重复触发");
    }
    for (const std::uint8_t released : {1, 2, 4, 8, 3, 9, 6, 12}) {
        AutoStopConfig manual_config;
        manual_config.counter_hold_ms = 17;
        manual_config.shot_after_release_ms = 9;
        AutoStopController controller(manual_config);
        WasdInputHistory history;
        controller.observe(history.observe(0, 1, 1, base), base);
        controller.observe(history.observe(released, 1, 2, base + ms), base + ms);
        controller.observe(history.observe(0, 1, 3, base + 2 * ms), base + 2 * ms);
        auto d = controller.request_manual_release(1, released, base + 3 * ms);
        expect(d.phase == AutoStopPhase::WAITING_ACK && d.desired_mask == 0,
               "真实松键后人工制动先零报告，不伪造持键");
        const auto first_command = d.command_id;
        expect(controller.request_manual_release(2, released, base + 3 * ms).command_id == first_command,
               "活动人工制动不得被第二个请求替换");
        d = controller.acknowledge(1, d.command_id, 0, base + 4 * ms);
        const auto inverse = static_cast<std::uint8_t>(((released & 1) << 2) | ((released & 4) >> 2) |
            ((released & 2) << 2) | ((released & 8) >> 2));
        expect(d.phase == AutoStopPhase::WAITING_ACK && d.desired_mask == inverse,
               "人工制动仅反转已释放原方向");
        d = controller.acknowledge(1, d.command_id, inverse, base + 5 * ms);
        expect(((released & 5) == 0 || d.axis_deadline_ns[0] == base + 22 * ms) &&
                   ((released & 10) == 0 || d.axis_deadline_ns[1] == base + 22 * ms),
               "反向按住计时从实际ACK开始并复用配置");
        expect(controller.tick(base + 22 * ms - 1).desired_mask == inverse,
               "人工反向按住不得提前释放");
        d = controller.tick(base + 22 * ms);
        expect(d.phase == AutoStopPhase::WAITING_ACK && d.desired_mask == 0,
               "人工hold到期须请求零报告");
        d = controller.acknowledge(1, d.command_id, 0, base + 25 * ms);
        expect(d.phase == AutoStopPhase::SETTLING && d.completion_ready_ns == base + 34 * ms,
               "人工settle从最终零ACK计算");
        expect(controller.tick(base + 34 * ms - 1).phase == AutoStopPhase::SETTLING,
               "人工settle期限前不得完成");
        d = controller.tick(base + 34 * ms);
        expect(d.phase == AutoStopPhase::COMPLETE_ESTIMATED && !d.fire_permitted,
               "人工制动完成仍不构成开火资格");
        controller.cancel(1, base + 35 * ms);
        controller.observe(history.observe(released, 1, 4, base + 36 * ms), base + 36 * ms);
        controller.observe(history.observe(0, 1, 5, base + 37 * ms), base + 37 * ms);
        d = controller.request_manual_release(2, released, base + 38 * ms);
        expect(d.request_id == 2 && d.phase == AutoStopPhase::WAITING_ACK && d.desired_mask == 0,
               "人工制动完成清理后下一次真实按下全松可开始新轮");
    }
    {
        AutoStopController controller;
        WasdInputHistory history;
        controller.observe(history.observe(0, 1, 1, base), base);
        controller.observe(history.observe(9, 1, 2, base + ms), base + ms);
        controller.observe(history.observe(8, 1, 3, base + 2 * ms), base + 2 * ms);
        for (const std::uint8_t invalid_mask : {0, 1, 5, 10, 16, 255, 2, 8})
            expect(controller.request_manual_release(1, invalid_mask, base + 3 * ms).request_id == 0,
                   "人工制动拒绝非法掩码、冲突轴及任何WASD仍持键");
        controller.observe(history.observe(0, 1, 4, base + 4 * ms), base + 4 * ms);
        for (const std::uint8_t invalid_mask : {0, 5, 10, 16, 255})
            expect(controller.request_manual_release(1, invalid_mask, base + 5 * ms).request_id == 0,
                   "全松后仍拒绝非法掩码及冲突轴");
        auto d = controller.request_manual_release(1, 8, base + 5 * ms);
        d = controller.acknowledge(1, d.command_id, 0, base + 6 * ms);
        expect(d.desired_mask == 2, "W+D先松W再松D只计划A，不累计S");
        auto canceled = controller.cancel(1, base + 7 * ms);
        expect(controller.request_manual_release(1, 8, base + 8 * ms).phase == canceled.phase,
               "取消后旧人工id不得重入");
        AutoStopController no_history;
        expect(no_history.request_manual_release(1, 1, base).request_id == 0,
               "无真实连续历史不能人工制动");
        AutoStopController old_model(legacy);
        WasdInputHistory old_history;
        old_model.observe(old_history.observe(0, 1, 1, base), base);
        expect(old_model.request_manual_release(1, 1, base + ms).request_id == 0,
               "人工释放不借旧动量模型改写hold时序");
    }
    for (const std::uint8_t direction : {1, 2, 4, 8, 3, 9, 6, 12}) {
        AutoStopController c; WasdInputHistory history;
        c.observe(history.observe(0, 1, 1, base), base);
        c.observe(history.observe(direction, 1, 2, base + ms), base + ms);
        auto d = c.request(1, base + 2 * ms);
        expect(d.phase == AutoStopPhase::WAITING_ACK && d.desired_mask == 0,
            "H40先提交全零，不注入测试正向移动");
        d = c.acknowledge(1, d.command_id, 0, base + 3 * ms);
        const auto inverse = static_cast<std::uint8_t>(((direction & 1) << 2) | ((direction & 4) >> 2) |
            ((direction & 2) << 2) | ((direction & 8) >> 2));
        expect(d.phase == AutoStopPhase::WAITING_ACK && d.desired_mask == inverse &&
            d.axis_deadline_ns == std::array<std::int64_t, 2>{}, "全零ACK后按真实方向反向，反向ACK前无hold期限");
        expect(c.tick(base + 70 * ms).phase == AutoStopPhase::WAITING_ACK,
            "反向ACK迟到不能按提交时刻提前释放");
        d = c.acknowledge(1, d.command_id, inverse, base + 80 * ms);
        expect(c.tick(base + 120 * ms - 1).phase == AutoStopPhase::BRAKING,
            "反向ACK加40ms之前必须保持");
        d = c.tick(base + 120 * ms);
        expect(d.phase == AutoStopPhase::WAITING_ACK && d.desired_mask == 0,
            "反向ACK加40ms请求全零释放");
        expect(c.tick(base + 180 * ms).phase == AutoStopPhase::WAITING_ACK,
            "最终全零未ACK不能开始18ms或提供完成资格");
        d = c.acknowledge(1, d.command_id, 0, base + 200 * ms);
        expect(d.phase == AutoStopPhase::SETTLING && d.completion_ready_ns == base + 218 * ms,
            "最终全零ACK后单独等待18ms");
        auto canceled = c;
        canceled.cancel(1, base + 205 * ms);
        expect(canceled.tick(base + 218 * ms).phase == AutoStopPhase::CANCELLED,
            "18ms等待期间取消不能恢复完成");
        expect(c.tick(base + 218 * ms - 1).phase == AutoStopPhase::SETTLING,
            "18ms等待未到不能完成");
        d = c.tick(base + 218 * ms);
        expect(d.phase == AutoStopPhase::COMPLETE_ESTIMATED && !d.fire_permitted && d.desired_mask == 0,
            "40+18仅完成估计，不伪造真实停稳观察或左键");
    }
    {
        WasdInputHistory history;
        expect(!history.observe(2, 1, 1, base).input_continuous, "首次持键不得冒充连续历史");
        expect(history.observe(0, 1, 2, base + ms).input_continuous, "真实全松建立输入连续性");
        history.observe(2, 1, 3, base + 2 * ms);
        const auto overlap = history.observe(10, 1, 4, base + 3 * ms);
        expect(overlap.input_continuous && !overlap.history_valid && overlap.conflicting,
            "相反键重叠保留真实连续输入，但不能冒充可靠运动估计");
        const auto single = history.observe(8, 1, 5, base + 4 * ms);
        expect(single.input_continuous && !single.history_valid, "重叠后单键可仅屏蔽，不虚构制动模型");
        auto skipped = history;
        expect(!skipped.observe(2, 1, 7, base + 6 * ms).input_continuous,
            "同代际序号前跳必须失去输入连续性，不能降级为仅屏蔽");
        expect(!history.observe(8, 1, 6, base + 5 * ms, true, true).input_continuous, "缺口必须撤销仅屏蔽资格");
        expect(!history.observe(2, 1, 7, base + 6 * ms).input_continuous, "缺口后非零改向不能重新取得资格");
        history.observe(0, 1, 8, base + 7 * ms);
        expect(!history.observe(2, 2, 1, base + 8 * ms).input_continuous, "新代际的持键不能复用旧输入资格");
    }
    auto duration = [&](int hold) {
        AutoStopController controller(legacy);
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
        WasdInputHistory history;
        AutoStopController completed(legacy);
        completed.observe(history.observe(0, 1, 1, base), base);
        const auto held = history.observe(1, 1, 2, base + ms);
        completed.observe(held, base + ms);
        auto pending = completed.request(1, base + 151 * ms);
        const auto waiting = completed;
        auto braking = completed.acknowledge(1, pending.command_id, 4, base + 152 * ms);
        const auto active = completed;
        const auto stopped_at = braking.axis_deadline_ns[0];
        pending = completed.tick(stopped_at);
        const auto unconfirmed = completed;
        completed.acknowledge(1, pending.command_id, 0, stopped_at);
        const auto returned_at = stopped_at + 300 * ms;
        for (auto invalid_phase : {AutoStopController{legacy}, waiting, active, unconfirmed})
            expect(!invalid_phase.resume_after_masked_hold(held, returned_at),
                   "只有零软件ACK后的完成阶段允许恢复");
        for (int kind = 0; kind < 12; ++kind) {
            auto candidate = completed;
            auto invalid = held;
            auto when = returned_at;
            switch (kind) {
                case 0: when = 0; break;
                case 1: when = stopped_at - 1; break;
                case 2: invalid.epoch = 2; break;
                case 3: invalid.sequence = 1; break;
                case 4: invalid.history_valid = false; break;
                case 5: invalid.conflicting = true; break;
                case 6: invalid.held_mask = 5; invalid.held_since_ns[2] = base + ms; break;
                case 7: invalid.held_since_ns[0] = 0; break;
                case 8: invalid.received_at_ns = when + 1; break;
                case 9: invalid.received_at_ns = base; break;
                case 10: invalid.horizontal = 1; break;
                case 11: invalid.held_since_ns[1] = base; break;
            }
            expect(!candidate.resume_after_masked_hold(invalid, when),
                   "恢复必须拒绝非法时间、代际、序号、冲突与不完整真实字段");
            expect(candidate.decision().phase == AutoStopPhase::COMPLETE_ESTIMATED,
                   "拒绝恢复不能改变原完成状态");
        }
        auto candidate = completed;
        expect(candidate.resume_after_masked_hold(held, returned_at),
               "真实W未释放可从受控归还恢复模型，不伪造零边沿");
        expect(candidate.decision().phase == AutoStopPhase::IDLE && !candidate.decision().fire_permitted,
               "恢复仅回到空闲估计模型，不授予开火");
        expect(!candidate.resume_after_masked_hold(held, returned_at + ms),
               "同一次受控完成不可重复恢复");
        candidate.observe(held, returned_at + ms);
        pending = candidate.request(2, returned_at + 150 * ms);
        expect(pending.phase == AutoStopPhase::WAITING_ACK && pending.desired_mask == 4,
               "连续持W松开再按允许键，应可产生第二次反向计划");
        braking = candidate.acknowledge(2, pending.command_id, 4, returned_at + 151 * ms);
        expect(braking.axis_deadline_ns[0] - (returned_at + 151 * ms) < 90 * ms,
               "第二次计划不能累计屏蔽期间的物理持键时长");
        auto residual = unconfirmed;
        const auto late_ack = stopped_at + 20 * ms;
        const auto zero = residual.decision();
        residual.acknowledge(1, zero.command_id, 0, late_ack);
        expect(residual.resume_after_masked_hold(held, late_ack + ms),
               "延迟零ACK后的模型也可在完整归还后承接");
        expect(residual.request(2, late_ack + ms).desired_mask == 1,
               "受控归还保留反向过冲的剩余估计，不能宣称或强设物理速度为零");
        auto canceled = completed;
        canceled.cancel(1, returned_at);
        expect(!canceled.resume_after_masked_hold(held, returned_at),
               "取消已撤销受控保持资格，不得再次复活");
    }
    {
        AutoStopController controller(legacy);
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
        AutoStopController controller(legacy);
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
        AutoStopController controller(legacy);
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
        AutoStopController controller(legacy);
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
        AutoStopController controller(legacy);
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
