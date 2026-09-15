#include "auto_stop/auto_stop_worker.h"
#include "trigger/trigger_worker.h"
#include "weapon/weapon_timing.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
std::int64_t ns(Clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
}
template<class Predicate> void until(Predicate predicate, std::chrono::milliseconds budget = 500ms) {
    const auto deadline = Clock::now() + budget;
    while (!predicate()) {
        if (Clock::now() >= deadline) throw std::runtime_error("组合测试等待超时，未获得所需执行证据");
        std::this_thread::sleep_for(1ms);
    }
}
struct KeyCommand {
    std::uint8_t mask;
    Clock::time_point submitted, acknowledged;
};
class FakeMouse final : public IMouseController {
public:
    bool open() noexcept override { return true; }
    MouseMoveReceipt move(const MouseMoveCommand&) noexcept override { ++moves; return {}; }
    bool output_owner_exclusive() const noexcept override { return true; }
    bool supports_wasd_keyboard() const noexcept override { return true; }
    bool supports_left_button() const noexcept override { return true; }
    bool left_button_faulted() const noexcept override { return false; }
    bool left_button_cleanup_required() const noexcept override { return left_down.load(); }
    bool poll_input(InputSnapshot& input) noexcept override {
        std::lock_guard lock(mutex);
        input = {}; input.state_valid = true; input.status = InputMonitorStatus::READY;
        input.sequence = sequence; input.virtual_keys[5] = activation;
        input.virtual_keys['W'] = (held & 1) != 0; input.virtual_keys['A'] = (held & 2) != 0;
        input.virtual_keys['S'] = (held & 4) != 0; input.virtual_keys['D'] = (held & 8) != 0;
        return true;
    }
    bool set_wasd_event_subscription(bool value) noexcept override {
        std::lock_guard lock(mutex); subscribed = value; return true;
    }
    bool read_wasd_events(WasdEventCursor& cursor, WasdEventBatch& batch) noexcept override {
        std::lock_guard lock(mutex);
        batch = {}; batch.subscribed = subscribed; cursor.epoch = 1;
        for (const auto& event : events) if (event.sequence > cursor.sequence && batch.count < batch.events.size()) {
            batch.events[batch.count++] = event; cursor.sequence = event.sequence;
        }
        delivered = std::max(delivered, cursor.sequence);
        return true;
    }
    static KeyboardReceipt keyboard_ack() noexcept {
        KeyboardReceipt result;
        result.datagram_sent = true; result.disposition = KeyboardDisposition::ACKNOWLEDGED;
        result.protocol_ack_received_at = Clock::now();
        // 故意区分协议ACK与后端返回，禁止用返回时刻冒充ACK。
        std::this_thread::sleep_for(2ms);
        result.backend_completed_at = Clock::now();
        return result;
    }
    KeyboardReceipt set_wasd_keyboard(std::uint8_t mask) noexcept override {
        const auto started = Clock::now();
        const auto receipt = keyboard_ack();
        std::lock_guard lock(mutex);
        software = mask; keys.push_back({mask, started, receipt.protocol_ack_received_at});
        return receipt;
    }
    KeyboardReceipt set_wasd_mask(std::uint8_t key, bool masked) noexcept override {
        const auto receipt = keyboard_ack();
        std::lock_guard lock(mutex);
        if (masked) masks |= key; else masks &= ~key;
        return receipt;
    }
    KeyboardReceipt cleanup_wasd_keyboard() noexcept override {
        const auto receipt = keyboard_ack();
        std::lock_guard lock(mutex);
        if (left_down.load()) cleanup_during_shot = true;
        masks = software = 0;
        return receipt;
    }
    ButtonReceipt set_left_button(bool down) noexcept override {
        ButtonReceipt result;
        result.datagram_sent = true; result.disposition = ButtonDisposition::ACKNOWLEDGED;
        result.protocol_ack_received_at = Clock::now();
        std::this_thread::sleep_for(2ms);
        result.backend_completed_at = Clock::now(); result.cleanup_required = down;
        left_down.store(down);
        if (down) ++downs; else ++ups;
        return result;
    }
    void physical(std::uint8_t mask, bool activate) {
        std::lock_guard lock(mutex); held = mask; activation = activate;
        events.push_back({mask, true, 1, ++sequence, ns(Clock::now())});
    }
    void allow(bool activate) { std::lock_guard lock(mutex); activation = activate; }
    bool drained() { std::lock_guard lock(mutex); return delivered == sequence; }
    bool released() { std::lock_guard lock(mutex); return masks == 0 && software == 0 && !left_down; }
    std::vector<KeyCommand> keyboard_commands() { std::lock_guard lock(mutex); return keys; }
    void close() noexcept override {}
    MouseStatus status() const noexcept override { return MouseStatus::READY; }
    std::string last_error() const override { return {}; }
    std::atomic<unsigned> downs{0}, ups{0}, moves{0};
    std::atomic<bool> cleanup_during_shot{false};
private:
    std::mutex mutex;
    std::vector<WasdEvent> events;
    std::vector<KeyCommand> keys;
    std::uint64_t sequence = 0, delivered = 0;
    std::uint8_t held = 0, masks = 0, software = 0;
    bool activation = false, subscribed = false;
    std::atomic<bool> left_down{false};
};
std::shared_ptr<TriggerObservation> fresh_observation(std::uint64_t sequence) {
    auto result = std::make_shared<TriggerObservation>();
    result->detections.push_back({30, 30, 70, 70, .9f, 0});
    result->center_x = result->center_y = 50;
    result->roi_width = result->roi_height = 100;
    result->epoch = 1; result->sequence = sequence;
    result->observed_at = Clock::now(); result->valid = result->timing_valid = true;
    return result;
}
void run_weapon(const char* weapon_id, bool cycle = false) {
    const auto catalog = weapon::default_timing_catalog();
    const auto* profile = weapon::find_timing(catalog, weapon_id);
    require(profile && profile->enabled, "组合测试须使用共享表有效武器");
    auto mouse = std::make_shared<FakeMouse>();
    auto arbiter = std::make_shared<AutoStopOutputArbiter>();
    std::atomic<std::uint64_t> next_id{0};
    std::atomic<unsigned> trigger_requests{0};
    AutoStopWorker stop(mouse, arbiter, [] { return true; }, [&] { return ++next_id; }, [] { return true; });
    std::function<void(std::uint64_t, TriggerTime)> resume;
    if (cycle) resume = [&](std::uint64_t id, TriggerTime deadline) {
        require(stop.resume_movement(id, deadline - 58ms), "循环必须接收当前点射对应的急停归还");
    };
    TriggerWorker trigger(mouse, arbiter, [] { return true; }, [] { return true; }, [&] { return ++next_id; },
        [&](std::uint64_t id) { ++trigger_requests; return stop.request(id); },
        [&](std::uint64_t id) { stop.cancel(id); }, [&] {
            TriggerContext result{1, true, true};
            result.timing_required = result.timing_valid = true;
            result.shot_hold_ms = profile->shot_hold_ms; result.fire_interval_ms = profile->fire_interval_ms;
            result.timing_catalog_revision = catalog.revision; result.timing_weapon_id = profile->canonical_id;
            return result;
        }, [&] { return stop.estimated_completion_id(); }, std::move(resume));
    AutoStopConfig stop_config{true, 5};
    stop_config.cycle_enabled = cycle;
    stop_config.counter_hold_ms = 40; stop_config.shot_after_release_ms = 18;
    TriggerConfig trigger_config;
    trigger_config.enabled = true; trigger_config.hold_virtual_key = 5; trigger_config.fire_delay_ms = 0;
    trigger_config.require_stop = trigger_config.allow_estimated_stop = true;
    trigger_config.max_observation_age_ms = 300;
    mouse->physical(0, false);
    require(stop.start(stop_config), "急停生产worker启动失败");
    require(trigger.start(trigger_config), "扳机生产worker启动失败");
    until([&] { return mouse->drained() && trigger.snapshot().reason == TriggerReason::RELEASED; });
    mouse->physical(2, false);
    until([&] { return mouse->drained(); });
    std::this_thread::sleep_for(100ms);
    require(mouse->downs == 0, "允许键未按下不得开火");
    mouse->allow(true);
    std::uint64_t sequence = 0;
    until([&] {
        stop.publish_target(Clock::now() + 300ms);
        trigger.publish(fresh_observation(++sequence));
        return mouse->ups >= 2 && !trigger.snapshot().button_may_be_down &&
            (!cycle || (stop.snapshot().cycle_count >= 2 && mouse->released()));
    }, 2500ms);
    const auto keys = mouse->keyboard_commands();
    const auto stopped = stop.snapshot();
    require(keys.size() == (cycle ? 6 : 3) && keys[0].mask == 0 && keys[1].mask == 8 && keys[2].mask == 0,
        "每次接管均须zero→D→zero，循环每发重新制动；旧模式只制动一次");
    if (cycle) require(keys[3].mask == 0 && keys[4].mask == 8 && keys[5].mask == 0 &&
        stopped.cycle_moving && stopped.cycle_count == 2 && !mouse->cleanup_during_shot.load(),
        "持续按A必须完成两轮反向制动，且仅在LEFT UP确认后归还移动");
    require(keys[2].submitted >= keys[1].acknowledged + 40ms, "反向按住不得早于ACK加40ms释放");
    require(stopped.requests == (cycle ? 2 : 1) && stopped.completed == (cycle ? 2 : 1) && trigger_requests == 0,
        "两发共享一次独立急停，不额外创建观察租约或急停请求");
    require(!stopped.fire_permitted, "估计时序不得伪造严格观察资格");
    mouse->allow(false);
    until([&] { return stop.estimated_completion_id() == 0 && mouse->released(); });
    trigger.stop(); stop.stop();
    const auto log = trigger.execution_log();
    std::vector<TriggerExecutionEvent> down_events, up_events;
    for (const auto& event : log.events) if (event.backend_called && event.receipt_status == TriggerReceiptStatus::ACKNOWLEDGED) {
        if (event.button_action == TriggerButtonAction::DOWN) down_events.push_back(event);
        if (event.button_action == TriggerButtonAction::UP) up_events.push_back(event);
    }
    require(down_events.size() == 2 && up_events.size() == 2, "共享worker应完成两对DOWN/UP并无多余点击");
    require(down_events[0].call_started_at >= keys[2].acknowledged + 18ms,
        "最终软件释放ACK后不足18ms不得发出LEFT DOWN");
    require(down_events[1].call_started_at - down_events[0].call_started_at >= std::chrono::milliseconds(profile->fire_interval_ms),
        "武器两次DOWN实际提交间隔不得短于共享参数");
    for (std::size_t index = 0; index < 2; ++index) {
        require(up_events[index].call_started_at >= down_events[index].protocol_ack_received_at + std::chrono::milliseconds(profile->shot_hold_ms),
            "LEFT UP提交不得早于DOWN协议ACK加武器按住时长");
        require(down_events[index].snapshot.estimated_stop_request_id == (cycle ? index + 1 : stopped.request_id),
            "每发须绑定对应急停worker的估计完成id；循环不可复用旧id");
        if (cycle) require(down_events[index].call_started_at >= keys[index * 3 + 2].acknowledged + 18ms,
            "每轮LEFT DOWN都需等待本轮反向释放ACK加18ms");
    }
    require(mouse->released() && mouse->moves == 0, "松允许键清理必须归还键鼠且不得产生鼠标位移");
    std::cout << weapon_id << "：共享生产worker组合证据通过，未连接设备\n";
}
} // namespace
int main() {
    try {
        for (const char* id : {"deagle", "ak47", "awp"}) { run_weapon(id); run_weapon(id, true); }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "武器急停组合回归失败：" << error.what() << '\n';
        return 1;
    }
}
