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

std::mutex kmbox_monitor_packet_observer_mutex;
std::weak_ptr<mouse::detail::IKmboxMonitorPacketObserver>
    kmbox_monitor_packet_observer;

void publish_kmbox_monitor_packet_observation(
        const mouse::detail::KmboxMonitorPacketObservation& observation,
        std::span<const std::uint8_t> payload) noexcept {
    std::shared_ptr<mouse::detail::IKmboxMonitorPacketObserver> observer;
    {
        std::lock_guard<std::mutex> lock(
            kmbox_monitor_packet_observer_mutex);
        observer = kmbox_monitor_packet_observer.lock();
    }
    if (observer) {
        observer->observe_kmbox_monitor_packet(observation, payload);
    }
}

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

            // 上次 ACK 丢失的清理责任跨关闭/重开保留；新连接必须先完成
            // 清理，才能报告 READY 或接纳新命令。
            if ((keyboard_dirty_ || owned_masks_) &&
                cleanup_keyboard_locked().disposition != KeyboardDisposition::ACKNOWLEDGED) {
                close_locked(false);
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
            monitor_local_port_ = static_cast<std::uint16_t>(monitor_port);
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

    MouseMoveReceipt move(
            const MouseMoveCommand& command) noexcept override {
        std::lock_guard<std::mutex> io_lock(io_mutex_);
        if (!config_.allow_send_input) {
            set_error("KMBOX NET 未在配置中显式允许");
            status_.store(MouseStatus::DISABLED, std::memory_order_release);
            return {};
        }
        if (command.dx_counts == 0 && command.dy_counts == 0) {
            set_error("鼠标移动命令不能同时为零");
            status_.store(MouseStatus::INVALID_COMMAND,
                          std::memory_order_release);
            return {};
        }
        constexpr int kMinMove = std::numeric_limits<std::int16_t>::min();
        constexpr int kMaxMove = std::numeric_limits<std::int16_t>::max();
        if (command.dx_counts < kMinMove || command.dx_counts > kMaxMove ||
            command.dy_counts < kMinMove || command.dy_counts > kMaxMove) {
            set_error("KMBOX NET 相对位移超出 int16 范围");
            status_.store(MouseStatus::INVALID_COMMAND,
                          std::memory_order_release);
            return {};
        }
        if (socket_ == INVALID_SOCKET || !winsock_started_) {
            set_error("KMBOX NET 尚未连接");
            status_.store(MouseStatus::CONNECTION_FAILED,
                          std::memory_order_release);
            return {};
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
            std::chrono::steady_clock::time_point acknowledged_at{};
            if (!send_and_wait_ack(packet.data(), packet.size(),
                                   kMouseMoveCommand, sequence_,
                                   config_.kmbox_command_timeout_ms,
                                   &acknowledged_at)) {
                return {};
            }
            status_.store(MouseStatus::READY, std::memory_order_release);
            set_error({});
            MouseMoveReceipt receipt;
            receipt.succeeded = true;
            receipt.protocol_ack_received = true;
            receipt.protocol_ack_received_at = acknowledged_at;
            receipt.backend_completed_at =
                std::chrono::steady_clock::now();
            return receipt;
        } catch (...) {
            set_error("KMBOX NET 发送移动命令时发生未知异常");
            status_.store(MouseStatus::SEND_FAILED,
                          std::memory_order_release);
            return {};
        }
    }

    bool supports_wasd_keyboard() const noexcept override { return true; }

    KeyboardReceipt set_wasd_keyboard(std::uint8_t mask) noexcept override {
        std::lock_guard<std::mutex> lock(io_mutex_);
        return keyboard_locked(mask);
    }
    KeyboardReceipt set_wasd_mask(std::uint8_t key, bool masked) noexcept override {
        std::lock_guard<std::mutex> lock(io_mutex_);
        return mask_locked(key, masked);
    }
    KeyboardReceipt cleanup_wasd_keyboard() noexcept override {
        std::lock_guard<std::mutex> lock(io_mutex_);
        return cleanup_keyboard_locked();
    }
    bool set_wasd_event_subscription(bool enabled) noexcept override {
        std::lock_guard<std::mutex> lock(monitor_mutex_);
        if (wasd_subscribed_ == enabled) return true;
        wasd_subscribed_ = enabled;
        ++wasd_epoch_;
        wasd_sequence_ = 0;
        // 订阅以未知态开始：历史快照不能冒充新边沿。
        return true;
    }
    bool read_wasd_events(WasdEventCursor& cursor, WasdEventBatch& batch) noexcept override {
        std::lock_guard<std::mutex> lock(monitor_mutex_);
        batch = {};
        batch.subscribed = wasd_subscribed_;
        if (!wasd_subscribed_) return true;
        if (cursor.epoch != wasd_epoch_) {
            batch.gap = cursor.epoch != 0;
            cursor = {wasd_epoch_, 0};
        }
        if (cursor.sequence > wasd_sequence_) {
            batch.gap = true;
            cursor.sequence = 0;
        }
        const auto oldest = wasd_sequence_ > wasd_events_.size()
            ? wasd_sequence_ - wasd_events_.size() + 1 : 1;
        if (cursor.sequence + 1 < oldest) {
            batch.gap = true;
            cursor.sequence = oldest - 1;
        }
        while (cursor.sequence < wasd_sequence_ && batch.count < batch.events.size()) {
            ++cursor.sequence;
            batch.events[batch.count++] = wasd_events_[(cursor.sequence - 1) % wasd_events_.size()];
        }
        return true;
    }

    bool poll_input(InputSnapshot& snapshot) noexcept override {
        snapshot = {};
        std::lock_guard<std::mutex> lock(monitor_mutex_);
        if (monitor_socket_ == INVALID_SOCKET ||
            monitor_stop_.load(std::memory_order_acquire)) {
            snapshot.status = InputMonitorStatus::CLOSED;
            return true;
        }
        if (!monitor_received_) {
            snapshot.status = monitor_failed_
                ? InputMonitorStatus::FAILURE
                : InputMonitorStatus::WAITING;
            return true;
        }
        // KMBOX monitor 只在物理输入变化时报告，不提供周期心跳。收到明确释放包前，
        // 最近键态始终有效；socket 故障只改变链路状态，不能伪造全释放。
        snapshot.status = monitor_failed_
            ? InputMonitorStatus::FAILURE
            : InputMonitorStatus::READY;
        snapshot.state_valid = monitor_keyboard_valid_;
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
    static std::uint8_t wasd_usage(std::uint8_t key) noexcept {
        switch (key) { case 1: return 0x1a; case 2: return 0x04;
            case 4: return 0x16; case 8: return 0x07; default: return 0; }
    }
    KeyboardReceipt keyboard_packet_locked(std::uint8_t* packet, std::size_t size,
                                           std::uint32_t command) noexcept {
        KeyboardReceipt result;
        result.disposition = KeyboardDisposition::REJECTED;
        if (!config_.allow_send_input || socket_ == INVALID_SOCKET || !winsock_started_) return result;
        std::chrono::steady_clock::time_point ack{};
        const bool acknowledged = send_and_wait_ack(packet, size, command, sequence_,
            config_.kmbox_command_timeout_ms, &ack, &result.datagram_sent);
        result.backend_completed_at = std::chrono::steady_clock::now();
        result.protocol_ack_received_at = ack;
        result.disposition = acknowledged ? KeyboardDisposition::ACKNOWLEDGED
            : (result.datagram_sent ? KeyboardDisposition::APPLICATION_UNKNOWN : KeyboardDisposition::REJECTED);
        return result;
    }
    KeyboardReceipt keyboard_locked(std::uint8_t mask) noexcept {
        if (mask > 15 || !config_.allow_send_input || socket_ == INVALID_SOCKET)
            return {KeyboardDisposition::REJECTED};
        constexpr std::uint32_t command = 0x123c2c2fU;
        std::array<std::uint8_t, 28> packet{};
        write_header(packet.data(), command, ++sequence_);
        std::size_t index = 18;
        for (std::uint8_t bit = 1; bit <= 8; bit <<= 1)
            if (mask & bit) packet[index++] = wasd_usage(bit);
        auto result = keyboard_packet_locked(packet.data(), packet.size(), command);
        if (result.datagram_sent) keyboard_dirty_ = true;
        if (result.disposition == KeyboardDisposition::ACKNOWLEDGED && mask == 0) keyboard_dirty_ = false;
        return result;
    }
    KeyboardReceipt mask_locked(std::uint8_t key, bool masked) noexcept {
        const auto usage = wasd_usage(key);
        if (!usage || !config_.allow_send_input || socket_ == INVALID_SOCKET)
            return {KeyboardDisposition::REJECTED};
        // 只解除本连接可能安装的屏蔽，禁止全局解除屏蔽。
        if (!masked && !(owned_masks_ & key)) return {KeyboardDisposition::REJECTED};
        const std::uint32_t command = masked ? 0x23234343U : 0x23344343U;
        std::array<std::uint8_t, 16> packet{};
        write_header(packet.data(), command, ++sequence_);
        write_u32_le(packet.data() + 4, static_cast<std::uint32_t>(usage) << 8);
        auto result = keyboard_packet_locked(packet.data(), packet.size(), command);
        if (masked && result.datagram_sent) owned_masks_ |= key;
        if (!masked && result.disposition == KeyboardDisposition::ACKNOWLEDGED) owned_masks_ &= ~key;
        return result;
    }
    KeyboardReceipt cleanup_keyboard_locked() noexcept {
        KeyboardReceipt result{KeyboardDisposition::ACKNOWLEDGED};
        if (keyboard_dirty_) {
            result = keyboard_locked(0);
            // 软件键释放尚未确认时，不恢复物理键直通。
            if (result.disposition != KeyboardDisposition::ACKNOWLEDGED) return result;
        }
        for (std::uint8_t bit = 1; bit <= 8; bit <<= 1) {
            if (!(owned_masks_ & bit)) continue;
            auto step = mask_locked(bit, false);
            if (step.disposition != KeyboardDisposition::ACKNOWLEDGED) result = step;
        }
        return result;
    }
    void publish_wasd_locked(bool valid, std::int64_t timestamp) noexcept {
        if (!wasd_subscribed_) return;
        std::uint8_t mask = 0;
        if (keyboard_keys_['W']) mask |= 1;
        if (keyboard_keys_['A']) mask |= 2;
        if (keyboard_keys_['S']) mask |= 4;
        if (keyboard_keys_['D']) mask |= 8;
        ++wasd_sequence_;
        wasd_events_[(wasd_sequence_ - 1) % wasd_events_.size()] =
            {mask, valid, wasd_epoch_, wasd_sequence_, timestamp};
    }

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
                           int timeout_ms,
                           std::chrono::steady_clock::time_point*
                               acknowledged_at = nullptr,
                           bool* datagram_sent = nullptr) noexcept {
        if (datagram_sent) *datagram_sent = false;
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

        if (datagram_sent) *datagram_sent = true;
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
            if (acknowledged_at) {
                *acknowledged_at = std::chrono::steady_clock::now();
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

    void close_locked(bool attempt_keyboard_cleanup = true) noexcept {
        if (attempt_keyboard_cleanup && (keyboard_dirty_ || owned_masks_)) {
            const auto cleanup = cleanup_keyboard_locked();
            if (cleanup.disposition != KeyboardDisposition::ACKNOWLEDGED)
                LOG_ERROR("mouse", "KMBOX WASD 清理未确认，设备状态未知");
        }
        {
            std::lock_guard<std::mutex> lock(monitor_mutex_);
            wasd_subscribed_ = false;
            ++wasd_epoch_;
            wasd_sequence_ = 0;
        }
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
                    if (wasd_subscribed_) publish_wasd_locked(false,
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count());
                }
                break;
            }

            mouse::detail::KmboxMonitorPacketObservation observation;
            observation.received_at_steady_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            observation.datagram_size = static_cast<std::size_t>(received);
            observation.source_address_size = source_size;
            observation.source_family = source.sin_family;
            observation.source_endpoint_valid =
                source_size == sizeof(sockaddr_in) &&
                source.sin_family == AF_INET;
            const std::uint32_t source_ipv4 =
                ntohl(source.sin_addr.S_un.S_addr);
            observation.source_ipv4 = {
                static_cast<std::uint8_t>(source_ipv4 >> 24U),
                static_cast<std::uint8_t>(source_ipv4 >> 16U),
                static_cast<std::uint8_t>(source_ipv4 >> 8U),
                static_cast<std::uint8_t>(source_ipv4),
            };
            observation.source_port = ntohs(source.sin_port);
            observation.monitor_local_port = monitor_local_port_;
            const std::uint32_t configured_ipv4 =
                ntohl(destination_.sin_addr.S_un.S_addr);
            observation.configured_device_ipv4 = {
                static_cast<std::uint8_t>(configured_ipv4 >> 24U),
                static_cast<std::uint8_t>(configured_ipv4 >> 16U),
                static_cast<std::uint8_t>(configured_ipv4 >> 8U),
                static_cast<std::uint8_t>(configured_ipv4),
            };
            observation.configured_device_port =
                ntohs(destination_.sin_port);
            observation.source_ip_matches_configured_device =
                source.sin_addr.S_un.S_addr ==
                    destination_.sin_addr.S_un.S_addr;
            observation.source_port_matches_configured_device =
                source.sin_port == destination_.sin_port;
            observation.exact_monitor_packet_size =
                received == static_cast<int>(kMonitorPacketBytes);
            observation.mouse_report_id_present = received >= 1;
            if (observation.mouse_report_id_present) {
                observation.mouse_report_id = packet[0];
            }
            observation.mouse_buttons_present = received >= 2;
            if (observation.mouse_buttons_present) {
                observation.mouse_buttons = packet[1];
            }
            observation.keyboard_report_id_present = received >= 9;
            if (observation.keyboard_report_id_present) {
                observation.keyboard_report_id = packet[8];
            }
            observation.keyboard_modifiers_present = received >= 10;
            if (observation.keyboard_modifiers_present) {
                observation.keyboard_modifiers = packet[9];
            }
            observation.accepted_as_monitor_state =
                received >= static_cast<int>(kMonitorPacketBytes) &&
                observation.source_ip_matches_configured_device;
            {
                std::lock_guard<std::mutex> lock(monitor_mutex_);
                observation.monitor_sequence_before = monitor_sequence_;
                if (observation.accepted_as_monitor_state) {
                    mouse_buttons_ = packet[1];
                    bool keyboard_valid = true;
                    for (std::size_t i = 10; i < 20; ++i)
                        if (packet[i] >= 1 && packet[i] <= 3) keyboard_valid = false;
                    if (keyboard_valid) {
                        mouse::detail::apply_hid_keyboard_report(
                            packet[9], packet.data() + 10U, 10U, keyboard_keys_);
                    }
                    monitor_keyboard_valid_ = keyboard_valid;
                    publish_wasd_locked(keyboard_valid && observation.exact_monitor_packet_size &&
                        observation.source_endpoint_valid, observation.received_at_steady_ns);
                    monitor_received_ = true;
                    ++monitor_sequence_;
                    observation.monitor_sequence = monitor_sequence_;
                }
                if (!observation.accepted_as_monitor_state &&
                    observation.source_ip_matches_configured_device)
                    publish_wasd_locked(false, observation.received_at_steady_ns);
                observation.monitor_sequence_after = monitor_sequence_;
            }
            publish_kmbox_monitor_packet_observation(
                observation,
                std::span<const std::uint8_t>(
                    packet.data(), static_cast<std::size_t>(received)));
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
    bool wasd_subscribed_ = false;
    std::uint64_t wasd_epoch_ = 1;
    std::uint64_t wasd_sequence_ = 0;
    std::array<WasdEvent, 256> wasd_events_{};
    bool keyboard_dirty_ = false;
    std::uint8_t owned_masks_ = 0;
    bool monitor_keyboard_valid_ = false;
    bool monitor_received_ = false;
    bool monitor_failed_ = false;
    bool monitor_configured_ = false;
    std::uint16_t monitor_local_port_ = 0;
    std::uint64_t monitor_sequence_ = 0;
    mutable std::mutex io_mutex_;
    std::atomic<MouseStatus> status_{MouseStatus::CLOSED};
    mutable std::mutex error_mutex_;
    std::string last_error_;
};

} // namespace

namespace mouse::detail {

bool install_kmbox_monitor_packet_observer(
        const std::shared_ptr<IKmboxMonitorPacketObserver>& observer) noexcept {
    if (!observer) return false;
    std::lock_guard<std::mutex> lock(kmbox_monitor_packet_observer_mutex);
    if (!kmbox_monitor_packet_observer.expired()) return false;
    kmbox_monitor_packet_observer = observer;
    return true;
}

std::unique_ptr<IMouseController> create_kmbox_net_controller(
        const MouseConfig& config) noexcept {
    try {
        return std::make_unique<KmboxNetMouseController>(config);
    } catch (...) {
        return nullptr;
    }
}

} // namespace mouse::detail
