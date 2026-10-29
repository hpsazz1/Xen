#include "auto_stop/hud_stop.h"
#include "auto_stop/stop_driver_internal.h"
#include <cmath>
#include <iostream>

namespace {
int failures = 0;
constexpr std::int64_t ms = 1000000, base = 1000000000;
void expect(bool ok, const char* message) { if (!ok) { ++failures; std::cerr << message << '\n'; } }
struct Fixture {
    HudStopController core;
    WasdInputHistory history;
    WasdMotionIntent input;
    std::uint64_t sequence = 0;
    void observe(std::uint8_t mask, std::int64_t time) {
        input = history.observe(mask, 1, ++sequence, time);
        core.observe(input, time);
    }
    void start(std::uint8_t mask = 8) { observe(0, base); observe(mask, base + ms); }
    AutoStopDecision ack(std::int64_t time) {
        auto d = core.decision(); return core.acknowledge(d.request_id, d.command_id, d.desired_mask, time);
    }
    AutoStopDecision brake(std::int64_t time) {
        core.request(1, time); ack(time); return ack(time);
    }
};
void trigger_idle_model_contracts() {
    {
        Fixture f;
        expect(!f.core.idle_for_trigger(base), "未同步不能免制动");
        f.observe(0, base);
        expect(f.core.idle_for_trigger(base), "最初真实全松及模型真零立即免制动，不添加固定等待");
        expect(!f.core.idle_for_trigger(0) && !f.core.idle_for_trigger(base - 1), "非法或回退时刻不能免制动");
        expect(f.core.idle_for_trigger(base), "只读无效查询不能污染健康模型");
    }
    {
        Fixture f; f.start(1);
        const auto released = base + 251 * ms;
        f.observe(0, released);
        const auto before = f.core.telemetry().velocity;
        const auto command = f.core.decision().command_id;
        expect(!f.core.idle_for_trigger(released + 104 * ms), "长按W全松104ms仍有自然滑行量，不得以零键免制动");
        expect(!f.core.idle_for_trigger(released + 399 * ms), "自然减速尚未到两轴零时持续拒绝");
        expect(f.core.idle_for_trigger(released + 400 * ms), "既有归一化自然减速归零后无需补反向即可免制动");
        expect(f.core.telemetry().velocity == before && f.core.decision().command_id == command,
            "查询不得修改积分时间、速度或生成新反向命令");
        f.observe(0, released + 10 * ms);
        expect(f.core.decision().phase == AutoStopPhase::IDLE &&
            !f.core.idle_for_trigger(released + 104 * ms), "未来查询不得跨过队列中稍早真实输入并污染积分顺序");
    }
    {
        Fixture f; f.start(8);
        const auto released = base + 11 * ms;
        f.observe(0, released);
        expect(!f.core.idle_for_trigger(released + 21 * ms) &&
            f.core.idle_for_trigger(released + 23 * ms), "短按按真实dt和当前模型量更早归零，不能固定等400ms");
    }
    {
        Fixture f; f.start(1);f.observe(9, base + 191 * ms);
        const auto released = base + 201 * ms;f.observe(0, released);
        expect(!f.core.idle_for_trigger(released + 23 * ms) &&
            f.core.idle_for_trigger(released + 400 * ms), "对角输入必须两轴自然归零，短轴停止不替代长轴");
    }
    {
        Fixture f; f.start();auto d=f.brake(base + 51 * ms);
        expect(!f.core.idle_for_trigger(base + 1000 * ms), "活动或仍接管输出时不能借未来模型零量免制动");
        f.core.tick(d.axis_deadline_ns[1]);d=f.ack(d.axis_deadline_ns[1]);
        f.core.tick(d.completion_ready_ns);
        const auto zero=f.history.observe(0,1,++f.sequence,d.completion_ready_ns);
        expect(f.core.resume_after_masked_hold(zero,d.completion_ready_ns) &&
            f.core.idle_for_trigger(d.completion_ready_ns), "已完成制动并清理的真零不额外叠加固定等待");
    }
    {
        Fixture f;f.start();auto invalid=f.input;invalid.sequence+=2;
        f.core.observe(invalid,base + 10 * ms);
        expect(!f.core.idle_for_trigger(base + 1000 * ms), "输入缺口失信后自然时间流逝不能恢复资格");
        AutoStopConfig config;config.shot_after_release_ms=-1;HudStopController bad(config);
        WasdInputHistory history;bad.observe(history.observe(0,1,1,base),base);
        expect(!bad.idle_for_trigger(base), "非法时序配置不能由初始零模型获取资格");
    }
    {
        AutoStopConfig config;config.experimental_hud_model=false;detail::StopDriver h40(config);
        expect(h40.idle_for_trigger(base), "H40兼容路径保持原有免制动策略");
    }
}
}
int main() {
    trigger_idle_model_contracts();
    for (const int hold : {10, 50, 250}) {
        Fixture f; f.start();
        auto d = f.brake(base + (1 + hold) * ms);
        const double speed = std::min(1.0, hold * .0055);
        const auto duration = std::int64_t(std::ceil(speed / 14 * 1e9));
        expect(d.phase == AutoStopPhase::BRAKING && d.desired_mask == 2, "反向来自模型，零ACK之后才制动");
        expect(std::abs(d.axis_deadline_ns[1] - (base + (1 + hold) * ms + duration)) <= 1, "短长按零交叉不同");
        expect(std::abs(f.core.telemetry().planned_ms[1] - speed / 14 * 1000) < 1e-8, "遥测计划不等于固定H40");
        auto wrong = f.core.acknowledge(999, d.command_id, 0, base + 500 * ms);
        expect(wrong.phase == d.phase, "错误ACK不推进模型");
        f.core.tick(d.axis_deadline_ns[1]);
        auto up = f.ack(d.axis_deadline_ns[1]);
        expect(up.phase == AutoStopPhase::SETTLING, "UP ACK后独立等待18ms");
        expect(f.core.tick(up.completion_ready_ns - 1).phase == AutoStopPhase::SETTLING, "18ms尚未到不能完成");
        const auto done = f.core.tick(up.completion_ready_ns);
        expect(done.phase == AutoStopPhase::COMPLETE_ESTIMATED && !done.fire_permitted, "完成仅估计");
        expect(f.core.resume_after_masked_hold(f.input, up.completion_ready_ns), "清理归还可接当前持续物理键");
        const auto again = f.core.request(2, up.completion_ready_ns + 10 * ms);
        expect(again.phase == AutoStopPhase::WAITING_ACK && again.desired_mask == 0, "归还后新积分新计划");
    }
    {
        Fixture f; f.start(); auto d = f.brake(base + 251 * ms);
        f.core.tick(d.axis_deadline_ns[1]);
        d = f.ack(d.axis_deadline_ns[1] + 20 * ms);
        const double residual = f.core.telemetry().velocity[1];
        expect(residual < -.05, "迟到20ms UP ACK保留反向残余");
        f.core.tick(d.completion_ready_ns);
        const double decayed = f.core.telemetry().velocity[1];
        expect(std::abs(decayed - (residual + .045)) < 1e-8, "18ms等待按2.5自然衰减，不强制归零");
        auto current = f.input;
        current.sequence += 5; current.received_at_ns = d.completion_ready_ns;
        expect(f.core.restart_after_cleanup(current, d.completion_ready_ns) &&
            f.core.telemetry().velocity[1] == decayed && !f.core.telemetry().seeded,
            "健康清理跨报告保留剩余量");
    }
    {
        Fixture f; f.start(1); f.observe(9, base + 101 * ms);
        auto d = f.brake(base + 121 * ms);
        expect(d.desired_mask == 6 && d.axis_deadline_ns[0] > d.axis_deadline_ns[1], "逐轴不同deadline");
        const auto longer = d.axis_deadline_ns[0];
        const auto shorter = d.axis_deadline_ns[1];
        auto part = f.core.tick(shorter);
        expect(part.desired_mask == 4, "仅到期横轴释放");
        part = f.ack(shorter + ms);
        expect(part.axis_deadline_ns[0] == longer, "部分释放ACK不能重置另一轴deadline");
        auto zero = f.core.tick(longer);
        expect(zero.desired_mask == 0 && zero.phase == AutoStopPhase::WAITING_ACK, "另一轴独立到期");
        f.ack(longer + 5 * ms);
        expect(std::abs(f.core.telemetry().velocity[0]) > 0, "迟到UP ACK积分实际旧mask，不伪造零速度");
    }
    {
        Fixture f; f.start(); auto d = f.core.request(1, base + 51 * ms);
        const auto before = f.core.telemetry().velocity;
        f.core.acknowledge(1, d.command_id + 1, 0, base + 60 * ms);
        expect(f.core.telemetry().velocity == before, "错误command不积分");
        f.ack(base + 52 * ms);
        auto reverse = f.core.decision();
        f.core.acknowledge(1, d.command_id, 0, base + 80 * ms);
        expect(f.core.decision().command_id == reverse.command_id, "重复zero ACK不得误认后续ACK");
        f.ack(base + 53 * ms);
        f.observe(2, base + 54 * ms);
        expect(f.core.decision().phase == AutoStopPhase::CANCELLED, "AD换向取消现计划");
        expect(f.core.restart_after_cleanup(f.input, base + 55 * ms), "清理ACK允许连续方向重建");
        expect(f.core.request(2, base + 56 * ms).phase == AutoStopPhase::WAITING_ACK, "清理后不继承旧完成");
    }
    {
        Fixture f; f.start(); f.observe(10, base + 31 * ms);
        expect(f.core.decision().phase == AutoStopPhase::INVALID, "冲突使模型失效");
        f.observe(8, base + 32 * ms);
        expect(f.core.restart_after_cleanup(f.input, base + 33 * ms), "冲突解除连续方向显式清理重建");
        expect(f.core.telemetry().seeded && f.core.telemetry().velocity[1] == 1, "不可信历史保守上界seed可追溯");
        expect(f.core.request(1, base + 33 * ms).phase == AutoStopPhase::WAITING_ACK, "seed仍完整零ACK流程");
    }
    {
        Fixture f; f.observe(8, base);
        expect(f.core.request(1, base + 100 * ms).phase == AutoStopPhase::INVALID, "首次持键不可伪造零初值");
    }
    {
        Fixture f; f.start(); f.observe(0, base + 51 * ms);
        auto d = f.core.request_manual_release(1, 8, base + 51 * ms);
        expect(d.phase == AutoStopPhase::WAITING_ACK && d.desired_mask == 0, "手动松键仍按剩余模型制动");
        f.ack(base + 51 * ms); d = f.ack(base + 51 * ms);
        expect(d.phase == AutoStopPhase::BRAKING && d.desired_mask == 2, "手动松键非固定H40");
    }
    for (int bad = 0; bad < 5; ++bad) {
        Fixture f; f.start(); auto intent = f.input;
        if (bad == 0) intent.sequence += 2;
        if (bad == 1) intent.received_at_ns = base + 50 * ms;
        if (bad == 2) { intent.held_mask = 2; intent.horizontal = -1; }
        if (bad == 3) ++intent.epoch;
        if (bad == 4) intent.input_continuous = false;
        f.core.observe(intent, base + 10 * ms);
        expect(f.core.request(1, base + 11 * ms).phase == AutoStopPhase::INVALID, "缺口未来同序换键换代失信拒绝");
    }
    {
        Fixture f; f.start(); auto d = f.brake(base + 51 * ms);
        f.core.tick(base + 52 * ms);
        f.input = f.history.observe(8, 1, ++f.sequence, base + 51 * ms);
        f.core.observe(f.input, base + 52 * ms);
        expect(f.core.decision().phase == AutoStopPhase::BRAKING, "同键迟到仅更新连续性不回拨模型");
        f.core.cancel(1, base + 53 * ms);
        expect(f.core.restart_after_cleanup(f.input, base + 54 * ms), "取消后清理持续键可恢复");
    }
    {
        Fixture f; f.start(); auto d = f.brake(base + ms + 100000);
        expect(d.axis_deadline_ns[1] == base + ms + 200000, "tiny残差使用上游5.5分支零交叉");
        AutoStopController h40;
        WasdInputHistory history;
        h40.observe(history.observe(0, 1, 1, base), base);
        h40.observe(history.observe(8, 1, 2, base + ms), base + ms);
        auto fixed = h40.request(1, base + ms + 100000);
        fixed = h40.acknowledge(1, fixed.command_id, 0, base + ms + 100000);
        fixed = h40.acknowledge(1, fixed.command_id, fixed.desired_mask, base + ms + 100000);
        expect(fixed.axis_deadline_ns[1] - (base + ms + 100000) == 40 * ms &&
            fixed.axis_deadline_ns[1] != d.axis_deadline_ns[1], "参考控制与生产H40为两个不同模型");
    }
    {
        Fixture f; f.start(); auto d = f.brake(base + 51 * ms);
        f.core.tick(d.axis_deadline_ns[1]);
        d = f.ack(d.axis_deadline_ns[1]);
        f.core.tick(d.completion_ready_ns);
        auto current = f.input;
        current.sequence += 5; current.received_at_ns = d.completion_ready_ns;
        expect(f.core.resume_after_masked_hold(current, d.completion_ready_ns),
            "worker已逐事件验证后的cleanup快照允许跨序号");
        expect(std::abs(f.core.telemetry().velocity[1]) < 1e-6 && !f.core.telemetry().seeded,
            "正常屏蔽持键期间只按已ACK零输出积分");
        current.sequence -= 1;
        expect(!f.core.restart_after_cleanup(current, d.completion_ready_ns + ms), "cleanup拒绝旧序号");
    }
    std::cout << "hud_stop_tests failures=" << failures << '\n';
    return failures ? 1 : 0;
}
