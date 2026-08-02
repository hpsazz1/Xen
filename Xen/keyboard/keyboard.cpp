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
    explicit Impl(const KeyboardConfig& value) : config(value) {}

    KeyboardConfig config;
    KeyboardStatus status = KeyboardStatus::CLOSED;
    keyboard::detail::KeyboardEventState event_state;
};

KeyboardListener::KeyboardListener(const KeyboardConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

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
        const auto poll_binding = [&](const std::vector<int>& virtual_keys) {
            for (const int virtual_key : virtual_keys) {
                key_active[static_cast<std::size_t>(virtual_key)] =
                    (GetAsyncKeyState(virtual_key) & 0x8000) != 0;
            }
        };
        poll_binding(impl_->config.aim_hold_virtual_keys);
        poll_binding(impl_->config.emergency_virtual_keys);
        poll_binding(impl_->config.runtime_toggle_virtual_keys);
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
