#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <array>
#include <cstddef>
#include <memory>
#include <vector>

#include "mouse/mouse.h"

enum class KeyboardStatus {
    CLOSED,
    READY,
    FAILURE,
};

enum class KeyboardEventType {
    AIM_HOLD_CHANGED,
    EMERGENCY_STOP,
    RUNTIME_TOGGLE,
};

struct KeyboardConfig {
    std::vector<int> aim_hold_virtual_keys{0x02};  // VK_RBUTTON
    std::vector<int> emergency_virtual_keys{0x23}; // VK_END
    std::vector<int> runtime_toggle_virtual_keys{0x77}; // VK_F8
};

// 空集合表示该功能未绑定；集合内任意键均可生效。所有已绑定的全局语义
// 必须互斥，避免一次按键同时改变运行生命周期和物理输出安全状态。
inline bool valid_keyboard_config(const KeyboardConfig& config) noexcept {
    std::array<bool, 256> assigned{};
    const auto claim = [&](const std::vector<int>& virtual_keys) {
        for (const int virtual_key : virtual_keys) {
            if (virtual_key < 1 || virtual_key > 0xFF ||
                assigned[static_cast<std::size_t>(virtual_key)]) {
                return false;
            }
            assigned[static_cast<std::size_t>(virtual_key)] = true;
        }
        return true;
    };
    return claim(config.aim_hold_virtual_keys) &&
           claim(config.emergency_virtual_keys) &&
           claim(config.runtime_toggle_virtual_keys);
}

struct KeyboardEvent {
    KeyboardEventType type = KeyboardEventType::AIM_HOLD_CHANGED;
    bool active = false;
};

class KeyboardListener {
public:
    explicit KeyboardListener(const KeyboardConfig& config);
    KeyboardListener(const KeyboardConfig& config,
                     std::shared_ptr<IMouseController> device);
    ~KeyboardListener();

    KeyboardListener(const KeyboardListener&) = delete;
    KeyboardListener& operator=(const KeyboardListener&) = delete;
    KeyboardListener(KeyboardListener&&) noexcept;
    KeyboardListener& operator=(KeyboardListener&&) noexcept;

    bool open() noexcept;
    std::vector<KeyboardEvent> poll() noexcept;
    void close() noexcept;
    KeyboardStatus status() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif // KEYBOARD_H
