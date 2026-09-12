#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <WinSock2.h>
#include <WS2tcpip.h>

#ifdef ERROR
#undef ERROR
#endif

#include "log/log.h"
#include "mouse/kmbox_net_internal.h"
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
    expect(!mouse->supports_wasd_keyboard() &&
        mouse->set_wasd_keyboard(1).disposition == KeyboardDisposition::UNSUPPORTED &&
        !mouse->set_wasd_event_subscription(true),
        "非 KMBOX 后端必须显式不支持键盘能力，不能假成功");
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

enum class AckMode {
    VALID,
    NONE,
    WRONG_SOURCE,
    WRONG_COMMAND,
    WRONG_SEQUENCE,
};

class FakeKmboxDevice {
public:
    explicit FakeKmboxDevice(std::vector<AckMode> responses,
                            int keyboard_release_acks_to_drop = 0)
        : responses_(std::move(responses)),
          keyboard_release_acks_to_drop_(keyboard_release_acks_to_drop) {
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
            if (keyboard_release_acks_to_drop_ > 0 && received == 28 &&
                read_u32_le(buffer.data() + 12U) == 0x123c2c2fU &&
                std::all_of(buffer.begin() + kHeaderBytes,
                            buffer.begin() + received,
                            [](auto value) { return value == 0; })) {
                --keyboard_release_acks_to_drop_;
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
    int keyboard_release_acks_to_drop_ = 0;
    std::thread worker_;
    mutable std::mutex packets_mutex_;
    std::vector<std::vector<std::uint8_t>> packets_;
    std::mutex monitor_mutex_;
    std::condition_variable monitor_ready_cv_;
    sockaddr_in monitor_destination_{};
    bool monitor_ready_ = false;
};


MouseConfig config_for(int port) {
    MouseConfig config;
    config.backend = MouseBackend::KMBOX_NET;
    config.allow_send_input = true;
    config.kmbox_ip = "127.0.0.1";
    config.kmbox_port = port;
    config.kmbox_uuid = "A1B2C3D4";
    config.kmbox_connect_timeout_ms = 100;
    config.kmbox_command_timeout_ms = 40;
    return config;
}
void test_packets_and_owned_cleanup() {
    FakeKmboxDevice device(std::vector<AckMode>(9, AckMode::VALID));
    auto mouse = create_test_mouse(config_for(device.port()));
    expect(mouse && mouse->open() && mouse->output_owner_exclusive(), "工厂独占所有者打开本机回环连接");
    expect(mouse->supports_wasd_keyboard(), "KMBOX 支持键盘能力");
    auto competing = create_test_mouse(config_for(device.port()));
    expect(!competing->open(), "第二个所有者不得取得输出权限");
    expect(competing->set_wasd_keyboard(1).disposition == KeyboardDisposition::REJECTED,
           "无所有权时拒绝键盘输出");
    expect(mouse->set_wasd_keyboard(3).disposition == KeyboardDisposition::ACKNOWLEDGED, "WA 整报告收到 ACK");
    expect(mouse->set_wasd_keyboard(10).disposition == KeyboardDisposition::ACKNOWLEDGED, "AD 整报告替换 WA");
    expect(mouse->set_wasd_mask(1, true).disposition == KeyboardDisposition::ACKNOWLEDGED, "屏蔽 W");
    expect(mouse->set_wasd_mask(2, true).disposition == KeyboardDisposition::ACKNOWLEDGED, "屏蔽 A");
    expect(mouse->set_wasd_mask(4, false).disposition == KeyboardDisposition::REJECTED, "不得解除不属于本连接的 S 屏蔽");
    expect(mouse->set_wasd_mask(3, true).disposition == KeyboardDisposition::REJECTED, "屏蔽接口只接受单键");
    expect(mouse->set_wasd_keyboard(16).disposition == KeyboardDisposition::REJECTED, "拒绝非 WASD 位");
    mouse->close();
    device.finish();
    const auto packets = device.packets();
    expect(packets.size() == 9, "握手、监听、两份报告、两次屏蔽、释放和两次解除屏蔽");
    if (packets.size() != 9) return;
    expect(packets[2].size() == 28 && read_u32_le(packets[2].data()+12) == 0x123c2c2f &&
        packets[2][16] == 0 && packets[2][17] == 0 && packets[2][18] == 0x1a && packets[2][19] == 0x04,
        "官方 12 字节键盘载荷包含 HID W、A");
    expect(packets[3][18] == 0x04 && packets[3][19] == 0x07 && packets[3][20] == 0, "AD 替换后不残留 W");
    expect(packets[4].size() == 16 && read_u32_le(packets[4].data()+12) == 0x23234343 &&
        read_u32_le(packets[4].data()+4) == 0x1a00, "官方逐键屏蔽报头");
    expect(read_u32_le(packets[6].data()+12) == 0x123c2c2f &&
        std::all_of(packets[6].begin()+16, packets[6].end(), [](auto b){return b == 0;}), "关闭时释放软件键报告");
    for (std::size_t i = 7; i < 9; ++i)
        expect(read_u32_le(packets[i].data()+12) == 0x23344343 && read_u32_le(packets[i].data()+4) != 0,
               "关闭只解除本连接的 usage 屏蔽，禁止 unmask_all");
}
void test_all_combinations_and_disabled_output() {
    FakeKmboxDevice device(std::vector<AckMode>(19, AckMode::VALID));
    auto mouse = create_test_mouse(config_for(device.port()));
    expect(mouse->open(), "组合测试连接成功");
    for (std::uint8_t mask = 0; mask < 16; ++mask)
        expect(mouse->set_wasd_keyboard(mask).disposition == KeyboardDisposition::ACKNOWLEDGED,
               "所有 WASD 报告可接受，包括防御性冲突组合");
    mouse->close(); device.finish();
    const auto packets = device.packets();
    const std::vector<std::vector<std::uint8_t>> expected = {
        {},{0x1a},{0x04},{0x1a,0x04},{0x16},{0x1a,0x16},{0x04,0x16},{0x1a,0x04,0x16},
        {0x07},{0x1a,0x07},{0x04,0x07},{0x1a,0x04,0x07},{0x16,0x07},{0x1a,0x16,0x07},
        {0x04,0x16,0x07},{0x1a,0x04,0x16,0x07}};
    expect(packets.size() == 19, "所有组合及最后释放");
    if (packets.size() == 19) for (std::size_t i=0; i<16; ++i) {
        std::vector<std::uint8_t> usages;
        for (std::size_t j=18; j<28; ++j) if(packets[i+2][j]) usages.push_back(packets[i+2][j]);
        expect(usages == expected[i], "HID 报告精确对应组合且无残留 usage");
    }
    FakeKmboxDevice readonly_device({AckMode::VALID,AckMode::VALID,AckMode::VALID});
    auto config = config_for(readonly_device.port()); config.allow_send_input = false;
    mouse = create_test_mouse(config);
    expect(mouse->open(), "只读设备可连接");
    expect(mouse->set_wasd_keyboard(1).disposition == KeyboardDisposition::REJECTED &&
        mouse->set_wasd_mask(1,true).disposition == KeyboardDisposition::REJECTED,
        "输出禁用时不得发送键盘或屏蔽命令");
    mouse->close(); readonly_device.finish();
    const auto readonly_packets = readonly_device.packets();
    expect(readonly_packets.size() == 3 && read_u32_le(readonly_packets.back().data()+12) == kMonitorCommand,
        "未使用键盘能力时关闭只停用监听");
}
void test_ack_loss_cleanup() {
    FakeKmboxDevice device({AckMode::VALID,AckMode::VALID,AckMode::NONE,AckMode::VALID,
        AckMode::WRONG_SEQUENCE,AckMode::VALID,AckMode::VALID});
    auto mouse = create_test_mouse(config_for(device.port()));
    expect(mouse->open(), "超时测试连接成功");
    const auto report = mouse->set_wasd_keyboard(9);
    expect(report.disposition == KeyboardDisposition::APPLICATION_UNKNOWN && report.datagram_sent,
        "已发送但无 ACK 的报告为未知，不是未应用");
    expect(mouse->set_wasd_mask(1,true).disposition == KeyboardDisposition::ACKNOWLEDGED, "部分屏蔽的首键收到 ACK");
    expect(mouse->set_wasd_mask(8,true).disposition == KeyboardDisposition::APPLICATION_UNKNOWN, "部分屏蔽的次键状态未知");
    // 显式清理的报告和 W 成功；D 没有假设备 ACK，保持未知。
    const auto cleanup = mouse->cleanup_wasd_keyboard();
    expect(cleanup.disposition == KeyboardDisposition::APPLICATION_UNKNOWN, "部分清理保留未知结果");
    mouse->close();
    device.finish();
}
void test_reopen_reconciles_unknown_debt() {
    // 首次关闭丢失释放 ACK：屏蔽责任保留。新握手后的释放也丢失 ACK，
    // 因此必须在监听/READY 前拒绝打开；第三次打开完成恢复。
    FakeKmboxDevice device({AckMode::VALID,AckMode::VALID,AckMode::VALID,AckMode::VALID,
        AckMode::NONE,AckMode::VALID,AckMode::VALID,AckMode::NONE,
        AckMode::VALID,AckMode::VALID,AckMode::VALID,AckMode::VALID,AckMode::VALID});
    auto mouse = create_test_mouse(config_for(device.port()));
    expect(mouse->open(), "重开测试连接成功");
    mouse->set_wasd_keyboard(1);
    mouse->set_wasd_mask(1,true);
    mouse->close();
    expect(!mouse->open(), "重连时旧软件键释放未知则拒绝 READY");
    expect(mouse->open(), "后续重连先清理软件键和屏蔽再进入 READY");
    mouse->close(); device.finish();
    const auto packets = device.packets();
    expect(packets.size() == 13, "重连清理的数据包数量符合预期");
    if (packets.size() == 13) {
        expect(read_u32_le(packets[5].data()+12) == kMonitorCommand,
            "软件键释放失败不得解除物理 W 屏蔽");
        expect(read_u32_le(packets[8].data()+12) == kConnectCommand &&
            read_u32_le(packets[9].data()+12) == 0x123c2c2f &&
            read_u32_le(packets[10].data()+12) == 0x23344343 &&
            read_u32_le(packets[11].data()+12) == kMonitorCommand,
            "新连接先释放再解除本连接屏蔽，最后启动监听");
    }
}
void test_factory_recreation_reconciles_endpoint_keyboard_debt() {
    // 只丢软件键释放的 ACK，握手和监听仍正常；遗忘债务的旧实现会错误进入 READY。
    std::vector<AckMode> responses(20, AckMode::VALID);
    responses[12] = AckMode::NONE;
    FakeKmboxDevice device(std::move(responses), 2);
    auto config = config_for(device.port());
    auto mouse = create_test_mouse(config);
    expect(mouse && mouse->open(), "跨 factory 恢复测试连接成功");
    if (!mouse) return;
    expect(mouse->set_wasd_keyboard(9).disposition == KeyboardDisposition::ACKNOWLEDGED &&
        mouse->set_wasd_mask(1, true).disposition == KeyboardDisposition::ACKNOWLEDGED &&
        mouse->set_wasd_mask(8, true).disposition == KeyboardDisposition::ACKNOWLEDGED,
        "旧 owner 持有软件 WD 与 W、D 屏蔽");
    mouse.reset();

    // 超时和 UUID 大小写改变仍是同一协议端点，必须继承已销毁 owner 的责任。
    config.kmbox_command_timeout_ms = 60;
    config.kmbox_uuid = "a1b2c3d4";
    mouse = create_test_mouse(config);
    expect(mouse && !mouse->open() && !mouse->output_owner_exclusive(),
        "旧 owner 已销毁，释放仍未知的新 owner 必须拒绝 READY 并释放 lease");
    mouse.reset();

    mouse = create_test_mouse(config);
    expect(mouse && !mouse->open(), "软件键释放已确认但 D 解除屏蔽未知仍拒绝 READY");
    mouse.reset();

    mouse = create_test_mouse(config);
    expect(mouse && mouse->open() && mouse->output_owner_exclusive(),
        "下个 factory owner 确认剩余 D 清理后才能进入 READY");
    mouse.reset();
    mouse = create_test_mouse(config);
    expect(mouse && mouse->open(), "清理完成后再次重建没有残留债务");
    mouse.reset();
    device.finish();

    const auto packets = device.packets();
    expect(packets.size() == 20, "跨 factory 清理和恢复的数据包数量符合协议边界");
    if (packets.size() != 20) return;
    const auto command = [&](std::size_t index) {
        return read_u32_le(packets[index].data() + 12U);
    };
    const auto is_release = [&](std::size_t index) {
        return packets[index].size() == 28 && command(index) == 0x123c2c2fU &&
            std::all_of(packets[index].begin() + kHeaderBytes, packets[index].end(),
                        [](auto value) { return value == 0; });
    };
    expect(is_release(5) && command(6) == kMonitorCommand &&
        command(7) == kConnectCommand && is_release(8),
        "关闭和首次重建只尝试释放软件键，未确认前不解除屏蔽或启动新监听");
    expect(command(9) == kConnectCommand && is_release(10) &&
        command(11) == 0x23344343U && read_u32_le(packets[11].data() + 4U) == 0x1a00U &&
        command(12) == 0x23344343U && read_u32_le(packets[12].data() + 4U) == 0x0700U,
        "软件键明确释放后只解除继承的 W、D，禁止 unmask_all 或触碰 A、S");
    expect(command(13) == kConnectCommand && command(14) == 0x23344343U &&
        read_u32_le(packets[14].data() + 4U) == 0x0700U && command(15) == kMonitorCommand,
        "部分清理跨销毁只保留未确认的 D，不重复软件键和 W 清理");
    expect(command(17) == kConnectCommand && command(18) == kMonitorCommand,
        "全部清理 ACK 后删除端点债务，再次重建直接握手和监听");
}
void test_event_journal() {
    FakeKmboxDevice device({AckMode::VALID,AckMode::VALID});
    auto mouse = create_test_mouse(config_for(device.port()));
    expect(mouse->open(), "事件测试连接成功");
    WasdEventCursor first, second;
    WasdEventBatch batch;
    expect(mouse->read_wasd_events(first,batch) && !batch.subscribed && batch.count == 0, "事件记录默认关闭");
    expect(mouse->set_wasd_event_subscription(true), "显式启用订阅");
    device.send_monitor(0,{0x1a});
    device.send_monitor(0,{0x1a,0x04});
    device.send_monitor(0,{});
    device.send_monitor(0,{0x1a,0x16});
    device.send_monitor(0,{1});
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(1);
    std::vector<WasdEvent> events;
    while (events.size() < 5 && std::chrono::steady_clock::now()<deadline) {
        mouse->read_wasd_events(first,batch);
        events.insert(events.end(),batch.events.begin(),batch.events.begin()+batch.count);
        std::this_thread::yield();
    }
    expect(events.size() == 5, "消费者读取前保留快速边沿");
    if (events.size() == 5) {
        expect(events[0].held_mask == 1 && events[1].held_mask == 3 && events[2].held_mask == 0 &&
            events[3].held_mask == 5 && events[3].state_valid, "W、WA、释放与 WS 冲突保留事实掩码");
        expect(!events[4].state_valid && events[4].held_mask == 5, "HID 滚动错误无效且不伪造释放");
        expect(events[0].received_at_steady_ns > 0 && events[4].sequence > events[0].sequence, "事件包含时间和序号");
    }
    mouse->read_wasd_events(second,batch);
    expect(batch.count == 5, "独立游标读取不破坏事件");
    InputSnapshot snapshot;
    mouse->poll_input(snapshot);
    expect(!snapshot.state_valid && snapshot.virtual_keys['W'], "滚动错误保留键态但快照无效");
    // 等待快照序号推进后再发送，避免回环 UDP 丢包被误认为环形缓冲溢出。
    for (int i=0; i<270; ++i) {
        mouse->poll_input(snapshot); const auto prior = snapshot.sequence;
        device.send_monitor(0, {(i%2) ? std::uint8_t(0x04) : std::uint8_t(0x1a)});
        const auto until = std::chrono::steady_clock::now()+std::chrono::seconds(1);
        do { mouse->poll_input(snapshot); std::this_thread::yield(); }
        while (snapshot.sequence == prior && std::chrono::steady_clock::now()<until);
    }
    mouse->read_wasd_events(first,batch);
    expect(batch.gap && batch.count == 64, "有界溢出显式报告");
    const auto old_epoch = first.epoch;
    mouse->set_wasd_event_subscription(false);
    mouse->set_wasd_event_subscription(true);
    mouse->read_wasd_events(first,batch);
    expect(batch.gap && batch.count == 0 && first.epoch != old_epoch, "订阅代际重置事实");
    mouse->close();
}
} // namespace
int main() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2,2), &data) != 0) return 2;
    test_packets_and_owned_cleanup();
    test_all_combinations_and_disabled_output();
    test_ack_loss_cleanup();
    test_reopen_reconciles_unknown_debt();
    test_factory_recreation_reconciles_endpoint_keyboard_debt();
    test_event_journal();
    test_win32_input_stays_unverified_without_owned_source();
    WSACleanup();
    std::cout << "KMBOX 键盘测试失败数=" << failures << '\n';
    return failures ? 1 : 0;
}
