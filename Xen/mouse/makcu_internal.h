#ifndef MOUSE_MAKCU_INTERNAL_H
#define MOUSE_MAKCU_INTERNAL_H

#include "mouse/mouse.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace mouse::detail {

enum class MakcuIoResult {
    SUCCESS,
    TIMEOUT,
    FAILED,
};

// 该接口仅用于把串口系统调用与协议状态机隔离；生产工厂始终注入 Win32 串口实现，
// 单元测试注入内存替身以覆盖 ACK、超时和安全门，不向真实 HID 设备发送输入。
class IMakcuTransport {
public:
    virtual ~IMakcuTransport() = default;

    IMakcuTransport(const IMakcuTransport&) = delete;
    IMakcuTransport& operator=(const IMakcuTransport&) = delete;

    virtual MakcuIoResult open(std::string_view port,
                               std::uint32_t baud_rate,
                               std::string& error) noexcept = 0;
    virtual MakcuIoResult write_exact(
        std::span<const std::uint8_t> bytes,
        int timeout_ms,
        std::string& error) noexcept = 0;
    virtual MakcuIoResult read_exact(
        std::span<std::uint8_t> bytes,
        int timeout_ms,
        std::string& error) noexcept = 0;
    virtual void close() noexcept = 0;

protected:
    IMakcuTransport() = default;
};

std::unique_ptr<IMouseController> create_makcu_controller(
    const MouseConfig& config) noexcept;

std::unique_ptr<IMouseController> create_makcu_controller_for_test(
    const MouseConfig& config,
    std::unique_ptr<IMakcuTransport> transport) noexcept;

} // namespace mouse::detail

#endif // MOUSE_MAKCU_INTERNAL_H
