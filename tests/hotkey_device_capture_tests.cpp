#include "keyboard/keyboard.h"
#include "mouse/input_internal.h"
#include "overlay/overlay_internal.h"
#include <iostream>
#include <utility>

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
void device_key(int key) {
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
        "本机全零时必须捕获后端物理键");
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
void grenade_keys() {
    const std::array<std::pair<int, KeyboardEventType>, 3> bindings{{
        {0x26, KeyboardEventType::GRENADE_ALIGN},
        {0x28, KeyboardEventType::GRENADE_THROW},
        {0x25, KeyboardEventType::GRENADE_CANCEL}}};
    for (const auto& [key, type] : bindings) {
        auto device = std::make_shared<Device>();
        KeyboardConfig config;
        config.grenade_assist_enabled = true;
        KeyboardListener listener(config, device);
        expect(listener.open(), "有效道具键配置应打开监听");
        const auto count = [&](const KeyboardPollResult& result) {
            return std::count_if(result.events.begin(), result.events.end(),
                [&](const auto& event) { return event.type == type && event.active; });
        };
        device->input.status = InputMonitorStatus::READY;
        device->input.state_valid = true; device->input.sequence = 1;
        device->input.virtual_keys[key] = true;
        expect(count(listener.poll()) == 0, "初始按住道具键不得补发动作");
        device->input.virtual_keys[key] = false; ++device->input.sequence;
        expect(count(listener.poll()) == 0, "释放只解除道具键阻塞");
        device->input.virtual_keys[key] = true; ++device->input.sequence;
        expect(count(listener.poll()) == 1, "新按下道具键只生成一次对应事件");
        expect(count(listener.poll()) == 0, "同一输入事实不重复道具事件");
        ++device->input.sequence;
        expect(count(listener.poll()) == 0, "持续按住不循环或累计道具事件");
        device->input.status = InputMonitorStatus::STALE; ++device->input.sequence;
        expect(count(listener.poll()) == 0, "失信输入不得补发道具事件");
        device->input.status = InputMonitorStatus::READY; ++device->input.sequence;
        expect(count(listener.poll()) == 0, "故障恢复时按住道具键必须先释放");
        device->input.virtual_keys[key] = false; ++device->input.sequence; listener.poll();
        device->input.virtual_keys[key] = true; ++device->input.sequence;
        expect(count(listener.poll()) == 1, "恢复后释放重按道具键才启动新动作");
        listener.close(); expect(listener.open(), "道具监听可重建");
        expect(count(listener.poll()) == 0, "监听重建不得重放当前按住的道具键");
        config.grenade_assist_enabled = false;
        KeyboardListener disabled(config, device); expect(disabled.open(), "禁用道具保留合法草稿");
        device->input.virtual_keys[key] = false; ++device->input.sequence; disabled.poll();
        device->input.virtual_keys[key] = true; ++device->input.sequence;
        expect(count(disabled.poll()) == 0, "辅助开关关闭时新按下也不发道具事件");
    }
    // 即使内存仍含旧菜单配置，右键也不能重新产生已废弃的语义事件。
    for (const int key : {0x27, 0x66}) {
        auto device = std::make_shared<Device>();
        KeyboardConfig config;
        config.grenade_assist_enabled = true;
        config.grenade_menu_virtual_keys = {key};
        KeyboardListener listener(config, device);
        expect(listener.open(), "旧菜单绑定不能阻止三键监听启动");
        device->input.status = InputMonitorStatus::READY;
        device->input.state_valid = true; device->input.sequence = 1;
        listener.poll();
        device->input.virtual_keys[key] = true; ++device->input.sequence;
        const auto polled = listener.poll();
        expect(std::none_of(polled.events.begin(), polled.events.end(), [](const auto& event) {
            return event.type == KeyboardEventType::GRENADE_MENU;
        }), "方向区右键和数字区6均不得呼出旧主机清单");
    }
}
void numeric_keypad_mapping() {
    std::array<bool, 256> keys{};
    const std::array<std::uint8_t, 8> usages{0x4f, 0x52, 0x51, 0x50, 0x5e, 0x60, 0x5a, 0x5c};
    mouse::detail::apply_hid_keyboard_report(0, usages.data(), usages.size(), keys);
    for (const int key : {0x27, 0x26, 0x28, 0x25, 0x66, 0x68, 0x62, 0x64})
        expect(keys[key], "方向区与数字区四向必须独立映射且可以同时捕获");
    const std::array<std::uint8_t, 2> numlock_and_up{0x53, 0x60};
    mouse::detail::apply_hid_keyboard_report(0, numlock_and_up.data(), numlock_and_up.size(), keys);
    expect(keys[0x68] && !keys[0x26], "NumLock键出现不把物理数字区8别名为方向区上键");
    const std::uint8_t up = 0x60;
    mouse::detail::apply_hid_keyboard_report(0, &up, 1, keys);
    expect(keys[0x68] && !keys[0x26], "NumLock释放后数字区8仍保持独立物理键身份");
}
}
int main() {
    grenade_keys();
    numeric_keypad_mapping();
    {
        auto device = std::make_shared<Device>();
        KeyboardConfig config;
        KeyboardListener listener(config, device);
        expect(listener.open(), "异常标记监听可打开");
        const auto has_mark = [](const KeyboardPollResult& poll) {
            for (const auto& event : poll.events) if (event.type == KeyboardEventType::ANOMALY_MARK) return true;
            return false;
        };
        device->input.status = InputMonitorStatus::READY;
        device->input.state_valid = true;
        device->input.sequence = 1;
        device->input.virtual_keys[0x78] = true;
        expect(!has_mark(listener.poll()), "初次发现已按住F9不得补标记");
        device->input.virtual_keys[0x78] = false; ++device->input.sequence;
        listener.poll();
        device->input.virtual_keys[0x78] = true; ++device->input.sequence;
        expect(has_mark(listener.poll()), "源设备F9新边沿必须生成异常标记");
        ++device->input.sequence;
        expect(!has_mark(listener.poll()), "持续按住F9不得重复标记");
        device->input.status = InputMonitorStatus::STALE; ++device->input.sequence;
        listener.poll();
        device->input.status = InputMonitorStatus::READY; ++device->input.sequence;
        expect(!has_mark(listener.poll()), "失信恢复按住F9不得伪造标记边沿");
    }
    device_key(5); device_key(6);
    for (const int key : {0x66, 0x68, 0x62, 0x64}) device_key(key);
    return failures ? 1 : 0;
}
