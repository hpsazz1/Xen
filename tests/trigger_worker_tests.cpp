#include "trigger/trigger_worker.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

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
    std::atomic<bool> unknown_down{false}, unknown_up{false};
    std::atomic<bool> block_down{false}, release_down{false}, down_entered{false};
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
    TriggerWorker worker{mouse, arbiter, [&] { return permitted.load(); }, [&] {
        if (focus_hook) focus_hook(); return focused.load();
    }, [&] { return std::uint64_t(++ids); }, [&](std::uint64_t) { ++requests; return true; },
        [&](std::uint64_t) { ++cancellations; }};
    bool start(bool stop=false, int age=50, int cleanup_budget_ms=1000) {
        TriggerConfig cfg;
        cfg.enabled=true; cfg.hold_virtual_key=5; cfg.fire_delay_ms=0;
        cfg.max_observation_age_ms=age; cfg.require_stop=stop;
        cfg.press_duration_ms=10; cfg.shot_interval_ms=120;
        if (!worker.start(cfg, cleanup_budget_ms)) return false;
        // 必须先取得健康的真实释放，不能启动即按住开火。
        return until([&] { return worker.snapshot().reason == TriggerReason::RELEASED; });
    }
    void fire() { mouse->held=true; worker.publish(observation()); }
};
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
void unverified_stop_and_contention() {
    Fixture f; expect(f.start(true), "急停依赖worker启动"); f.fire();
    expect(until([&] { return f.requests.load()==1; }), "有候选才请求一次急停");
    expect(f.ids==1 && f.mouse->count(true)==0, "ESTIMATED不能开火或每tick分配id");
    expect(until([&] { return f.cancellations.load()==1; }), "无新帧过期须取消急停");
    f.worker.stop();

    Fixture busy; expect(busy.start(), "仲裁测试启动");
    auto lock = busy.arbiter->try_enter_cleanup();
    expect(lock.owns_lock(), "测试持有输出门");
    busy.fire();
    expect(until([&] {
        const auto value=busy.worker.snapshot();
        return value.command_id>0 && (value.reason==TriggerReason::CANCELED || value.reason==TriggerReason::NO_CANDIDATE);
    }), "仲裁忙时拒绝down且不阻塞");
    expect(busy.mouse->count(true)==0, "未获得门不能down");
    lock.unlock(); busy.worker.stop();
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
}
int main() {
    autonomous_cleanup(); unknown_and_late_ack(); final_revalidation(); unverified_stop_and_contention();
    not_sent_up_receipt_regression();
    shared_debt_and_shutdown_contention();
    return failures ? 1 : 0;
}
