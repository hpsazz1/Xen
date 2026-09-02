#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <WinSock2.h>
#include <WS2tcpip.h>

#ifdef ERROR
#undef ERROR
#endif

#include "log/log.h"
#include "mouse/kmbox_net_internal.h"
#include "mouse/makcu_internal.h"
#include "mouse/mouse.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t kConnectCommand = 0xaf3c2828U;
constexpr std::uint32_t kMouseMoveCommand = 0xaede7345U;
constexpr std::uint32_t kMonitorCommand = 0x27388020U;
constexpr std::size_t kHeaderBytes = 16;
constexpr std::size_t kMovePacketBytes = 72;
constexpr std::uint8_t kMakcuFrameStart = 0x50U;
constexpr std::uint8_t kMakcuMoveCommand = 0x0dU;
constexpr std::uint8_t kMakcuBaudCommand = 0xb1U;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << message << '\n';
}

std::unique_ptr<IMouseController> create_test_mouse(
        const MouseConfig& config) {
    return MouseDeviceFactory::create(
        config, MouseOutputOwnerScope::CURRENT_PROCESS_TEST);
}

void test_win32_input_stays_unverified_without_owned_source() {
    MouseConfig config;
    config.backend = MouseBackend::WIN32_SEND_INPUT;
    config.allow_send_input = false;
    auto mouse = create_test_mouse(config);
    expect(mouse && mouse->open(),
           "Win32 输出 adapter 必须能在物理输出禁用态打开");
    InputSnapshot first;
    InputSnapshot second;
    expect(mouse && mouse->poll_input(first) && mouse->poll_input(second) &&
               first.status == InputMonitorStatus::UNVERIFIED &&
               second.status == InputMonitorStatus::UNVERIFIED &&
               !first.state_valid && !second.state_valid &&
               first.sequence == 0 && second.sequence == 0,
           "GetAsyncKeyState 不得被包装成可验证的完整键态或推进事实序号");
}

std::uint32_t read_u32_le(const std::uint8_t* input) noexcept {
    return static_cast<std::uint32_t>(input[0]) |
           (static_cast<std::uint32_t>(input[1]) << 8U) |
           (static_cast<std::uint32_t>(input[2]) << 16U) |
           (static_cast<std::uint32_t>(input[3]) << 24U);
}

void write_u32_le(std::uint8_t* output, std::uint32_t value) noexcept {
    output[0] = static_cast<std::uint8_t>(value);
    output[1] = static_cast<std::uint8_t>(value >> 8U);
    output[2] = static_cast<std::uint8_t>(value >> 16U);
    output[3] = static_cast<std::uint8_t>(value >> 24U);
}

struct FakeMakcuState {
    int open_calls = 0;
    int close_calls = 0;
    std::string port;
    std::uint32_t baud_rate = 0;
    mouse::detail::MakcuIoResult open_result =
        mouse::detail::MakcuIoResult::SUCCESS;
    std::vector<mouse::detail::MakcuIoResult> read_results;
    std::vector<std::vector<std::uint8_t>> responses;
    std::vector<std::vector<std::uint8_t>> writes;
    std::vector<int> write_timeouts_ms;
    std::vector<int> read_timeouts_ms;
    std::size_t read_index = 0;
    std::size_t response_offset = 0;
};

class FakeMakcuTransport final : public mouse::detail::IMakcuTransport {
public:
    explicit FakeMakcuTransport(std::shared_ptr<FakeMakcuState> state)
        : state_(std::move(state)) {}

    mouse::detail::MakcuIoResult open(
            std::string_view port,
            std::uint32_t baud_rate,
            std::string& error) noexcept override {
        ++state_->open_calls;
        state_->port = port;
        state_->baud_rate = baud_rate;
        if (state_->open_result != mouse::detail::MakcuIoResult::SUCCESS) {
            error = "假串口打开失败";
        }
        return state_->open_result;
    }

    mouse::detail::MakcuIoResult write_exact(
            std::span<const std::uint8_t> bytes,
            int timeout_ms,
            std::string& error) noexcept override {
        try {
            state_->writes.emplace_back(bytes.begin(), bytes.end());
            state_->write_timeouts_ms.push_back(timeout_ms);
            error.clear();
            return mouse::detail::MakcuIoResult::SUCCESS;
        } catch (...) {
            error = "假串口记录写入失败";
            return mouse::detail::MakcuIoResult::FAILED;
        }
    }

    mouse::detail::MakcuIoResult read_exact(
            std::span<std::uint8_t> bytes,
            int timeout_ms,
            std::string& error) noexcept override {
        try {
            state_->read_timeouts_ms.push_back(timeout_ms);
            const std::size_t index = state_->read_index;
            const auto result = index < state_->read_results.size()
                ? state_->read_results[index]
                : mouse::detail::MakcuIoResult::SUCCESS;
            if (result != mouse::detail::MakcuIoResult::SUCCESS) {
                error = result == mouse::detail::MakcuIoResult::TIMEOUT
                    ? "假串口读取超时" : "假串口读取失败";
                return result;
            }
            if (index >= state_->responses.size()) {
                if (timeout_ms == 0) {
                    error = "假串口当前无输入";
                    return mouse::detail::MakcuIoResult::TIMEOUT;
                }
                error = "假串口缺少响应";
                return mouse::detail::MakcuIoResult::FAILED;
            }
            const auto& response = state_->responses[index];
            if (state_->response_offset + bytes.size() > response.size()) {
                error = "假串口响应长度不足";
                return mouse::detail::MakcuIoResult::FAILED;
            }
            std::copy(response.begin() + state_->response_offset,
                      response.begin() + state_->response_offset + bytes.size(),
                      bytes.begin());
            state_->response_offset += bytes.size();
            if (state_->response_offset == response.size()) {
                ++state_->read_index;
                state_->response_offset = 0;
            }
            error.clear();
            return mouse::detail::MakcuIoResult::SUCCESS;
        } catch (...) {
            error = "假串口复制响应失败";
            return mouse::detail::MakcuIoResult::FAILED;
        }
    }

    void close() noexcept override {
        ++state_->close_calls;
    }

private:
    std::shared_ptr<FakeMakcuState> state_;
};

std::vector<std::uint8_t> makcu_baud_response(
        std::uint32_t baud_rate) {
    std::vector<std::uint8_t> response{
        kMakcuFrameStart, kMakcuBaudCommand, 4U, 0U, 0U, 0U, 0U, 0U};
    write_u32_le(response.data() + 4U, baud_rate);
    return response;
}

std::vector<std::uint8_t> makcu_move_response(std::uint8_t status = 0U) {
    return {kMakcuFrameStart, kMakcuMoveCommand, 1U, 0U, status};
}

std::vector<std::uint8_t> makcu_stream_response(
        std::uint8_t command, std::uint8_t status = 0U) {
    return {kMakcuFrameStart, command, 1U, 0U, status};
}

std::vector<std::uint8_t> makcu_mouse_stream(std::uint8_t buttons) {
    return {kMakcuFrameStart, 0x0cU, 8U, 0U, buttons,
            0U, 0U, 0U, 0U, 0U, 0U, 0U};
}

std::vector<std::uint8_t> makcu_keyboard_stream(
        std::initializer_list<std::uint8_t> keys) {
    std::vector<std::uint8_t> frame{
        kMakcuFrameStart, 0xa5U, 15U, 0U, 0U};
    frame.resize(19U, 0U);
    std::copy(keys.begin(), keys.end(), frame.begin() + 5U);
    return frame;
}

MouseConfig make_makcu_config() {
    MouseConfig config;
    config.backend = MouseBackend::MAKCU;
    config.allow_send_input = true;
    config.makcu_port = "COM7";
    config.makcu_baud_rate = 4000000;
    config.makcu_connect_timeout_ms = 90;
    config.makcu_command_timeout_ms = 40;
    return config;
}

std::unique_ptr<IMouseController> make_fake_makcu(
        const MouseConfig& config,
        const std::shared_ptr<FakeMakcuState>& state) {
    return mouse::detail::create_makcu_controller_for_test(
        config, std::make_unique<FakeMakcuTransport>(state));
}

enum class AckMode {
    VALID,
    NONE,
    WRONG_SOURCE,
    WRONG_COMMAND,
    WRONG_SEQUENCE,
};

class FakeKmboxDevice {
public:
    explicit FakeKmboxDevice(std::vector<AckMode> responses)
        : responses_(std::move(responses)) {
        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == INVALID_SOCKET) return;

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(socket_, reinterpret_cast<const sockaddr*>(&address),
                 sizeof(address)) == SOCKET_ERROR) {
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
            return;
        }
        int address_size = sizeof(address);
        if (getsockname(socket_, reinterpret_cast<sockaddr*>(&address),
                        &address_size) == SOCKET_ERROR) {
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
            return;
        }
        port_ = ntohs(address.sin_port);

        const DWORD receive_timeout_ms = 1500;
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&receive_timeout_ms),
                   sizeof(receive_timeout_ms));
        worker_ = std::thread([this] { run(); });
    }

    ~FakeKmboxDevice() {
        finish();
        if (socket_ != INVALID_SOCKET) closesocket(socket_);
    }

    FakeKmboxDevice(const FakeKmboxDevice&) = delete;
    FakeKmboxDevice& operator=(const FakeKmboxDevice&) = delete;

    bool valid() const noexcept {
        return socket_ != INVALID_SOCKET;
    }

    int port() const noexcept {
        return port_;
    }

    void finish() {
        if (worker_.joinable()) worker_.join();
    }

    std::vector<std::vector<std::uint8_t>> packets() const {
        std::lock_guard<std::mutex> lock(packets_mutex_);
        return packets_;
    }

    bool send_monitor(std::uint8_t mouse_buttons,
                      std::initializer_list<std::uint8_t> keys = {}) {
        sockaddr_in destination{};
        {
            std::unique_lock<std::mutex> lock(monitor_mutex_);
            if (!monitor_ready_cv_.wait_for(
                    lock, std::chrono::milliseconds(500),
                    [&] { return monitor_ready_; })) {
                return false;
            }
            destination = monitor_destination_;
        }
        std::array<std::uint8_t, 20U> report{};
        report[1] = mouse_buttons;
        std::copy(keys.begin(), keys.end(), report.begin() + 10U);
        return send_monitor_packet(report, destination);
    }

    bool send_monitor_packet(std::span<const std::uint8_t> report) {
        sockaddr_in destination{};
        {
            std::unique_lock<std::mutex> lock(monitor_mutex_);
            if (!monitor_ready_cv_.wait_for(
                    lock, std::chrono::milliseconds(500),
                    [&] { return monitor_ready_; })) {
                return false;
            }
            destination = monitor_destination_;
        }
        return send_monitor_packet(report, destination);
    }

    bool send_monitor_packet_from_alternate_source(
            std::span<const std::uint8_t> report) {
        sockaddr_in destination{};
        {
            std::unique_lock<std::mutex> lock(monitor_mutex_);
            if (!monitor_ready_cv_.wait_for(
                    lock, std::chrono::milliseconds(500),
                    [&] { return monitor_ready_; })) {
                return false;
            }
            destination = monitor_destination_;
        }
        const SOCKET alternate = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (alternate == INVALID_SOCKET) return false;
        const bool sent = sendto(
            alternate, reinterpret_cast<const char*>(report.data()),
            static_cast<int>(report.size()), 0,
            reinterpret_cast<const sockaddr*>(&destination),
            sizeof(destination)) == static_cast<int>(report.size());
        closesocket(alternate);
        return sent;
    }

private:
    bool send_monitor_packet(std::span<const std::uint8_t> report,
                             const sockaddr_in& destination) {
        return sendto(socket_, reinterpret_cast<const char*>(report.data()),
                      static_cast<int>(report.size()), 0,
                      reinterpret_cast<const sockaddr*>(&destination),
                      sizeof(destination)) == static_cast<int>(report.size());
    }

    void run() noexcept {
        for (const AckMode mode : responses_) {
            std::array<std::uint8_t, 128> buffer{};
            sockaddr_in client{};
            int client_size = sizeof(client);
            const int received = recvfrom(
                socket_, reinterpret_cast<char*>(buffer.data()),
                static_cast<int>(buffer.size()), 0,
                reinterpret_cast<sockaddr*>(&client), &client_size);
            if (received == SOCKET_ERROR) return;
            try {
                std::lock_guard<std::mutex> lock(packets_mutex_);
                packets_.emplace_back(buffer.begin(),
                                      buffer.begin() + received);
            } catch (...) {
                return;
            }
            if (mode == AckMode::NONE ||
                received < static_cast<int>(kHeaderBytes)) {
                continue;
            }

            std::array<std::uint8_t, kHeaderBytes> ack{};
            std::copy_n(buffer.begin(), kHeaderBytes, ack.begin());
            if (mode == AckMode::WRONG_COMMAND) {
                write_u32_le(ack.data() + 12,
                             read_u32_le(ack.data() + 12) ^ 1U);
            } else if (mode == AckMode::WRONG_SEQUENCE) {
                write_u32_le(ack.data() + 8,
                             read_u32_le(ack.data() + 8) + 1U);
            }

            SOCKET response_socket = socket_;
            SOCKET wrong_source_socket = INVALID_SOCKET;
            if (mode == AckMode::WRONG_SOURCE) {
                wrong_source_socket = socket(
                    AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                if (wrong_source_socket == INVALID_SOCKET) continue;
                response_socket = wrong_source_socket;
            }
            sendto(response_socket,
                   reinterpret_cast<const char*>(ack.data()),
                   static_cast<int>(ack.size()), 0,
                   reinterpret_cast<const sockaddr*>(&client), client_size);
            if (mode == AckMode::VALID &&
                read_u32_le(buffer.data() + 12U) == kMonitorCommand) {
                std::lock_guard<std::mutex> lock(monitor_mutex_);
                monitor_destination_ = client;
                monitor_destination_.sin_port = htons(static_cast<u_short>(
                    read_u32_le(buffer.data() + 4U) & 0xffffU));
                monitor_ready_ = true;
                monitor_ready_cv_.notify_all();
            }
            if (wrong_source_socket != INVALID_SOCKET) {
                closesocket(wrong_source_socket);
            }
        }
    }

    SOCKET socket_ = INVALID_SOCKET;
    int port_ = 0;
    std::vector<AckMode> responses_;
    std::thread worker_;
    mutable std::mutex packets_mutex_;
    std::vector<std::vector<std::uint8_t>> packets_;
    std::mutex monitor_mutex_;
    std::condition_variable monitor_ready_cv_;
    sockaddr_in monitor_destination_{};
    bool monitor_ready_ = false;
};

class CapturingKmboxMonitorPacketObserver final
        : public mouse::detail::IKmboxMonitorPacketObserver {
public:
    void observe_kmbox_monitor_packet(
            const mouse::detail::KmboxMonitorPacketObservation& observation,
            std::span<const std::uint8_t> payload) noexcept override {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            observations_.push_back(observation);
            payloads_.emplace_back(payload.begin(), payload.end());
            received_cv_.notify_all();
        } catch (...) {
        }
    }

    bool wait_for_count(
            std::size_t expected_count,
            std::vector<mouse::detail::KmboxMonitorPacketObservation>&
                observations,
            std::vector<std::vector<std::uint8_t>>& payloads) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!received_cv_.wait_for(
                lock, std::chrono::milliseconds(500),
                [&] { return observations_.size() >= expected_count; })) {
            return false;
        }
        observations = observations_;
        payloads = payloads_;
        return true;
    }

private:
    std::mutex mutex_;
    std::condition_variable received_cv_;
    std::vector<mouse::detail::KmboxMonitorPacketObservation> observations_;
    std::vector<std::vector<std::uint8_t>> payloads_;
};

MouseConfig make_config(int port) {
    MouseConfig config;
    config.backend = MouseBackend::KMBOX_NET;
    config.allow_send_input = true;
    config.kmbox_ip = "127.0.0.1";
    config.kmbox_port = port;
    config.kmbox_uuid = "A1B2C3D4";
    config.kmbox_connect_timeout_ms = 80;
    config.kmbox_command_timeout_ms = 60;
    return config;
}

void test_disabled_does_not_access_network() {
    FakeKmboxDevice device({AckMode::VALID, AckMode::VALID});
    expect(device.valid(), "未授权测试 UDP 端点必须创建成功");
    if (!device.valid()) return;
    MouseConfig config;
    config.backend = MouseBackend::KMBOX_NET;
    config.allow_send_input = false;
    config.kmbox_ip = "127.0.0.1";
    config.kmbox_port = device.port();
    config.kmbox_uuid = "A1B2C3D4";
    auto mouse = create_test_mouse(config);
    expect(mouse && mouse->open(),
           "未授权 KMBOX NET 应直接初始化为禁用状态");
    expect(mouse && mouse->status() == MouseStatus::DISABLED,
           "未授权 KMBOX NET 状态必须为 DISABLED");
    expect(mouse && !mouse->move({1, 1}),
           "未授权 KMBOX NET 不得发送移动命令");
    mouse.reset();
    device.finish();
    const auto packets = device.packets();
    expect(packets.size() == 2U,
           "未授权 KMBOX NET 允许只读握手与 monitor 配置，但不得发送移动");
}

void test_packet_layout_and_sequence() {
    FakeKmboxDevice device(
        {AckMode::VALID, AckMode::VALID, AckMode::VALID, AckMode::VALID});
    expect(device.valid(), "成功路径假设备必须创建成功");
    if (!device.valid()) return;
    auto mouse = create_test_mouse(make_config(device.port()));
    expect(mouse && mouse->open(), "合法 ACK 应完成 KMBOX NET 握手" +
               (mouse ? ": " + mouse->last_error() : ""));
    expect(mouse && mouse->status() == MouseStatus::READY,
           "握手后 KMBOX NET 状态必须为 READY");
    const MouseMoveReceipt first_receipt = mouse
        ? mouse->move({120, -45}) : MouseMoveReceipt{};
    expect(mouse && first_receipt.succeeded &&
               first_receipt.backend_completed_at !=
                   std::chrono::steady_clock::time_point{} &&
               first_receipt.protocol_ack_received &&
               first_receipt.protocol_ack_received_at !=
                   std::chrono::steady_clock::time_point{} &&
               !first_receipt.physical_effect_observed,
           "KMBOX 匹配响应必须与 backend completion 分列，且不能冒充物理效果");
    expect(mouse && mouse->move({-32768, 32767}),
           "int16 边界 KMBOX NET 相对移动应成功");
    device.finish();

    const auto packets = device.packets();
    expect(packets.size() == 4, "假设备应收到握手、monitor 和两条移动命令");
    if (packets.size() != 4) return;
    expect(packets[0].size() == kHeaderBytes &&
               read_u32_le(packets[0].data()) == 0xA1B2C3D4U &&
               read_u32_le(packets[0].data() + 8) == 0U &&
               read_u32_le(packets[0].data() + 12) == kConnectCommand,
           "connect 必须是 16 字节小端序 UUID/序号/命令头");
    expect(packets[2].size() == kMovePacketBytes &&
               read_u32_le(packets[2].data() + 8) == 2U &&
               read_u32_le(packets[2].data() + 12) == kMouseMoveCommand &&
               static_cast<std::int32_t>(
                   read_u32_le(packets[2].data() + 20)) == 120 &&
               static_cast<std::int32_t>(
                   read_u32_le(packets[2].data() + 24)) == -45,
           "move 必须是 72 字节且 x/y 位于 payload 固定偏移");
    bool zero_fields = true;
    for (std::size_t index = 16; index < packets[2].size(); ++index) {
        if ((index >= 20 && index < 28)) continue;
        zero_fields = zero_fields && packets[2][index] == 0;
    }
    expect(zero_fields,
           "未使用的 button/wheel/轨迹点必须显式清零");
    expect(read_u32_le(packets[3].data() + 8) == 3U &&
               static_cast<std::int32_t>(
                   read_u32_le(packets[3].data() + 20)) == -32768 &&
               static_cast<std::int32_t>(
                   read_u32_le(packets[3].data() + 24)) == 32767,
           "连续 KMBOX NET 命令序号必须递增且保留有符号边界");
}

void test_kmbox_monitor_retains_state_until_explicit_release() {
    FakeKmboxDevice device({AckMode::VALID, AckMode::VALID});
    auto mouse = create_test_mouse(make_config(device.port()));
    expect(mouse && mouse->open(), "KMBOX monitor 输入测试必须先连接");
    expect(device.send_monitor(0x02U, {0x4dU, 0x41U}),
           "假 KMBOX 必须能发送右键、End 与 F8 物理报告");
    InputSnapshot snapshot;
    for (int retry = 0; retry < 50; ++retry) {
        mouse->poll_input(snapshot);
        if (snapshot.status == InputMonitorStatus::READY) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    expect(snapshot.status == InputMonitorStatus::READY &&
               snapshot.state_valid &&
               snapshot.virtual_keys[0x02] &&
               snapshot.virtual_keys[0x23] &&
               snapshot.virtual_keys[0x77],
           "KMBOX monitor 必须映射右键、End 和 F8");

    std::uint64_t pressed_sequence = snapshot.sequence;
    expect(device.send_monitor(0U), "假 KMBOX 必须能发送全释放报告");
    for (int retry = 0; retry < 50; ++retry) {
        mouse->poll_input(snapshot);
        if (snapshot.sequence > pressed_sequence) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    expect(snapshot.status == InputMonitorStatus::READY &&
               snapshot.state_valid &&
               !snapshot.virtual_keys[0x02] &&
               !snapshot.virtual_keys[0x23] &&
               !snapshot.virtual_keys[0x77],
           "KMBOX monitor 释放报告不得保留旧按键状态");

    const std::uint64_t released_sequence = snapshot.sequence;
    expect(device.send_monitor(0x02U), "长按测试必须先发送右键按下");
    for (int retry = 0; retry < 50; ++retry) {
        mouse->poll_input(snapshot);
        if (snapshot.sequence > released_sequence) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    pressed_sequence = snapshot.sequence;
    std::this_thread::sleep_for(std::chrono::milliseconds(750));
    mouse->poll_input(snapshot);
    expect(snapshot.status == InputMonitorStatus::READY &&
               snapshot.state_valid &&
               snapshot.virtual_keys[0x02] &&
               snapshot.sequence == pressed_sequence,
           "KMBOX monitor 静止长按没有新事件包时必须保留最近按下状态");

    expect(device.send_monitor(0U),
           "KMBOX monitor 长按后必须能发送明确全释放报告");
    for (int retry = 0; retry < 50; ++retry) {
        mouse->poll_input(snapshot);
        if (snapshot.sequence > pressed_sequence) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    expect(snapshot.status == InputMonitorStatus::READY &&
               snapshot.state_valid &&
               !snapshot.virtual_keys[0x02] &&
               snapshot.sequence > pressed_sequence,
           "KMBOX monitor 只有收到明确释放报告后才能清除右键状态");
}

void test_kmbox_monitor_packet_identity_observer_captures_raw_datagram() {
    FakeKmboxDevice device({AckMode::VALID, AckMode::VALID});
    auto observer =
        std::make_shared<CapturingKmboxMonitorPacketObserver>();
    expect(mouse::detail::install_kmbox_monitor_packet_observer(observer),
           "KMBOX monitor packet observer 必须能在打开设备前唯一安装");
    auto mouse = create_test_mouse(make_config(device.port()));
    expect(mouse && mouse->open(),
           "KMBOX packet identity 测试必须先连接 monitor");

    std::vector<std::uint8_t> short_report(19U, 0U);
    short_report[0] = 0x11U;
    short_report[1] = 0x02U;
    short_report[8] = 0x22U;
    short_report[9] = 0x04U;
    expect(device.send_monitor_packet(short_report),
           "假 KMBOX 必须能发送会被状态 parser 拒绝的短数据报");

    std::vector<std::uint8_t> report(24U, 0U);
    report[0] = 0x01U;
    report[1] = 0x02U;
    report[8] = 0x02U;
    report[9] = 0x08U;
    report[10] = 0x4dU;
    expect(device.send_monitor_packet_from_alternate_source(report),
           "假 KMBOX 必须能发送非精确长度的同源 monitor 数据报");
    std::vector<std::uint8_t> rejected_after_state(19U, 0U);
    rejected_after_state[0] = 0x33U;
    rejected_after_state[1] = 0x02U;
    expect(device.send_monitor_packet(rejected_after_state),
           "假 KMBOX 必须能在状态更新后发送被拒绝的数据报");

    std::vector<mouse::detail::KmboxMonitorPacketObservation> observations;
    std::vector<std::vector<std::uint8_t>> captured_payloads;
    expect(observer->wait_for_count(3U, observations, captured_payloads),
           "observer 必须在 parser 前收到全部数据报的原始身份");
    expect(observations.size() == 3U && captured_payloads.size() == 3U &&
               observations[0].datagram_size == short_report.size() &&
               observations[0].configured_device_ipv4 ==
                   std::array<std::uint8_t, 4>{127U, 0U, 0U, 1U} &&
               observations[0].configured_device_port == device.port() &&
               observations[0].keyboard_modifiers_present &&
               observations[0].keyboard_modifiers == 0x04U &&
               !observations[0].accepted_as_monitor_state &&
               observations[0].monitor_sequence_before == 0U &&
               observations[0].monitor_sequence_after == 0U &&
               captured_payloads[0] == short_report,
           "observer 必须记录短包但不得把它伪造成状态或 sequence");
    const auto& observation = observations[1];
    expect(observation.received_at_steady_ns > 0 &&
               observation.datagram_size == report.size() &&
               observation.source_endpoint_valid &&
               observation.source_ipv4 ==
                   std::array<std::uint8_t, 4>{127U, 0U, 0U, 1U} &&
               observation.source_port > 0U &&
               observation.source_port != device.port() &&
               observation.monitor_local_port > 0U &&
               observation.source_ip_matches_configured_device &&
               !observation.source_port_matches_configured_device &&
               observation.configured_device_ipv4 ==
                   std::array<std::uint8_t, 4>{127U, 0U, 0U, 1U} &&
               observation.configured_device_port == device.port() &&
               !observation.exact_monitor_packet_size &&
               observation.mouse_report_id_present &&
               observation.mouse_report_id == 0x01U &&
               observation.mouse_buttons_present &&
               observation.mouse_buttons == 0x02U &&
               observation.keyboard_report_id_present &&
               observation.keyboard_report_id == 0x02U &&
               observation.keyboard_modifiers_present &&
               observation.keyboard_modifiers == 0x08U &&
               observation.accepted_as_monitor_state &&
               observation.monitor_sequence_before == 0U &&
               observation.monitor_sequence_after == 1U &&
               captured_payloads[1] == report,
           "observer 必须保留长度、来源 endpoint、report-id、button、接纳结果、sequence 与原始 payload");
    expect(!observations[2].accepted_as_monitor_state &&
               observations[2].monitor_sequence_before == 1U &&
               observations[2].monitor_sequence_after == 1U &&
               observations[2].monitor_sequence == 0U &&
               captured_payloads[2] == rejected_after_state,
           "parser 拒绝包必须记录当时未改变的 before/after sequence");
    if (mouse) mouse->close();
}

void test_invalid_commands_are_not_sent() {
    FakeKmboxDevice device({AckMode::VALID, AckMode::VALID});
    expect(device.valid(), "非法命令测试假设备必须创建成功");
    if (!device.valid()) return;
    auto mouse = create_test_mouse(make_config(device.port()));
    expect(mouse && mouse->open(), "非法命令测试必须先握手成功");
    expect(mouse && !mouse->move({0, 0}) &&
               mouse->status() == MouseStatus::INVALID_COMMAND,
           "零移动必须在发送前拒绝");
    expect(mouse && !mouse->move({32768, 0}) &&
               mouse->status() == MouseStatus::INVALID_COMMAND,
           "超出 int16 正边界的移动必须在发送前拒绝");
    expect(mouse && !mouse->move({0, -32769}) &&
               mouse->status() == MouseStatus::INVALID_COMMAND,
           "超出 int16 负边界的移动必须在发送前拒绝");
    device.finish();
    expect(device.packets().size() == 2,
           "非法移动不得产生握手之外的数据报");
}

void test_invalid_config() {
    MouseConfig config;
    config.backend = MouseBackend::KMBOX_NET;
    config.allow_send_input = true;
    auto mouse = create_test_mouse(config);
    expect(mouse && !mouse->open() &&
               mouse->status() == MouseStatus::INVALID_CONFIG,
           "缺少 KMBOX NET 设备配置必须在创建 socket 前失败");
}

void test_open_response_failure(AckMode mode,
                                MouseStatus expected_status,
                                const std::string& name) {
    FakeKmboxDevice device({mode});
    expect(device.valid(), name + " 假设备必须创建成功");
    if (!device.valid()) return;
    auto mouse = create_test_mouse(make_config(device.port()));
    expect(mouse && !mouse->open(), name + " 必须导致握手失败");
    expect(mouse && mouse->status() == expected_status,
           name + " 必须返回明确 MouseStatus");
    expect(mouse && !mouse->last_error().empty(),
           name + " 必须保留可观测错误原因");
    device.finish();
}

void test_move_response_failure() {
    FakeKmboxDevice device({AckMode::VALID, AckMode::VALID,
                             AckMode::WRONG_SEQUENCE});
    expect(device.valid(), "移动 ACK 失败假设备必须创建成功");
    if (!device.valid()) return;
    auto mouse = create_test_mouse(make_config(device.port()));
    expect(mouse && mouse->open(), "移动 ACK 失败测试必须先握手成功");
    expect(mouse && !mouse->move({4, -3}) &&
               mouse->status() == MouseStatus::INVALID_RESPONSE,
           "移动 ACK 序号错误必须传递为失败供 Runtime 触发急停");
    device.finish();
}

void test_makcu_disabled_does_not_access_serial() {
    MouseConfig config;
    config.backend = MouseBackend::MAKCU;
    config.allow_send_input = false;
    config.makcu_port = "COM7";
    config.makcu_baud_rate = 4000000;
    auto state = std::make_shared<FakeMakcuState>();
    state->responses = {makcu_baud_response(4000000U),
                        makcu_stream_response(0x0cU),
                        makcu_stream_response(0xa5U)};
    auto mouse = make_fake_makcu(config, state);
    expect(mouse && mouse->open() &&
               mouse->status() == MouseStatus::DISABLED,
           "未授权 MAKCU 仍应打开同一设备监听并保持 DISABLED 输出状态");
    expect(mouse && !mouse->move({1, 1}) &&
               mouse->status() == MouseStatus::DISABLED,
           "未授权 MAKCU 不得发送移动命令");
    expect(state->open_calls == 1 && state->writes.size() == 3U,
           "未授权 MAKCU 只允许 baud 与两条监听配置，不得发送移动帧");

    auto production_mouse = create_test_mouse(config);
    expect(std::string(MouseBackendName(MouseBackend::MAKCU)) == "makcu" &&
               production_mouse && !production_mouse->open(),
           "生产工厂必须创建 MAKCU 且校验监听所需 COM 口");
}

void test_makcu_binary_protocol_and_boundaries() {
    auto state = std::make_shared<FakeMakcuState>();
    state->responses = {
        makcu_baud_response(4000000U),
        makcu_stream_response(0x0cU),
        makcu_stream_response(0xa5U),
        makcu_move_response(),
        makcu_move_response(),
    };
    auto mouse = make_fake_makcu(make_makcu_config(), state);
    expect(mouse && mouse->open(),
           "合法 MAKCU 波特率响应应完成握手" +
               (mouse ? ": " + mouse->last_error() : ""));
    expect(mouse && mouse->status() == MouseStatus::READY,
           "MAKCU 握手成功后状态必须为 READY");
    const MouseMoveReceipt first_receipt = mouse
        ? mouse->move({120, -45}) : MouseMoveReceipt{};
    expect(mouse && first_receipt.succeeded &&
               first_receipt.protocol_ack_received &&
               first_receipt.backend_completed_at !=
                   std::chrono::steady_clock::time_point{} &&
               !first_receipt.physical_effect_observed,
           "MAKCU V2 move ACK 必须独立记录且不能冒充物理效果");
    expect(mouse && mouse->move({-32768, 32767}),
           "MAKCU V2 相对移动应接受 int16 边界");
    expect(state->open_calls == 1 && state->port == "COM7" &&
               state->baud_rate == 4000000U,
           "MAKCU 必须使用显式 COM 口与 4M 波特率打开串口");
    expect(state->writes.size() == 5U,
           "MAKCU 成功路径应写 baud、鼠标流、键盘流和两次 move");
    if (state->writes.size() == 5U) {
        expect(state->writes[0] ==
                   std::vector<std::uint8_t>{
                       kMakcuFrameStart, kMakcuBaudCommand, 0U, 0U},
               "MAKCU open 必须使用 V2 baud getter 完成设备握手");
        expect(state->writes[1] ==
                   std::vector<std::uint8_t>{kMakcuFrameStart, 0x0cU, 2U, 0U, 1U, 10U},
               "MAKCU open 必须启用鼠标物理流");
        expect(state->writes[2] ==
                   std::vector<std::uint8_t>{kMakcuFrameStart, 0xa5U, 2U, 0U, 1U, 10U},
               "MAKCU open 必须启用键盘物理流");
        const auto& first = state->writes[3];
        expect(first.size() == 11U && first[0] == kMakcuFrameStart &&
                   first[1] == kMakcuMoveCommand && first[2] == 7U &&
                   first[3] == 0U &&
                   static_cast<std::int16_t>(
                       static_cast<std::uint16_t>(first[4]) |
                       (static_cast<std::uint16_t>(first[5]) << 8U)) == 120 &&
                   static_cast<std::int16_t>(
                       static_cast<std::uint16_t>(first[6]) |
                       (static_cast<std::uint16_t>(first[7]) << 8U)) == -45 &&
                   first[8] == 1U && first[9] == 0U && first[10] == 0U,
               "MAKCU move 必须是 11 字节 V2 小端序直线帧");
        const auto& boundary = state->writes[4];
        expect(boundary[4] == 0U && boundary[5] == 0x80U &&
                   boundary[6] == 0xffU && boundary[7] == 0x7fU,
               "MAKCU move 必须保留 int16 正负边界位模式");
    }
    expect(state->write_timeouts_ms == std::vector<int>{90, 90, 90, 40, 40} &&
               state->read_timeouts_ms.size() >= 15U &&
               state->read_timeouts_ms[0] > 0 &&
               state->read_timeouts_ms[0] <= 90 &&
               std::all_of(state->read_timeouts_ms.begin(),
                           state->read_timeouts_ms.end(),
                           [](int timeout) { return timeout > 0 && timeout <= 90; }),
           "MAKCU 握手与命令必须共享各自单次端到端超时预算");
}

void test_makcu_invalid_config_and_commands() {
    for (const std::string port : {"COM0", "COM01", "COM257", "USB3"}) {
        auto config = make_makcu_config();
        config.makcu_port = port;
        auto state = std::make_shared<FakeMakcuState>();
        auto mouse = make_fake_makcu(config, state);
        expect(mouse && !mouse->open() &&
                   mouse->status() == MouseStatus::INVALID_CONFIG &&
                   state->open_calls == 0,
               "非法 MAKCU COM 名称必须在系统调用前拒绝: " + port);
    }

    auto config = make_makcu_config();
    config.makcu_baud_rate = 921600;
    auto invalid_baud_state = std::make_shared<FakeMakcuState>();
    auto invalid_baud = make_fake_makcu(config, invalid_baud_state);
    expect(invalid_baud && !invalid_baud->open() &&
               invalid_baud->status() == MouseStatus::INVALID_CONFIG,
           "MAKCU 必须拒绝非官方稳定档位波特率");

    auto state = std::make_shared<FakeMakcuState>();
    state->responses = {makcu_baud_response(4000000U),
                        makcu_stream_response(0x0cU),
                        makcu_stream_response(0xa5U)};
    auto mouse = make_fake_makcu(make_makcu_config(), state);
    expect(mouse && mouse->open(), "MAKCU 非法命令测试必须先握手成功");
    expect(mouse && !mouse->move({0, 0}) &&
               mouse->status() == MouseStatus::INVALID_COMMAND,
           "MAKCU 零移动必须在写串口前拒绝");
    expect(mouse && !mouse->move({32768, 0}) &&
               mouse->status() == MouseStatus::INVALID_COMMAND,
           "MAKCU 超出 int16 的移动必须在写串口前拒绝");
    expect(state->writes.size() == 3U,
           "MAKCU 非法移动不得产生握手和两条监听配置之外的协议帧");

    auto minimum_timeout_state = std::make_shared<FakeMakcuState>();
    minimum_timeout_state->responses = {
        makcu_baud_response(4000000U), makcu_stream_response(0x0cU),
        makcu_stream_response(0xa5U), makcu_move_response()};
    auto minimum_timeout_config = make_makcu_config();
    minimum_timeout_config.makcu_command_timeout_ms = 1;
    auto minimum_timeout_mouse = make_fake_makcu(
        minimum_timeout_config, minimum_timeout_state);
    expect(minimum_timeout_mouse && minimum_timeout_mouse->open() &&
               minimum_timeout_mouse->move({1, 0}) &&
               std::find(minimum_timeout_state->read_timeouts_ms.begin(),
                         minimum_timeout_state->read_timeouts_ms.end(), 1) !=
                   minimum_timeout_state->read_timeouts_ms.end(),
           "MAKCU 1 ms 合法下边界不得因剩余预算截断为零");
}

void test_makcu_response_failures() {
    auto timeout_state = std::make_shared<FakeMakcuState>();
    timeout_state->responses = {makcu_baud_response(4000000U)};
    timeout_state->read_results = {
        mouse::detail::MakcuIoResult::TIMEOUT};
    auto timeout_mouse = make_fake_makcu(
        make_makcu_config(), timeout_state);
    expect(timeout_mouse && !timeout_mouse->open() &&
               timeout_mouse->status() == MouseStatus::RESPONSE_TIMEOUT,
           "MAKCU 握手读取超时必须返回 RESPONSE_TIMEOUT");

    auto wrong_baud_state = std::make_shared<FakeMakcuState>();
    wrong_baud_state->responses = {makcu_baud_response(115200U)};
    auto wrong_baud_mouse = make_fake_makcu(
        make_makcu_config(), wrong_baud_state);
    expect(wrong_baud_mouse && !wrong_baud_mouse->open() &&
               wrong_baud_mouse->status() == MouseStatus::INVALID_RESPONSE,
           "MAKCU 设备波特率响应与配置不一致时必须失败关闭");

    auto rejected_mouse_stream_state = std::make_shared<FakeMakcuState>();
    rejected_mouse_stream_state->responses = {
        makcu_baud_response(4000000U), makcu_stream_response(0x0cU, 1U)};
    auto rejected_mouse_stream = make_fake_makcu(
        make_makcu_config(), rejected_mouse_stream_state);
    expect(rejected_mouse_stream && !rejected_mouse_stream->open() &&
               rejected_mouse_stream->status() == MouseStatus::INVALID_RESPONSE,
           "MAKCU 鼠标流启用被拒绝时必须失败关闭");

    auto rejected_keyboard_stream_state = std::make_shared<FakeMakcuState>();
    rejected_keyboard_stream_state->responses = {
        makcu_baud_response(4000000U), makcu_stream_response(0x0cU),
        makcu_stream_response(0xa5U, 1U)};
    auto rejected_keyboard_stream = make_fake_makcu(
        make_makcu_config(), rejected_keyboard_stream_state);
    expect(rejected_keyboard_stream && !rejected_keyboard_stream->open() &&
               rejected_keyboard_stream->status() == MouseStatus::INVALID_RESPONSE,
           "MAKCU 键盘流启用被拒绝时必须失败关闭");

    auto rejected_state = std::make_shared<FakeMakcuState>();
    rejected_state->responses = {
        makcu_baud_response(4000000U), makcu_stream_response(0x0cU),
        makcu_stream_response(0xa5U), makcu_move_response(1U)};
    auto rejected_mouse = make_fake_makcu(
        make_makcu_config(), rejected_state);
    expect(rejected_mouse && rejected_mouse->open() &&
               !rejected_mouse->move({2, -1}) &&
               rejected_mouse->status() == MouseStatus::SEND_FAILED,
           "MAKCU 明确错误 ACK 必须传递为 SEND_FAILED");

    auto malformed_state = std::make_shared<FakeMakcuState>();
    malformed_state->responses = {
        makcu_baud_response(4000000U), makcu_stream_response(0x0cU),
        makcu_stream_response(0xa5U),
        {kMakcuFrameStart, 0xffU, 1U, 0U, 0U}};
    auto malformed_mouse = make_fake_makcu(
        make_makcu_config(), malformed_state);
    expect(malformed_mouse && malformed_mouse->open() &&
               !malformed_mouse->move({2, -1}) &&
               malformed_mouse->status() == MouseStatus::INVALID_RESPONSE,
           "MAKCU 错误命令码 ACK 必须返回 INVALID_RESPONSE");
}

void test_makcu_interleaved_input_streams() {
    auto state = std::make_shared<FakeMakcuState>();
    state->responses = {
        makcu_baud_response(4000000U),
        makcu_stream_response(0x0cU),
        makcu_stream_response(0xa5U),
        makcu_mouse_stream(0x02U),
        makcu_keyboard_stream({0x4dU, 0x41U}),
        makcu_move_response(),
    };
    auto mouse = make_fake_makcu(make_makcu_config(), state);
    expect(mouse && mouse->open(), "MAKCU 交错流测试必须先完成监听握手");
    expect(mouse && mouse->move({3, -2}),
           "MAKCU 输入流先于 move ACK 时仍必须匹配正确 ACK");
    InputSnapshot snapshot;
    expect(mouse && mouse->poll_input(snapshot) &&
               snapshot.status == InputMonitorStatus::READY &&
               snapshot.virtual_keys[0x02] &&
               snapshot.virtual_keys[0x23] &&
               snapshot.virtual_keys[0x77],
           "MAKCU 同一串口必须发布右键、End 与 F8 物理状态");

    state->responses.push_back(makcu_mouse_stream(0U));
    state->responses.push_back(makcu_keyboard_stream({}));
    expect(mouse && mouse->poll_input(snapshot) &&
               snapshot.status == InputMonitorStatus::READY &&
               !snapshot.virtual_keys[0x02] &&
               !snapshot.virtual_keys[0x23] &&
               !snapshot.virtual_keys[0x77] &&
               std::find(state->read_timeouts_ms.begin(),
                         state->read_timeouts_ms.end(), 0) !=
                   state->read_timeouts_ms.end(),
           "MAKCU poll 必须用非阻塞首字节探测并发布物理全释放");

    std::this_thread::sleep_for(std::chrono::milliseconds(720));
    expect(mouse && mouse->poll_input(snapshot) &&
               snapshot.status == InputMonitorStatus::STALE &&
               !snapshot.virtual_keys[0x02] &&
               !snapshot.virtual_keys[0x23] &&
               !snapshot.virtual_keys[0x77],
           "MAKCU 输入流陈旧后必须失败关闭为全释放");
}

void test_mouse_output_owner_lease_is_process_exclusive_and_recoverable() {
    MouseOutputOwnerLease first;
    MouseOutputOwnerLease second;
    std::string error;
    expect(first.acquire(MouseOutputOwnerScope::CURRENT_PROCESS_TEST,
                         "mouse-tests-first", error) && first.held(),
           "首个测试 Mouse owner lease 应获取成功: " + error);
    expect(!second.acquire(MouseOutputOwnerScope::CURRENT_PROCESS_TEST,
                           "mouse-tests-second", error) && !second.held(),
           "同一测试 scope 的第二个 Mouse owner 必须 fail-closed");
    first.release();
    expect(!first.held() &&
               second.acquire(MouseOutputOwnerScope::CURRENT_PROCESS_TEST,
                              "mouse-tests-second", error) &&
               second.held(),
           "首 owner 释放后另一个 owner 应能获取同一锁: " + error);
    second.release();
}

void test_factory_adapter_owns_output_lease_for_open_lifetime() {
    MouseConfig config;
    config.backend = MouseBackend::WIN32_SEND_INPUT;
    config.allow_send_input = false;
    auto first = MouseDeviceFactory::create(
        config, MouseOutputOwnerScope::CURRENT_PROCESS_TEST);
    auto second = MouseDeviceFactory::create(
        config, MouseOutputOwnerScope::CURRENT_PROCESS_TEST);
    expect(first && second && first->open() &&
               first->output_owner_exclusive(),
           "首个 factory adapter 必须在 open 生命周期持有 owner lease");
    expect(!second->open() &&
               second->status() == MouseStatus::OWNER_CONFLICT &&
               !second->output_owner_exclusive(),
           "第二个 factory adapter 必须在设备初始化前报告 OWNER_CONFLICT");
    first->close();
    expect(second->open() && second->output_owner_exclusive(),
           "首 adapter close 后第二个 adapter 必须能获取 lease");
    second->close();
}

} // namespace

int main() {
    WSADATA wsa_data{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        std::cerr << "Mouse 测试无法初始化 Winsock\n";
        return 1;
    }
    LogConfig log_config;
    log_config.enable_console = false;
    log_config.enable_file = false;
    log_config.enable_ringbuf = false;
    Log::init(log_config);

    test_win32_input_stays_unverified_without_owned_source();
    test_disabled_does_not_access_network();
    test_packet_layout_and_sequence();
    test_kmbox_monitor_retains_state_until_explicit_release();
    test_kmbox_monitor_packet_identity_observer_captures_raw_datagram();
    test_invalid_commands_are_not_sent();
    test_invalid_config();
    test_open_response_failure(
        AckMode::WRONG_SOURCE, MouseStatus::INVALID_RESPONSE,
        "错误来源 ACK");
    test_open_response_failure(
        AckMode::WRONG_COMMAND, MouseStatus::INVALID_RESPONSE,
        "错误命令 ACK");
    test_open_response_failure(
        AckMode::WRONG_SEQUENCE, MouseStatus::INVALID_RESPONSE,
        "错误序号 ACK");
    test_open_response_failure(
        AckMode::NONE, MouseStatus::RESPONSE_TIMEOUT,
        "ACK 超时");
    test_move_response_failure();
    test_makcu_disabled_does_not_access_serial();
    test_makcu_binary_protocol_and_boundaries();
    test_makcu_invalid_config_and_commands();
    test_makcu_response_failures();
    test_makcu_interleaved_input_streams();
    test_mouse_output_owner_lease_is_process_exclusive_and_recoverable();
    test_factory_adapter_owns_output_lease_for_open_lifetime();

    Log::shutdown();
    WSACleanup();
    if (failures != 0) {
        std::cerr << "Mouse 测试失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "Mouse 测试全部通过。\n";
    return 0;
}
