#ifndef MOUSE_H
#define MOUSE_H

#include <memory>
#include <string>

enum class MouseBackend {
    WIN32_SEND_INPUT,
};

enum class MouseStatus {
    CLOSED,
    READY,
    DISABLED,
    INVALID_COMMAND,
    SEND_FAILED,
};

const char* MouseStatusName(MouseStatus status) noexcept;

struct MouseConfig {
    MouseBackend backend = MouseBackend::WIN32_SEND_INPUT;
    // 双重安全门：即使 Runtime 误发命令，未显式允许时也不能注入输入。
    bool allow_send_input = false;
};

struct MouseMoveCommand {
    int dx_counts = 0;
    int dy_counts = 0;
};

class IMouseController {
public:
    virtual ~IMouseController() = default;

    IMouseController(const IMouseController&) = delete;
    IMouseController& operator=(const IMouseController&) = delete;

    virtual bool open() noexcept = 0;
    virtual bool move(const MouseMoveCommand& command) noexcept = 0;
    virtual void close() noexcept = 0;
    virtual MouseStatus status() const noexcept = 0;
    virtual std::string last_error() const = 0;

protected:
    IMouseController() = default;
};

class MouseDeviceFactory {
public:
    static std::unique_ptr<IMouseController> create(
        const MouseConfig& config) noexcept;

private:
    MouseDeviceFactory() = delete;
};

#endif // MOUSE_H
