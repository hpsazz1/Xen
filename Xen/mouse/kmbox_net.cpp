#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <WinSock2.h>
#include <WS2tcpip.h>

#ifdef ERROR
#undef ERROR
#endif

#include "mouse/kmbox_net_internal.h"
#include "mouse/input_internal.h"

#include "log/log.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {

constexpr std::uint32_t kConnectCommand = 0xaf3c2828U;
constexpr std::uint32_t kMouseMoveCommand = 0xaede7345U;
constexpr std::uint32_t kMonitorCommand = 0x27388020U;
constexpr std::size_t kHeaderBytes = 16;
constexpr std::size_t kMousePayloadBytes = 56;
constexpr std::size_t kMousePacketBytes = kHeaderBytes + kMousePayloadBytes;
constexpr int kMaxConnectTimeoutMs = 10000;
constexpr int kMaxCommandTimeoutMs = 1000;
constexpr std::size_t kMonitorPacketBytes = 20U;
constexpr auto kInputFreshness = std::chrono::milliseconds(700);

void write_u32_le(std::uint8_t* output, std::uint32_t value) noexcept {
    output[0] = static_cast<std::uint8_t>(value);
    output[1] = static_cast<std::uint8_t>(value >> 8U);
    output[2] = static_cast<std::uint8_t>(value >> 16U);
    output[3] = static_cast<std::uint8_t>(value >> 24U);
}

std::uint32_t read_u32_le(const std::uint8_t* input) noexcept {
    return static_cast<std::uint32_t>(input[0]) |
           (static_cast<std::uint32_t>(input[1]) << 8U) |
           (static_cast<std::uint32_t>(input[2]) << 16U) |
           (static_cast<std::uint32_t>(input[3]) << 24U);
}

bool parse_uuid(const std::string& text, std::uint32_t& value) noexcept {
    if (text.size() != 8) return false;
    std::uint32_t parsed = 0;
    for (const char ch : text) {
        std::uint32_t digit = 0;
        if (ch >= '0' && ch <= '9') {
            digit = static_cast<std::uint32_t>(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            digit = static_cast<std::uint32_t>(ch - 'a' + 10);
        } else if (ch >= 'A' && ch <= 'F') {
            digit = static_cast<std::uint32_t>(ch - 'A' + 10);
        } else {
            return false;
        }
        parsed = (parsed << 4U) | digit;
    }
    value = parsed;
    return true;
}

class KmboxNetMouseController final : public IMouseController {
public:
    explicit KmboxNetMouseController(const MouseConfig& config)
        : config_(config) {}

    ~KmboxNetMouseController() override {
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

            WSADATA wsa_data{};
            const int startup_result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
            if (startup_result != 0 ||
                LOBYTE(wsa_data.wVersion) != 2 ||
                HIBYTE(wsa_data.wVersion) != 2) {
                if (startup_result == 0) WSACleanup();
                set_winsock_error("Winsock 2.2 初始化失败", startup_result);
                status_.store(MouseStatus::CONNECTION_FAILED,
                              std::memory_order_release);
                return false;
            }
            winsock_started_ = true;

            socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (socket_ == INVALID_SOCKET) {
                set_winsock_error("KMBOX NET UDP socket 创建失败",
                                   WSAGetLastError());
                status_.store(MouseStatus::CONNECTION_FAILED,
                              std::memory_order_release);
                close_locked();
                return false;
            }

            destination_ = {};
            destination_.sin_family = AF_INET;
            destination_.sin_port = htons(
                static_cast<unsigned short>(config_.kmbox_port));
            if (InetPtonA(AF_INET, config_.kmbox_ip.c_str(),
                          &destination_.sin_addr) != 1) {
                set_error("KMBOX NET IP 必须是有效 IPv4 地址");
                status_.store(MouseStatus::INVALID_CONFIG,
                              std::memory_order_release);
                close_locked();
                return false;
            }

            const DWORD send_timeout = static_cast<DWORD>(
                config_.kmbox_command_timeout_ms);
            if (setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO,
                           reinterpret_cast<const char*>(&send_timeout),
                           sizeof(send_timeout)) == SOCKET_ERROR) {
                set_winsock_error("KMBOX NET 发送超时配置失败",
                                   WSAGetLastError());
                status_.store(MouseStatus::CONNECTION_FAILED,
                              std::memory_order_release);
                close_locked();
                return false;
            }

            random_state_ = static_cast<std::uint32_t>(
                std::chrono::steady_clock::now().time_since_epoch().count()) ^
                uuid_ ^ static_cast<std::uint32_t>(config_.kmbox_port);
            if (random_state_ == 0) random_state_ = 0x6d2b79f5U;
            sequence_ = 0;

            std::array<std::uint8_t, kHeaderBytes> connect_packet{};
            write_header(connect_packet.data(), kConnectCommand, sequence_);
            if (!send_and_wait_ack(connect_packet.data(),
                                   connect_packet.size(),
                                   kConnectCommand, sequence_,
                                   config_.kmbox_connect_timeout_ms)) {
                close_locked();
                return false;
            }

            monitor_socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (monitor_socket_ == INVALID_SOCKET) {
                set_winsock_error("KMBOX NET monitor socket 创建失败",
                                   WSAGetLastError());
                close_locked();
                return false;
            }
            sockaddr_in monitor_address{};
            monitor_address.sin_family = AF_INET;
            monitor_address.sin_addr.s_addr = htonl(INADDR_ANY);
            monitor_address.sin_port = 0;
            if (bind(monitor_socket_,
                     reinterpret_cast<const sockaddr*>(&monitor_address),
                     sizeof(monitor_address)) == SOCKET_ERROR) {
                set_winsock_error("KMBOX NET monitor 端口绑定失败",
                                   WSAGetLastError());
                close_locked();
                return false;
            }
            int monitor_address_size = sizeof(monitor_address);
            if (getsockname(monitor_socket_,
                            reinterpret_cast<sockaddr*>(&monitor_address),
                            &monitor_address_size) == SOCKET_ERROR) {
                set_winsock_error("KMBOX NET monitor 端口读取失败",
                                   WSAGetLastError());
                close_locked();
                return false;
            }
            const int monitor_port = ntohs(monitor_address.sin_port);
            std::array<std::uint8_t, kHeaderBytes> monitor_packet{};
            write_header(monitor_packet.data(), kMonitorCommand, ++sequence_);
            write_u32_le(monitor_packet.data() + 4U,
                         static_cast<std::uint32_t>(monitor_port) |
                         (0xaa55U << 16U));
            if (!send_and_wait_ack(monitor_packet.data(),
                                   monitor_packet.size(), kMonitorCommand,
                                   sequence_, config_.kmbox_connect_timeout_ms)) {
                close_locked();
                return false;
            }
            monitor_configured_ = true;
            {
                std::lock_guard<std::mutex> lock(monitor_mutex_);
                keyboard_keys_.fill(false);
                mouse_buttons_ = 0;
                monitor_received_ = false;
                monitor_failed_ = false;
                monitor_sequence_ = 0;
            }
            monitor_stop_.store(false, std::memory_order_release);
            monitor_thread_ = std::thread([this] { monitor_loop(); });

            status_.store(config_.allow_send_input
                              ? MouseStatus::READY : MouseStatus::DISABLED,
                          std::memory_order_release);
            set_error({});
            LOG_INFO("mouse", "KMBOX NET 后端已连接并启用物理键鼠监听: {}:{}",
                     config_.kmbox_ip, config_.kmbox_port);
            return true;
        } catch (...) {
            set_error("KMBOX NET 初始化时发生未知异常");
            status_.store(MouseStatus::CONNECTION_FAILED,
                          std::memory_order_release);
            close_locked();
            return false;
        }
    }

    bool move(const MouseMoveCommand& command) noexcept override {
        std::lock_guard<std::mutex> io_lock(io_mutex_);
        if (!config_.allow_send_input) {
            set_error("KMBOX NET 未在配置中显式允许");
            status_.store(MouseStatus::DISABLED, std::memory_order_release);
            return false;
        }
        if (command.dx_counts == 0 && command.dy_counts == 0) {
            set_error("鼠标移动命令不能同时为零");
            status_.store(MouseStatus::INVALID_COMMAND,
                          std::memory_order_release);
            return false;
        }
        constexpr int kMinMove = std::numeric_limits<std::int16_t>::min();
        constexpr int kMaxMove = std::numeric_limits<std::int16_t>::max();
        if (command.dx_counts < kMinMove || command.dx_counts > kMaxMove ||
            command.dy_counts < kMinMove || command.dy_counts > kMaxMove) {
            set_error("KMBOX NET 相对位移超出 int16 范围");
            status_.store(MouseStatus::INVALID_COMMAND,
                          std::memory_order_release);
            return false;
        }
        if (socket_ == INVALID_SOCKET || !winsock_started_) {
            set_error("KMBOX NET 尚未连接");
            status_.store(MouseStatus::CONNECTION_FAILED,
                          std::memory_order_release);
            return false;
        }

        try {
            ++sequence_;
            std::array<std::uint8_t, kMousePacketBytes> packet{};
            write_header(packet.data(), kMouseMoveCommand, sequence_);
            // payload 前四项为 button/x/y/wheel，后续十个轨迹点保持为零。
            write_u32_le(packet.data() + kHeaderBytes + 4,
                         static_cast<std::uint32_t>(command.dx_counts));
            write_u32_le(packet.data() + kHeaderBytes + 8,
                         static_cast<std::uint32_t>(command.dy_counts));
            if (!send_and_wait_ack(packet.data(), packet.size(),
                                   kMouseMoveCommand, sequence_,
                                   config_.kmbox_command_timeout_ms)) {
                return false;
            }
            status_.store(MouseStatus::READY, std::memory_order_release);
            set_error({});
            return true;
        } catch (...) {
            set_error("KMBOX NET 发送移动命令时发生未知异常");
            status_.store(MouseStatus::SEND_FAILED,
                          std::memory_order_release);
            return false;
        }
    }

    bool poll_input(InputSnapshot& snapshot) noexcept override {
        snapshot = {};
        std::lock_guard<std::mutex> lock(monitor_mutex_);
        if (monitor_socket_ == INVALID_SOCKET ||
            monitor_stop_.load(std::memory_order_acquire)) {
            snapshot.status = InputMonitorStatus::CLOSED;
            return true;
        }
        if (monitor_failed_) {
            snapshot.status = InputMonitorStatus::FAILURE;
            return true;
        }
        if (!monitor_received_) {
            snapshot.status = InputMonitorStatus::WAITING;
            return true;
        }
        if (std::chrono::steady_clock::now() - monitor_received_at_ >
            kInputFreshness) {
            snapshot.status = InputMonitorStatus::STALE;
            return true;
        }
        snapshot.status = InputMonitorStatus::READY;
        snapshot.virtual_keys = keyboard_keys_;
        snapshot.virtual_keys[0x01] = (mouse_buttons_ & 0x01U) != 0;
        snapshot.virtual_keys[0x02] = (mouse_buttons_ & 0x02U) != 0;
        snapshot.virtual_keys[0x04] = (mouse_buttons_ & 0x04U) != 0;
        snapshot.virtual_keys[0x05] = (mouse_buttons_ & 0x08U) != 0;
        snapshot.virtual_keys[0x06] = (mouse_buttons_ & 0x10U) != 0;
        snapshot.sequence = monitor_sequence_;
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
    bool validate_config() noexcept {
        if (config_.kmbox_ip.empty()) {
            set_error("KMBOX NET IP 不能为空");
            return false;
        }
        if (config_.kmbox_port <= 0 || config_.kmbox_port > 65535) {
            set_error("KMBOX NET 端口必须在 1..65535");
            return false;
        }
        if (!parse_uuid(config_.kmbox_uuid, uuid_)) {
            set_error("KMBOX NET UUID 必须是 8 个十六进制字符");
            return false;
        }
        if (config_.kmbox_connect_timeout_ms <= 0 ||
            config_.kmbox_connect_timeout_ms > kMaxConnectTimeoutMs) {
            set_error("KMBOX NET 连接超时必须在 1..10000 ms");
            return false;
        }
        if (config_.kmbox_command_timeout_ms <= 0 ||
            config_.kmbox_command_timeout_ms > kMaxCommandTimeoutMs) {
            set_error("KMBOX NET 命令超时必须在 1..1000 ms");
            return false;
        }
        return true;
    }

    void write_header(std::uint8_t* output,
                      std::uint32_t command,
                      std::uint32_t sequence) noexcept {
        write_u32_le(output, uuid_);
        write_u32_le(output + 4, next_random());
        write_u32_le(output + 8, sequence);
        write_u32_le(output + 12, command);
    }

    std::uint32_t next_random() noexcept {
        // 协议 random 字段仅作混淆且设备不校验；xorshift 保证实例内无全局 rand 状态。
        random_state_ ^= random_state_ << 13U;
        random_state_ ^= random_state_ >> 17U;
        random_state_ ^= random_state_ << 5U;
        return random_state_;
    }

    bool send_and_wait_ack(const std::uint8_t* packet,
                           std::size_t packet_size,
                           std::uint32_t command,
                           std::uint32_t sequence,
                           int timeout_ms) noexcept {
        const int sent = sendto(
            socket_, reinterpret_cast<const char*>(packet),
            static_cast<int>(packet_size), 0,
            reinterpret_cast<const sockaddr*>(&destination_),
            sizeof(destination_));
        if (sent != static_cast<int>(packet_size)) {
            set_winsock_error("KMBOX NET 数据报发送失败", WSAGetLastError());
            status_.store(MouseStatus::SEND_FAILED,
                          std::memory_order_release);
            return false;
        }

        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(timeout_ms);
        bool saw_invalid_response = false;
        const char* invalid_reason = "KMBOX NET 收到不匹配的 ACK";
        while (std::chrono::steady_clock::now() < deadline) {
            const auto remaining = std::chrono::duration_cast<
                std::chrono::microseconds>(
                    deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0) break;

            fd_set read_set;
            FD_ZERO(&read_set);
            FD_SET(socket_, &read_set);
            timeval timeout{};
            timeout.tv_sec = static_cast<long>(
                remaining.count() / 1000000);
            timeout.tv_usec = static_cast<long>(
                remaining.count() % 1000000);
            const int ready = select(0, &read_set, nullptr, nullptr, &timeout);
            if (ready == 0) break;
            if (ready == SOCKET_ERROR) {
                set_winsock_error("KMBOX NET 等待 ACK 失败",
                                   WSAGetLastError());
                status_.store(MouseStatus::CONNECTION_FAILED,
                              std::memory_order_release);
                return false;
            }

            std::array<std::uint8_t, kMousePacketBytes> response{};
            sockaddr_in source{};
            int source_size = sizeof(source);
            const int received = recvfrom(
                socket_, reinterpret_cast<char*>(response.data()),
                static_cast<int>(response.size()), 0,
                reinterpret_cast<sockaddr*>(&source), &source_size);
            if (received == SOCKET_ERROR) {
                set_winsock_error("KMBOX NET 接收 ACK 失败",
                                   WSAGetLastError());
                status_.store(MouseStatus::CONNECTION_FAILED,
                              std::memory_order_release);
                return false;
            }

            if (source_size != sizeof(sockaddr_in) ||
                source.sin_family != AF_INET ||
                source.sin_port != destination_.sin_port ||
                source.sin_addr.S_un.S_addr !=
                    destination_.sin_addr.S_un.S_addr) {
                saw_invalid_response = true;
                invalid_reason = "KMBOX NET ACK 来源地址不匹配";
                continue;
            }
            if (received < static_cast<int>(kHeaderBytes)) {
                saw_invalid_response = true;
                invalid_reason = "KMBOX NET ACK 长度小于 16 字节";
                continue;
            }
            if (read_u32_le(response.data() + 12) != command) {
                saw_invalid_response = true;
                invalid_reason = "KMBOX NET ACK 命令码不匹配";
                continue;
            }
            if (read_u32_le(response.data() + 8) != sequence) {
                saw_invalid_response = true;
                invalid_reason = "KMBOX NET ACK 序号不匹配";
                continue;
            }
            return true;
        }

        if (saw_invalid_response) {
            set_error(invalid_reason);
            status_.store(MouseStatus::INVALID_RESPONSE,
                          std::memory_order_release);
        } else {
            set_error("KMBOX NET 等待 ACK 超时");
            status_.store(MouseStatus::RESPONSE_TIMEOUT,
                          std::memory_order_release);
        }
        return false;
    }

    void close_locked() noexcept {
        if (monitor_configured_ && socket_ != INVALID_SOCKET) {
            std::array<std::uint8_t, kHeaderBytes> disable_packet{};
            write_header(disable_packet.data(), kMonitorCommand, ++sequence_);
            write_u32_le(disable_packet.data() + 4U, 0U);
            sendto(socket_, reinterpret_cast<const char*>(disable_packet.data()),
                   static_cast<int>(disable_packet.size()), 0,
                   reinterpret_cast<const sockaddr*>(&destination_),
                   sizeof(destination_));
        }
        monitor_configured_ = false;
        monitor_stop_.store(true, std::memory_order_release);
        if (monitor_socket_ != INVALID_SOCKET) {
            closesocket(monitor_socket_);
            monitor_socket_ = INVALID_SOCKET;
        }
        if (monitor_thread_.joinable()) monitor_thread_.join();
        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }
        if (winsock_started_) {
            WSACleanup();
            winsock_started_ = false;
        }
    }

    void monitor_loop() noexcept {
        std::array<std::uint8_t, 1024> packet{};
        while (!monitor_stop_.load(std::memory_order_acquire)) {
            sockaddr_in source{};
            int source_size = sizeof(source);
            const int received = recvfrom(
                monitor_socket_, reinterpret_cast<char*>(packet.data()),
                static_cast<int>(packet.size()), 0,
                reinterpret_cast<sockaddr*>(&source), &source_size);
            if (received == SOCKET_ERROR) {
                if (!monitor_stop_.load(std::memory_order_acquire)) {
                    std::lock_guard<std::mutex> lock(monitor_mutex_);
                    monitor_failed_ = true;
                }
                break;
            }
            if (received < static_cast<int>(kMonitorPacketBytes) ||
                source.sin_addr.S_un.S_addr != destination_.sin_addr.S_un.S_addr) {
                continue;
            }
            std::lock_guard<std::mutex> lock(monitor_mutex_);
            mouse_buttons_ = packet[1];
            mouse::detail::apply_hid_keyboard_report(
                packet[9], packet.data() + 10U, 10U, keyboard_keys_);
            monitor_received_ = true;
            monitor_received_at_ = std::chrono::steady_clock::now();
            ++monitor_sequence_;
        }
    }

    void set_error(const std::string& message) noexcept {
        try {
            std::lock_guard<std::mutex> error_lock(error_mutex_);
            last_error_ = message;
        } catch (...) {
        }
    }

    void set_winsock_error(const char* prefix, int code) noexcept {
        try {
            set_error(std::string(prefix) + ", WSAError=" +
                      std::to_string(code));
        } catch (...) {
            set_error(prefix);
        }
    }

    MouseConfig config_;
    SOCKET socket_ = INVALID_SOCKET;
    SOCKET monitor_socket_ = INVALID_SOCKET;
    bool winsock_started_ = false;
    sockaddr_in destination_{};
    std::uint32_t uuid_ = 0;
    std::uint32_t random_state_ = 0;
    std::uint32_t sequence_ = 0;
    std::atomic<bool> monitor_stop_{true};
    std::thread monitor_thread_;
    mutable std::mutex monitor_mutex_;
    std::array<bool, 256> keyboard_keys_{};
    std::uint8_t mouse_buttons_ = 0;
    bool monitor_received_ = false;
    bool monitor_failed_ = false;
    bool monitor_configured_ = false;
    std::uint64_t monitor_sequence_ = 0;
    std::chrono::steady_clock::time_point monitor_received_at_{};
    mutable std::mutex io_mutex_;
    std::atomic<MouseStatus> status_{MouseStatus::CLOSED};
    mutable std::mutex error_mutex_;
    std::string last_error_;
};

} // namespace

namespace mouse::detail {

std::unique_ptr<IMouseController> create_kmbox_net_controller(
        const MouseConfig& config) noexcept {
    try {
        return std::make_unique<KmboxNetMouseController>(config);
    } catch (...) {
        return nullptr;
    }
}

} // namespace mouse::detail
