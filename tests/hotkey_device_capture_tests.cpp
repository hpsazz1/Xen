#include "keyboard/keyboard.h"
#include "overlay/overlay_internal.h"
#include <iostream>

namespace {
class Device final : public IMouseController {
public:
    InputSnapshot input;
    bool open() noexcept override { return true; }
    MouseMoveReceipt move(const MouseMoveCommand&) noexcept override { return {}; }
    bool poll_input(InputSnapshot& value) noexcept override { value = input; return true; }
    void close() noexcept override {}
    MouseStatus status() const noexcept override { return MouseStatus::READY; }
    std::string last_error() const override { return {}; }
};
int failures = 0;
void expect(bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << message << '\n'; }
}
void side_button(int key) {
    using namespace overlay::detail;
    auto device = std::make_shared<Device>();
    device->input.state_valid = true;
    device->input.status = InputMonitorStatus::READY;
    device->input.sequence = 1;
    KeyboardListener keyboard({}, device);
    expect(keyboard.open(), "假设备监听应打开");
    std::array<bool, 256> local{};
    local[1] = true; // 辅机点击采集按钮，主机鼠标没有按下。
    HotkeyCaptureState state;
    auto poll = keyboard.poll();
    const auto keys = [](const KeyboardPollResult& p) { return p.capture_state_valid ? &p.capture_virtual_keys : nullptr; };
    begin_hotkey_capture(state, local, keys(poll));
    expect(update_hotkey_capture(state, local, keys(poll)).type == HotkeyCaptureResultType::NONE,
        "开始捕获的本机点击不能误绑定");
    local.fill(false);
    update_hotkey_capture(state, local, keys(poll));
    device->input.virtual_keys[key] = true;
    ++device->input.sequence;
    poll = keyboard.poll();
    const auto result = update_hotkey_capture(state, local, keys(poll));
    expect(result.type == HotkeyCaptureResultType::ASSIGNED && result.virtual_key == key,
        "本机全零时必须捕获后端鼠标侧键");
    begin_hotkey_capture(state, local, keys(poll));
    expect(update_hotkey_capture(state, local, keys(poll)).type == HotkeyCaptureResultType::NONE,
        "捕获开始前按住的后端键不能重复绑定");
    device->input.status = InputMonitorStatus::STALE;
    poll = keyboard.poll();
    expect(!poll.capture_state_valid, "过期输入不可用于配置捕获");
    update_hotkey_capture(state, local, keys(poll));
    device->input.status = InputMonitorStatus::READY;
    ++device->input.sequence;
    poll = keyboard.poll();
    expect(update_hotkey_capture(state, local, keys(poll)).type == HotkeyCaptureResultType::NONE,
        "故障恢复的按住缓存仅建立基线");
    device->input.virtual_keys[key] = false;
    ++device->input.sequence;
    poll = keyboard.poll();
    update_hotkey_capture(state, local, keys(poll));
    device->input.virtual_keys[key] = true;
    ++device->input.sequence;
    poll = keyboard.poll();
    local[0x1B] = true;
    expect(update_hotkey_capture(state, local, keys(poll)).type == HotkeyCaptureResultType::CLEARED,
        "本机Esc优先于同时到达的后端侧键");
}
}
int main() {
    side_button(5); side_button(6);
    return failures ? 1 : 0;
}
