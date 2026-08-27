#ifndef MOUSE_H
#define MOUSE_H

#include <chrono>
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
    // 仅当键态来自真实输入报告时为 true；链路状态本身不能推导按键释放。
    bool state_valid = false;
    std::array<bool, 256> virtual_keys{};
    std::uint64_t sequence = 0;
};

const char* MouseStatusName(MouseStatus status) noexcept;

struct MouseConfig {
    MouseBackend backend = MouseBackend::WIN32_SEND_INPUT;
    // 双重安全门：即使 Runtime 误发命令，未显式允许时也不能注入输入。
    bool allow_send_input = false;
    // 仅允许人工诊断进入与 Physical 相同的锁定/按键控制状态；最终 Mouse
    // 派发仍必须由 allow_send_input 单独授权，两者不得同时启用。
    bool allow_observe_only_control = false;
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

struct MouseMoveReceipt {
    bool succeeded = false;
    // 后端同步调用返回前的本机完成时刻。Win32 只证明事件已插入输入流；
    // KMBOX/MAKCU 则还可能带独立 ACK，但都不能据此推导物理移动已出现。
    std::chrono::steady_clock::time_point backend_completed_at{};
    // 只表示后端收到了可与当前命令匹配的协议响应；不推导设备已执行，
    // 更不推导物理移动已在屏幕或传感器上出现。
    bool protocol_ack_received = false;
    std::chrono::steady_clock::time_point protocol_ack_received_at{};
    bool physical_effect_observed = false;
    std::chrono::steady_clock::time_point physical_effect_at{};

    operator bool() const noexcept { return succeeded; }
};

class IMouseController {
public:
    virtual ~IMouseController() = default;

    IMouseController(const IMouseController&) = delete;
    IMouseController& operator=(const IMouseController&) = delete;

    virtual bool open() noexcept = 0;
    virtual MouseMoveReceipt move(
        const MouseMoveCommand& command) noexcept = 0;
    // 非阻塞取得与当前鼠标输出后端绑定的物理键鼠状态；只有 state_valid
    // 快照可改变键态，status 只描述链路，不得被调用方猜测为全释放。
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
