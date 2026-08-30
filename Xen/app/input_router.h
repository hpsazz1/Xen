#ifndef APP_INPUT_ROUTER_H
#define APP_INPUT_ROUTER_H

#include "keyboard/keyboard.h"
#include "runtime/runtime.h"

namespace app::detail {

struct KeyboardRoutingResult {
    bool emergency_pressed = false;
    bool runtime_toggle_pressed = false;
};

// 输入健康是 SafetyGate 的独立事实，必须先于 Overlay 渲染投递，且不受
// 热键绑定捕获影响。
bool route_input_health(
    Runtime& runtime, const KeyboardPollResult& poll) noexcept;

// 只有语义按键事件受 Overlay 捕获抑制；此函数不重复投递 health。
KeyboardRoutingResult route_keyboard_events(
    Runtime& runtime,
    const KeyboardPollResult& poll,
    bool hotkey_capture_consumed) noexcept;

} // namespace app::detail

#endif // APP_INPUT_ROUTER_H
