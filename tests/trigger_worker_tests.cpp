#include "trigger/trigger_worker.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
#include <string_view>
#include <stdexcept>

namespace {
using namespace std::chrono_literals;
int failures = 0;
void expect(bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << message << '\n'; }
}
template<class F> bool until(F predicate, std::chrono::milliseconds timeout = 400ms) {
    const auto end = TriggerClock::now() + timeout;
    while (TriggerClock::now() < end) {
        if (predicate()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}
class Mouse final : public IMouseController {
public:
    bool open() noexcept override { return true; }
    MouseMoveReceipt move(const MouseMoveCommand&) noexcept override { return {}; }
    bool poll_input(InputSnapshot& out) noexcept override {
        out = {};
        out.status = healthy ? InputMonitorStatus::READY : InputMonitorStatus::STALE;
        out.state_valid = healthy;
        out.virtual_keys[5] = held;
        out.virtual_keys[1] = physical_left;
        out.virtual_keys['W'] = moving;
        out.sequence = ++sequence;
        return true;
    }
    bool supports_left_button() const noexcept override { return true; }
    bool left_button_faulted() const noexcept override { return fault; }
    bool left_button_cleanup_required() const noexcept override { return dirty; }
    ButtonReceipt set_left_button(bool down) noexcept override {
        {
            std::lock_guard lock(mutex);
            commands.push_back(down);
        }
        if (down) {
            dirty = true;
            down_entered = true;
            while (block_down && !release_down) std::this_thread::sleep_for(1ms);
        }
        ButtonReceipt receipt;
        receipt.datagram_sent = true;
        receipt.backend_completed_at = TriggerClock::now();
        receipt.disposition = down && unknown_down || !down && unknown_up
            ? ButtonDisposition::APPLICATION_UNKNOWN : ButtonDisposition::ACKNOWLEDGED;
        if (receipt.disposition == ButtonDisposition::APPLICATION_UNKNOWN) fault = true;
        else { receipt.protocol_ack_received_at = receipt.backend_completed_at; if (!down) { fault = false; dirty = false; } }
        receipt.cleanup_required = down || fault;
        if (down) {
            switch (timing_mode.load()) {
                case 1: receipt.backend_completed_at = {}; break;
                case 2: receipt.backend_completed_at -= 1s; receipt.protocol_ack_received_at = receipt.backend_completed_at; break;
                case 3: receipt.backend_completed_at += 1s; break;
                case 4: receipt.protocol_ack_received_at = {}; break;
                case 5: receipt.protocol_ack_received_at = receipt.backend_completed_at + 1s; break;
                case 6: receipt.protocol_ack_received_at -= 1s; break;
            }
            if (return_delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(return_delay_ms.load()));
        }
        return receipt;
    }
    void close() noexcept override {}
    MouseStatus status() const noexcept override { return MouseStatus::READY; }
    std::string last_error() const override { return {}; }
    std::size_t count(bool down) {
        std::lock_guard lock(mutex);
        std::size_t total=0; for (bool value : commands) if (value == down) ++total; return total;
    }
    std::atomic<bool> held{false}, healthy{true}, physical_left{false}, fault{false}, dirty{false};
    std::atomic<bool> moving{false};
    std::atomic<bool> unknown_down{false}, unknown_up{false};
    std::atomic<bool> block_down{false}, release_down{false}, down_entered{false};
    std::atomic<int> timing_mode{0}, return_delay_ms{0};
private:
    std::atomic<std::uint64_t> sequence{0};
    std::mutex mutex;
    std::vector<bool> commands;
};
auto observation(std::uint64_t sequence=1) {
    auto value = std::make_shared<TriggerObservation>();
    value->detections.push_back({30, 30, 70, 70, 0.9f, 0});
    value->center_x = value->center_y = 50;
    value->roi_width = value->roi_height = 100;
    value->epoch = 1; value->sequence = sequence;
    value->observed_at = TriggerClock::now();
    value->valid = value->timing_valid = true;
    return value;
}
struct Fixture {
    std::shared_ptr<Mouse> mouse = std::make_shared<Mouse>();
    std::shared_ptr<AutoStopOutputArbiter> arbiter = std::make_shared<AutoStopOutputArbiter>();
    std::atomic<bool> permitted{true}, focused{true};
    std::atomic<unsigned> requests{0}, cancellations{0}, ids{0};
    std::function<void()> focus_hook;
    std::atomic<std::uint64_t> context_generation{0};
    std::atomic<bool> context_required{false}, context_valid{false};
    std::atomic<std::uint64_t> trust_generation{0};
    std::atomic<bool> session_trusted{false};
    std::atomic<bool> timing_required{false}, timing_valid{false};
    std::atomic<int> shot_hold_ms{60}, fire_interval_ms{600};
    std::atomic<std::uint64_t> estimated_id{0};
    std::function<void()> context_hook;
    std::function<void()> estimated_hook;
    std::atomic<bool> stop_idle{false};
    std::function<void()> idle_hook;
    TriggerWorker worker{mouse, arbiter, [&] { return permitted.load(); }, [&] {
        if (focus_hook) focus_hook(); return focused.load();
    }, [&] { return std::uint64_t(++ids); }, [&](std::uint64_t) { ++requests; return true; },
        [&](std::uint64_t) { ++cancellations; }, [&] {
            if (context_hook) context_hook();
            return TriggerContext{context_generation.load(), context_required.load(), context_valid.load(),
                timing_required.load(), timing_valid.load(), shot_hold_ms.load(), fire_interval_ms.load(),
                0, {}, trust_generation.load(), session_trusted.load()};
        }, [&] { if (estimated_hook) estimated_hook(); return estimated_id.load(); }, {}, {},
        [&](const InputSnapshot&) { if (idle_hook) idle_hook(); return stop_idle.load(); }};
    bool start(bool stop=false, int age=50, int cleanup_budget_ms=1000, int press_ms=10, bool fire_enabled=true,
        bool estimated=false) {
        TriggerConfig cfg;
        cfg.enabled=true; cfg.hold_virtual_key=5; cfg.fire_delay_ms=0;
        cfg.fire_enabled = fire_enabled;
        cfg.max_observation_age_ms=age; cfg.require_stop=stop;
        cfg.allow_estimated_stop = estimated;
        cfg.press_duration_ms=press_ms; cfg.shot_interval_ms=120;
        if (!worker.start(cfg, cleanup_budget_ms)) return false;
        // 必须先取得健康的真实释放，不能启动即按住开火。
        return until([&] { return worker.snapshot().reason == TriggerReason::RELEASED; });
    }
    void fire() { mouse->held=true; worker.publish(observation()); }
};
void cycle_resume_after_safe_up() {
    for (int mode = 0; mode < 5; ++mode) {
        auto mouse = std::make_shared<Mouse>();
        auto arbiter = std::make_shared<AutoStopOutputArbiter>();
        std::atomic<unsigned> resumed{0}, canceled{0};
        std::atomic<bool> focused{true};
        TriggerWorker worker(mouse, arbiter, [] { return true; }, [&] { return focused.load(); },
            [] { return std::uint64_t{8}; }, [](std::uint64_t) { return true; },
            [&](std::uint64_t id) { if (id == 7) ++canceled; }, {}, [] { return std::uint64_t{7}; },
            [&](std::uint64_t id, TriggerTime) { if (id == 7 && !mouse->dirty) ++resumed; });
        TriggerConfig config;
        config.enabled = config.require_stop = config.allow_estimated_stop = true;
        config.hold_virtual_key = 5; config.fire_delay_ms = 0;
        config.press_duration_ms = 80; config.shot_interval_ms = 250; config.max_observation_age_ms = 500;
        expect(worker.start(config), "循环扳机测试启动");
        expect(until([&] { return worker.snapshot().reason == TriggerReason::RELEASED; }), "等待松键准入");
        mouse->held = true; worker.publish(observation());
        expect(until([&] { return mouse->count(true) == 1 && worker.firing_signal().confirmed_down; }), "等待循环首DOWN确认");
        if (mode == 1) worker.cancel();
        if (mode == 2) mouse->physical_left = true;
        if (mode >= 3) {
            if (mode == 4) focused = false;
            auto missing = observation(2);
            missing->detections.clear();
            worker.publish(missing);
        }
        expect(until([&] { return mouse->count(false) >= 1 && resumed + canceled >= 1; }), "LEFT UP后完成归还或取消回调");
        const bool recoverable = mode == 0 || mode == 3;
        expect(resumed == (recoverable ? 1u : 0u) && canceled == (recoverable ? 0u : 1u),
            "正常或候选失效UP确认且安全时可续轮，显式取消、物理左键和失焦不得续轮");
        worker.publish(observation(3));
        std::this_thread::sleep_for(270ms);
        expect(mouse->count(true) == 1, "归还未完成或旧stop ID不得触发第二发");
        worker.stop();
    }
}

void physical_left_takes_over_until_trigger_rearmed() {
    auto mouse = std::make_shared<Mouse>();
    auto arbiter = std::make_shared<AutoStopOutputArbiter>();
    std::atomic<unsigned> resumed{0}, canceled{0}, retained{0};
    std::atomic<std::uint64_t> estimated_id{7};
    TriggerWorker worker(mouse, arbiter, [] { return true; }, [] { return true; },
        [] { return std::uint64_t{9}; }, [](std::uint64_t) { return true; },
        [&](std::uint64_t) { ++canceled; }, {}, [&] { return estimated_id.load(); },
        [&](std::uint64_t, TriggerTime) { ++resumed; },
        [&](std::uint64_t id) { if (id != 7) return false; ++retained; return true; });
    TriggerConfig config;
    config.enabled = config.require_stop = config.allow_estimated_stop = true;
    config.hold_virtual_key = 5; config.fire_delay_ms = 0;
    config.press_duration_ms = 500; config.shot_interval_ms = 600;
    config.max_observation_age_ms = 1000;
    expect(worker.start(config), "人工接管回归启动");
    expect(until([&] { return worker.snapshot().reason == TriggerReason::RELEASED; }),
        "人工接管前先取得扳机键释放边沿");
    mouse->held = true; worker.publish(observation());
    expect(until([&] { return mouse->count(true) == 1 && worker.firing_signal().confirmed_down; }),
        "人工接管前自动DOWN已经ACK确认");
    mouse->physical_left = true;
    worker.publish(observation(2));
    expect(until([&] { return mouse->count(false) == 1 && !mouse->dirty &&
        !worker.firing_signal().confirmed_down && retained == 1; }),
        "人工左键接管后清理软件DOWN，同时保留急停");
    // 提供新的急停资格并持续更新有效图像，排除旧stop ID、冷却和图像过期掩盖重发。
    estimated_id = 8;
    std::uint64_t sequence = 3;
    const auto hold_until = TriggerClock::now() + 650ms;
    while (TriggerClock::now() < hold_until) {
        worker.publish(observation(sequence++));
        std::this_thread::sleep_for(3ms);
    }
    expect(mouse->count(true) == 1 && mouse->count(false) == 1 && resumed == 0 && canceled == 0 && retained == 1 &&
        worker.snapshot().reason == TriggerReason::PERMISSION,
        "人工持续左键期间不发新DOWN、不重复UP且不归还移动");
    mouse->physical_left = false;
    worker.publish(observation(sequence++));
    expect(until([&] { return worker.snapshot().reason == TriggerReason::WAIT_RELEASE; }),
        "人工左键释放但扳机键仍按住时等待扳机键重新武装");
    expect(mouse->count(true) == 1 && mouse->count(false) == 1 && resumed == 0,
        "人工接管结束不会自动恢复旧点射或移动周期");
    mouse->held = false;
    expect(until([&] { return worker.snapshot().reason == TriggerReason::RELEASED; }),
        "重新释放扳机键建立新许可边沿");
    mouse->held = true; worker.publish(observation(sequence++));
    expect(until([&] { return mouse->count(true) == 2 && worker.firing_signal().confirmed_down; }),
        "扳机键重新按下后才可开始新的自动点射");
    worker.stop();
}
void rejected_manual_takeover_preserves_held_stop() {
    for (const bool lose_focus : {false, true}) {
        auto mouse = std::make_shared<Mouse>();
        std::atomic<bool> focused{true};
        std::atomic<unsigned> canceled{0}, resumed{0}, attempts{0};
        TriggerWorker worker(mouse, std::make_shared<AutoStopOutputArbiter>(), [] { return true; },
            [&] { return focused.load(); }, [] { return std::uint64_t{8}; },
            [](std::uint64_t) { return true; }, [&](std::uint64_t) { ++canceled; },
            {}, [] { return std::uint64_t{7}; },
            [&](std::uint64_t, TriggerTime) { ++resumed; },
            [&](std::uint64_t) { ++attempts; return false; });
        TriggerConfig config;
        config.enabled = config.require_stop = config.allow_estimated_stop = true;
        config.hold_virtual_key = 5; config.fire_delay_ms = 0;
        config.press_duration_ms = 500; config.shot_interval_ms = 600;
        config.max_observation_age_ms = 1000;
        expect(worker.start(config), "人工保持拒绝测试启动");
        expect(until([&] { return worker.snapshot().reason == TriggerReason::RELEASED; }), "先释放扳机许可");
        mouse->held = true; worker.publish(observation());
        expect(until([&] { return worker.firing_signal().confirmed_down; }), "人工接管前确认DOWN");
        if (lose_focus) focused = false;
        mouse->physical_left = true; worker.publish(observation(2));
        expect(until([&] { return mouse->count(false) == 1 && !worker.firing_signal().confirmed_down; }),
            "接管拒绝也必须确认软件UP");
        std::this_thread::sleep_for(10ms);
        expect(resumed == 0 && canceled == (lose_focus ? 1u : 0u),
            "安全按住时人工保持拒绝不得取消独立急停，失焦仍须取消");
        if (!lose_focus) expect(attempts > 0, "确实经过人工保持拒绝分支");
        worker.stop();
    }
}
void observation_release_preserves_independent_stop() {
    for (int cause = 0; cause != 4; ++cause) {
        auto mouse = std::make_shared<Mouse>();
        std::atomic<unsigned> canceled{0}, resumed{0};
        TriggerWorker worker(mouse, std::make_shared<AutoStopOutputArbiter>(), [] { return true; },
            [] { return true; }, [] { return std::uint64_t{8}; }, [](std::uint64_t) { return true; },
            [&](std::uint64_t) { ++canceled; }, {}, [] { return std::uint64_t{7}; },
            [&](std::uint64_t, TriggerTime) { ++resumed; });
        TriggerConfig config;
        config.enabled = config.require_stop = config.allow_estimated_stop = true;
        config.hold_virtual_key = 5; config.fire_delay_ms = 0;
        config.press_duration_ms = 200; config.shot_interval_ms = 250; config.max_observation_age_ms = 1000;
        expect(worker.start(config), "观测撤销与独立急停所有权测试启动");
        expect(until([&] { return worker.snapshot().reason == TriggerReason::RELEASED; }), "先观察许可释放");
        mouse->held = true; worker.publish(observation());
        expect(until([&] { return worker.firing_signal().confirmed_down; }), "观测撤销前确认DOWN");
        auto rejected = observation(2);
        if (cause == 0) rejected->valid = false;
        if (cause == 1) rejected->observed_at -= 2s;
        if (cause == 2) rejected->timing_valid = false;
        if (cause == 3) rejected->detections = {{48, 48, 52, 52, 0.9f, 0}};
        worker.publish(rejected);
        expect(until([&] { return mouse->count(false) == 1 && !worker.firing_signal().confirmed_down; }),
            "观测失效或候选变化必须清理软件LEFT");
        std::this_thread::sleep_for(10ms);
        expect(canceled == 0 && resumed == 0, "扳机局部观测撤销不得归还或取消仍有效的独立急停");
        worker.publish(observation(3));
        expect(until([&] { return mouse->count(true) == 2; }), "新有效帧与原急停资格可在冷却后恢复扳机");
        worker.stop();
    }
}

void fire_disabled_no_output_or_receipt() {
    Fixture f; expect(f.start(false, 200, 1000, 10, false), "不开枪调试启动"); f.fire();
    expect(until([&] { return f.worker.snapshot().reason == TriggerReason::FIRE_DISABLED; }), "快照明确不开枪原因");
    f.worker.stop();
    expect(f.mouse->count(true) == 0 && f.mouse->count(false) == 0 && !f.worker.firing_signal().confirmed_down,
        "关闭开枪不调用设备且不伪造firing信号");
    for (const auto& event : f.worker.execution_log().events)
        expect(!event.backend_called && event.receipt_status != TriggerReceiptStatus::ACKNOWLEDGED,
            "关闭开枪不伪造ACK事件");
    Fixture stop; expect(stop.start(true, 200, 1000, 10, false), "急停联调启动"); stop.fire();
    expect(until([&] { return stop.requests.load() == 1; }), "不开枪仍调用急停请求");
    stop.worker.stop();
    expect(stop.cancellations == 1 && stop.mouse->count(true) == 0 && !stop.worker.firing_signal().confirmed_down,
        "调试结束取消急停且从未确认发枪");
}
void autonomous_cleanup() {
    Fixture f; expect(f.start(), "worker启动及释放边沿"); f.fire();
    expect(until([&] { return f.mouse->count(false)==1; }), "无下一图像仍应按deadline释放");
    expect(f.mouse->count(true)==1, "同一观测不能重复开枪");
    f.worker.cancel();
    f.worker.publish(observation(2));
    expect(until([&] { return f.worker.snapshot().reason == TriggerReason::WAIT_RELEASE; }), "取消后长按不得复活");
    expect(f.mouse->count(true)==1, "取消新帧不产生down");
    f.worker.stop();
}
void unknown_and_late_ack() {
    Fixture f; f.mouse->unknown_down=true; f.mouse->unknown_up=true;
    expect(f.start(), "未知ACK测试启动"); f.fire();
    expect(until([&] { return f.mouse->count(false)>=3; }), "未知down须清理且未知up有界重试");
    expect(f.mouse->count(true)==1 && f.worker.snapshot().faulted, "未知ACK永不重试down且锁fault");
    f.mouse->unknown_up=false;
    f.worker.stop();
    expect(!f.mouse->fault && f.worker.snapshot().faulted, "清理成功不解除会话fault");

    Fixture late; late.mouse->block_down=true;
    expect(late.start(), "迟到ACK测试启动"); late.fire();
    expect(until([&] { return late.mouse->down_entered.load(); }), "down已在途");
    late.worker.cancel(); late.mouse->held=false; late.mouse->release_down=true;
    expect(until([&] { return late.mouse->count(false)==1; }), "取消后的迟到ACK必须up");
    expect(late.mouse->count(true)==1, "迟到ACK不能新发down");
    late.worker.stop();
}
void final_revalidation() {
    Fixture f;
    std::atomic<bool> replace{false};
    f.focus_hook = [&] {
        if (replace.exchange(false)) {
            auto changed = observation(); // 故意同epoch/seq，指针和内容却改变。
            changed->valid=false;
            f.worker.publish(changed);
        }
    };
    expect(f.start(), "重验测试启动");
    replace=true; f.fire();
    expect(until([&] {
        const auto reason=f.worker.snapshot().reason;
        return !replace.load() && (reason==TriggerReason::INVALID_OBSERVATION || reason==TriggerReason::NO_CANDIDATE);
    }), "替换观测应被取消");
    expect(f.mouse->count(true)==0, "latest改变禁止使用旧几何down");
    f.worker.stop();

    Fixture permission;
    expect(permission.start(), "许可测试启动");
    permission.permitted=false; permission.fire();
    expect(until([&] { return permission.worker.snapshot().reason == TriggerReason::PERMISSION; }), "失去许可应拒绝");
    expect(permission.mouse->count(true)==0, "无许可无down"); permission.worker.stop();
}
void estimated_stop_callback_and_revalidation() {
    Fixture f;
    expect(f.start(true, 1000, 1000, 100, true, true), "估计策略worker启动");
    f.fire();
    expect(until([&] { return f.worker.snapshot().reason == TriggerReason::STOP_UNVERIFIED; }),
        "估计回调为空时等待资格");
    expect(f.requests == 0 && f.ids == 0 && f.mouse->count(true) == 0, "估计策略不申请显式租约");
    f.estimated_id = 77;
    expect(until([&] { return f.worker.firing_signal().confirmed_down; }), "独立估计资格就绪允许DOWN");
    expect(f.worker.snapshot().estimated_stop_request_id == 77, "运行快照保存独立急停id");
    f.estimated_id = 0;
    expect(until([&] { return f.mouse->count(false) == 1 && !f.worker.firing_signal().confirmed_down; }),
        "估计资格撤销立即UP并归零开火信号");
    f.worker.stop();
    expect(f.requests == 0 && f.cancellations == 0, "扳机不申请或取消独立急停");

    Fixture rejected;
    rejected.estimated_id = 88;
    std::atomic<bool> at_gate{true};
    rejected.estimated_hook = [&] {
        if (rejected.arbiter->snapshot().sources[static_cast<std::size_t>(OutputArbiterSource::TRIGGER)].acquired &&
            at_gate.exchange(false)) rejected.estimated_id = 0;
    };
    expect(rejected.start(true, 1000, 1000, 100, true, true), "估计资格二检回归启动"); rejected.fire();
    expect(until([&] {
        for (const auto& event : rejected.worker.execution_log().events)
            if (event.button_action == TriggerButtonAction::DOWN && !event.backend_called &&
                std::string_view(event.rejection_reason) == "stop_unverified") return true;
        return false;
    }), "拿门后撤销估计资格必须记为未发送");
    rejected.worker.stop();
    expect(rejected.mouse->count(true) == 0 && rejected.mouse->count(false) == 0,
        "二检撤销不发送DOWN也不伪造UP");
}
void stationary_stop_bypass_and_revalidation() {
    Fixture f;
    f.stop_idle = true;
    expect(f.start(true, 1000, 1000, 100, true, true), "原地联动启动");
    f.fire();
    expect(until([&] { return f.worker.firing_signal().confirmed_down; }), "原地无急停编号可触发");
    expect(f.worker.snapshot().stop_not_needed && f.worker.snapshot().estimated_stop_request_id == 0 && f.requests == 0,
        "原地资格单独记录且不伪造急停事务");
    f.mouse->moving = true;
    expect(until([&] { return f.mouse->count(false) == 1 && !f.mouse->dirty; }), "重新移动立即释放");
    f.worker.publish(observation(2));
    expect(until([&] { return f.worker.snapshot().reason == TriggerReason::STOP_UNVERIFIED; }),
        "移动时即使owner曾空闲也必须等待制动");
    f.estimated_id = 31;
    expect(until([&] { return f.mouse->count(true) == 2; }), "制动完成不额外索要观察停稳证明");
    f.worker.stop();

    Fixture braking;
    expect(braking.start(true, 1000, 1000, 100, true, true), "松键制动等待启动");
    braking.fire();
    expect(until([&] { return braking.worker.snapshot().reason == TriggerReason::STOP_UNVERIFIED; }),
        "WASD全松但owner仍制动时不能抢先开火");
    braking.stop_idle = true;
    expect(until([&] { return braking.worker.firing_signal().confirmed_down; }),
        "清理完成即恢复原地触发，无需松开许可键重按");
    braking.worker.stop();

    Fixture rejected;
    rejected.stop_idle = true;
    rejected.idle_hook = [&] {
        if (rejected.arbiter->snapshot().sources[static_cast<std::size_t>(OutputArbiterSource::TRIGGER)].acquired)
            rejected.stop_idle = false;
    };
    expect(rejected.start(true, 1000, 1000, 100, true, true), "原地资格二检启动");
    rejected.fire();
    expect(until([&] {
        for (const auto& event : rejected.worker.execution_log().events)
            if (event.button_action == TriggerButtonAction::DOWN && !event.backend_called &&
                std::string_view(event.rejection_reason) == "stop_unverified") return true;
        return false;
    }), "发送前owner不再空闲必须拒绝旧原地决策");
    rejected.worker.stop();
    expect(rejected.mouse->count(true) == 0, "二检失败无DOWN");
}
void timing_change_at_down_revalidation() {
    Fixture f;
    f.context_generation = 1; f.timing_required = f.timing_valid = true;
    std::atomic<bool> change_at_gate{true};
    f.context_hook = [&] {
        if (f.arbiter->snapshot().sources[static_cast<std::size_t>(OutputArbiterSource::TRIGGER)].acquired &&
            change_at_gate.exchange(false)) f.shot_hold_ms = 80;
    };
    expect(f.start(false, 1000), "仅武器时序上下文二检启动"); f.fire();
    expect(until([&] {
        for (const auto& event : f.worker.execution_log().events)
            if (event.button_action == TriggerButtonAction::DOWN && !event.backend_called &&
                std::string_view(event.rejection_reason) == "context_changed") return true;
        return false;
    }), "二检即使代际漏增也拒绝变化的点射标量");
    f.worker.stop();
    expect(f.mouse->count(true) == 0, "不要求GSI身份仍须检查武器时序变更");
}
void context_change_at_down_revalidation() {
    for (const bool invalid : {false, true}) {
        Fixture f;
        f.context_required = f.context_valid = true; f.context_generation = 1;
        std::atomic<bool> change_at_output_gate{true};
        f.context_hook = [&] {
            // 通过真实仲裁器的公开计数识别 DOWN 已拿到门、正在二检的边界。
            const auto gate = f.arbiter->snapshot();
            if (gate.sources[static_cast<std::size_t>(OutputArbiterSource::TRIGGER)].acquired &&
                change_at_output_gate.exchange(false)) {
                f.context_generation = 2;
                f.context_valid = !invalid;
            }
        };
        expect(f.start(false, 200), "上下文二检回归启动");
        f.fire();
        expect(until([&] { return !change_at_output_gate.load(); }), "上下文在 DOWN 拿门后二检时改变");
        expect(until([&] {
            for (const auto& event : f.worker.execution_log().events)
                if (event.button_action == TriggerButtonAction::DOWN && !event.backend_called &&
                    std::string_view(event.rejection_reason) == "context_changed") return true;
            return false;
        }), "DOWN 二检变化必须记为未发出且保留取消原因");
        expect(f.mouse->count(true) == 0 && f.mouse->count(false) == 0,
            "新上下文不能放行旧决定，明确未发送不生成伪 UP");
        f.context_generation = 3; f.context_valid = true;
        f.worker.publish(observation(2));
        expect(until([&] { return f.worker.snapshot().reason == TriggerReason::WAIT_RELEASE; }),
            "连续查询或失效恢复不能消费一次 false 后重新放行旧 held");
        expect(f.mouse->count(true) == 0, "仍持键的新帧不产生 DOWN");
        f.mouse->held = false;
        expect(until([&] { return f.worker.snapshot().reason == TriggerReason::RELEASED; }), "取得新上下文的真实释放边沿");
        f.mouse->held = true; f.worker.publish(observation(3));
        expect(until([&] { return f.mouse->count(true) == 1; }),
            "完成释放再按下后新上下文仍可正常 DOWN");
        f.worker.stop();
    }
}
void ordinary_context_change_at_dispatch() {
    for (const bool trust_break : {false, true}) {
        Fixture f;
        f.context_required = f.context_valid = f.session_trusted = true;
        f.context_generation = f.trust_generation = 1;
        std::atomic<bool> change{true};
        f.context_hook = [&] {
            if (f.arbiter->snapshot().sources[static_cast<std::size_t>(OutputArbiterSource::TRIGGER)].acquired &&
                change.exchange(false)) {
                f.context_generation = 2;
                if (trust_break) f.trust_generation = 2;
            }
        };
        expect(f.start(false, 300), "可信上下文发送前回归启动"); f.fire();
        expect(until([&] {
            for (const auto& e : f.worker.execution_log().events)
                if (e.button_action == TriggerButtonAction::DOWN && !e.backend_called &&
                    std::string_view(e.rejection_reason) == "context_changed") return true;
            return false;
        }), "发送前切枪拒绝旧DOWN");
        expect(f.mouse->count(true) == 0 && f.mouse->count(false) == 0, "未发送旧DOWN不产生按钮债务");
        std::uint64_t seq = 1;
        if (trust_break) {
            expect(until([&] { f.worker.publish(observation(++seq));
                return f.worker.snapshot().reason == TriggerReason::WAIT_RELEASE; }), "信任变代不能持键恢复");
        } else {
            expect(until([&] { f.worker.publish(observation(++seq)); return f.mouse->count(true) == 1; }),
                "普通切枪发送前拒绝不撤销持键许可，新观测可恢复");
        }
        f.worker.stop();
    }
}
void context_change_releases_held_button() {
    Fixture f;
    f.context_required = f.context_valid = true; f.context_generation = 1;
    expect(f.start(false, 200, 1000, 100), "上下文 HELD 回归启动");
    f.fire();
    expect(until([&] { return f.worker.firing_signal().confirmed_down; }), "切枪前确实已确认 DOWN");
    f.context_generation = 2;
    expect(until([&] {
        for (const auto& event : f.worker.execution_log().events)
            if (event.button_action == TriggerButtonAction::UP && event.backend_called &&
                event.snapshot.reason == TriggerReason::CONTEXT_CHANGED) return true;
        return false;
    }), "切枪通过运行清理路径立即归还 HELD，不依赖新图或原 hold 截止");
    expect(!f.worker.firing_signal().confirmed_down && f.mouse->count(false) == 1,
        "切枪后的确认开火信号归零并清理一次");
    f.worker.publish(observation(2));
    expect(until([&] { return f.worker.snapshot().reason == TriggerReason::WAIT_RELEASE; }) && f.mouse->count(true) == 1,
        "旧许可键仍持有时不续用新武器开火");
    f.worker.stop();
}
void unverified_stop_and_contention() {
    Fixture f; expect(f.start(true), "急停依赖worker启动"); f.fire();
    expect(until([&] { return f.requests.load()==1; }), "有候选才请求一次急停");
    expect(f.ids==1 && f.mouse->count(true)==0, "ESTIMATED不能开火或每tick分配id");
    expect(until([&] { return f.cancellations.load()==1; }), "无新帧过期须取消急停");
    f.worker.stop();

    Fixture busy; expect(busy.start(false, 1000), "仲裁测试启动");
    auto lock = busy.arbiter->try_enter_cleanup();
    expect(lock.owns_lock(), "测试持有输出门");
    busy.fire();
    expect(until([&] {
        for (const auto& event : busy.worker.execution_log().events)
            if (event.button_action == TriggerButtonAction::DOWN &&
                std::string_view(event.rejection_reason) == "arbiter_lock_busy") return true;
        return false;
    }), "短事务竞争须可见且不阻塞worker");
    const auto queued_id = busy.worker.snapshot().command_id;
    expect(busy.mouse->count(true)==0, "未获得门不能down");
    lock.unlock();
    expect(until([&] { return busy.mouse->count(true) == 1; }),
        "短锁竞争结束后原有效候选无需新图即可开火");
    bool same_command = false;
    for (const auto& event : busy.worker.execution_log().events)
        if (event.button_action == TriggerButtonAction::DOWN && event.backend_called &&
            event.snapshot.command_id == queued_id) same_command = true;
    expect(same_command, "争锁重试保留原DOWN命令编号，不取消再建候选");
    busy.worker.stop();
}
void not_sent_up_receipt_regression() {
    TriggerController controller;
    TriggerConfig cfg;
    cfg.enabled=true; cfg.hold_virtual_key=5; cfg.fire_delay_ms=0;
    expect(controller.configure(cfg), "回执回归配置");
    TriggerPermit permit;
    permit.enabled=permit.healthy=permit.focused=permit.armed=true;
    const auto now=TriggerClock::now();
    controller.tick(permit, now);
    permit.held=true;
    auto value=observation();
    auto down=controller.observe(*value, permit, value->observed_at);
    expect(down.button_action==TriggerButtonAction::DOWN, "回执回归down");
    controller.acknowledge({down.command_id, TriggerButtonAction::DOWN,
        TriggerReceiptStatus::ACKNOWLEDGED, value->observed_at}, value->observed_at);
    auto up=controller.cancel(TriggerReason::CANCELED, value->observed_at);
    controller.acknowledge({up.command_id, TriggerButtonAction::UP,
        TriggerReceiptStatus::NOT_SENT, value->observed_at}, value->observed_at);
    expect(controller.snapshot().faulted && controller.snapshot().button_may_be_down, "未发UP保留清理责任");
    controller.acknowledge({up.command_id, TriggerButtonAction::UP,
        TriggerReceiptStatus::ACKNOWLEDGED, value->observed_at}, value->observed_at);
    expect(controller.snapshot().faulted && !controller.snapshot().button_may_be_down,
        "同id UP重试ACK可消债但不复活fault会话");
}
void deferred_down_revalidates_and_cancels() {
    for (int ending = 0; ending < 8; ++ending) {
        Fixture f;
        expect(f.start(false, ending == 7 ? 50 : 1000), "待发命令撤销回归启动");
        auto lock = f.arbiter->try_enter_cleanup();
        f.fire();
        expect(until([&] { return f.worker.snapshot().phase == TriggerPhase::DOWN_PENDING; }),
            "争锁期间保留待发DOWN");
        if (ending == 0) f.mouse->held = false;
        if (ending == 1) f.focused = false;
        if (ending == 2) f.worker.cancel();
        if (ending == 3) {
            auto expired = observation(2); expired->observed_at -= 2s;
            f.worker.publish(expired);
        }
        if (ending == 4) {
            auto missing = observation(2); missing->detections.clear();
            f.worker.publish(missing);
        }
        if (ending == 6) {
            auto changed = observation(2);
            changed->detections = {{45, 45, 55, 55, 0.9f, 0}};
            f.worker.publish(changed);
        }
        if (ending == 5) {
            f.worker.publish(observation(2));
            expect(until([&] { return f.worker.snapshot().observation_sequence == 2; }),
                "等待短事务期间仍消费同候选的新帧");
            lock.unlock();
            expect(until([&] { return f.mouse->count(true) == 1; }), "同候选新鲜帧允许重试原命令");
        } else {
            expect(until([&] { return !f.worker.snapshot().button_may_be_down; }),
                "未提交DOWN撤销无需等待设备锁或发送UP");
            lock.unlock();
            std::this_thread::sleep_for(15ms);
            expect(f.mouse->count(true) == 0 && f.mouse->count(false) == 0,
                "松键失焦取消过期或候选退出更换后不补发DOWN也不伪造UP");
        }
        f.worker.stop();
    }
}
void withdraw_unsent_rejects_backend_debt() {
    for (int mode = 0; mode < 3; ++mode) {
        TriggerController controller;
        TriggerConfig cfg; cfg.enabled = true; cfg.fire_delay_ms = 0;
        expect(controller.configure(cfg), "未提交撤销契约配置");
        TriggerPermit p; p.enabled = p.healthy = p.focused = p.armed = true;
        const auto now = TriggerClock::now();
        controller.tick(p, now); p.held = true;
        auto frame = observation(); frame->observed_at = now;
        const auto down = controller.observe(*frame, p, now + 1ms);
        expect(down.button_action == TriggerButtonAction::DOWN &&
            !controller.withdraw_unsent(down.command_id + 1, now + 1ms) &&
            controller.snapshot().button_may_be_down,
            "错误命令ID不能清理待发或真实按钮债务");
        if (mode == 0) {
            controller.cancel(TriggerReason::CANCELED, now + 2ms);
            expect(controller.withdraw_unsent(down.command_id, now + 2ms) &&
                !controller.snapshot().button_may_be_down &&
                !controller.withdraw_unsent(down.command_id, now + 3ms),
                "原未提交DOWN只能撤销一次，取消生成的逻辑UP不需要设备回执");
        } else {
            TriggerReceipt receipt;
            receipt.command_id = down.command_id; receipt.action = TriggerButtonAction::DOWN;
            receipt.status = mode == 1 ? TriggerReceiptStatus::ACKNOWLEDGED : TriggerReceiptStatus::UNKNOWN;
            receipt.completed_at = now + 2ms;
            controller.acknowledge(receipt, now + 2ms);
            expect(!controller.withdraw_unsent(down.command_id, now + 3ms) &&
                controller.snapshot().button_may_be_down,
                "已ACK或UNKNOWN的实际按钮债务不可当未提交撤销");
        }
    }
}
void shared_debt_and_shutdown_contention() {
    Fixture dirty; dirty.mouse->dirty=true;
    expect(!dirty.start(), "复用设备已知down但无fault也必须拒绝启动");
    dirty.mouse->dirty=false;

    Fixture recover; expect(recover.start(), "停止争锁测试启动"); recover.fire();
    expect(until([&] { return recover.mouse->count(true)==1; }), "停止前down");
    auto lock = recover.arbiter->try_enter_cleanup();
    expect(lock.owns_lock(), "测试持有80ms输出门");
    std::thread stopper([&] { recover.worker.stop(); });
    std::this_thread::sleep_for(80ms);
    if (lock.owns_lock()) lock.unlock();
    stopper.join();
    expect(!recover.mouse->dirty && recover.mouse->count(false)>=1, "争锁80ms后stop仍需明确up");

    Fixture overdue; expect(overdue.start(false, 50, 30), "短清理预算测试启动"); overdue.fire();
    expect(until([&] { return overdue.mouse->count(true)==1; }), "超限前down");
    auto blocked = overdue.arbiter->try_enter_cleanup();
    expect(blocked.owns_lock(), "超限测试持有门");
    std::atomic<bool> stopped{false};
    std::thread stop_blocked([&] { overdue.worker.stop(); stopped=true; });
    expect(until([&] { return stopped.load(); }), "超限stop必须有界返回");
    if (blocked.owns_lock()) blocked.unlock();
    stop_blocked.join();
    expect(overdue.worker.snapshot().faulted && overdue.worker.snapshot().button_may_be_down && overdue.mouse->dirty,
        "争锁超限必须保留FAULT及按下债务快照");
    overdue.mouse->set_left_button(false); // 仅fake资源收尾，不抹掉worker验收快照。
}
void running_cleanup_contention() {
    Fixture f; expect(f.start(false, 200, 1000), "运行争锁回归启动"); f.fire();
    expect(until([&] { return f.mouse->count(true)==1; }), "运行争锁前DOWN");
    auto lock=f.arbiter->try_enter_cleanup();
    expect(lock.owns_lock(), "运行阶段持有输出门");
    f.mouse->held=false;
    expect(until([&] {
        unsigned blocked=0;
        for(const auto& e:f.worker.execution_log().events)
            if(e.button_action==TriggerButtonAction::UP && !e.backend_called) ++blocked;
        return blocked>=3;
    }), "等待明确三次争锁失败，不以睡眠推断尝试次数");
    expect(f.mouse->count(false)==0, "争锁不应记作实际UP发送");
    lock.unlock();
    expect(until([&] { return f.mouse->count(false)==1 && !f.worker.snapshot().button_may_be_down; }),
        "不停机且争锁超过三次后仍必须明确UP");
    expect(f.mouse->count(true)==1 && f.worker.snapshot().faulted, "运行清理不重发DOWN也不抹掉FAULT");
    auto log=f.worker.execution_log();
    unsigned actual_up=0, blocked_up=0;
    for (const auto& e:log.events) if(e.button_action==TriggerButtonAction::UP) {
        if(e.backend_called) ++actual_up; else ++blocked_up;
    }
    expect(actual_up==1 && blocked_up>=3, "事件区分争锁与实际发送次数");
    f.worker.stop();

    Fixture expired; expect(expired.start(false,200,30), "运行清理短预算启动"); expired.fire();
    expect(until([&] { return expired.mouse->count(true)==1; }), "超期前DOWN");
    auto held=expired.arbiter->try_enter_cleanup(); expired.mouse->held=false;
    std::this_thread::sleep_for(80ms); held.unlock();
    expect(until([&] {
        const auto events=expired.worker.execution_log();
        for(const auto& e:events.events) if(std::string_view(e.rejection_reason)=="cleanup_deadline_expired") return true;
        return false;
    }), "运行超期必须记清理截止证据");
    std::this_thread::sleep_for(20ms);
    expect(expired.mouse->count(false)==0 && expired.worker.snapshot().button_may_be_down,
        "超期不无限重试或丢弃债务");
    expect(!expired.arbiter->try_enter_aim().owns_lock(), "清理超期锁存共享故障");
    expired.worker.stop();
    expect(!expired.mouse->dirty && expired.worker.snapshot().faulted,
        "停止阶段可有界清债但不恢复会话");
}
void receipt_time_and_event_history() {
    for(int mode=1;mode<=6;++mode) {
        Fixture f; f.mouse->timing_mode=mode;
        expect(f.start(false,200), "异常时间fixture启动"); f.fire();
        // fake 在 UP 调用入口计数；Worker 完成回执链后才发布 fault 快照。
        // 必须等待两个完成事实，不能把“已进入设备调用”当成快照已发布。
        expect(until([&] { return f.mouse->count(false)>=1 && f.worker.snapshot().faulted; }),
            "异常按钮回执需要UP及已发布的故障快照");
        expect(f.worker.snapshot().faulted && !f.worker.firing_signal().confirmed_down,
            "缺失倒退未来回执不可产生有效开火信号");
        bool rejected=false;
        for(const auto& e:f.worker.execution_log().events)
            if(e.button_action==TriggerButtonAction::DOWN && e.backend_called &&
                e.receipt_status==TriggerReceiptStatus::UNKNOWN && std::string_view(e.rejection_reason)=="invalid_receipt_time") rejected=true;
        expect(rejected, "异常原始回执在独立事件中保留");
        f.worker.stop();
    }
    Fixture delayed; delayed.mouse->return_delay_ms=30;
    expect(delayed.start(false,200,1000,100), "合法延迟回执fixture启动"); delayed.fire();
    expect(until([&] { return delayed.worker.firing_signal().confirmed_down; }), "合法回执产生命令信号");
    const auto firing=delayed.worker.firing_signal();
    expect(firing.started_at==firing.backend_completed_at && firing.started_at!=TriggerTime{} &&
        firing.observed_at-firing.started_at>=25ms, "命令起点透传真实后端时间而不包含返回调度延迟");
    expect(firing.protocol_ack_received_at>=firing.call_started_at &&
        firing.protocol_ack_received_at<=firing.backend_completed_at &&
        firing.uncertainty==std::chrono::duration_cast<std::chrono::nanoseconds>(firing.backend_completed_at-firing.call_started_at),
        "原始ACK和命令区间宽度完整保留");
    delayed.worker.stop();
    const auto log=delayed.worker.execution_log();
    expect(log.dropped_count==0 && log.first_sequence==log.events.front().sequence &&
        log.last_sequence==log.events.back().sequence, "冷查询返回明确序号范围");
    bool down=false,up=false;
    for(std::size_t i=0;i<log.events.size();++i) {
        const auto& e=log.events[i];
        if(i) expect(e.sequence==log.events[i-1].sequence+1, "事件序号必须连续");
        if(e.backend_called && e.receipt_status==TriggerReceiptStatus::ACKNOWLEDGED) {
            down|=e.button_action==TriggerButtonAction::DOWN;
            up|=e.button_action==TriggerButtonAction::UP;
        }
    }
    expect(down&&up, "不依赖普通Log的完整按下释放事件");
}
void event_ring_is_bounded() {
    Fixture f; expect(f.start(), "事件环fixture启动");
    // 无输出，只切换健康许可；每次等待状态确认，避免生产者通知合并影响边沿数。
    for(int i=0;i<2100;++i) {
        const bool allowed=(i&1)!=0;
        f.permitted=allowed;
        expect(until([&] { return f.worker.snapshot().reason==
            (allowed?TriggerReason::RELEASED:TriggerReason::PERMISSION); }), "事件边沿可观察");
    }
    const auto log=f.worker.execution_log();
    expect(log.events.size()==2048 && log.dropped_count>0 && log.first_sequence>1 &&
        log.last_sequence-log.first_sequence+1==log.events.size(), "环满明确记录覆盖且保留连续后缀");
    expect(f.mouse->count(true)==0 && f.mouse->count(false)==0, "事件压力不生成设备命令");
    f.worker.stop();
}
void exception_uses_bounded_cleanup() {
    Fixture f;
    std::atomic<bool> throw_once{false};
    f.focus_hook=[&] { if(throw_once.exchange(false)) throw std::runtime_error("fake焦点失败"); };
    expect(f.start(false,200,1000,100), "异常清理fixture启动"); f.fire();
    expect(until([&] { return f.worker.firing_signal().confirmed_down; }), "异常前已确认DOWN");
    throw_once=true;
    expect(until([&] { return f.worker.snapshot().faulted && !f.mouse->dirty; }),
        "许可回调异常仍经公共清理路径明确UP并保留FAULT");
    expect(f.mouse->count(true)==1 && f.mouse->count(false)==1 && !f.worker.firing_signal().confirmed_down,
        "异常不重发DOWN且释放信号归零");
    f.worker.stop();
}

}
int main() {
    deferred_down_revalidates_and_cancels();
    withdraw_unsent_rejects_backend_debt();
    stationary_stop_bypass_and_revalidation();
    cycle_resume_after_safe_up();
    physical_left_takes_over_until_trigger_rearmed();
    rejected_manual_takeover_preserves_held_stop();
    observation_release_preserves_independent_stop();
    estimated_stop_callback_and_revalidation(); timing_change_at_down_revalidation();
    fire_disabled_no_output_or_receipt();
    autonomous_cleanup(); unknown_and_late_ack(); final_revalidation(); unverified_stop_and_contention();
    context_change_at_down_revalidation(); ordinary_context_change_at_dispatch(); context_change_releases_held_button();
    not_sent_up_receipt_regression();
    running_cleanup_contention(); receipt_time_and_event_history(); event_ring_is_bounded();
    exception_uses_bounded_cleanup();
    shared_debt_and_shutdown_contention();
    return failures ? 1 : 0;
}
