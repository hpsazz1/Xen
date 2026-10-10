#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <WS2tcpip.h>
#include "mouse/mouse.h"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace {
constexpr std::uint32_t right = 0x238d8212U, keyboard = 0x123c2c2fU;
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
        release_observer();
        if (worker_.joinable()) worker_.join();
        if (socket_ != INVALID_SOCKET) closesocket(socket_);
    }
    std::vector<std::array<unsigned char, 72>> packets() {
        std::lock_guard<std::mutex> lock(mutex_); return packets_;
    }
    bool wait_outputs(std::size_t count) {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, std::chrono::milliseconds(500), [&] { return packets_.size() >= count || receiver_error_; });
        return !receiver_error_ && packets_.size() >= count;
    }
    void hold_next_output() {
        std::lock_guard lock(mutex_); hold_output_ = true;
    }
    void release_observer() {
        { std::lock_guard lock(mutex_); hold_output_ = false; }
        changed_.notify_all();
    }
    // 发送端已停止后，等待接收线程完成一次新的空队列观察；不把UDP当可靠有序流。
    bool finish_observation() {
        std::unique_lock lock(mutex_);
        const auto generation = ++requested_idle_;
        const bool settled = changed_.wait_for(lock, std::chrono::milliseconds(500),
            [&] { return observed_idle_ >= generation || receiver_error_; }) && !receiver_error_;
        stop_ = true;
        lock.unlock();
        if (worker_.joinable()) worker_.join();
        return settled;
    }
    void resume_observation() {
        std::lock_guard lock(mutex_);
        requested_idle_ = observed_idle_ = 0;
        stop_ = false;
        worker_ = std::thread([this] { run(); });
    }
    // 负对照也经过真实回环socket，验证最终精确计数不会忽略额外释放包。
    bool inject_duplicate_output() {
        std::array<unsigned char, 72> packet;
        { std::lock_guard lock(mutex_); if (packets_.empty()) return false; packet = packets_.back(); }
        SOCKET sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sender == INVALID_SOCKET) return false;
        sockaddr_in address{};
        address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<unsigned short>(port));
        const auto sent = sendto(sender, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
            reinterpret_cast<sockaddr*>(&address), sizeof(address));
        closesocket(sender);
        return sent == packet.size();
    }
    void mark(const char* text) {
        std::lock_guard lock(mutex_);
        marks_.push_back({elapsed_us(), text, packets_.size()});
    }
    void dump() {
        std::lock_guard lock(mutex_);
        std::cerr << "[kmbox-observer] receiver_error=" << receiver_error_ << " gate_expired=" << gate_expired_
            << " requested_idle=" << requested_idle_ << " observed_idle=" << observed_idle_ << '\n';
        for (const auto& event : trace_)
            std::cerr << "[kmbox-observer] recv=" << event.ordinal << " received_us=" << event.received_us
                << " recorded_us=" << event.recorded_us << " bytes=" << event.length
                << " source_port=" << event.source_port << " command=" << event.command
                << " sequence=" << event.sequence << " buttons=" << event.buttons << " ack_mode=" << event.mode << '\n';
        for (const auto& mark : marks_)
            std::cerr << "[kmbox-observer] mark=" << mark.text << " us=" << mark.time_us << " outputs=" << mark.outputs << '\n';
    }
    bool physical_left(bool down, bool right_down = false, bool space = false) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!monitor_destination_.sin_port) return false;
        std::array<unsigned char, 20> report{};
        report[1] = (down ? 1 : 0) | (right_down ? 2 : 0);
        report[10] = space ? 0x2c : 0;
        return sendto(socket_, reinterpret_cast<char*>(report.data()), 20, 0,
            reinterpret_cast<sockaddr*>(&monitor_destination_), sizeof(monitor_destination_)) == 20;
    }
    int port = 0;
    // 0=匹配ACK，1=丢ACK，2=错序号；只影响输出命令。
    std::atomic<int> response{0};
private:
    long long elapsed_us() const {
        return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started_).count();
    }
    void run() {
        while (!stop_) {
            std::size_t idle_generation;
            { std::lock_guard lock(mutex_); idle_generation = requested_idle_; }
            std::array<unsigned char, 72> bytes{};
            sockaddr_in source{}; int size = sizeof(source);
            int received = recvfrom(socket_, reinterpret_cast<char*>(bytes.data()), 72, 0,
                reinterpret_cast<sockaddr*>(&source), &size);
            if (received < 16) {
                const int error = received == SOCKET_ERROR ? WSAGetLastError() : 0;
                if (error == WSAETIMEDOUT) {
                    {
                        std::lock_guard lock(mutex_);
                        observed_idle_ = idle_generation;
                        if (idle_generation) stop_ = true;
                    }
                    changed_.notify_all();
                } else if (error) {
                    { std::lock_guard lock(mutex_); receiver_error_ = error; stop_ = true; }
                    changed_.notify_all();
                }
                continue;
            }
            const auto received_us = elapsed_us();
            auto command = read(bytes.data() + 12);
            bool output = command == left || command == right || command == keyboard || command == move;
            const int mode = response.load();
            {
                std::unique_lock lock(mutex_);
                if (output && !changed_.wait_for(lock, std::chrono::seconds(3), [&] { return !hold_output_ || stop_; })) {
                    gate_expired_ = true;
                    receiver_error_ = WSAETIMEDOUT;
                    stop_ = true;
                }
                trace_.push_back({trace_.size() + 1, received_us, elapsed_us(), received, ntohs(source.sin_port),
                    command, read(bytes.data() + 8), read(bytes.data() + 16), mode});
                if (output) packets_.push_back(bytes);
                if (command == monitor && read(bytes.data() + 4)) {
                    monitor_destination_ = source;
                    monitor_destination_.sin_port = htons(static_cast<unsigned short>(read(bytes.data() + 4)));
                }
            }
            changed_.notify_all();
            // 生产close单向关闭monitor后立即销毁socket；fixture不向已关闭端口发送无消费者ACK。
            if (command == monitor && read(bytes.data() + 4) == 0) continue;
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
    std::condition_variable changed_;
    bool hold_output_ = false;
    bool gate_expired_ = false;
    int receiver_error_ = 0;
    std::size_t requested_idle_ = 0, observed_idle_ = 0;
    const std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    struct Trace {
        std::size_t ordinal;
        long long received_us, recorded_us;
        int length, source_port;
        std::uint32_t command, sequence, buttons;
        int mode;
    };
    struct Mark { long long time_us; const char* text; std::size_t outputs; };
    std::vector<Trace> trace_;
    std::vector<Mark> marks_;
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
void unknown_and_reuse(bool delayed_observer = false, bool duplicate_observer = false) {
    const auto failures_before = failures;
    Device device; auto cfg = config(device); auto mouse = create(cfg);
    expect(mouse && mouse->open(), "unknown测试连接"); if (!mouse) return;
    device.response = 1;
    auto down = mouse->set_left_button(true);
    device.mark("down_return");
    expect(down.disposition == ButtonDisposition::APPLICATION_UNKNOWN && down.datagram_sent && down.cleanup_required,
        "丢ACK的down必须带债务");
    expect(mouse->left_button_faulted(), "未知down锁存fault");
    expect(!mouse->move({1, 1}) && mouse->set_left_button(true).disposition == ButtonDisposition::REJECTED,
        "fault拒绝move和新down");
    auto up = mouse->set_left_button(false);
    device.mark("up_return");
    expect(up.disposition == ButtonDisposition::APPLICATION_UNKNOWN && mouse->left_button_faulted(), "未知up保留fault");
    // UNKNOWN只约束发送侧；先确立前序包观察基线，再观察close新增的唯一释放。
    expect(device.wait_outputs(2), "前序两包必须被fixture观察");
    if (delayed_observer) device.hold_next_output();
    const auto before = device.packets().size();
    expect(before == 2, "close前仅有一次down和一次显式up");
    device.mark("close_begin");
    mouse->close();
    device.mark("close_return");
    device.release_observer();
    expect(device.wait_outputs(before + 1), "close释放必须被fixture观察");
    if (duplicate_observer) {
        expect(device.inject_duplicate_output(), "重复释放负对照必须经真实回环发送");
        expect(device.wait_outputs(before + 2), "负对照额外包必须被fixture观察");
    }
    // producer已经关闭；等待接收线程新的空队列观察后精确检查，不能达到下限就提前成功。
    expect(device.finish_observation(), "fixture收束观察必须有界完成");
    device.mark("observation_settled");
    const auto closed_packets = device.packets();
    const bool single_release = closed_packets.size() == before + 1 &&
        read(closed_packets.back().data() + 12) == left && read(closed_packets.back().data() + 16) == 0 &&
        before == 2 && read(closed_packets.back().data() + 8) == read(closed_packets[1].data() + 8) + 1;
    if (duplicate_observer) {
        expect(!single_release && closed_packets.size() == before + 2, "精确释放判据必须拒绝真实重复包");
    } else expect(single_release, "close仅一次有界释放");
    if (delayed_observer || failures != failures_before) {
        std::cerr << "[kmbox-observer] down=" << static_cast<int>(down.disposition)
            << " down_sent=" << down.datagram_sent << " down_cleanup=" << down.cleanup_required
            << " up=" << static_cast<int>(up.disposition) << " up_sent=" << up.datagram_sent
            << " close_fault=" << mouse->left_button_faulted() << " close_cleanup=" << mouse->left_button_cleanup_required()
            << " error=" << mouse->last_error() << '\n';
        device.dump();
    }
    mouse.reset();
    device.response = 0;
    device.resume_observation();
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
void combined_buttons_and_space() {
    Device device; auto mouse = create(config(device));
    expect(mouse && mouse->open(), "组合输入fake连接"); if (!mouse) return;
    expect(mouse->set_left_button(true).disposition == ButtonDisposition::ACKNOWLEDGED, "left down");
    expect(mouse->set_right_button(true).disposition == ButtonDisposition::ACKNOWLEDGED, "right down");
    expect(mouse->set_left_button(false).disposition == ButtonDisposition::ACKNOWLEDGED, "left up保留right");
    expect(mouse->left_button_cleanup_required(), "right仍有清理责任");
    expect(mouse->move({1, 0}).succeeded, "right期间move");
    expect(mouse->set_right_button(false).disposition == ButtonDisposition::ACKNOWLEDGED, "right up");
    expect(!mouse->left_button_cleanup_required(), "两侧明确释放才清债");
    expect(mouse->set_wasd_keyboard(1).disposition == KeyboardDisposition::ACKNOWLEDGED, "W down");
    expect(mouse->set_space_key(true).disposition == KeyboardDisposition::ACKNOWLEDGED, "Space down");
    expect(mouse->set_wasd_keyboard(0).disposition == KeyboardDisposition::ACKNOWLEDGED, "W up保留Space");
    expect(mouse->set_space_key(false).disposition == KeyboardDisposition::ACKNOWLEDGED, "Space up");
    auto packets = device.packets();
    expect(packets.size() == 9, "组合输入报告数");
    if (packets.size() == 9) {
        expect(read(packets[1].data()+12) == right && read(packets[1].data()+16) == 3, "右键协议保留left位");
        expect(read(packets[2].data()+16) == 2 && read(packets[3].data()+16) == 2, "left up和move保留right位");
        expect(packets[6][18] == 0x1a && packets[6][19] == 0x2c, "Space报告合并W HID usage");
        expect(packets[7][18] == 0x2c && packets[7][19] == 0, "W释放不得吞Space");
        expect(packets[8][18] == 0, "Space释放为空键盘报告");
        for (std::size_t i = 5; i < packets.size(); ++i)
            expect(read(packets[i].data()+12) == keyboard, "Space只用键盘命令，禁止wheel");
    }
    expect(device.physical_left(false, true, true), "fake右键和Space物理报告");
    InputSnapshot snapshot;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    do { mouse->poll_input(snapshot); std::this_thread::yield(); }
    while (!snapshot.state_valid && std::chrono::steady_clock::now() < deadline);
    expect(snapshot.state_valid && snapshot.virtual_keys[2] && snapshot.virtual_keys[0x20],
           "物理右键/Space读回不被软件释放改写");
    device.response = 1;
    expect(mouse->set_right_button(true).disposition == ButtonDisposition::APPLICATION_UNKNOWN, "right丢ACK为UNKNOWN");
    expect(mouse->set_left_button(true).disposition == ButtonDisposition::REJECTED, "right故障禁止新left down");
    device.response = 0;
    mouse->set_left_button(false);
    expect(mouse->left_button_faulted(), "left up不能清除right未知债务");
    expect(mouse->set_right_button(false).disposition == ButtonDisposition::ACKNOWLEDGED && !mouse->left_button_faulted(),
           "right明确up清债");
    device.response = 1;
    expect(mouse->set_space_key(true).disposition == KeyboardDisposition::APPLICATION_UNKNOWN, "Space丢ACK为UNKNOWN");
    device.response = 0;
    expect(mouse->cleanup_wasd_keyboard().disposition == KeyboardDisposition::ACKNOWLEDGED, "共用键盘cleanup释放Space");
    expect(device.packets().back()[18] == 0, "Space未知清理报告为空");
    mouse->close();
    expect(mouse->set_space_key(true).disposition == KeyboardDisposition::REJECTED,
           "关闭后无owner禁止Space");
    device.response = 1;
    mouse = create(config(device));
    expect(mouse && mouse->open(), "right重建准备"); if (!mouse) return;
    expect(mouse->set_right_button(true).disposition == ButtonDisposition::APPLICATION_UNKNOWN, "right重建前未知down");
    mouse->close(); mouse.reset();
    device.response = 0;
    mouse = create(config(device));
    expect(mouse && mouse->open() && mouse->left_button_faulted(), "重建保留right债务"); if (!mouse) return;
    expect(mouse->set_right_button(false).disposition == ButtonDisposition::ACKNOWLEDGED && !mouse->left_button_faulted(),
           "重建后right up解除准确位债务");
    mouse->close();
}
void unsupported() {
    MouseConfig cfg;
    auto mouse = create(cfg);
    expect(mouse && mouse->open(), "无输出Win32可打开");
    expect(mouse->set_right_button(true).disposition == ButtonDisposition::UNSUPPORTED &&
        mouse->set_space_key(true).disposition == KeyboardDisposition::UNSUPPORTED, "其他后端显式拒绝右键与Space");
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
int main(int argc, char** argv) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2,2), &data)) return 2;
    if (argc == 2 && std::string_view(argv[1]) == "--delayed-close-observer") unknown_and_reuse(true);
    else if (argc == 2 && std::string_view(argv[1]) == "--duplicate-close-observer") unknown_and_reuse(true, true);
    else { normal_and_physical(); unknown_and_reuse(); unsupported(); serialized_release(); combined_buttons_and_space(); }
    WSACleanup();
    return failures ? 1 : 0;
}
