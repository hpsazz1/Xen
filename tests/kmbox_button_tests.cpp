#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <WS2tcpip.h>
#include "mouse/mouse.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace {
constexpr std::uint32_t left = 0x9823ae8dU, move = 0xaede7345U, monitor = 0x27388020U;
std::uint32_t read(const unsigned char* p) {
    return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 |
        std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
}
int failures = 0;
void expect(bool result, const char* message) {
    if (!result) { ++failures; std::cerr << message << '\n'; }
}
// 仅回环UDP，所有配置由临时端口生成，不读取真实设备凭据。
class Device {
public:
    Device() {
        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (socket_ == INVALID_SOCKET || bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address))) return;
        int size = sizeof(address);
        if (getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &size)) return;
        port = ntohs(address.sin_port);
        DWORD timeout = 20;
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        worker_ = std::thread([this] { run(); });
    }
    ~Device() {
        stop_ = true;
        if (worker_.joinable()) worker_.join();
        if (socket_ != INVALID_SOCKET) closesocket(socket_);
    }
    std::vector<std::array<unsigned char, 72>> packets() {
        std::lock_guard<std::mutex> lock(mutex_); return packets_;
    }
    bool physical_left(bool down) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!monitor_destination_.sin_port) return false;
        std::array<unsigned char, 20> report{};
        report[1] = down ? 1 : 0;
        return sendto(socket_, reinterpret_cast<char*>(report.data()), 20, 0,
            reinterpret_cast<sockaddr*>(&monitor_destination_), sizeof(monitor_destination_)) == 20;
    }
    int port = 0;
    // 0=匹配ACK，1=丢ACK，2=错序号；只影响输出命令。
    std::atomic<int> response{0};
private:
    void run() {
        while (!stop_) {
            std::array<unsigned char, 72> bytes{};
            sockaddr_in source{}; int size = sizeof(source);
            int received = recvfrom(socket_, reinterpret_cast<char*>(bytes.data()), 72, 0,
                reinterpret_cast<sockaddr*>(&source), &size);
            if (received < 16) continue;
            auto command = read(bytes.data() + 12);
            bool output = command == left || command == move;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (output) packets_.push_back(bytes);
                if (command == monitor && read(bytes.data() + 4)) {
                    monitor_destination_ = source;
                    monitor_destination_.sin_port = htons(static_cast<unsigned short>(read(bytes.data() + 4)));
                }
            }
            const int mode = response.load();
            if (output && mode == 1) continue;
            if (output && mode == 2) bytes[8] ^= 1;
            sendto(socket_, reinterpret_cast<char*>(bytes.data()), 16, 0,
                reinterpret_cast<sockaddr*>(&source), size);
        }
    }
    SOCKET socket_ = INVALID_SOCKET;
    std::atomic<bool> stop_{false};
    std::thread worker_;
    std::mutex mutex_;
    sockaddr_in monitor_destination_{};
    std::vector<std::array<unsigned char, 72>> packets_;
};
MouseConfig config(Device& device) {
    MouseConfig result;
    result.backend = MouseBackend::KMBOX_NET;
    result.allow_send_input = true;
    result.kmbox_ip = "127.0.0.1";
    result.kmbox_port = device.port;
    result.kmbox_uuid = "12345678";
    result.kmbox_command_timeout_ms = 25;
    return result;
}
auto create(const MouseConfig& cfg) {
    return MouseDeviceFactory::create(cfg, MouseOutputOwnerScope::CURRENT_PROCESS_TEST);
}
void normal_and_physical() {
    Device device;
    auto mouse = create(config(device));
    expect(mouse && mouse->open(), "fake连接必须成功");
    if (!mouse) return;
    expect(mouse->supports_left_button(), "Exclusive必须转发左键能力");
    expect(mouse->set_left_button(true).disposition == ButtonDisposition::ACKNOWLEDGED, "down必须匹配ACK");
    expect(mouse->left_button_cleanup_required() && !mouse->left_button_faulted(), "已知down也有清理责任");
    expect(mouse->set_left_button(true).disposition == ButtonDisposition::ACKNOWLEDGED, "重复down幂等");
    expect(mouse->move({2, -3}).succeeded, "按下期间允许move");
    expect(mouse->set_left_button(false).disposition == ButtonDisposition::ACKNOWLEDGED, "up必须确认");
    expect(!mouse->left_button_cleanup_required(), "up ACK清理债务");
    expect(device.physical_left(true), "fake物理monitor发送");
    InputSnapshot snapshot;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    do { mouse->poll_input(snapshot); std::this_thread::yield(); }
    while (!snapshot.state_valid && std::chrono::steady_clock::now() < deadline);
    expect(snapshot.state_valid && snapshot.virtual_keys[1], "软件up不应改写物理left");
    expect(mouse->move({1, 1}).succeeded, "软件释放后的move成功");
    auto packets = device.packets();
    expect(packets.size() == 4, "重复down不应发送第二包");
    if (packets.size() == 4) {
        expect(read(packets[0].data()+12) == left && read(packets[0].data()+16) == 1, "左键专用协议码与down状态");
        expect(read(packets[1].data()+16) == 1 && read(packets[1].data()+20) == 2, "move必须保留软件left");
        expect(read(packets[3].data()+16) == 0, "物理left不能混入软件buttons");
    }
    mouse->close();
    expect(mouse->set_left_button(true).disposition == ButtonDisposition::REJECTED, "无owner不得输出");
}
void unknown_and_reuse() {
    Device device; auto cfg = config(device); auto mouse = create(cfg);
    expect(mouse && mouse->open(), "unknown测试连接"); if (!mouse) return;
    device.response = 1;
    auto down = mouse->set_left_button(true);
    expect(down.disposition == ButtonDisposition::APPLICATION_UNKNOWN && down.datagram_sent && down.cleanup_required,
        "丢ACK的down必须带债务");
    expect(mouse->left_button_faulted(), "未知down锁存fault");
    expect(!mouse->move({1, 1}) && mouse->set_left_button(true).disposition == ButtonDisposition::REJECTED,
        "fault拒绝move和新down");
    auto up = mouse->set_left_button(false);
    expect(up.disposition == ButtonDisposition::APPLICATION_UNKNOWN && mouse->left_button_faulted(), "未知up保留fault");
    const auto before = device.packets().size();
    mouse->close();
    expect(device.packets().size() == before + 1, "close仅一次有界释放");
    mouse.reset();
    device.response = 0;
    mouse = create(cfg);
    expect(mouse && mouse->open(), "可显式重开连接"); if (!mouse) return;
    expect(mouse->left_button_faulted() && mouse->set_left_button(true).disposition == ButtonDisposition::REJECTED,
        "同endpoint重建不能自动ARM");
    expect(mouse->set_left_button(false).disposition == ButtonDisposition::ACKNOWLEDGED && !mouse->left_button_faulted(),
        "显式up ACK才解除后端债务");
    expect(mouse->set_left_button(true).disposition == ButtonDisposition::ACKNOWLEDGED, "清理后上层新会话可down");
    device.response = 2;
    expect(!mouse->move({1, 2}) && mouse->left_button_faulted(), "携left的move错ACK也需fault");
    device.response = 0;
    mouse->close();
    expect(!mouse->left_button_faulted(), "close明确up ACK可清债但不重开会话");
}
void unsupported() {
    MouseConfig cfg;
    auto mouse = create(cfg);
    expect(mouse && mouse->open(), "无输出Win32可打开");
    expect(!mouse->supports_left_button() && mouse->set_left_button(true).disposition == ButtonDisposition::UNSUPPORTED,
        "其他后端显式UNSUPPORTED");
}
void serialized_release() {
    Device device; auto mouse = create(config(device));
    expect(mouse && mouse->open(), "并发测试连接"); if (!mouse) return;
    expect(mouse->set_left_button(true).disposition == ButtonDisposition::ACKNOWLEDGED, "并发前down");
    std::atomic<bool> valid{true};
    std::thread mover([&] { for (int i=0; i<12; ++i) if (!mouse->move({1, 1})) valid=false; });
    std::thread releaser([&] {
        if (mouse->set_left_button(false).disposition != ButtonDisposition::ACKNOWLEDGED) valid=false;
    });
    mover.join(); releaser.join();
    expect(valid, "并发move/up应由单io mutex完成");
    bool released = false;
    for (const auto& packet : device.packets()) {
        if (read(packet.data()+12) == left && read(packet.data()+16) == 0) released = true;
        if (released) expect(read(packet.data()+16) == 0, "up之后所有move必须携释放态，不能恢复left");
    }
    expect(released, "并发路径应发出up");
    mouse->close();
}
}
int main() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2,2), &data)) return 2;
    normal_and_physical(); unknown_and_reuse(); unsupported(); serialized_release();
    WSACleanup();
    return failures ? 1 : 0;
}
