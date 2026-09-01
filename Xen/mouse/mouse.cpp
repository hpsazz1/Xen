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
#include <array>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>

namespace {

void set_output_owner_error(std::string& output,
                            std::string_view value) noexcept {
    try {
        output.assign(value);
    } catch (...) {
    }
}

bool output_owner_lock_path(MouseOutputOwnerScope scope,
                            std::filesystem::path& path,
                            std::string& error) noexcept {
    try {
        std::array<wchar_t, 32768> buffer{};
        const DWORD length = GetTempPathW(
            static_cast<DWORD>(buffer.size()), buffer.data());
        if (length == 0 || length >= buffer.size()) {
            set_output_owner_error(error,
                "无法解析 Mouse owner 临时锁目录");
            return false;
        }
        path = std::filesystem::path(
            std::wstring_view(buffer.data(), length));
        if (scope == MouseOutputOwnerScope::PRODUCTION) {
            path /= L"Xen-mouse-output-owner-v1.lock";
        } else if (scope ==
                       MouseOutputOwnerScope::CURRENT_PROCESS_TEST) {
            path /= L"Xen-mouse-output-owner-test-" +
                std::to_wstring(GetCurrentProcessId()) + L".lock";
        } else {
            set_output_owner_error(error, "Mouse owner scope 非法");
            return false;
        }
        error.clear();
        return true;
    } catch (...) {
        set_output_owner_error(error,
            "解析 Mouse owner 临时锁路径时发生未知异常");
        return false;
    }
}

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
        // GetAsyncKeyState 返回 0 同时可能表示未按下或 desktop/UIPI 失败，
        // API 没有可据此验证完整 256 键快照的错误合同。在引入有明确
        // health owner 的输入源前，Win32 输出 adapter 必须保持输入未验证。
        snapshot.status = InputMonitorStatus::UNVERIFIED;
        return true;
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
    mutable std::mutex error_mutex_;
    std::string last_error_;
};

class ExclusiveMouseController final : public IMouseController {
public:
    ExclusiveMouseController(std::unique_ptr<IMouseController> inner,
                             MouseOutputOwnerScope owner_scope)
        : inner_(std::move(inner)), owner_scope_(owner_scope) {}

    ~ExclusiveMouseController() override { close(); }

    bool open() noexcept override {
        close();
        if (!inner_) {
            owner_conflict_ = true;
            conflict_error_ = "Mouse factory adapter 缺少内部后端";
            return false;
        }
        std::string lease_error;
        if (!output_owner_.acquire(
                owner_scope_, "mouse-device-factory", lease_error)) {
            owner_conflict_ = true;
            try {
                conflict_error_ = std::move(lease_error);
            } catch (...) {
            }
            return false;
        }
        owner_conflict_ = false;
        conflict_error_.clear();
        if (!inner_->open()) {
            output_owner_.release();
            return false;
        }
        return true;
    }

    MouseMoveReceipt move(
            const MouseMoveCommand& command) noexcept override {
        if (!inner_ || !output_owner_.held()) return {};
        return inner_->move(command);
    }

    bool poll_input(InputSnapshot& snapshot) noexcept override {
        if (!inner_ || !output_owner_.held()) {
            snapshot = {};
            snapshot.status = owner_conflict_
                ? InputMonitorStatus::FAILURE
                : InputMonitorStatus::CLOSED;
            return true;
        }
        return inner_->poll_input(snapshot);
    }

    bool output_owner_exclusive() const noexcept override {
        return output_owner_.held();
    }

    void close() noexcept override {
        if (inner_) inner_->close();
        output_owner_.release();
        owner_conflict_ = false;
        conflict_error_.clear();
    }

    MouseStatus status() const noexcept override {
        if (owner_conflict_) return MouseStatus::OWNER_CONFLICT;
        return inner_ ? inner_->status() : MouseStatus::INVALID_CONFIG;
    }

    std::string last_error() const override {
        if (owner_conflict_) return conflict_error_;
        return inner_ ? inner_->last_error()
                      : "Mouse factory adapter 缺少内部后端";
    }

private:
    std::unique_ptr<IMouseController> inner_;
    MouseOutputOwnerScope owner_scope_ = MouseOutputOwnerScope::PRODUCTION;
    MouseOutputOwnerLease output_owner_;
    bool owner_conflict_ = false;
    std::string conflict_error_;
};

} // namespace

class MouseOutputOwnerLease::Impl {
public:
    ~Impl() {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }

    HANDLE handle = INVALID_HANDLE_VALUE;
    std::filesystem::path path;
    std::string owner;
};

MouseOutputOwnerLease::MouseOutputOwnerLease() noexcept
    : impl_(new (std::nothrow) Impl) {}

MouseOutputOwnerLease::~MouseOutputOwnerLease() = default;

bool MouseOutputOwnerLease::acquire(
        MouseOutputOwnerScope scope,
        const std::string& owner,
        std::string& error) noexcept {
    if (!impl_) {
        set_output_owner_error(error, "无法分配 Mouse owner lease 状态");
        return false;
    }
    if (impl_->handle != INVALID_HANDLE_VALUE) {
        set_output_owner_error(error, "当前对象已持有 Mouse owner lease");
        return false;
    }
    if (owner.empty() || owner.size() > 128U ||
        owner.find_first_of("\r\n") != std::string::npos) {
        set_output_owner_error(error, "Mouse owner 名称非法");
        return false;
    }
    std::filesystem::path lock_path;
    if (!output_owner_lock_path(scope, lock_path, error)) return false;
    const HANDLE handle = CreateFileW(
        lock_path.c_str(), GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        set_output_owner_error(error,
            "Mouse 输出已由另一 owner 独占或锁文件不可用，Win32Error=" +
            std::to_string(code));
        return false;
    }
    impl_->handle = handle;
    impl_->path = std::move(lock_path);
    try {
        impl_->owner = owner;
    } catch (...) {
        CloseHandle(impl_->handle);
        impl_->handle = INVALID_HANDLE_VALUE;
        impl_->path.clear();
        set_output_owner_error(error, "记录 Mouse owner 名称失败");
        return false;
    }
    error.clear();
    return true;
}

void MouseOutputOwnerLease::release() noexcept {
    if (!impl_ || impl_->handle == INVALID_HANDLE_VALUE) return;
    CloseHandle(impl_->handle);
    impl_->handle = INVALID_HANDLE_VALUE;
    impl_->path.clear();
    impl_->owner.clear();
}

bool MouseOutputOwnerLease::held() const noexcept {
    return impl_ && impl_->handle != INVALID_HANDLE_VALUE;
}

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
        case MouseStatus::OWNER_CONFLICT: return "OWNER_CONFLICT";
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
        const MouseConfig& config,
        MouseOutputOwnerScope owner_scope) noexcept {
    try {
        std::unique_ptr<IMouseController> inner;
        if (config.backend == MouseBackend::WIN32_SEND_INPUT) {
            inner = std::make_unique<Win32MouseController>(config);
        } else if (config.backend == MouseBackend::KMBOX_NET) {
            inner = mouse::detail::create_kmbox_net_controller(config);
        } else if (config.backend == MouseBackend::MAKCU) {
            inner = mouse::detail::create_makcu_controller(config);
        }
        if (!inner) return nullptr;
        return std::make_unique<ExclusiveMouseController>(
            std::move(inner), owner_scope);
    } catch (...) {
        LOG_ERROR("mouse", "创建鼠标后端时发生未知异常");
    }
    return nullptr;
}
