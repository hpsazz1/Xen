#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <memory>
#include <vector>

enum class KeyboardStatus {
    CLOSED,
    READY,
    FAILURE,
};

enum class KeyboardEventType {
    AIM_HOLD_CHANGED,
    EMERGENCY_STOP,
};

struct KeyboardConfig {
    int aim_hold_virtual_key = 0x02;  // VK_RBUTTON
    int emergency_virtual_key = 0x23; // VK_END
};

struct KeyboardEvent {
    KeyboardEventType type = KeyboardEventType::AIM_HOLD_CHANGED;
    bool active = false;
};

class KeyboardListener {
public:
    explicit KeyboardListener(const KeyboardConfig& config);
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
