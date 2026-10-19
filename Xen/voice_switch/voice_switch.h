#ifndef VOICE_SWITCH_H
#define VOICE_SWITCH_H
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace voice_switch {
struct Config {
    std::string bind_address = "127.0.0.1";
    std::string allowed_peer_ipv4;
    std::uint16_t port = 5014;
};
struct Snapshot {
    bool running = false;
    bool connected = false;
    std::uint64_t connection_epoch = 0;
    std::uint16_t port = 0;
    std::string error;
};
struct Message {
    std::uint64_t connection_epoch = 0;
    std::string body;
};
// 生命周期由 App 串行调用；poll/send/snapshot 不执行网络等待。
class Server final {
public:
    Server();
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    bool start(const Config& config) noexcept;
    void stop() noexcept;
    Snapshot snapshot() const;
    std::vector<Message> poll();
    // 仅入队成功，不代表对端收到或业务完成；旧连接响应不能发送到新连接。
    bool send(std::uint64_t connection_epoch, std::string body) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
#endif
