#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <WS2tcpip.h>
#include "voice_switch/voice_switch.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace voice_switch {
namespace {
constexpr std::size_t kMaxLine = 65536;
constexpr std::size_t kMaxQueue = 32;
bool json_line(const std::string& line) noexcept {
    if (line.empty() || line.size() > kMaxLine || line.find_first_of("\r\n") != std::string::npos) return false;
    try {
        const auto value = nlohmann::json::parse(line, [](int depth, nlohmann::json::parse_event_t, nlohmann::json&) {
            if (depth > 16) throw std::runtime_error("JSON depth limit");
            return true;
        }, false);
        return value.is_object();
    } catch (...) { return false; }
}
bool nonblocking(SOCKET socket) { u_long mode = 1; return ioctlsocket(socket, FIONBIO, &mode) == 0; }
void close_socket(SOCKET& socket) noexcept {
    if (socket != INVALID_SOCKET) { closesocket(socket); socket = INVALID_SOCKET; }
}
}
struct Server::Impl {
    mutable std::mutex mutex;
    Snapshot state;
    std::deque<std::string> outgoing;
    std::vector<Message> incoming;
    std::atomic<bool> stopping{false};
    std::thread worker;
    SOCKET listener = INVALID_SOCKET, client = INVALID_SOCKET;
    bool winsock = false;
    unsigned long allowed_peer = 0;
    std::string input, output;
    std::size_t output_offset = 0;

    void disconnect(const char* error) {
        close_socket(client);
        input.clear(); output.clear(); output_offset = 0;
        std::lock_guard lock(mutex);
        state.connected = false;
        if (error) state.error = error;
        incoming.clear(); outgoing.clear();
    }
    void run() noexcept {
        try {
            while (!stopping) {
                fd_set reads, writes;
                FD_ZERO(&reads); FD_ZERO(&writes); FD_SET(listener, &reads);
                if (client != INVALID_SOCKET) {
                    FD_SET(client, &reads);
                    if (output.empty()) {
                        std::lock_guard lock(mutex);
                        if (!outgoing.empty()) { output = std::move(outgoing.front()); outgoing.pop_front(); }
                    }
                    if (!output.empty()) FD_SET(client, &writes);
                }
                timeval timeout{0, 10000};
                if (select(0, &reads, &writes, nullptr, &timeout) == SOCKET_ERROR) {
                    disconnect("语音监听失败"); break;
                }
                if (FD_ISSET(listener, &reads)) {
                    sockaddr_in peer{}; int size = sizeof(peer);
                    SOCKET accepted = accept(listener, reinterpret_cast<sockaddr*>(&peer), &size);
                    if (accepted != INVALID_SOCKET) {
                        if (client != INVALID_SOCKET || peer.sin_addr.s_addr != allowed_peer || !nonblocking(accepted)) close_socket(accepted);
                        else {
                            std::lock_guard lock(mutex);
                            if (state.connection_epoch == std::numeric_limits<std::uint64_t>::max()) close_socket(accepted);
                            else {
                                client = accepted; ++state.connection_epoch; state.connected = true; state.error.clear();
                                incoming.clear(); outgoing.clear();
                            }
                        }
                    }
                }
                if (client == INVALID_SOCKET) continue;
                if (FD_ISSET(client, &reads)) {
                    char buffer[4096];
                    const int received = recv(client, buffer, sizeof(buffer), 0);
                    if (received == 0 || (received < 0 && WSAGetLastError() != WSAEWOULDBLOCK)) {
                        disconnect("语音连接已断开"); continue;
                    }
                    if (received > 0) {
                        input.append(buffer, static_cast<std::size_t>(received));
                        bool invalid = false;
                        for (;;) {
                            const auto newline = input.find('\n');
                            if (newline == std::string::npos) break;
                            auto line = input.substr(0, newline); input.erase(0, newline + 1);
                            if (!line.empty() && line.back() == '\r') line.pop_back();
                            if (!json_line(line)) { invalid = true; break; }
                            std::lock_guard lock(mutex);
                            if (incoming.size() >= kMaxQueue) { invalid = true; break; }
                            incoming.push_back({state.connection_epoch, std::move(line)});
                        }
                        if (invalid || input.size() > kMaxLine) { disconnect("语音消息无效或队列已满"); continue; }
                    }
                }
                if (FD_ISSET(client, &writes) && !output.empty()) {
                    const int sent = ::send(client, output.data() + output_offset, static_cast<int>(output.size() - output_offset), 0);
                    if (sent < 0 && WSAGetLastError() != WSAEWOULDBLOCK) { disconnect("语音发送失败"); continue; }
                    if (sent > 0) {
                        output_offset += static_cast<std::size_t>(sent);
                        if (output_offset == output.size()) { output.clear(); output_offset = 0; }
                    }
                }
            }
        } catch (...) { disconnect("语音传输异常"); }
        disconnect(nullptr);
        std::lock_guard lock(mutex); state.running = false;
    }
};
Server::Server() : impl_(std::make_unique<Impl>()) {}
Server::~Server() { stop(); }
bool Server::start(const Config& config) noexcept {
    stop();
    try {
        sockaddr_in address{}, allowed{}; address.sin_family = AF_INET; address.sin_port = htons(config.port);
        const auto peer = config.allowed_peer_ipv4.empty() ? std::string("127.0.0.1") : config.allowed_peer_ipv4;
        if (inet_pton(AF_INET, config.bind_address.c_str(), &address.sin_addr) != 1 ||
            inet_pton(AF_INET, peer.c_str(), &allowed.sin_addr) != 1 ||
            (config.bind_address != "127.0.0.1" && config.allowed_peer_ipv4.empty())) throw std::runtime_error("语音监听地址或来源限制无效");
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("语音网络初始化失败");
        impl_->winsock = true;
        impl_->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        BOOL exclusive = TRUE;
        if (impl_->listener == INVALID_SOCKET ||
            setsockopt(impl_->listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) != 0 ||
            bind(impl_->listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(impl_->listener, 1) != 0 || !nonblocking(impl_->listener)) throw std::runtime_error("语音端口绑定失败");
        int size = sizeof(address);
        if (getsockname(impl_->listener, reinterpret_cast<sockaddr*>(&address), &size) != 0) throw std::runtime_error("语音监听端口不可用");
        impl_->allowed_peer = allowed.sin_addr.s_addr;
        { std::lock_guard lock(impl_->mutex); impl_->state.running = true; impl_->state.port = ntohs(address.sin_port); impl_->state.error.clear(); }
        impl_->stopping = false;
        impl_->worker = std::thread([this] { impl_->run(); });
        return true;
    } catch (const std::exception& error) {
        stop(); std::lock_guard lock(impl_->mutex); impl_->state.error = error.what(); return false;
    } catch (...) {
        stop(); std::lock_guard lock(impl_->mutex); impl_->state.error = "语音服务启动异常"; return false;
    }
}
void Server::stop() noexcept {
    impl_->stopping = true;
    if (impl_->worker.joinable()) impl_->worker.join();
    close_socket(impl_->listener);
    if (impl_->winsock) { WSACleanup(); impl_->winsock = false; }
    std::lock_guard lock(impl_->mutex);
    impl_->state.running = false; impl_->state.connected = false;
    impl_->incoming.clear(); impl_->outgoing.clear();
}
Snapshot Server::snapshot() const { std::lock_guard lock(impl_->mutex); return impl_->state; }
std::vector<Message> Server::poll() {
    std::lock_guard lock(impl_->mutex);
    std::vector<Message> messages; messages.swap(impl_->incoming); return messages;
}
bool Server::send(std::uint64_t epoch, std::string body) noexcept {
    try {
        if (!json_line(body)) return false;
        body.push_back('\n');
        std::lock_guard lock(impl_->mutex);
        if (!impl_->state.connected || impl_->state.connection_epoch != epoch || impl_->outgoing.size() >= kMaxQueue) return false;
        impl_->outgoing.push_back(std::move(body)); return true;
    } catch (...) { return false; }
}
}
