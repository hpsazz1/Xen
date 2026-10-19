#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <WS2tcpip.h>
#include "voice_switch/voice_switch.h"
#include <chrono>
#include <iostream>
#include <thread>
namespace {
int failures = 0;
void expect(bool condition, const char* message) { if (!condition) { ++failures; std::cerr << message << '\n'; } }
template<class Predicate> bool wait(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}
SOCKET connect_to(std::uint16_t port) {
    SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) { closesocket(client); return INVALID_SOCKET; }
    DWORD timeout = 2000; setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    return client;
}
}
int main() {
    voice_switch::Server server;
    voice_switch::Config config; config.port = 0;
    expect(server.start(config), "回环服务启动");
    if (!server.snapshot().running) return 1;
    auto client = connect_to(server.snapshot().port);
    expect(client != INVALID_SOCKET && wait([&] { return server.snapshot().connected; }), "生产服务接受回环连接");
    const auto epoch = server.snapshot().connection_epoch;
    const std::string first = "{\"type\":\"ready\"}";
    ::send(client, first.data(), 5, 0);
    expect(server.poll().empty(), "半行不提交消息");
    const auto tail = first.substr(5) + "\n{\"type\":\"select\",\"id\":1}\n";
    ::send(client, tail.data(), static_cast<int>(tail.size()), 0);
    std::vector<voice_switch::Message> received;
    expect(wait([&] { auto batch = server.poll(); for (auto& item : batch) received.push_back(std::move(item)); return received.size() == 2; }), "拆包粘包组成两条消息");
    expect(received.size() == 2 && received[0].body == first && received[0].connection_epoch == epoch, "内容与连接身份准确");
    expect(!server.send(epoch + 1, "{}") && !server.send(epoch, "[]") && !server.send(epoch, "{\n}"), "拒绝旧连接与无效JSON行");
    expect(server.send(epoch, "{\"ok\":true}"), "结果进入发送队列");
    char buffer[256]{}; const int count = recv(client, buffer, sizeof(buffer), 0);
    expect(count > 0 && std::string(buffer, count) == "{\"ok\":true}\n", "生产网络发送结果并分帧");
    closesocket(client);
    expect(wait([&] { return !server.snapshot().connected; }), "断开可观察");
    expect(!server.send(epoch, "{}") && server.poll().empty(), "断线清队列且不接收迟到响应");
    client = connect_to(server.snapshot().port);
    expect(wait([&] { return server.snapshot().connected; }) && server.snapshot().connection_epoch > epoch, "重连建立新代次");
    expect(!server.send(epoch, "{}"), "重连后不发送旧响应");
    const std::string bad = "not json\n"; ::send(client, bad.data(), static_cast<int>(bad.size()), 0);
    expect(wait([&] { return !server.snapshot().connected; }), "无效协议关闭当前连接");
    closesocket(client);
    client = connect_to(server.snapshot().port);
    expect(wait([&] { return server.snapshot().connected; }), "协议错误后仍能重新连接");
    const std::string oversized(65537, 'x'); ::send(client, oversized.data(), static_cast<int>(oversized.size()), 0);
    expect(wait([&] { return !server.snapshot().connected; }), "超长未完成行有界拒绝");
    closesocket(client);
    server.stop(); expect(!server.snapshot().running, "停止服务回收工作线程");
    config.bind_address = "0.0.0.0";
    expect(!server.start(config), "局域网监听必须明确允许的来源");
    return failures == 0 ? 0 : 1;
}
