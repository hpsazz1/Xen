#include "trigger/trigger.h"

#include <iostream>
#include <limits>

namespace {
int failures = 0;
void expect(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << message << '\n'; }
}
TriggerTime at(int ms) { return TriggerTime(std::chrono::milliseconds(1000 + ms)); }
TriggerConfig config() {
    TriggerConfig c;
    c.enabled = true; c.hold_virtual_key = 5;
    return c;
}
TriggerPermit permit(bool held = true) {
    TriggerPermit p;
    p.enabled = p.healthy = p.focused = p.armed = true; p.held = held;
    return p;
}
TriggerPermit context_permit(std::uint64_t generation, bool valid = true, bool held = true) {
    auto p = permit(held);
    p.context = {generation, true, valid};
    return p;
}
TriggerObservation frame(int ms, std::uint64_t sequence = 1) {
    TriggerObservation o;
    o.valid = o.timing_valid = true;
    o.epoch = 1; o.sequence = sequence; o.observed_at = at(ms);
    o.roi_width = o.roi_height = 300; o.center_x = o.center_y = 150;
    o.detections.push_back({100, 100, 200, 200, 0.9f, 0});
    return o;
}
void arm(TriggerController& controller, TriggerConfig c = config()) {
    expect(controller.configure(c), "合法配置应接纳");
    controller.tick(permit(false), at(0));
}
TriggerDecision ack(TriggerController& c, TriggerDecision d, int ms, TriggerReceiptStatus status = TriggerReceiptStatus::ACKNOWLEDGED) {
    return c.acknowledge({d.command_id, d.button_action, status, at(ms)}, at(ms));
}
void geometry_and_timing() {
    TriggerController c; arm(c);
    auto o = frame(1);
    expect(c.observe(o, permit(), at(1)).snapshot.phase == TriggerPhase::QUALIFYING, "当前身体内域可独立开始驻留");
    expect(c.tick(permit(), at(20)).button_action == TriggerButtonAction::NONE, "不能提前消耗延迟");
    auto down = c.tick(permit(), at(21));
    expect(down.button_action == TriggerButtonAction::DOWN, "身体合格不等头部或Aim锁定");
    ack(c, down, 23);
    auto up = c.tick(permit(), at(43));
    expect(up.button_action == TriggerButtonAction::UP, "无新图也按回执起算点射释放");
    ack(c, up, 44);
    auto newer = frame(45, 2);
    c.observe(newer, permit(), at(45));
    expect(c.tick(permit(), at(65)).snapshot.reason == TriggerReason::COOLDOWN, "重进域不能消除冷却");
    newer = frame(130, 3); c.observe(newer, permit(), at(130));
    expect(c.tick(permit(), at(150)).button_action == TriggerButtonAction::DOWN, "新帧和冷却共同满足后点射");

    TriggerConfig fast = config(); fast.fire_delay_ms = 0;
    TriggerController boundary; arm(boundary, fast);
    o = frame(1); o.center_x = 175;
    expect(boundary.observe(o, permit(), at(1)).button_action == TriggerButtonAction::DOWN, "椭圆数学边界包含");
    TriggerController outside; arm(outside, fast); o.center_x = 175.01f;
    expect(outside.observe(o, permit(), at(1)).button_action == TriggerButtonAction::NONE, "不添加框外epsilon");
    TriggerController clipped; arm(clipped, fast); o = frame(1); o.detections[0].x1 = 0;
    expect(clipped.observe(o, permit(), at(1)).button_action == TriggerButtonAction::NONE, "ROI截边拒绝开火");
    TriggerController unknown; arm(unknown, fast); o = frame(1); o.detections[0].class_id = 42;
    expect(unknown.observe(o, permit(), at(1)).snapshot.reason == TriggerReason::NO_CANDIDATE, "未知类别不能自动泛化");
    TriggerController stale; arm(stale, fast); o = frame(1); o.uncertainty = std::chrono::milliseconds(40);
    expect(stale.observe(o, permit(), at(11)).snapshot.reason == TriggerReason::STALE, "年龄上界包括时钟不确定性");
    TriggerController nan; arm(nan, fast); o = frame(1); o.detections[0].x1 = std::numeric_limits<float>::quiet_NaN();
    expect(nan.observe(o, permit(), at(1)).button_action == TriggerButtonAction::NONE, "NaN框必须拒绝");
}
void association() {
    TriggerController c; arm(c);
    auto o = frame(1);
    o.detections = {{100, 40, 200, 220, 0.9f, 0}, {130, 50, 170, 90, 0.9f, 1}};
    // 准星保持固定：同一人物头域移动到准星，人体IoU仍唯一。
    c.observe(o, permit(), at(1));
    const auto id = c.snapshot().candidate_id;
    o.sequence = 2; o.observed_at = at(11);
    o.detections[1] = {130, 130, 170, 170, 0.9f, 1};
    c.observe(o, permit(), at(11));
    expect(c.snapshot().candidate_id == id, "唯一头身配对共享人物身份");
    expect(c.tick(permit(), at(21)).button_action == TriggerButtonAction::DOWN, "连续头身交接保留驻留");

    TriggerController ambiguous; arm(ambiguous);
    ambiguous.observe(frame(1), permit(), at(1));
    auto both = frame(11, 2); both.detections.push_back({101, 100, 201, 200, 0.8f, 0});
    ambiguous.observe(both, permit(), at(11));
    expect(ambiguous.tick(permit(), at(21)).button_action == TriggerButtonAction::NONE, "多人关联歧义须重计驻留");
    expect(ambiguous.tick(permit(), at(31)).button_action == TriggerButtonAction::DOWN, "歧义不永久排除当前几何合格域");
}
void permissions_and_receipts() {
    auto fast = config(); fast.fire_delay_ms = 0;
    TriggerController startup; expect(startup.configure(fast), "配置");
    expect(startup.observe(frame(1), permit(), at(1)).snapshot.reason == TriggerReason::WAIT_RELEASE, "启动已按住不能自动激活");
    TriggerController c; arm(c, fast);
    auto down = c.observe(frame(1), permit(), at(1));
    auto unknown = ack(c, down, 2, TriggerReceiptStatus::UNKNOWN);
    expect(unknown.button_action == TriggerButtonAction::UP && unknown.snapshot.faulted, "未知down不重发，执行释放并锁故障");
    expect(c.tick(permit(), at(3)).button_action == TriggerButtonAction::NONE, "故障不重复提交释放或down");
    ack(c, unknown, 4);
    expect(c.snapshot().faulted && !c.snapshot().button_may_be_down, "释放ACK清债但不自动解锁故障");
    expect(!c.configure(fast), "故障不能通过热更配置抹除");

    TriggerController lost; arm(lost, fast);
    down = lost.observe(frame(1), permit(), at(1));
    auto up = lost.cancel(TriggerReason::CANCELED, at(2));
    expect(up.button_action == TriggerButtonAction::UP, "在途down取消仍需释放");
    ack(lost, down, 3);
    expect(lost.snapshot().phase == TriggerPhase::UP_PENDING, "迟到down ACK不能使取消会话HELD");
    ack(lost, up, 4);
    lost.tick(permit(false), at(5));
    expect(lost.observe(frame(6, 2), permit(), at(6)).snapshot.reason == TriggerReason::COOLDOWN, "取消在途down保留保守冷却");

    TriggerController focus; arm(focus, fast);
    down = focus.observe(frame(1), permit(), at(1)); ack(focus, down, 2);
    auto p = permit(); p.focused = false;
    up = focus.tick(p, at(3));
    expect(up.button_action == TriggerButtonAction::UP, "失焦不等下一帧释放"); ack(focus, up, 4);
    expect(focus.observe(frame(5, 2), permit(), at(5)).snapshot.reason == TriggerReason::WAIT_RELEASE, "恢复前台仍需新人手边沿");

    TriggerController repeated; arm(repeated, fast);
    auto o = frame(1); down = repeated.observe(o, permit(), at(1)); ack(repeated, down, 2);
    expect(repeated.observe(o, permit(), at(3)).button_action == TriggerButtonAction::UP, "重复发布不能续命并须撤销持有");
}
void stop_and_automatic() {
    auto cfg = config(); cfg.require_stop = true; cfg.fire_delay_ms = 0; cfg.fire_mode = TriggerFireMode::AUTOMATIC;
    TriggerController c; arm(c, cfg);
    auto p = permit(); p.next_stop_request_id = 41;
    auto request = c.observe(frame(1), p, at(1));
    expect(request.stop_action == TriggerStopAction::REQUEST && request.stop_request_id == 41, "消费Runtime请求id一次");
    p.stop_request_id = 41; p.stop_observation_epoch = 1;
    p.stop_release_deadline = at(400); p.stop_expires_at = at(25);
    expect(c.tick(p, at(2)).button_action == TriggerButtonAction::NONE, "ESTIMATED false不能放行");
    p.stop_observed_qualified = true;
    auto down = c.tick(p, at(3));
    expect(down.button_action == TriggerButtonAction::DOWN, "同请求有效观测停稳可授予资格"); ack(c, down, 4);
    auto up = c.tick(p, at(25));
    expect(up.button_action == TriggerButtonAction::UP && up.stop_action == TriggerStopAction::CANCEL && up.stop_request_id == 41,
        "HELD中资格过期须释放再取消原急停");
    ack(c, up, 26);
    auto same_id = c.observe(frame(27, 2), p, at(27));
    expect(same_id.stop_action == TriggerStopAction::NONE, "取消后不能复用id续租");

    cfg.require_stop = false;
    TriggerController age; arm(age, cfg);
    down = age.observe(frame(1), permit(), at(1)); ack(age, down, 2);
    expect(age.tick(permit(), at(51)).button_action == TriggerButtonAction::UP, "Automatic无图到原有效期释放，worker tick不能延寿");
}
void weapon_context_cancels_qualification() {
    TriggerController c;
    expect(c.configure(config()), "上下文驻留配置");
    c.tick(context_permit(1, true, false), at(0));
    expect(c.observe(frame(1), context_permit(1), at(1)).snapshot.phase == TriggerPhase::QUALIFYING,
        "有效上下文及松键后允许新驻留");
    const auto changed = c.tick(context_permit(2), at(10));
    expect(changed.snapshot.region == TriggerRegion::NONE && changed.snapshot.reason == TriggerReason::CONTEXT_CHANGED,
        "QUALIFYING 换武器立即清除旧驻留");
    expect(c.observe(frame(11, 2), context_permit(2), at(11)).snapshot.reason == TriggerReason::WAIT_RELEASE,
        "换武器后保持许可键和新帧不能复活旧会话");
    c.tick(context_permit(2, true, false), at(12));
    expect(c.observe(frame(13, 3), context_permit(2), at(13)).snapshot.phase == TriggerPhase::QUALIFYING,
        "新上下文完整松键再按下可重新驻留");
    expect(c.tick(context_permit(2), at(32)).button_action == TriggerButtonAction::NONE &&
        c.tick(context_permit(2), at(33)).button_action == TriggerButtonAction::DOWN,
        "新会话重新等待完整全局延迟，不能沿用旧驻留");

    TriggerController heartbeat;
    expect(heartbeat.configure(config()), "心跳兼容配置");
    heartbeat.tick(context_permit(4, true, false), at(0));
    heartbeat.observe(frame(1), context_permit(4), at(1));
    const auto candidate = heartbeat.snapshot().candidate_id;
    heartbeat.observe(frame(10, 2), context_permit(4), at(10));
    expect(heartbeat.snapshot().candidate_id == candidate &&
        heartbeat.tick(context_permit(4), at(21)).button_action == TriggerButtonAction::DOWN,
        "同武器持续有效心跳保留驻留并正常开火");

    TriggerController manual; arm(manual);
    manual.observe(frame(1), permit(), at(1));
    auto disabled = permit(); disabled.context = {99, false, false};
    expect(manual.tick(disabled, at(21)).button_action == TriggerButtonAction::DOWN,
        "未要求 GSI 上下文时保留全局手动模式");
}
void weapon_context_invalidity_and_held_cleanup() {
    auto cfg = config(); cfg.fire_delay_ms = 0; cfg.fire_mode = TriggerFireMode::AUTOMATIC;
    TriggerController c;
    expect(c.configure(cfg), "上下文失效配置");
    c.tick(context_permit(1, true, false), at(0));
    auto down = c.observe(frame(1), context_permit(1), at(1));
    expect(down.button_action == TriggerButtonAction::DOWN, "失效回归先确认有效上下文确实能 DOWN");
    ack(c, down, 2);
    auto up = c.tick(context_permit(2, false), at(3));
    expect(up.button_action == TriggerButtonAction::UP && up.snapshot.reason == TriggerReason::CONTEXT_UNAVAILABLE,
        "HELD 上下文失效不等新图立即 UP");
    ack(c, up, 4);
    expect(c.observe(frame(5, 2), context_permit(3), at(5)).button_action == TriggerButtonAction::NONE &&
        c.tick(context_permit(3), at(6)).snapshot.reason == TriggerReason::WAIT_RELEASE,
        "来源恢复但仍持键不能自动重开");
    c.tick(context_permit(3, true, false), at(7));
    expect(c.observe(frame(130, 3), context_permit(3), at(130)).button_action == TriggerButtonAction::DOWN,
        "来源恢复并完成松键、新按下及冷却后可正常开火");

    cfg.require_stop = true;
    TriggerController strict;
    expect(strict.configure(cfg), "上下文取消急停配置");
    strict.tick(context_permit(7, true, false), at(0));
    auto p = context_permit(7); p.next_stop_request_id = 41;
    expect(strict.observe(frame(1), p, at(1)).stop_action == TriggerStopAction::REQUEST,
        "上下文有效时仍沿用原急停请求契约");
    p.stop_request_id = 41; p.stop_observation_epoch = 1; p.stop_observed_qualified = true;
    p.stop_expires_at = p.stop_release_deadline = at(400);
    down = strict.tick(p, at(2)); ack(strict, down, 3);
    p.context.generation = 8;
    up = strict.tick(p, at(4));
    expect(up.button_action == TriggerButtonAction::UP && up.stop_action == TriggerStopAction::CANCEL && up.stop_request_id == 41,
        "HELD 切枪同时释放旧按钮和取消原急停 id");
    ack(strict, up, 5, TriggerReceiptStatus::UNKNOWN);
    expect(strict.snapshot().faulted && strict.snapshot().button_may_be_down,
        "上下文变化引发的未知 UP 仍保留故障和清理责任");
    ack(strict, up, 6);
    expect(strict.snapshot().faulted && !strict.snapshot().button_may_be_down,
        "随后 UP ACK 只消债，不用新上下文解除故障");
}
}

void fire_disabled_preserves_qualification_and_stop() {
    auto cfg = config(); cfg.fire_enabled = false; cfg.fire_delay_ms = 0;
    TriggerController c; arm(c, cfg);
    auto d = c.observe(frame(1), permit(), at(1));
    expect(d.button_action == TriggerButtonAction::NONE && d.snapshot.reason == TriggerReason::FIRE_DISABLED &&
        d.snapshot.region == TriggerRegion::BODY && !d.snapshot.button_may_be_down && d.command_id == 0,
        "关闭开枪仍保留候选资格且不生成DOWN或按钮债务");
    expect(c.tick(permit(), at(2)).button_action == TriggerButtonAction::NONE, "调试等待不延迟补发DOWN");
    cfg.require_stop = true;
    TriggerController stopped; arm(stopped, cfg);
    auto p = permit(); p.next_stop_request_id = 41;
    d = stopped.observe(frame(1), p, at(1));
    expect(d.stop_action == TriggerStopAction::REQUEST && d.stop_request_id == 41 &&
        d.button_action == TriggerButtonAction::NONE, "关闭开枪仍请求急停");
    p.stop_request_id = 41; p.stop_observation_epoch = 1; p.stop_observed_qualified = true;
    p.stop_expires_at = p.stop_release_deadline = at(40);
    d = stopped.tick(p, at(2));
    expect(d.button_action == TriggerButtonAction::NONE && d.snapshot.reason == TriggerReason::FIRE_DISABLED,
        "即使停稳资格齐全关闭开枪仍无DOWN");
    d = stopped.tick(permit(false), at(3));
    expect(d.stop_action == TriggerStopAction::CANCEL && d.stop_request_id == 41,
        "调试模式松键仍取消急停");
    TriggerController active; auto enabled = cfg; enabled.require_stop = false; enabled.fire_enabled = true;
    arm(active, enabled);
    auto down = active.observe(frame(1), permit(), at(1)); ack(active, down, 2);
    expect(!active.configure(cfg), "切为不开枪不能绕过既有按钮债务");
    auto up = active.cancel(TriggerReason::CANCELED, at(3));
    expect(up.button_action == TriggerButtonAction::UP, "关闭前仍生成真实UP清理决策");
    ack(active, up, 4);
    expect(active.configure(cfg), "UP明确确认后才能应用不开枪配置");
}

void weapon_timing_submission_and_cleanup() {
    auto cfg = config(); cfg.fire_delay_ms = 0; cfg.max_observation_age_ms = 5000;
    auto p = context_permit(1); p.context.timing_required = p.context.timing_valid = true;
    p.context.shot_hold_ms = 60; p.context.fire_interval_ms = 600;
    const auto arm_weapon = [&](TriggerController& c) {
        expect(c.configure(cfg), "武器点射配置应通过");
        auto released = p; released.held = false; c.tick(released, at(0));
    };
    TriggerController c; arm_weapon(c);
    const auto down = c.observe(frame(1), p, at(1));
    expect(down.button_action == TriggerButtonAction::DOWN, "首发不等待虚构的武器间隔");
    c.acknowledge({down.command_id, down.button_action, TriggerReceiptStatus::ACKNOWLEDGED, at(11), at(3)}, at(11));
    expect(c.tick(p, at(70)).button_action == TriggerButtonAction::NONE, "按住60ms由ACK而非提交起算");
    auto up = c.tick(p, at(71));
    expect(up.button_action == TriggerButtonAction::UP, "ACK加60ms才请求松开");
    ack(c, up, 72);
    expect(c.observe(frame(602, 2), p, at(602)).snapshot.reason == TriggerReason::COOLDOWN,
        "提交3ms加600ms前不能下发第二次按下");
    auto second = c.tick(p, at(603));
    expect(second.button_action == TriggerButtonAction::DOWN, "下一DOWN取提交加间隔，不把ACK延迟叠加");

    TriggerController waiting; arm_weapon(waiting);
    auto d = waiting.observe(frame(1), p, at(1));
    waiting.acknowledge({d.command_id, d.button_action, TriggerReceiptStatus::ACKNOWLEDGED, at(11), at(3)}, at(11));
    up = waiting.tick(p, at(71));
    expect(waiting.observe(frame(650, 2), p, at(650)).button_action == TriggerButtonAction::NONE,
        "间隔虽到UP未确认仍不能新DOWN");
    ack(waiting, up, 651);
    d = waiting.tick(p, at(652));
    expect(d.button_action == TriggerButtonAction::DOWN && waiting.tick(p, at(652)).button_action == TriggerButtonAction::NONE,
        "延误后仅发一次，不能补发积压枪");

    TriggerController legacy; arm_weapon(legacy);
    d = legacy.observe(frame(1), p, at(1)); ack(legacy, d, 11);
    up = legacy.tick(p, at(71)); ack(legacy, up, 72);
    expect(legacy.observe(frame(603, 2), p, at(603)).button_action == TriggerButtonAction::NONE &&
        legacy.tick(p, at(611)).button_action == TriggerButtonAction::DOWN,
        "旧回执缺少提交时间时使用ACK保守兼容");

    TriggerController changed; arm_weapon(changed);
    d = changed.observe(frame(1), p, at(1));
    auto next = p; next.context.generation = 2; next.context.fire_interval_ms = 100;
    up = changed.tick(next, at(2));
    expect(up.button_action == TriggerButtonAction::UP, "在途换时序先清理旧DOWN");
    ack(changed, up, 3);
    next.held = false; changed.tick(next, at(4)); next.held = true;
    expect(changed.observe(frame(103, 2), next, at(103)).snapshot.reason == TriggerReason::COOLDOWN,
        "旧DOWN未确认时使用旧600ms快照，不被新100ms缩短");
    expect(changed.tick(next, at(603)).button_action == TriggerButtonAction::DOWN, "旧冷却完成后才采用新资料");

    TriggerController invalid; arm_weapon(invalid);
    auto invalid_p = p; invalid_p.context.shot_hold_ms = 601;
    expect(invalid.observe(frame(1), invalid_p, at(1)).snapshot.reason == TriggerReason::CONTEXT_UNAVAILABLE,
        "时序不合法拒绝开火，不能回退全局较短时序");
    TriggerController receipt_time; arm_weapon(receipt_time);
    d = receipt_time.observe(frame(1), p, at(1));
    auto rejected = receipt_time.acknowledge({d.command_id, d.button_action, TriggerReceiptStatus::ACKNOWLEDGED, at(3), at(4)}, at(4));
    expect(rejected.button_action == TriggerButtonAction::UP && rejected.snapshot.faulted,
        "提交晚于ACK的回执锁故障并清理");

    TriggerController protocol; arm_weapon(protocol);
    d = protocol.observe(frame(1), p, at(1));
    protocol.acknowledge({d.command_id,d.button_action,TriggerReceiptStatus::ACKNOWLEDGED,at(11),at(3),at(5)},at(11));
    expect(protocol.tick(p,at(64)).button_action == TriggerButtonAction::NONE &&
        protocol.tick(p,at(65)).button_action == TriggerButtonAction::UP,
        "点射60ms从协议ACK5ms起算，不叠加后端完成到11ms的延迟");
    for (const int invalid_ack : {2,12}) {
        TriggerController invalid_protocol; arm_weapon(invalid_protocol);
        d = invalid_protocol.observe(frame(1),p,at(1));
        rejected = invalid_protocol.acknowledge({d.command_id,d.button_action,TriggerReceiptStatus::ACKNOWLEDGED,
            at(11),at(3),at(invalid_ack)},at(11));
        expect(rejected.button_action == TriggerButtonAction::UP && rejected.snapshot.faulted,
            "协议ACK必须在提交和后端完成之间，否则故障清理");
    }

    cfg.fire_mode = TriggerFireMode::AUTOMATIC;
    TriggerController automatic; arm_weapon(automatic);
    d = automatic.observe(frame(1), invalid_p, at(1)); ack(automatic, d, 2);
    expect(d.button_action == TriggerButtonAction::DOWN && automatic.tick(invalid_p, at(62)).button_action == TriggerButtonAction::NONE &&
        automatic.tick(invalid_p, at(302)).button_action == TriggerButtonAction::UP,
        "连续扫射忽略点射资料且保留max_hold释放期限");
    TriggerController automatic_protocol; arm_weapon(automatic_protocol);
    d = automatic_protocol.observe(frame(1),invalid_p,at(1));
    automatic_protocol.acknowledge({d.command_id,d.button_action,TriggerReceiptStatus::ACKNOWLEDGED,at(11),at(3),at(5)},at(11));
    expect(automatic_protocol.tick(invalid_p,at(305)).button_action == TriggerButtonAction::NONE &&
        automatic_protocol.tick(invalid_p,at(311)).button_action == TriggerButtonAction::UP,
        "连续扫射仍从后端完成11ms起算300ms，不受点射协议ACK相位调整影响");
}

void estimated_stop_is_explicit_and_parallel() {
    auto cfg = config(); cfg.fire_delay_ms = 0; cfg.max_observation_age_ms = 5000;
    cfg.require_stop = cfg.allow_estimated_stop = true;
    TriggerController c; arm(c, cfg);
    auto p = permit(); p.next_stop_request_id = 41;
    auto d = c.observe(frame(1), p, at(1));
    expect(d.stop_action == TriggerStopAction::NONE && d.snapshot.reason == TriggerReason::STOP_UNVERIFIED,
        "估计策略缺少资格等待但不申请观察租约");
    p.stop_estimated_qualified = true; p.estimated_stop_request_id = 70;
    d = c.tick(p, at(2));
    expect(d.button_action == TriggerButtonAction::DOWN && d.snapshot.estimated_stop_request_id == 70,
        "显式估计策略记录独立request id后开火");
    ack(c, d, 3);
    auto up = c.tick(p, at(23));
    expect(c.tick(p, at(24)).snapshot.reason == TriggerReason::COMMAND_PENDING,
        "正常UP待确认不把已经清理的估计id误判为停稳失效");
    ack(c, up, 25);
    p.estimated_stop_request_id = 71;
    expect(c.observe(frame(120, 2), p, at(120)).snapshot.reason == TriggerReason::COOLDOWN,
        "新的独立急停已完成仍须等待上一发间隔");
    d = c.tick(p, at(123));
    expect(d.button_action == TriggerButtonAction::DOWN && d.stop_action == TriggerStopAction::NONE,
        "间隔与独立急停并行到期，不追加一段急停等待");
    ack(c, d, 124);
    p.stop_estimated_qualified = false;
    up = c.tick(p, at(125));
    expect(up.button_action == TriggerButtonAction::UP && up.stop_action == TriggerStopAction::NONE &&
        up.snapshot.reason == TriggerReason::STOP_EXPIRED, "HELD估计资格失效立刻UP且不取消不属于扳机的急停");

    cfg.allow_estimated_stop = false;
    TriggerController strict; arm(strict, cfg);
    p.stop_estimated_qualified = true;
    d = strict.observe(frame(1), p, at(1));
    expect(d.stop_action == TriggerStopAction::REQUEST && strict.tick(p, at(2)).button_action == TriggerButtonAction::NONE,
        "严格模式不能拿估计完成冒充观察停稳");
}

int main() {
    weapon_timing_submission_and_cleanup(); estimated_stop_is_explicit_and_parallel();
    fire_disabled_preserves_qualification_and_stop();
    geometry_and_timing(); association(); permissions_and_receipts(); stop_and_automatic();
    weapon_context_cancels_qualification(); weapon_context_invalidity_and_held_cleanup();
    auto bad = config(); bad.person_class_ids.push_back(1);
    expect(!valid_trigger_config(bad), "头身映射相交必须拒绝");
    bad = config(); bad.shot_interval_ms = 1;
    expect(!valid_trigger_config(bad), "点射间隔小于press必须拒绝");
    if (failures) return 1;
    std::cout << "Trigger 几何、时序、关联、取消和回执专项通过\n";
    return 0;
}
