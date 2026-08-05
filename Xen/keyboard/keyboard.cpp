#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "keyboard/keyboard.h"
#include "keyboard/keyboard_internal.h"

#include "log/log.h"

#include <Windows.h>

#ifdef ERROR
#undef ERROR
#endif

#include <memory>
#include <utility>

struct KeyboardListener::Impl {
    explicit Impl(const KeyboardConfig& value,
                  std::shared_ptr<IMouseController> device_value = {})
        : config(value), device(std::move(device_value)) {}

    KeyboardConfig config;
    std::shared_ptr<IMouseController> device;
    KeyboardStatus status = KeyboardStatus::CLOSED;
    keyboard::detail::KeyboardEventState event_state;
};

KeyboardListener::KeyboardListener(const KeyboardConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

KeyboardListener::KeyboardListener(
        const KeyboardConfig& config,
        std::shared_ptr<IMouseController> device)
    : impl_(std::make_unique<Impl>(config, std::move(device))) {}

KeyboardListener::~KeyboardListener() = default;

KeyboardListener::KeyboardListener(KeyboardListener&& other) noexcept
    = default;

KeyboardListener& KeyboardListener::operator=(KeyboardListener&& other) noexcept = default;

bool KeyboardListener::open() noexcept {
    if (!impl_ || !valid_keyboard_config(impl_->config)) {
        if (impl_) impl_->status = KeyboardStatus::FAILURE;
        return false;
    }
    Log::register_module("keyboard", LogLevel::INFO);
    impl_->event_state = {};
    impl_->status = KeyboardStatus::READY;
    LOG_INFO("keyboard", "全局按键轮询已启用: hold_keys={}, emergency_keys={}, toggle_keys={}",
             impl_->config.aim_hold_virtual_keys.size(),
             impl_->config.emergency_virtual_keys.size(),
             impl_->config.runtime_toggle_virtual_keys.size());
    return true;
}

std::vector<KeyboardEvent> KeyboardListener::poll() noexcept {
    std::vector<KeyboardEvent> events;
    if (!impl_ || impl_->status != KeyboardStatus::READY) return events;
    try {
        std::array<bool, 256> key_active{};
        if (impl_->device) {
            InputSnapshot snapshot;
            if (!impl_->device->poll_input(snapshot)) {
                impl_->status = KeyboardStatus::FAILURE;
                return events;
            }
            // 只有来自真实输入报告的快照才能推进按键状态。status 只描述链路，
            // 等待、陈旧、故障或关闭都不能被猜测成“所有键已释放”。
            if (!snapshot.state_valid) return events;
            key_active = snapshot.virtual_keys;
        } else {
            const auto poll_binding = [&](const std::vector<int>& virtual_keys) {
                for (const int virtual_key : virtual_keys) {
                    key_active[static_cast<std::size_t>(virtual_key)] =
                        (GetAsyncKeyState(virtual_key) & 0x8000) != 0;
                }
            };
            poll_binding(impl_->config.aim_hold_virtual_keys);
            poll_binding(impl_->config.emergency_virtual_keys);
            poll_binding(impl_->config.runtime_toggle_virtual_keys);
        }
        const auto polled = keyboard::detail::update_keyboard_events(
            impl_->event_state, impl_->config, key_active);
        events.reserve(polled.count);
        for (std::size_t index = 0; index < polled.count; ++index) {
            events.push_back(polled.events[index]);
        }
    } catch (...) {
        impl_->status = KeyboardStatus::FAILURE;
        events.clear();
    }
    return events;
}

void KeyboardListener::close() noexcept {
    if (!impl_) return;
    impl_->event_state = {};
    impl_->status = KeyboardStatus::CLOSED;
}

KeyboardStatus KeyboardListener::status() const noexcept {
    return impl_ ? impl_->status : KeyboardStatus::FAILURE;
}
