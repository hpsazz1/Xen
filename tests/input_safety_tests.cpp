#include "app/input_router.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << message << '\n';
}

class FakeInputDevice final : public IMouseController {
public:
    bool open() noexcept override { return true; }
    MouseMoveReceipt move(const MouseMoveCommand&) noexcept override {
        return {};
    }
    bool poll_input(InputSnapshot& snapshot) noexcept override {
        if (!poll_succeeds) return false;
        snapshot = current;
        return true;
    }
    void close() noexcept override {}
    MouseStatus status() const noexcept override {
        return MouseStatus::DISABLED;
    }
    std::string last_error() const override { return {}; }

    InputSnapshot current;
    bool poll_succeeds = true;
};

bool contains_event(
        const KeyboardPollResult& poll,
        KeyboardEventType type,
        bool active) {
    return std::any_of(
        poll.events.begin(), poll.events.end(),
        [&](const KeyboardEvent& event) {
            return event.type == type && event.active == active;
        });
}

app::detail::KeyboardRoutingResult route_poll(
        Runtime& runtime,
        const KeyboardPollResult& poll,
        bool hotkey_capture_consumed = false) {
    expect(app::detail::route_input_health(runtime, poll),
           "生产路由必须接受独立 input-health intent");
    return app::detail::route_keyboard_events(
        runtime, poll, hotkey_capture_consumed);
}

void set_snapshot(
        FakeInputDevice& device,
        InputMonitorStatus status,
        bool state_valid,
        std::uint64_t sequence,
        bool hold_active) {
    device.current = {};
    device.current.status = status;
    device.current.state_valid = state_valid;
    device.current.sequence = sequence;
    device.current.virtual_keys[0x02] = hold_active;
}

void test_startup_loss_and_recovery() {
    auto device = std::make_shared<FakeInputDevice>();
    set_snapshot(*device, InputMonitorStatus::WAITING, false, 0, false);
    KeyboardListener keyboard(KeyboardConfig{}, device);
    Runtime runtime;
    expect(keyboard.open(), "Keyboard 必须成功打开");

    auto poll = keyboard.poll();
    expect(poll.monitor_status == InputMonitorStatus::WAITING &&
               !poll.input_healthy && !poll.new_input_fact &&
               poll.events.empty(),
           "启动 WAITING 必须保持无事实、无健康、无按键事件");
    route_poll(runtime, poll);
    RuntimeSnapshot snapshot = runtime.snapshot();
    expect(!snapshot.input_healthy && !snapshot.output_armed &&
               !snapshot.emergency_stopped &&
               !runtime.post_intent({RuntimeIntentType::ARM_OUTPUT, true}),
           "启动输入未健康时计算 owner 可存在，但物理门不得武装");

    set_snapshot(*device, InputMonitorStatus::READY, true, 1, true);
    poll = keyboard.poll();
    expect(poll.input_healthy && poll.new_input_fact &&
               poll.sequence == 1 &&
               contains_event(
                   poll, KeyboardEventType::AIM_HOLD_CHANGED, true),
           "首个 READY 新序号必须建立健康与真实按下事实");
    route_poll(runtime, poll);
    expect(runtime.post_intent({RuntimeIntentType::ARM_OUTPUT, true}),
           "输入健康后必须允许显式武装");
    snapshot = runtime.snapshot();
    expect(snapshot.input_healthy && snapshot.output_armed &&
               snapshot.aim_hold_active,
           "健康、武装与真实 hold 必须同时进入 Runtime Snapshot");

    set_snapshot(*device, InputMonitorStatus::STALE, false, 1, false);
    poll = keyboard.poll();
    expect(!poll.input_healthy && !poll.new_input_fact &&
               poll.sequence == 1 && poll.events.empty(),
           "STALE 不得伪造 release 或丢失最近事实序号");
    KeyboardPollResult captured_poll = poll;
    captured_poll.events = {
        {KeyboardEventType::AIM_HOLD_CHANGED, false},
        {KeyboardEventType::EMERGENCY_STOP, true},
        {KeyboardEventType::RUNTIME_TOGGLE, true}};
    const auto captured = route_poll(runtime, captured_poll, true);
    snapshot = runtime.snapshot();
    expect(!snapshot.input_healthy && !snapshot.output_armed &&
               snapshot.aim_hold_active && !snapshot.emergency_stopped &&
               !captured.emergency_pressed &&
               !captured.runtime_toggle_pressed,
           "热键捕获必须吞语义事件，但不能吞健康失败或伪造急停");

    set_snapshot(*device, InputMonitorStatus::READY, true, 1, true);
    poll = keyboard.poll();
    route_poll(runtime, poll);
    expect(!poll.input_healthy && !poll.new_input_fact &&
               !runtime.post_intent({RuntimeIntentType::ARM_OUTPUT, true}),
           "故障后的同序号缓存不得恢复健康或允许武装");

    set_snapshot(*device, InputMonitorStatus::READY, true, 2, true);
    poll = keyboard.poll();
    route_poll(runtime, poll);
    snapshot = runtime.snapshot();
    expect(poll.input_healthy && poll.new_input_fact && poll.events.empty() &&
               snapshot.input_healthy && !snapshot.output_armed &&
               snapshot.aim_hold_active,
           "新输入事实可恢复健康，但不得重复 hold 或自动重武装");
    expect(runtime.post_intent({RuntimeIntentType::ARM_OUTPUT, true}),
           "健康恢复后仍必须由显式 ARM 才能重新武装");

    set_snapshot(*device, InputMonitorStatus::READY, true, 3, false);
    poll = keyboard.poll();
    route_poll(runtime, poll);
    snapshot = runtime.snapshot();
    expect(contains_event(
               poll, KeyboardEventType::AIM_HOLD_CHANGED, false) &&
               snapshot.input_healthy && !snapshot.aim_hold_active,
           "严格新序号的真实释放必须独立清除 hold 事实");
}

void test_failure_cache_and_new_release() {
    auto device = std::make_shared<FakeInputDevice>();
    set_snapshot(*device, InputMonitorStatus::READY, true, 10, true);
    KeyboardListener keyboard(KeyboardConfig{}, device);
    Runtime runtime;
    expect(keyboard.open(), "FAILURE 场景 Keyboard 必须打开");
    auto poll = keyboard.poll();
    route_poll(runtime, poll);
    expect(runtime.post_intent({RuntimeIntentType::ARM_OUTPUT, true}),
           "FAILURE 场景必须先显式武装");

    set_snapshot(*device, InputMonitorStatus::FAILURE, true, 10, false);
    poll = keyboard.poll();
    route_poll(runtime, poll);
    RuntimeSnapshot snapshot = runtime.snapshot();
    expect(!poll.new_input_fact && poll.events.empty() &&
               !snapshot.input_healthy && !snapshot.output_armed &&
               snapshot.aim_hold_active && !snapshot.emergency_stopped,
           "FAILURE 同序号缓存必须解除武装但保留按下和急停事实");

    set_snapshot(*device, InputMonitorStatus::FAILURE, true, 11, false);
    poll = keyboard.poll();
    route_poll(runtime, poll);
    snapshot = runtime.snapshot();
    expect(poll.new_input_fact && !poll.input_healthy &&
               contains_event(
                   poll, KeyboardEventType::AIM_HOLD_CHANGED, false) &&
               !snapshot.aim_hold_active && !snapshot.input_healthy,
           "FAILURE 携带严格新序号时不得吞掉真实释放事实");
}

void test_poll_failure_visible_in_same_result() {
    auto device = std::make_shared<FakeInputDevice>();
    set_snapshot(*device, InputMonitorStatus::READY, true, 1, true);
    KeyboardListener keyboard(KeyboardConfig{}, device);
    Runtime runtime;
    expect(keyboard.open(), "poll_input 失败场景 Keyboard 必须打开");
    auto poll = keyboard.poll();
    route_poll(runtime, poll);
    expect(runtime.post_intent({RuntimeIntentType::ARM_OUTPUT, true}),
           "poll_input 失败前必须能显式武装");

    device->poll_succeeds = false;
    poll = keyboard.poll();
    route_poll(runtime, poll);
    const RuntimeSnapshot snapshot = runtime.snapshot();
    expect(poll.monitor_status == InputMonitorStatus::FAILURE &&
               !poll.input_healthy && poll.events.empty() &&
               keyboard.status() == KeyboardStatus::FAILURE &&
               !snapshot.output_armed && snapshot.aim_hold_active,
           "poll_input=false 必须在同一次结果暴露健康失败并关闭物理门");
}

void test_listener_generation_resets_sequence_owner() {
    auto device = std::make_shared<FakeInputDevice>();
    set_snapshot(*device, InputMonitorStatus::READY, true, 100, true);
    KeyboardListener first(KeyboardConfig{}, device);
    expect(first.open(), "第一代 Listener 必须打开");
    const auto first_poll = first.poll();
    expect(first_poll.new_input_fact && first_poll.sequence == 100,
           "第一代必须记录自己的输入序号");
    first.close();

    set_snapshot(*device, InputMonitorStatus::READY, true, 1, true);
    KeyboardListener replacement(KeyboardConfig{}, device);
    Runtime runtime;
    expect(replacement.open(), "替换 Listener 必须打开");
    const auto replacement_poll = replacement.poll();
    route_poll(runtime, replacement_poll);
    const RuntimeSnapshot snapshot = runtime.snapshot();
    expect(replacement_poll.new_input_fact &&
               replacement_poll.sequence == 1 &&
               replacement_poll.input_healthy &&
               snapshot.aim_hold_active && !snapshot.output_armed,
           "新设备代际首个有效序号必须重新建事实且不得继承武装");
}

} // namespace

int main() {
    test_startup_loss_and_recovery();
    test_failure_cache_and_new_release();
    test_poll_failure_visible_in_same_result();
    test_listener_generation_resets_sequence_owner();

    if (failures != 0) {
        std::cerr << "input_safety_tests 失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "input_safety_tests 全部通过\n";
    return 0;
}
