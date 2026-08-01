#ifndef KEYBOARD_INTERNAL_H
#define KEYBOARD_INTERNAL_H

#include "keyboard/keyboard.h"

#include <array>
#include <cstddef>

namespace keyboard::detail {

struct KeyboardEventState {
    bool aim_hold_active = false;
    bool emergency_active = false;
};

struct KeyboardPollResult {
    std::array<KeyboardEvent, 2> events{};
    std::size_t count = 0;
};

// 按住启用需要同时报告按下和释放；急停只在上升沿报告，避免轮询期间重复提交。
inline KeyboardPollResult update_keyboard_events(
        KeyboardEventState& state,
        bool aim_hold_active,
        bool emergency_active) noexcept {
    KeyboardPollResult result;
    if (aim_hold_active != state.aim_hold_active) {
        result.events[result.count++] = {
            KeyboardEventType::AIM_HOLD_CHANGED, aim_hold_active};
    }
    if (emergency_active && !state.emergency_active) {
        result.events[result.count++] = {
            KeyboardEventType::EMERGENCY_STOP, true};
    }
    state.aim_hold_active = aim_hold_active;
    state.emergency_active = emergency_active;
    return result;
}

} // namespace keyboard::detail

#endif // KEYBOARD_INTERNAL_H
