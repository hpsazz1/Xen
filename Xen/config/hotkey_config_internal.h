#ifndef HOTKEY_CONFIG_INTERNAL_H
#define HOTKEY_CONFIG_INTERNAL_H

#include "config/config.h"

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <utility>

namespace config::detail {

enum class HotkeyTarget {
    NONE, RUNTIME_TOGGLE, AIM_HOLD, EMERGENCY, AUTO_STOP, AUTO_STOP_RELEASE,
    TRIGGER, DEBUG_TEST, ANOMALY_MARK, MOVEMENT_SPIN, MOVEMENT_LARGE, VOICE,
    GRENADE_MENU, GRENADE_ALIGN, GRENADE_THROW, GRENADE_CANCEL
};

struct HotkeyBindingView {
    HotkeyTarget target;
    std::span<const int> keys;
    bool enabled;
};

// 配置保存、界面编辑和反向捕获共用同一占用表；禁用功能只保留草稿。
inline auto hotkey_bindings(const AppConfig& config) noexcept {
    using Target = HotkeyTarget;
    const auto& keyboard = config.keyboard;
    const auto& move = config.movement;
    return std::array<HotkeyBindingView, 15>{{
        {Target::VOICE, keyboard.voice_virtual_keys, true},
        {Target::GRENADE_MENU, keyboard.grenade_menu_virtual_keys, keyboard.grenade_assist_enabled},
        {Target::GRENADE_ALIGN, keyboard.grenade_align_virtual_keys, keyboard.grenade_assist_enabled},
        {Target::GRENADE_THROW, keyboard.grenade_throw_virtual_keys, keyboard.grenade_assist_enabled},
        {Target::GRENADE_CANCEL, keyboard.grenade_cancel_virtual_keys, keyboard.grenade_assist_enabled},
        {Target::RUNTIME_TOGGLE, keyboard.runtime_toggle_virtual_keys, true},
        {Target::AIM_HOLD, keyboard.aim_hold_virtual_keys, true},
        {Target::EMERGENCY, keyboard.emergency_virtual_keys, true},
        {Target::DEBUG_TEST, keyboard.debug_test_virtual_keys, keyboard.debug_test_enabled},
        {Target::ANOMALY_MARK, keyboard.anomaly_mark_virtual_keys, true},
        {Target::AUTO_STOP, {&config.auto_stop.activation_virtual_key, 1}, config.auto_stop.enabled},
        {Target::AUTO_STOP_RELEASE, config.auto_stop.release_virtual_keys, config.auto_stop.enabled},
        {Target::TRIGGER, {&config.trigger.hold_virtual_key, 1}, config.trigger.enabled},
        {Target::MOVEMENT_SPIN, {&move.spin_virtual_key, 1},
            move.enabled && move.spin_enabled && move.spin_trigger == movement::Trigger::KEY},
        {Target::MOVEMENT_LARGE, {&move.large_virtual_key, 1},
            move.enabled && move.large_enabled && move.large_trigger == movement::Trigger::KEY}
    }};
}

inline bool hotkey_targets_can_share(HotkeyTarget left, HotkeyTarget right) noexcept {
    using Target = HotkeyTarget;
    const auto hold_permission = [](Target target) {
        return target == Target::AIM_HOLD || target == Target::AUTO_STOP || target == Target::TRIGGER;
    };
    if (left == right || (hold_permission(left) && hold_permission(right))) return true;
    // 急停释放保留已有共享语义；不得兼作本模块允许键、调试、标记或身法触发。
    if (right == Target::AUTO_STOP_RELEASE) std::swap(left, right);
    return left == Target::AUTO_STOP_RELEASE &&
        (right == Target::RUNTIME_TOGGLE || right == Target::EMERGENCY ||
         right == Target::AIM_HOLD || right == Target::TRIGGER);
}

inline bool hotkey_binding_conflicts(
        const AppConfig& config, HotkeyTarget target, int key) noexcept {
    using Target = HotkeyTarget;
    if (key == 0) return false;
    const bool wasd = key == 'W' || key == 'A' || key == 'S' || key == 'D';
    if (key < 0 || key > 255 ||
        ((target == Target::DEBUG_TEST || target == Target::ANOMALY_MARK) && (key == 1 || wasd)) ||
        ((target == Target::AUTO_STOP || target == Target::AUTO_STOP_RELEASE) && wasd) ||
        (target == Target::TRIGGER && (key == 1 || key == 0x23 || key == 0x77 || wasd))) return true;

    const auto bindings = hotkey_bindings(config);
    const auto current = std::find_if(bindings.begin(), bindings.end(),
        [target](const auto& binding) { return binding.target == target; });
    if (current == bindings.end() || !current->enabled) return false;
    return std::any_of(bindings.begin(), bindings.end(), [&](const auto& binding) {
        return binding.enabled && !hotkey_targets_can_share(target, binding.target) &&
            std::find(binding.keys.begin(), binding.keys.end(), key) != binding.keys.end();
    });
}

inline bool hotkey_config_conflicts(const AppConfig& config) noexcept {
    for (const auto& binding : hotkey_bindings(config)) {
        if (!binding.enabled) continue;
        for (const int key : binding.keys) {
            if (hotkey_binding_conflicts(config, binding.target, key)) return true;
        }
    }
    return false;
}

inline bool movement_hotkeys_conflict(const AppConfig& config) noexcept {
    return hotkey_binding_conflicts(config, HotkeyTarget::MOVEMENT_SPIN, config.movement.spin_virtual_key) ||
        hotkey_binding_conflicts(config, HotkeyTarget::MOVEMENT_LARGE, config.movement.large_virtual_key);
}

// 默认布局是用户显式选择的事务；右键转为清单，只移除旧语音中的同键绑定，保留其他可选语音键。
// 先验证整组再写回，不覆盖其他功能，不依赖麦克风可用性。
// 普通开关传 false，仅恢复已保存的自定义布局，数字区捕获不会被再次改回方向区。
inline bool enable_grenade_assist_hotkeys(
        AppConfig& config, bool use_direction_defaults, std::string& error) {
    auto candidate = config;
    candidate.keyboard.grenade_assist_enabled = true;
    if (use_direction_defaults) {
        std::erase(candidate.keyboard.voice_virtual_keys, 0x27);
        candidate.keyboard.grenade_menu_virtual_keys = {0x27};
        candidate.keyboard.grenade_align_virtual_keys = {0x26};
        candidate.keyboard.grenade_throw_virtual_keys = {0x28};
        candidate.keyboard.grenade_cancel_virtual_keys = {0x25};
    }
    if (!valid_keyboard_config(candidate.keyboard) || hotkey_config_conflicts(candidate)) {
        error = "道具辅助按键与已有功能冲突或键值非法；未改动任何绑定，请先调整冲突键。";
        return false;
    }
    config.keyboard = std::move(candidate.keyboard);
    error.clear();
    return true;
}

} // namespace config::detail

#endif // HOTKEY_CONFIG_INTERNAL_H
