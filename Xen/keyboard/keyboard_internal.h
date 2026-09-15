#ifndef KEYBOARD_INTERNAL_H
#define KEYBOARD_INTERNAL_H

#include "keyboard/keyboard.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace keyboard::detail {

struct KeyboardEventState {
    bool aim_hold_active = false;
    std::array<bool,256> debug_blocked_until_release{};
    std::array<bool, 256> previous_key_active{};
};

struct KeyboardEventPollResult {
    std::array<KeyboardEvent, 4> events{};
    std::size_t count = 0;
};

// 按住类对绑定集合取 OR；切换类按每个物理键的独立上升沿触发，避免一个键
// 持续按住时屏蔽同组其他绑定键。
inline KeyboardEventPollResult update_keyboard_events(
        KeyboardEventState& state,
        const KeyboardConfig& config,
        const std::array<bool, 256>& key_active) noexcept {
    const auto any_active = [&](const std::vector<int>& virtual_keys) {
        return std::any_of(
            virtual_keys.begin(), virtual_keys.end(),
            [&](int virtual_key) {
                return key_active[static_cast<std::size_t>(virtual_key)];
            });
    };
    const auto any_pressed = [&](const std::vector<int>& virtual_keys) {
        return std::any_of(
            virtual_keys.begin(), virtual_keys.end(),
            [&](int virtual_key) {
                const std::size_t index =
                    static_cast<std::size_t>(virtual_key);
                return key_active[index] &&
                       !state.previous_key_active[index];
            });
    };
    const bool aim_hold_active = any_active(config.aim_hold_virtual_keys);
    const bool emergency_pressed = any_pressed(config.emergency_virtual_keys);
    const bool runtime_toggle_pressed =
        any_pressed(config.runtime_toggle_virtual_keys);
    bool debug_pressed = false;
    for (const int key : config.debug_test_virtual_keys) {
        const auto index = static_cast<std::size_t>(key);
        if (!key_active[index]) state.debug_blocked_until_release[index] = false;
        if (config.debug_test_enabled && key_active[index] && !state.previous_key_active[index] &&
            !state.debug_blocked_until_release[index]) debug_pressed = true;
    }
    KeyboardEventPollResult result;
    if (aim_hold_active != state.aim_hold_active) {
        result.events[result.count++] = {
            KeyboardEventType::AIM_HOLD_CHANGED, aim_hold_active};
    }
    if (emergency_pressed) {
        result.events[result.count++] = {
            KeyboardEventType::EMERGENCY_STOP, true};
    }
    if (runtime_toggle_pressed) {
        result.events[result.count++] = {
            KeyboardEventType::RUNTIME_TOGGLE, true};
    }
    if (debug_pressed) result.events[result.count++] = {KeyboardEventType::DEBUG_TEST,true};
    state.aim_hold_active = aim_hold_active;
    state.previous_key_active = key_active;
    return result;
}

} // namespace keyboard::detail

#endif // KEYBOARD_INTERNAL_H
