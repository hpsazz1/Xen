#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "keyboard/keyboard.h"

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
    bool aim_hold_active = false;
    bool emergency_active = false;
};

KeyboardListener::KeyboardListener(const KeyboardConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

KeyboardListener::~KeyboardListener() = default;

KeyboardListener::KeyboardListener(KeyboardListener&& other) noexcept
    = default;

KeyboardListener& KeyboardListener::operator=(KeyboardListener&& other) noexcept = default;

bool KeyboardListener::open() noexcept {
    if (!impl_ || impl_->config.aim_hold_virtual_key <= 0 ||
        impl_->config.emergency_virtual_key <= 0) {
        if (impl_) impl_->status = KeyboardStatus::FAILURE;
        return false;
    }
    Log::register_module("keyboard", LogLevel::INFO);
    impl_->aim_hold_active = false;
    impl_->emergency_active = false;
    impl_->status = KeyboardStatus::READY;
    LOG_INFO("keyboard", "全局按键轮询已启用: hold_vk={}, emergency_vk={}",
             impl_->config.aim_hold_virtual_key,
             impl_->config.emergency_virtual_key);
    return true;
}

std::vector<KeyboardEvent> KeyboardListener::poll() noexcept {
    std::vector<KeyboardEvent> events;
    if (!impl_ || impl_->status != KeyboardStatus::READY) return events;
    try {
        const bool hold_active =
            (GetAsyncKeyState(impl_->config.aim_hold_virtual_key) & 0x8000) != 0;
        if (hold_active != impl_->aim_hold_active) {
            impl_->aim_hold_active = hold_active;
            events.push_back(
                {KeyboardEventType::AIM_HOLD_CHANGED, hold_active});
        }

        const bool emergency_active =
            (GetAsyncKeyState(impl_->config.emergency_virtual_key) & 0x8000) != 0;
        if (emergency_active && !impl_->emergency_active) {
            events.push_back({KeyboardEventType::EMERGENCY_STOP, true});
        }
        impl_->emergency_active = emergency_active;
    } catch (...) {
        impl_->status = KeyboardStatus::FAILURE;
        events.clear();
    }
    return events;
}

void KeyboardListener::close() noexcept {
    if (!impl_) return;
    impl_->aim_hold_active = false;
    impl_->emergency_active = false;
    impl_->status = KeyboardStatus::CLOSED;
}

KeyboardStatus KeyboardListener::status() const noexcept {
    return impl_ ? impl_->status : KeyboardStatus::FAILURE;
}
