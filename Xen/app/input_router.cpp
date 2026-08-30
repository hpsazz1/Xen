#include "app/input_router.h"

namespace app::detail {

bool route_input_health(
        Runtime& runtime, const KeyboardPollResult& poll) noexcept {
    return runtime.post_intent({
        RuntimeIntentType::INPUT_HEALTH_CHANGED, poll.input_healthy});
}

KeyboardRoutingResult route_keyboard_events(
        Runtime& runtime,
        const KeyboardPollResult& poll,
        bool hotkey_capture_consumed) noexcept {
    KeyboardRoutingResult result;
    if (hotkey_capture_consumed) return result;

    for (const auto& event : poll.events) {
        if (event.type == KeyboardEventType::AIM_HOLD_CHANGED) {
            runtime.post_intent({
                RuntimeIntentType::AIM_HOLD_CHANGED, event.active});
        } else if (event.type == KeyboardEventType::EMERGENCY_STOP) {
            result.emergency_pressed = true;
            runtime.post_intent({RuntimeIntentType::EMERGENCY_STOP, true});
        } else if (event.type == KeyboardEventType::RUNTIME_TOGGLE) {
            result.runtime_toggle_pressed = true;
        }
    }
    return result;
}

} // namespace app::detail
