#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <WinSock2.h>
#include <WS2tcpip.h>

#ifdef ERROR
#undef ERROR
#endif

#include "log/log.h"
#include "mouse/mouse.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t kConnectCommand = 0xaf3c2828U;
constexpr std::uint32_t kMouseMoveCommand = 0xaede7345U;
constexpr std::size_t kHeaderBytes = 16;
constexpr std::size_t kMovePacketBytes = 72;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << message << '\n';
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

private:
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
    FakeKmboxDevice device({AckMode::NONE});
    expect(device.valid(), "未授权测试 UDP 端点必须创建成功");
    if (!device.valid()) return;
    MouseConfig config;
    config.backend = MouseBackend::KMBOX_NET;
    config.allow_send_input = false;
    config.kmbox_ip = "invalid";
    config.kmbox_port = device.port();
    auto mouse = MouseDeviceFactory::create(config);
    expect(mouse && mouse->open(),
           "未授权 KMBOX NET 应直接初始化为禁用状态");
    expect(mouse && mouse->status() == MouseStatus::DISABLED,
           "未授权 KMBOX NET 状态必须为 DISABLED");
    expect(mouse && !mouse->move({1, 1}),
           "未授权 KMBOX NET 不得发送移动命令");
    mouse.reset();
    device.finish();
    expect(device.packets().empty(),
           "未授权 KMBOX NET 不得创建协议流量");
}

void test_packet_layout_and_sequence() {
    FakeKmboxDevice device(
        {AckMode::VALID, AckMode::VALID, AckMode::VALID});
    expect(device.valid(), "成功路径假设备必须创建成功");
    if (!device.valid()) return;
    auto mouse = MouseDeviceFactory::create(make_config(device.port()));
    expect(mouse && mouse->open(), "合法 ACK 应完成 KMBOX NET 握手" +
               (mouse ? ": " + mouse->last_error() : ""));
    expect(mouse && mouse->status() == MouseStatus::READY,
           "握手后 KMBOX NET 状态必须为 READY");
    expect(mouse && mouse->move({120, -45}),
           "首条 KMBOX NET 相对移动应成功");
    expect(mouse && mouse->move({-32768, 32767}),
           "int16 边界 KMBOX NET 相对移动应成功");
    device.finish();

    const auto packets = device.packets();
    expect(packets.size() == 3, "假设备应收到握手和两条移动命令");
    if (packets.size() != 3) return;
    expect(packets[0].size() == kHeaderBytes &&
               read_u32_le(packets[0].data()) == 0xA1B2C3D4U &&
               read_u32_le(packets[0].data() + 8) == 0U &&
               read_u32_le(packets[0].data() + 12) == kConnectCommand,
           "connect 必须是 16 字节小端序 UUID/序号/命令头");
    expect(packets[1].size() == kMovePacketBytes &&
               read_u32_le(packets[1].data() + 8) == 1U &&
               read_u32_le(packets[1].data() + 12) == kMouseMoveCommand &&
               static_cast<std::int32_t>(
                   read_u32_le(packets[1].data() + 20)) == 120 &&
               static_cast<std::int32_t>(
                   read_u32_le(packets[1].data() + 24)) == -45,
           "move 必须是 72 字节且 x/y 位于 payload 固定偏移");
    bool zero_fields = true;
    for (std::size_t index = 16; index < packets[1].size(); ++index) {
        if ((index >= 20 && index < 28)) continue;
        zero_fields = zero_fields && packets[1][index] == 0;
    }
    expect(zero_fields,
           "未使用的 button/wheel/轨迹点必须显式清零");
    expect(read_u32_le(packets[2].data() + 8) == 2U &&
               static_cast<std::int32_t>(
                   read_u32_le(packets[2].data() + 20)) == -32768 &&
               static_cast<std::int32_t>(
                   read_u32_le(packets[2].data() + 24)) == 32767,
           "连续 KMBOX NET 命令序号必须递增且保留有符号边界");
}

void test_invalid_commands_are_not_sent() {
    FakeKmboxDevice device({AckMode::VALID});
    expect(device.valid(), "非法命令测试假设备必须创建成功");
    if (!device.valid()) return;
    auto mouse = MouseDeviceFactory::create(make_config(device.port()));
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
    expect(device.packets().size() == 1,
           "非法移动不得产生握手之外的数据报");
}

void test_invalid_config() {
    MouseConfig config;
    config.backend = MouseBackend::KMBOX_NET;
    config.allow_send_input = true;
    auto mouse = MouseDeviceFactory::create(config);
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
    auto mouse = MouseDeviceFactory::create(make_config(device.port()));
    expect(mouse && !mouse->open(), name + " 必须导致握手失败");
    expect(mouse && mouse->status() == expected_status,
           name + " 必须返回明确 MouseStatus");
    expect(mouse && !mouse->last_error().empty(),
           name + " 必须保留可观测错误原因");
    device.finish();
}

void test_move_response_failure() {
    FakeKmboxDevice device({AckMode::VALID, AckMode::WRONG_SEQUENCE});
    expect(device.valid(), "移动 ACK 失败假设备必须创建成功");
    if (!device.valid()) return;
    auto mouse = MouseDeviceFactory::create(make_config(device.port()));
    expect(mouse && mouse->open(), "移动 ACK 失败测试必须先握手成功");
    expect(mouse && !mouse->move({4, -3}) &&
               mouse->status() == MouseStatus::INVALID_RESPONSE,
           "移动 ACK 序号错误必须传递为失败供 Runtime 触发急停");
    device.finish();
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

    test_disabled_does_not_access_network();
    test_packet_layout_and_sequence();
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

    Log::shutdown();
    WSACleanup();
    if (failures != 0) {
        std::cerr << "Mouse 测试失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "Mouse 测试全部通过。\n";
    return 0;
}
