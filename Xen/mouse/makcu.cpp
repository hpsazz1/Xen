#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>

#ifdef ERROR
#undef ERROR
#endif

#include "mouse/makcu_internal.h"
#include "mouse/input_internal.h"

#include "log/log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr std::uint8_t kFrameStart = 0x50U;
constexpr std::uint8_t kMoveCommand = 0x0dU;
constexpr std::uint8_t kBaudCommand = 0xb1U;
constexpr std::size_t kQueryFrameBytes = 4U;
constexpr std::size_t kMoveFrameBytes = 11U;
constexpr std::size_t kBaudResponseBytes = 8U;
constexpr std::size_t kSetterResponseBytes = 5U;
constexpr std::size_t kMouseStreamFrameBytes = 12U;
constexpr std::size_t kKeyboardStreamFrameBytes = 19U;
constexpr std::size_t kMaximumFrameBytes = 68U;
constexpr int kMaximumPortNumber = 256;
constexpr int kMaximumConnectTimeoutMs = 10000;
constexpr int kMaximumCommandTimeoutMs = 1000;

void write_u16_le(std::uint8_t* output, std::uint16_t value) noexcept {
    output[0] = static_cast<std::uint8_t>(value);
    output[1] = static_cast<std::uint8_t>(value >> 8U);
}

std::uint32_t read_u32_le(const std::uint8_t* input) noexcept {
    return static_cast<std::uint32_t>(input[0]) |
           (static_cast<std::uint32_t>(input[1]) << 8U) |
           (static_cast<std::uint32_t>(input[2]) << 16U) |
           (static_cast<std::uint32_t>(input[3]) << 24U);
}

bool valid_makcu_port(std::string_view port) noexcept {
    if (port.size() < 4U || port.size() > 6U ||
        std::toupper(static_cast<unsigned char>(port[0])) != 'C' ||
        std::toupper(static_cast<unsigned char>(port[1])) != 'O' ||
        std::toupper(static_cast<unsigned char>(port[2])) != 'M') {
        return false;
    }
    if (port.size() > 4U && port[3] == '0') return false;
    int value = 0;
    for (std::size_t index = 3U; index < port.size(); ++index) {
        const unsigned char character =
            static_cast<unsigned char>(port[index]);
        if (!std::isdigit(character)) return false;
        value = value * 10 + static_cast<int>(character - '0');
    }
    return value >= 1 && value <= kMaximumPortNumber;
}

std::array<std::uint8_t, kQueryFrameBytes> make_baud_query() noexcept {
    return {kFrameStart, kBaudCommand, 0U, 0U};
}

std::array<std::uint8_t, kMoveFrameBytes> make_move_frame(
        std::int16_t dx, std::int16_t dy) noexcept {
    std::array<std::uint8_t, kMoveFrameBytes> frame{
        kFrameStart, kMoveCommand, 7U, 0U};
    write_u16_le(frame.data() + 4U, static_cast<std::uint16_t>(dx));
    write_u16_le(frame.data() + 6U, static_cast<std::uint16_t>(dy));
    // segments=1 表示单段直线；两个控制点为零。后端不擅自生成轨迹或改变 Aim counts。
    frame[8] = 1U;
    frame[9] = 0U;
    frame[10] = 0U;
    return frame;
}

class Win32MakcuSerialTransport final
    : public mouse::detail::IMakcuTransport {
public:
    ~Win32MakcuSerialTransport() override {
        close();
    }

    mouse::detail::MakcuIoResult open(
            std::string_view port,
            std::uint32_t baud_rate,
            std::string& error) noexcept override {
        close();
        try {
            const std::string device_path = "\\\\.\\" + std::string(port);
            const std::wstring wide_path(device_path.begin(), device_path.end());
            handle_ = CreateFileW(
                wide_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                nullptr);
            if (handle_ == INVALID_HANDLE_VALUE) {
                set_win32_error(error, "打开 MAKCU 串口失败", GetLastError());
                return mouse::detail::MakcuIoResult::FAILED;
            }

            if (!SetupComm(handle_, 4096, 4096)) {
                set_win32_error(error, "配置 MAKCU 串口缓冲区失败",
                                GetLastError());
                close();
                return mouse::detail::MakcuIoResult::FAILED;
            }

            DCB state{};
            state.DCBlength = sizeof(state);
            if (!GetCommState(handle_, &state)) {
                set_win32_error(error, "读取 MAKCU 串口参数失败",
                                GetLastError());
                close();
                return mouse::detail::MakcuIoResult::FAILED;
            }
            state.BaudRate = baud_rate;
            state.ByteSize = 8;
            state.Parity = NOPARITY;
            state.StopBits = ONESTOPBIT;
            state.fBinary = TRUE;
            state.fParity = FALSE;
            state.fOutxCtsFlow = FALSE;
            state.fOutxDsrFlow = FALSE;
            state.fDtrControl = DTR_CONTROL_DISABLE;
            state.fDsrSensitivity = FALSE;
            state.fTXContinueOnXoff = TRUE;
            state.fOutX = FALSE;
            state.fInX = FALSE;
            state.fErrorChar = FALSE;
            state.fNull = FALSE;
            state.fRtsControl = RTS_CONTROL_DISABLE;
            state.fAbortOnError = FALSE;
            if (!SetCommState(handle_, &state)) {
                set_win32_error(error, "设置 MAKCU 串口参数失败",
                                GetLastError());
                close();
                return mouse::detail::MakcuIoResult::FAILED;
            }

            COMMTIMEOUTS timeouts{};
            if (!SetCommTimeouts(handle_, &timeouts) ||
                !PurgeComm(handle_, PURGE_RXABORT | PURGE_RXCLEAR |
                                    PURGE_TXABORT | PURGE_TXCLEAR)) {
                set_win32_error(error, "初始化 MAKCU 串口收发状态失败",
                                GetLastError());
                close();
                return mouse::detail::MakcuIoResult::FAILED;
            }

            read_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            write_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!read_event_ || !write_event_) {
                set_win32_error(error, "创建 MAKCU 串口事件失败",
                                GetLastError());
                close();
                return mouse::detail::MakcuIoResult::FAILED;
            }
            error.clear();
            return mouse::detail::MakcuIoResult::SUCCESS;
        } catch (...) {
            error = "打开 MAKCU 串口时发生未知异常";
            close();
            return mouse::detail::MakcuIoResult::FAILED;
        }
    }

    mouse::detail::MakcuIoResult write_exact(
            std::span<const std::uint8_t> bytes,
            int timeout_ms,
            std::string& error) noexcept override {
        return transfer_exact(bytes.data(), nullptr, bytes.size(), timeout_ms,
                              false, error);
    }

    mouse::detail::MakcuIoResult read_exact(
            std::span<std::uint8_t> bytes,
            int timeout_ms,
            std::string& error) noexcept override {
        return transfer_exact(nullptr, bytes.data(), bytes.size(), timeout_ms,
                              true, error);
    }

    void close() noexcept override {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CancelIoEx(handle_, nullptr);
        }
        if (read_event_) {
            CloseHandle(read_event_);
            read_event_ = nullptr;
        }
        if (write_event_) {
            CloseHandle(write_event_);
            write_event_ = nullptr;
        }
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

private:
    mouse::detail::MakcuIoResult transfer_exact(
            const std::uint8_t* input,
            std::uint8_t* output,
            std::size_t byte_count,
            int timeout_ms,
            bool reading,
            std::string& error) noexcept {
        if (handle_ == INVALID_HANDLE_VALUE || byte_count == 0U ||
            byte_count > static_cast<std::size_t>(
                std::numeric_limits<DWORD>::max())) {
            error = "MAKCU 串口未打开或传输长度非法";
            return mouse::detail::MakcuIoResult::FAILED;
        }

        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(timeout_ms);
        // 仅输入流首字节允许 0 ms：仍发起一次重叠读并立即探测，避免把
        // “非阻塞”误实现为完全不访问串口。命令与帧剩余部分继续使用正预算。
        const bool nonblocking_read = reading && timeout_ms == 0;
        std::size_t completed = 0U;
        while (completed < byte_count) {
            const auto now = std::chrono::steady_clock::now();
            if (!nonblocking_read && now >= deadline) {
                error = reading ? "读取 MAKCU 响应超时" :
                                  "写入 MAKCU 命令超时";
                return mouse::detail::MakcuIoResult::TIMEOUT;
            }

            HANDLE event = reading ? read_event_ : write_event_;
            ResetEvent(event);
            OVERLAPPED operation{};
            operation.hEvent = event;
            DWORD transferred = 0;
            const DWORD requested = static_cast<DWORD>(byte_count - completed);
            const BOOL started = reading
                ? ReadFile(handle_, output + completed, requested,
                           &transferred, &operation)
                : WriteFile(handle_, input + completed, requested,
                            &transferred, &operation);
            if (!started) {
                const DWORD start_error = GetLastError();
                if (start_error != ERROR_IO_PENDING) {
                    set_win32_error(error,
                        reading ? "读取 MAKCU 串口失败" :
                                  "写入 MAKCU 串口失败",
                        start_error);
                    return mouse::detail::MakcuIoResult::FAILED;
                }
                const auto remaining = std::chrono::duration_cast<
                    std::chrono::milliseconds>(deadline - now);
                const DWORD wait_ms = nonblocking_read
                    ? 0U
                    : static_cast<DWORD>((std::max)(
                          std::int64_t{1}, remaining.count()));
                const DWORD wait_result = WaitForSingleObject(event, wait_ms);
                if (wait_result == WAIT_TIMEOUT) {
                    CancelIoEx(handle_, &operation);
                    GetOverlappedResult(handle_, &operation, &transferred,
                                        TRUE);
                    error = reading ? "读取 MAKCU 响应超时" :
                                      "写入 MAKCU 命令超时";
                    return mouse::detail::MakcuIoResult::TIMEOUT;
                }
                if (wait_result != WAIT_OBJECT_0 ||
                    !GetOverlappedResult(handle_, &operation, &transferred,
                                         FALSE)) {
                    set_win32_error(error,
                        reading ? "完成 MAKCU 串口读取失败" :
                                  "完成 MAKCU 串口写入失败",
                        GetLastError());
                    return mouse::detail::MakcuIoResult::FAILED;
                }
            }
            if (transferred == 0U) {
                error = reading ? "MAKCU 串口返回零字节" :
                                  "MAKCU 串口写入零字节";
                return mouse::detail::MakcuIoResult::FAILED;
            }
            completed += transferred;
        }
        error.clear();
        return mouse::detail::MakcuIoResult::SUCCESS;
    }

    static void set_win32_error(std::string& error,
                                const char* prefix,
                                DWORD code) noexcept {
        try {
            error = std::string(prefix) + ", Win32Error=" +
                    std::to_string(code);
        } catch (...) {
            error = prefix;
        }
    }

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    HANDLE read_event_ = nullptr;
    HANDLE write_event_ = nullptr;
};

class MakcuMouseController final : public IMouseController {
public:
    MakcuMouseController(
            const MouseConfig& config,
            std::unique_ptr<mouse::detail::IMakcuTransport> transport)
        : config_(config), transport_(std::move(transport)) {}

    ~MakcuMouseController() override {
        close();
    }

    bool open() noexcept override {
        Log::register_module("mouse", LogLevel::INFO);
        std::lock_guard<std::mutex> io_lock(io_mutex_);
        close_locked();
        set_error({});

        try {
            if (!validate_config()) {
                status_.store(MouseStatus::INVALID_CONFIG,
                              std::memory_order_release);
                return false;
            }
            if (!transport_) {
                set_error("MAKCU 串口传输实例为空");
                status_.store(MouseStatus::CONNECTION_FAILED,
                              std::memory_order_release);
                return false;
            }

            std::string transport_error;
            if (transport_->open(
                    config_.makcu_port,
                    static_cast<std::uint32_t>(config_.makcu_baud_rate),
                    transport_error) != mouse::detail::MakcuIoResult::SUCCESS) {
                set_error(transport_error.empty()
                              ? "打开 MAKCU 串口失败"
                              : transport_error);
                status_.store(MouseStatus::CONNECTION_FAILED,
                              std::memory_order_release);
                close_locked();
                return false;
            }

            const auto query = make_baud_query();
            std::array<std::uint8_t, kBaudResponseBytes> response{};
            if (!exchange(query, response,
                          config_.makcu_connect_timeout_ms, "握手")) {
                close_locked();
                return false;
            }
            if (response[0] != kFrameStart || response[1] != kBaudCommand ||
                response[2] != 4U || response[3] != 0U ||
                read_u32_le(response.data() + 4U) !=
                    static_cast<std::uint32_t>(config_.makcu_baud_rate)) {
                set_error("MAKCU 波特率握手响应与配置不匹配");
                status_.store(MouseStatus::INVALID_RESPONSE,
                              std::memory_order_release);
                close_locked();
                return false;
            }

            connected_ = true;
            const std::array<std::uint8_t, 6U> mouse_stream{
                kFrameStart, 0x0cU, 2U, 0U, 1U, 10U};
            std::array<std::uint8_t, kSetterResponseBytes> stream_response{};
            if (!exchange(mouse_stream, stream_response,
                          config_.makcu_connect_timeout_ms, "鼠标监听")) {
                close_locked();
                return false;
            }
            if (stream_response !=
                    std::array<std::uint8_t, kSetterResponseBytes>{
                        kFrameStart, 0x0cU, 1U, 0U, 0U}) {
                set_error("MAKCU 鼠标监听 ACK 非法或被设备拒绝");
                status_.store(MouseStatus::INVALID_RESPONSE,
                              std::memory_order_release);
                close_locked();
                return false;
            }
            const std::array<std::uint8_t, 6U> keyboard_stream{
                kFrameStart, 0xa5U, 2U, 0U, 1U, 10U};
            if (!exchange(keyboard_stream, stream_response,
                          config_.makcu_connect_timeout_ms, "键盘监听")) {
                close_locked();
                return false;
            }
            if (stream_response !=
                    std::array<std::uint8_t, kSetterResponseBytes>{
                        kFrameStart, 0xa5U, 1U, 0U, 0U}) {
                set_error("MAKCU 键盘监听 ACK 非法或被设备拒绝");
                status_.store(MouseStatus::INVALID_RESPONSE,
                              std::memory_order_release);
                close_locked();
                return false;
            }
            keyboard_keys_.fill(false);
            mouse_buttons_ = 0;
            input_received_ = false;
            input_sequence_ = 0;
            status_.store(config_.allow_send_input
                              ? MouseStatus::READY : MouseStatus::DISABLED,
                          std::memory_order_release);
            set_error({});
            LOG_INFO("mouse", "MAKCU 后端已连接并启用物理键鼠监听: port={}, baud={}",
                     config_.makcu_port, config_.makcu_baud_rate);
            return true;
        } catch (...) {
            set_error("MAKCU 初始化时发生未知异常");
            status_.store(MouseStatus::CONNECTION_FAILED,
                          std::memory_order_release);
            close_locked();
            return false;
        }
    }

    bool move(const MouseMoveCommand& command) noexcept override {
        std::lock_guard<std::mutex> io_lock(io_mutex_);
        if (!config_.allow_send_input) {
            set_error("MAKCU 未在配置中显式允许");
            status_.store(MouseStatus::DISABLED, std::memory_order_release);
            return false;
        }
        if (command.dx_counts == 0 && command.dy_counts == 0) {
            set_error("鼠标移动命令不能同时为零");
            status_.store(MouseStatus::INVALID_COMMAND,
                          std::memory_order_release);
            return false;
        }
        constexpr int kMinimumMove =
            std::numeric_limits<std::int16_t>::min();
        constexpr int kMaximumMove =
            std::numeric_limits<std::int16_t>::max();
        if (command.dx_counts < kMinimumMove ||
            command.dx_counts > kMaximumMove ||
            command.dy_counts < kMinimumMove ||
            command.dy_counts > kMaximumMove) {
            set_error("MAKCU 相对位移超出 int16 范围");
            status_.store(MouseStatus::INVALID_COMMAND,
                          std::memory_order_release);
            return false;
        }
        if (!connected_ || !transport_) {
            set_error("MAKCU 尚未连接");
            status_.store(MouseStatus::CONNECTION_FAILED,
                          std::memory_order_release);
            return false;
        }

        try {
            const auto request = make_move_frame(
                static_cast<std::int16_t>(command.dx_counts),
                static_cast<std::int16_t>(command.dy_counts));
            std::array<std::uint8_t, kSetterResponseBytes> response{};
            if (!exchange(request, response,
                          config_.makcu_command_timeout_ms, "移动命令")) {
                return false;
            }
            if (response[0] != kFrameStart ||
                response[1] != kMoveCommand || response[2] != 1U ||
                response[3] != 0U) {
                set_error("MAKCU 移动 ACK 帧头或长度非法");
                status_.store(MouseStatus::INVALID_RESPONSE,
                              std::memory_order_release);
                return false;
            }
            if (response[4] != 0U) {
                set_error("MAKCU 设备拒绝移动命令，status=" +
                          std::to_string(response[4]));
                status_.store(MouseStatus::SEND_FAILED,
                              std::memory_order_release);
                return false;
            }
            status_.store(MouseStatus::READY, std::memory_order_release);
            set_error({});
            return true;
        } catch (...) {
            set_error("发送 MAKCU 移动命令时发生未知异常");
            status_.store(MouseStatus::SEND_FAILED,
                          std::memory_order_release);
            return false;
        }
    }

    bool poll_input(InputSnapshot& snapshot) noexcept override {
        snapshot = {};
        std::lock_guard<std::mutex> io_lock(io_mutex_);
        if (!connected_ || !transport_) {
            snapshot.status = InputMonitorStatus::CLOSED;
            return true;
        }
        MakcuFrame frame;
        for (int count = 0; count < 16; ++count) {
            const auto result = read_frame(frame, 0);
            if (result == FrameReadResult::NO_DATA) break;
            if (result != FrameReadResult::SUCCESS) {
                set_error("MAKCU 物理输入流读取失败");
                snapshot.status = InputMonitorStatus::FAILURE;
                return true;
            }
            handle_stream_frame(frame);
        }
        if (!input_received_) {
            snapshot.status = InputMonitorStatus::WAITING;
            return true;
        }
        if (std::chrono::steady_clock::now() - input_received_at_ >
            std::chrono::milliseconds(700)) {
            snapshot.status = InputMonitorStatus::STALE;
            return true;
        }
        snapshot.status = InputMonitorStatus::READY;
        snapshot.state_valid = true;
        snapshot.virtual_keys = keyboard_keys_;
        snapshot.virtual_keys[0x01] = (mouse_buttons_ & 0x01U) != 0;
        snapshot.virtual_keys[0x02] = (mouse_buttons_ & 0x02U) != 0;
        snapshot.virtual_keys[0x04] = (mouse_buttons_ & 0x04U) != 0;
        snapshot.virtual_keys[0x05] = (mouse_buttons_ & 0x08U) != 0;
        snapshot.virtual_keys[0x06] = (mouse_buttons_ & 0x10U) != 0;
        snapshot.sequence = input_sequence_;
        return true;
    }

    void close() noexcept override {
        std::lock_guard<std::mutex> io_lock(io_mutex_);
        close_locked();
        status_.store(MouseStatus::CLOSED, std::memory_order_release);
    }

    MouseStatus status() const noexcept override {
        return status_.load(std::memory_order_acquire);
    }

    std::string last_error() const override {
        std::lock_guard<std::mutex> error_lock(error_mutex_);
        return last_error_;
    }

private:
    enum class FrameReadResult { SUCCESS, NO_DATA, FAILURE };

    struct MakcuFrame {
        std::array<std::uint8_t, kMaximumFrameBytes> bytes{};
        std::size_t size = 0U;
    };

    FrameReadResult read_frame(MakcuFrame& frame,
                               int timeout_ms) noexcept {
        frame.size = 0U;
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds((std::max)(0, timeout_ms));
        const auto remaining_timeout = [&]() noexcept {
            if (timeout_ms == 0) return 1;
            const auto remaining = std::chrono::duration_cast<
                std::chrono::milliseconds>(deadline -
                                           std::chrono::steady_clock::now()).count();
            return static_cast<int>((std::max)(std::int64_t{1}, remaining));
        };
        std::uint8_t start = 0;
        std::string error;
        const auto first = transport_->read_exact(
            std::span<std::uint8_t>(&start, 1U), timeout_ms, error);
        if (first == mouse::detail::MakcuIoResult::TIMEOUT) {
            return FrameReadResult::NO_DATA;
        }
        if (first != mouse::detail::MakcuIoResult::SUCCESS ||
            start != kFrameStart) {
            return FrameReadResult::FAILURE;
        }
        std::array<std::uint8_t, 3U> tail{};
        if (transport_->read_exact(tail, remaining_timeout(), error) !=
            mouse::detail::MakcuIoResult::SUCCESS) {
            return FrameReadResult::FAILURE;
        }
        const std::size_t payload_size =
            static_cast<std::size_t>(tail[1]) |
            (static_cast<std::size_t>(tail[2]) << 8U);
        if (payload_size > 64U) return FrameReadResult::FAILURE;
        frame.size = 4U + payload_size;
        frame.bytes[0] = start;
        frame.bytes[1] = tail[0];
        frame.bytes[2] = tail[1];
        frame.bytes[3] = tail[2];
        if (payload_size != 0U &&
            transport_->read_exact(
                std::span<std::uint8_t>(frame.bytes.data() + 4U, payload_size),
                remaining_timeout(), error) !=
                    mouse::detail::MakcuIoResult::SUCCESS) {
            frame.size = 0U;
            return FrameReadResult::FAILURE;
        }
        return FrameReadResult::SUCCESS;
    }

    void handle_stream_frame(const MakcuFrame& frame) noexcept {
        if (frame.size == kMouseStreamFrameBytes &&
            frame.bytes[1] == 0x0cU) {
            mouse_buttons_ = frame.bytes[4];
        } else if (frame.size == kKeyboardStreamFrameBytes &&
                   frame.bytes[1] == 0xa5U) {
            mouse::detail::apply_hid_keyboard_report(
                frame.bytes[4], frame.bytes.data() + 5U, 14U,
                keyboard_keys_);
        } else {
            return;
        }
        input_received_ = true;
        input_received_at_ = std::chrono::steady_clock::now();
        ++input_sequence_;
    }

    bool validate_config() noexcept {
        if (!valid_makcu_port(config_.makcu_port)) {
            set_error("MAKCU 串口必须是 COM1..COM256");
            return false;
        }
        if (config_.makcu_baud_rate != 4000000) {
            set_error("MAKCU 物理键鼠监听要求波特率为 4000000");
            return false;
        }
        if (config_.makcu_connect_timeout_ms <= 0 ||
            config_.makcu_connect_timeout_ms > kMaximumConnectTimeoutMs) {
            set_error("MAKCU 连接超时必须在 1..10000 ms");
            return false;
        }
        if (config_.makcu_command_timeout_ms <= 0 ||
            config_.makcu_command_timeout_ms > kMaximumCommandTimeoutMs) {
            set_error("MAKCU 命令超时必须在 1..1000 ms");
            return false;
        }
        return true;
    }

    template<std::size_t RequestBytes, std::size_t ResponseBytes>
    bool exchange(const std::array<std::uint8_t, RequestBytes>& request,
                  std::array<std::uint8_t, ResponseBytes>& response,
                  int timeout_ms,
                  const char* phase) noexcept {
        std::string transport_error;
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(timeout_ms);
        const auto write_result = transport_->write_exact(
            request, timeout_ms, transport_error);
        if (write_result != mouse::detail::MakcuIoResult::SUCCESS) {
            set_error(std::string("MAKCU ") + phase + "写入失败: " +
                      transport_error);
            status_.store(MouseStatus::SEND_FAILED,
                          std::memory_order_release);
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            set_error(std::string("MAKCU ") + phase +
                      "在等待响应前已耗尽超时预算");
            status_.store(MouseStatus::RESPONSE_TIMEOUT,
                          std::memory_order_release);
            return false;
        }
        // ACK 与异步流共用串口；逐帧读取并消费流帧，直到找到本次请求的响应。
        while (std::chrono::steady_clock::now() < deadline) {
            const auto remaining = std::chrono::duration_cast<
                std::chrono::milliseconds>(deadline -
                                           std::chrono::steady_clock::now()).count();
            const int read_timeout_ms = static_cast<int>(
                (std::max)(std::int64_t{1}, remaining));
            MakcuFrame frame;
            const auto read_result = read_frame(frame, read_timeout_ms);
            if (read_result == FrameReadResult::NO_DATA) break;
            if (read_result != FrameReadResult::SUCCESS) {
                set_error(std::string("MAKCU ") + phase +
                          "读取到非法或不完整 V2 帧");
                status_.store(MouseStatus::CONNECTION_FAILED,
                              std::memory_order_release);
                return false;
            }
            if ((frame.size == kMouseStreamFrameBytes &&
                 frame.bytes[1] == 0x0cU) ||
                (frame.size == kKeyboardStreamFrameBytes &&
                 frame.bytes[1] == 0xa5U)) {
                handle_stream_frame(frame);
                continue;
            }
            if (frame.size != response.size()) {
                set_error(std::string("MAKCU ") + phase + "响应长度非法");
                status_.store(MouseStatus::INVALID_RESPONSE,
                              std::memory_order_release);
                return false;
            }
            std::copy_n(frame.bytes.begin(), frame.size, response.begin());
            return true;
        }
        set_error(std::string("MAKCU ") + phase + "读取响应超时");
        status_.store(MouseStatus::RESPONSE_TIMEOUT,
                      std::memory_order_release);
        return false;
    }

    void close_locked() noexcept {
        if (connected_ && transport_) {
            const std::array<std::uint8_t, 6U> mouse_stream_off{
                kFrameStart, 0x0cU, 2U, 0U, 0U, 0U};
            const std::array<std::uint8_t, 6U> keyboard_stream_off{
                kFrameStart, 0xa5U, 2U, 0U, 0U, 0U};
            std::string ignored;
            transport_->write_exact(mouse_stream_off,
                                    config_.makcu_command_timeout_ms, ignored);
            transport_->write_exact(keyboard_stream_off,
                                    config_.makcu_command_timeout_ms, ignored);
        }
        connected_ = false;
        if (transport_) transport_->close();
    }

    void set_error(const std::string& message) noexcept {
        try {
            std::lock_guard<std::mutex> error_lock(error_mutex_);
            last_error_ = message;
        } catch (...) {
        }
    }

    MouseConfig config_;
    std::unique_ptr<mouse::detail::IMakcuTransport> transport_;
    bool connected_ = false;
    std::array<bool, 256> keyboard_keys_{};
    std::uint8_t mouse_buttons_ = 0;
    bool input_received_ = false;
    std::uint64_t input_sequence_ = 0;
    std::chrono::steady_clock::time_point input_received_at_{};
    mutable std::mutex io_mutex_;
    std::atomic<MouseStatus> status_{MouseStatus::CLOSED};
    mutable std::mutex error_mutex_;
    std::string last_error_;
};

} // namespace

namespace mouse::detail {

std::unique_ptr<IMouseController> create_makcu_controller(
        const MouseConfig& config) noexcept {
    try {
        return std::make_unique<MakcuMouseController>(
            config, std::make_unique<Win32MakcuSerialTransport>());
    } catch (...) {
        return nullptr;
    }
}

std::unique_ptr<IMouseController> create_makcu_controller_for_test(
        const MouseConfig& config,
        std::unique_ptr<IMakcuTransport> transport) noexcept {
    try {
        return std::make_unique<MakcuMouseController>(
            config, std::move(transport));
    } catch (...) {
        return nullptr;
    }
}

} // namespace mouse::detail
