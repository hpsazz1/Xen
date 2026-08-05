#ifndef MOUSE_H
#define MOUSE_H

#include <memory>
#include <array>
#include <cstdint>
#include <string>

enum class MouseBackend {
    WIN32_SEND_INPUT,
    KMBOX_NET,
    MAKCU,
};

const char* MouseBackendName(MouseBackend backend) noexcept;

enum class MouseStatus {
    CLOSED,
    READY,
    DISABLED,
    INVALID_CONFIG,
    INVALID_COMMAND,
    CONNECTION_FAILED,
    SEND_FAILED,
    RESPONSE_TIMEOUT,
    INVALID_RESPONSE,
};

enum class InputMonitorStatus {
    CLOSED,
    WAITING,
    READY,
    STALE,
    FAILURE,
};

struct InputSnapshot {
    InputMonitorStatus status = InputMonitorStatus::CLOSED;
    std::array<bool, 256> virtual_keys{};
    std::uint64_t sequence = 0;
};

const char* MouseStatusName(MouseStatus status) noexcept;

struct MouseConfig {
    MouseBackend backend = MouseBackend::WIN32_SEND_INPUT;
    // 双重安全门：即使 Runtime 误发命令，未显式允许时也不能注入输入。
    bool allow_send_input = false;
    // KMBOX NET 凭据不提供可误用的设备默认值，选择该后端时必须显式填写。
    std::string kmbox_ip;
    int kmbox_port = 0;
    std::string kmbox_uuid;
    // 连接允许设备完成握手；热路径命令超时必须保持较短，避免长期阻塞 Pipeline。
    int kmbox_connect_timeout_ms = 1000;
    int kmbox_command_timeout_ms = 300;
    // MAKCU 只接受显式 COM 口；完整物理键鼠 streaming 固定使用 4 Mbps。
    std::string makcu_port;
    int makcu_baud_rate = 4000000;
    // 连接超时覆盖串口打开后的协议握手，命令超时覆盖每条 move 的写入与 ACK。
    int makcu_connect_timeout_ms = 1000;
    int makcu_command_timeout_ms = 300;
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
    // 非阻塞取得与当前鼠标输出后端绑定的物理键鼠状态；失败或陈旧时必须全释放。
    virtual bool poll_input(InputSnapshot& snapshot) noexcept = 0;
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
