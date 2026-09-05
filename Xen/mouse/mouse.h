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
    OWNER_CONFLICT,
};

enum class InputMonitorStatus {
    CLOSED,
    // 输入 adapter 存在，但当前 API 无法证明完整键态快照是否有效。
    UNVERIFIED,
    WAITING,
    READY,
    STALE,
    FAILURE,
};

struct InputSnapshot {
    InputMonitorStatus status = InputMonitorStatus::CLOSED;
    // 仅当键态来自可排序的真实输入事实时为 true；链路状态本身不能
    // 推导按键释放。同一设备代际内 sequence 必须随新事实严格递增。
    bool state_valid = false;
    std::array<bool, 256> virtual_keys{};
    std::uint64_t sequence = 0;
};

const char* MouseStatusName(MouseStatus status) noexcept;

enum class MouseOutputOwnerScope {
    PRODUCTION,
    CURRENT_PROCESS_TEST,
};

// 跨进程独占 Mouse 输出所有权。生产 scope 使用当前用户 LocalAppData 固定
// 文件共享锁，并保留当前临时目录的旧版锁；旧版不同 TMP 的进程不参与新仲裁。
// 进程异常退出时 Windows 自动释放句柄，不发送任何清理命令。
class MouseOutputOwnerLease {
public:
    MouseOutputOwnerLease() noexcept;
    ~MouseOutputOwnerLease();

    MouseOutputOwnerLease(const MouseOutputOwnerLease&) = delete;
    MouseOutputOwnerLease& operator=(const MouseOutputOwnerLease&) = delete;

    bool acquire(MouseOutputOwnerScope scope,
                 const std::string& owner,
                 std::string& error) noexcept;
    void release() noexcept;
    bool held() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

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
    // 仅 MouseDeviceFactory 的跨进程 lease adapter 在成功 open 后返回 true；
    // 测试 fake 默认 false，不能用调用方布尔声明冒充生产独占事实。
    virtual bool output_owner_exclusive() const noexcept { return false; }
    virtual void close() noexcept = 0;
    virtual MouseStatus status() const noexcept = 0;
    virtual std::string last_error() const = 0;

protected:
    IMouseController() = default;
};

class MouseDeviceFactory {
public:
    static std::unique_ptr<IMouseController> create(
        const MouseConfig& config,
        MouseOutputOwnerScope owner_scope =
            MouseOutputOwnerScope::PRODUCTION) noexcept;

private:
    MouseDeviceFactory() = delete;
};

#endif // MOUSE_H
