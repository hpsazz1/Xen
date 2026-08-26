#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "mouse/mouse.h"

#include "mouse/kmbox_net_internal.h"
#include "mouse/makcu_internal.h"

#include "log/log.h"

#include <Windows.h>

#ifdef ERROR
#undef ERROR
#endif

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace {

class Win32MouseController final : public IMouseController {
public:
    explicit Win32MouseController(const MouseConfig& config)
        : config_(config) {}

    bool open() noexcept override {
        Log::register_module("mouse", LogLevel::INFO);
        set_error({});
        status_.store(config_.allow_send_input
                          ? MouseStatus::READY
                          : MouseStatus::DISABLED,
                      std::memory_order_release);
        LOG_INFO("mouse", "Win32 SendInput 后端已初始化，物理输出={}",
                 config_.allow_send_input ? "允许" : "禁用");
        return true;
    }

    MouseMoveReceipt move(
            const MouseMoveCommand& command) noexcept override {
        if (!config_.allow_send_input) {
            set_error("Win32 SendInput 未在配置中显式允许");
            status_.store(MouseStatus::DISABLED, std::memory_order_release);
            return {};
        }
        if (command.dx_counts == 0 && command.dy_counts == 0) {
            set_error("鼠标移动命令不能同时为零");
            status_.store(MouseStatus::INVALID_COMMAND,
                          std::memory_order_release);
            return {};
        }

        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dx = command.dx_counts;
        input.mi.dy = command.dy_counts;
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        if (SendInput(1, &input, sizeof(input)) != 1) {
            set_error("SendInput 未提交鼠标移动事件，Win32Error=" +
                      std::to_string(GetLastError()));
            status_.store(MouseStatus::SEND_FAILED,
                          std::memory_order_release);
            return {};
        }
        status_.store(MouseStatus::READY, std::memory_order_release);
        MouseMoveReceipt receipt;
        receipt.succeeded = true;
        receipt.backend_completed_at = std::chrono::steady_clock::now();
        return receipt;
    }

    bool poll_input(InputSnapshot& snapshot) noexcept override {
        snapshot = {};
        if (status_.load(std::memory_order_acquire) == MouseStatus::CLOSED) {
            snapshot.status = InputMonitorStatus::CLOSED;
            return true;
        }
        try {
            for (int virtual_key = 1; virtual_key <= 0xff; ++virtual_key) {
                snapshot.virtual_keys[static_cast<std::size_t>(virtual_key)] =
                    (GetAsyncKeyState(virtual_key) & 0x8000) != 0;
            }
            snapshot.status = InputMonitorStatus::READY;
            snapshot.state_valid = true;
            snapshot.sequence = ++input_sequence_;
            return true;
        } catch (...) {
            snapshot.status = InputMonitorStatus::FAILURE;
            return false;
        }
    }

    void close() noexcept override {
        status_.store(MouseStatus::CLOSED, std::memory_order_release);
    }

    MouseStatus status() const noexcept override {
        return status_.load(std::memory_order_acquire);
    }

    std::string last_error() const override {
        std::lock_guard<std::mutex> lock(error_mutex_);
        return last_error_;
    }

private:
    void set_error(const std::string& message) noexcept {
        try {
            std::lock_guard<std::mutex> lock(error_mutex_);
            last_error_ = message;
        } catch (...) {
        }
    }

    MouseConfig config_;
    std::atomic<MouseStatus> status_{MouseStatus::CLOSED};
    std::uint64_t input_sequence_ = 0;
    mutable std::mutex error_mutex_;
    std::string last_error_;
};

} // namespace

const char* MouseStatusName(MouseStatus status) noexcept {
    switch (status) {
        case MouseStatus::CLOSED: return "CLOSED";
        case MouseStatus::READY: return "READY";
        case MouseStatus::DISABLED: return "DISABLED";
        case MouseStatus::INVALID_CONFIG: return "INVALID_CONFIG";
        case MouseStatus::INVALID_COMMAND: return "INVALID_COMMAND";
        case MouseStatus::CONNECTION_FAILED: return "CONNECTION_FAILED";
        case MouseStatus::SEND_FAILED: return "SEND_FAILED";
        case MouseStatus::RESPONSE_TIMEOUT: return "RESPONSE_TIMEOUT";
        case MouseStatus::INVALID_RESPONSE: return "INVALID_RESPONSE";
    }
    return "UNKNOWN";
}

const char* MouseBackendName(MouseBackend backend) noexcept {
    switch (backend) {
        case MouseBackend::WIN32_SEND_INPUT: return "win32_send_input";
        case MouseBackend::KMBOX_NET: return "kmbox_net";
        case MouseBackend::MAKCU: return "makcu";
    }
    return "unknown";
}

std::unique_ptr<IMouseController> MouseDeviceFactory::create(
        const MouseConfig& config) noexcept {
    try {
        if (config.backend == MouseBackend::WIN32_SEND_INPUT) {
            return std::make_unique<Win32MouseController>(config);
        }
        if (config.backend == MouseBackend::KMBOX_NET) {
            return mouse::detail::create_kmbox_net_controller(config);
        }
        if (config.backend == MouseBackend::MAKCU) {
            return mouse::detail::create_makcu_controller(config);
        }
    } catch (...) {
        LOG_ERROR("mouse", "创建鼠标后端时发生未知异常");
    }
    return nullptr;
}
