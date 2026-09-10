#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <bcrypt.h>

#include "source_context/source_context_internal.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

namespace source_context {
namespace {
constexpr std::size_t kPayloadBytes = 104;

bool valid_process(std::string_view name) noexcept {
    if (name.size() < 5 || name.size() > 63) return false;
    for (const unsigned char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
    }
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
    return name[name.size() - 4] == '.' && lower(name[name.size() - 3]) == 'e' &&
           lower(name[name.size() - 2]) == 'x' && lower(name.back()) == 'e';
}

bool random_bytes(std::span<std::uint8_t> output) noexcept {
    return BCryptGenRandom(nullptr, output.data(), static_cast<ULONG>(output.size()),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
}

bool hmac(std::span<const std::uint8_t> input, std::string_view token,
          std::array<std::uint8_t, 32>& output) noexcept {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool success = false;
    try {
        if (token.size() < 32 || token.size() > 1024) return false;
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr,
                                        BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0) return false;
        DWORD object_size = 0, written = 0;
        if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &written, 0) >= 0) {
            std::vector<std::uint8_t> object(object_size);
            if (BCryptCreateHash(algorithm, &hash, object.data(), object_size,
                    reinterpret_cast<PUCHAR>(const_cast<char*>(token.data())),
                    static_cast<ULONG>(token.size()), 0) >= 0) {
                success = BCryptHashData(hash,
                    const_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()), 0) >= 0 &&
                    BCryptFinishHash(hash, output.data(), static_cast<ULONG>(output.size()), 0) >= 0;
                BCryptDestroyHash(hash);
                hash = nullptr;
            }
        }
    } catch (...) { success = false; }
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return success;
}

void put_u64(detail::Packet& packet, std::size_t offset, std::uint64_t value) noexcept {
    for (std::size_t i = 0; i < 8; ++i) packet[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
}
std::uint64_t get_u64(std::span<const std::uint8_t> packet, std::size_t offset) noexcept {
    std::uint64_t result = 0;
    for (std::size_t i = 0; i < 8; ++i) result |= std::uint64_t(packet[offset + i]) << (i * 8);
    return result;
}

class Socket final {
public:
    SOCKET handle = INVALID_SOCKET;
    bool wsa = false;
    ~Socket() { close(); }
    void close() noexcept {
        if (handle != INVALID_SOCKET) closesocket(handle);
        handle = INVALID_SOCKET;
        if (wsa) WSACleanup();
        wsa = false;
    }
    bool open(const SourceContextConfig& config, bool server) noexcept {
        close();
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
        wsa = true;
        handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (handle == INVALID_SOCKET) { close(); return false; }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(config.port);
        if (InetPtonA(AF_INET, config.host.c_str(), &address.sin_addr) != 1) { close(); return false; }
        const int result = server
            ? bind(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address))
            : connect(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        if (result == SOCKET_ERROR) { close(); return false; }
        return true;
    }
};

int readable(SOCKET handle, int timeout_ms) noexcept {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(handle, &set);
    timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    return select(0, &set, nullptr, nullptr, &timeout);
}

detail::Focus foreground_focus(std::string_view process_name) noexcept {
    const HWND window = GetForegroundWindow();
    if (!window) return detail::Focus::Unknown;
    DWORD process_id = 0;
    if (!GetWindowThreadProcessId(window, &process_id) || process_id == 0) return detail::Focus::Unknown;
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (!process) return detail::Focus::Unknown;
    wchar_t path[32768];
    DWORD size = static_cast<DWORD>(std::size(path));
    const bool queried = QueryFullProcessImageNameW(process, 0, path, &size) != 0;
    CloseHandle(process);
    // 查询期间窗口切换就不给焦点许可，不把查询前后的不同状态拼成一条证据。
    if (!queried || GetForegroundWindow() != window) return detail::Focus::Unknown;
    std::size_t start = size;
    while (start > 0 && path[start - 1] != L'\\' && path[start - 1] != L'/') --start;
    char name[64]{};
    if (size - start >= std::size(name)) return detail::Focus::Background;
    for (std::size_t i = start; i < size; ++i) {
        if (path[i] > 127) return detail::Focus::Background;
        name[i - start] = static_cast<char>(path[i]);
    }
    return detail::exact_process_match(process_name, name)
        ? detail::Focus::Foreground : detail::Focus::Background;
}
} // namespace

namespace detail {
bool valid_config(const SourceContextConfig& config) noexcept {
    return config.enabled && !config.host.empty() && config.port != 0 && config.token.size() >= 32 &&
        config.token.size() <= 1024 && valid_process(config.process_name) &&
        config.ttl_ms >= 20 && config.ttl_ms <= 2000;
}
bool exact_process_match(std::string_view expected, std::string_view actual) noexcept {
    if (!valid_process(expected) || !valid_process(actual) || expected.size() != actual.size()) return false;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
        if (lower(expected[i]) != lower(actual[i])) return false;
    }
    return true;
}
bool encode(const Message& message, std::string_view token, Packet& packet) noexcept {
    if (!valid_process(message.process_name) || static_cast<int>(message.focus) > 2 ||
        (message.response && (!message.session || !message.sequence)) ||
        (!message.response && (message.session || message.sequence || message.focus != Focus::Unknown)) ||
        std::all_of(message.nonce.begin(), message.nonce.end(), [](auto x) { return x == 0; })) return false;
    packet.fill(0);
    packet[0] = 'X'; packet[1] = 'S'; packet[2] = 'C'; packet[3] = 'X';
    packet[4] = 1;
    packet[5] = message.response ? 2 : 1;
    packet[6] = static_cast<std::uint8_t>(message.focus);
    std::copy(message.nonce.begin(), message.nonce.end(), packet.begin() + 8);
    put_u64(packet, 24, message.session);
    put_u64(packet, 32, message.sequence);
    std::copy(message.process_name.begin(), message.process_name.end(), packet.begin() + 40);
    std::array<std::uint8_t, 32> tag{};
    if (!hmac(std::span(packet).first(kPayloadBytes), token, tag)) return false;
    std::copy(tag.begin(), tag.end(), packet.begin() + kPayloadBytes);
    return true;
}
bool decode(std::span<const std::uint8_t> packet, std::string_view token, Message& message) noexcept {
    try {
        if (packet.size() != kPacketBytes || packet[0] != 'X' || packet[1] != 'S' ||
            packet[2] != 'C' || packet[3] != 'X' || packet[4] != 1 ||
            (packet[5] != 1 && packet[5] != 2) || packet[6] > 2 || packet[7] != 0) return false;
        std::array<std::uint8_t, 32> tag{};
        if (!hmac(packet.first(kPayloadBytes), token, tag)) return false;
        unsigned difference = 0;
        for (std::size_t i = 0; i < tag.size(); ++i) difference |= tag[i] ^ packet[kPayloadBytes + i];
        if (difference) return false;
        std::size_t end = 40;
        while (end < kPayloadBytes && packet[end]) ++end;
        if (end == kPayloadBytes) return false;
        for (std::size_t i = end; i < kPayloadBytes; ++i) if (packet[i]) return false;
        Message parsed;
        parsed.response = packet[5] == 2;
        parsed.focus = static_cast<Focus>(packet[6]);
        std::copy_n(packet.begin() + 8, parsed.nonce.size(), parsed.nonce.begin());
        parsed.session = get_u64(packet, 24);
        parsed.sequence = get_u64(packet, 32);
        parsed.process_name.assign(reinterpret_cast<const char*>(packet.data() + 40), end - 40);
        Packet canonical{};
        if (!encode(parsed, token, canonical)) return false;
        message = std::move(parsed);
        return true;
    } catch (...) { return false; }
}
void Evidence::begin(const Nonce& nonce, Clock::time_point sent_at) noexcept {
    nonce_ = nonce; sent_at_ = sent_at; pending_ = true;
}
bool Evidence::accept(const Message& response, std::string_view process,
                      Clock::time_point now, int ttl_ms) noexcept {
    if (!pending_ || !response.response || response.nonce != nonce_ ||
        !response.session || !response.sequence || static_cast<int>(response.focus) > 2 ||
        !exact_process_match(process, response.process_name) || now < sent_at_ || ttl_ms <= 0 ||
        now >= sent_at_ + std::chrono::milliseconds(ttl_ms) ||
        (response.session == snapshot_.session_id && response.sequence <= snapshot_.sequence)) return false;
    pending_ = false;
    snapshot_.available = response.focus != Focus::Unknown;
    snapshot_.focused = response.focus == Focus::Foreground;
    snapshot_.session_id = response.session;
    snapshot_.sequence = response.sequence;
    evidence_at_ = sent_at_;
    // 用请求发送时刻保守计算年龄，网络往返耗时不能免费延长许可。
    expires_at_ = sent_at_ + std::chrono::milliseconds(ttl_ms);
    return true;
}
void Evidence::invalidate() noexcept { snapshot_ = {}; pending_ = false; }
SourceContextSnapshot Evidence::snapshot(Clock::time_point now) const noexcept {
    auto result = snapshot_;
    if (result.session_id && now >= evidence_at_) {
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - evidence_at_).count();
        result.age_ms = static_cast<int>(std::min<std::int64_t>(age, std::numeric_limits<int>::max()));
    }
    if (now < evidence_at_ || now >= expires_at_) { result.available = false; result.focused = false; }
    return result;
}
} // namespace detail

struct SourceContextClient::Impl {
    Socket socket;
    SourceContextConfig config;
    std::thread worker;
    std::atomic<bool> stop_requested{false};
    mutable std::mutex mutex;
    detail::Evidence evidence;
    std::string error;
    void set_error(const char* message) {
        std::lock_guard lock(mutex);
        error = message;
    }
    void run() noexcept {
        try {
            while (!stop_requested.load()) {
                detail::Message request;
                request.process_name = config.process_name;
                detail::Packet packet{};
                if (!random_bytes(request.nonce) || !detail::encode(request, config.token, packet)) break;
                const auto sent_at = detail::Clock::now();
                { std::lock_guard lock(mutex); evidence.begin(request.nonce, sent_at); }
                if (send(socket.handle, reinterpret_cast<const char*>(packet.data()),
                         static_cast<int>(packet.size()), 0) != static_cast<int>(packet.size())) break;
                const auto poll_deadline = sent_at + std::chrono::milliseconds(std::max(10, config.ttl_ms / 3));
                bool accepted = false;
                while (!stop_requested.load() && detail::Clock::now() < poll_deadline) {
                    const int ready = readable(socket.handle, 5);
                    if (ready < 0) throw 1;
                    if (!ready) continue;
                    std::array<std::uint8_t, detail::kPacketBytes + 1> received{};
                    const int size = recv(socket.handle, reinterpret_cast<char*>(received.data()),
                                          static_cast<int>(received.size()), 0);
                    detail::Message response;
                    if (size != static_cast<int>(detail::kPacketBytes) ||
                        !detail::decode(std::span(received).first(detail::kPacketBytes), config.token, response)) continue;
                    { std::lock_guard lock(mutex);
                      accepted = evidence.accept(response, config.process_name, detail::Clock::now(), config.ttl_ms); }
                    if (accepted) break;
                }
                while (!stop_requested.load() && detail::Clock::now() < poll_deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        } catch (...) {}
        std::lock_guard lock(mutex);
        evidence.invalidate();
        if (!stop_requested.load()) error = "源端状态接收线程已停止";
    }
};
SourceContextClient::SourceContextClient() : impl_(std::make_unique<Impl>()) {}
SourceContextClient::~SourceContextClient() { stop(); }
bool SourceContextClient::start(const SourceContextConfig& config) noexcept {
    stop();
    try {
        if (!detail::valid_config(config)) { impl_->set_error("源端状态配置无效或未启用"); return false; }
        impl_->config = config;
        if (!impl_->socket.open(config, false)) { impl_->set_error("连接源端状态失败"); return false; }
        impl_->set_error("");
        impl_->stop_requested.store(false);
        impl_->worker = std::thread([this] { impl_->run(); });
        return true;
    } catch (...) { stop(); return false; }
}
void SourceContextClient::stop() noexcept {
    impl_->stop_requested.store(true);
    if (impl_->worker.joinable()) impl_->worker.join();
    impl_->socket.close();
    std::lock_guard lock(impl_->mutex);
    impl_->evidence.invalidate();
}
SourceContextSnapshot SourceContextClient::snapshot() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->evidence.snapshot(detail::Clock::now());
}
std::string SourceContextClient::last_error() const {
    std::lock_guard lock(impl_->mutex); return impl_->error;
}

struct SourceContextServer::Impl {
    Socket socket;
    SourceContextConfig config;
    std::uint64_t session = 0;
    std::uint64_t sequence = 0;
    std::string error;
};
SourceContextServer::SourceContextServer() : impl_(std::make_unique<Impl>()) {}
SourceContextServer::~SourceContextServer() { stop(); }
bool SourceContextServer::start(const SourceContextConfig& config) noexcept {
    stop();
    try {
        if (!detail::valid_config(config)) { impl_->error = "源端状态配置无效或未启用"; return false; }
        impl_->config = config;
        detail::Nonce seed{};
        if (!random_bytes(seed)) return false;
        impl_->session = get_u64(seed, 0);
        if (!impl_->session) return false;
        if (!impl_->socket.open(config, true)) { impl_->error = "绑定源端状态失败"; return false; }
        impl_->error.clear();
        return true;
    } catch (...) { stop(); return false; }
}
bool SourceContextServer::serve_once(int timeout_ms) noexcept {
    try {
        if (impl_->socket.handle == INVALID_SOCKET || timeout_ms < 0 || timeout_ms > 1000) return false;
        const int ready = readable(impl_->socket.handle, timeout_ms);
        if (ready == 0) return true;
        if (ready < 0) return false;
        std::array<std::uint8_t, detail::kPacketBytes + 1> received{};
        sockaddr_in peer{};
        int peer_size = sizeof(peer);
        const int size = recvfrom(impl_->socket.handle, reinterpret_cast<char*>(received.data()),
            static_cast<int>(received.size()), 0, reinterpret_cast<sockaddr*>(&peer), &peer_size);
        detail::Message request;
        if (size != static_cast<int>(detail::kPacketBytes) ||
            !detail::decode(std::span(received).first(detail::kPacketBytes), impl_->config.token, request) ||
            request.response || !detail::exact_process_match(request.process_name, impl_->config.process_name)) return true;
        if (impl_->sequence == std::numeric_limits<std::uint64_t>::max()) return false;
        request.response = true;
        request.session = impl_->session;
        request.sequence = ++impl_->sequence;
        request.focus = foreground_focus(impl_->config.process_name);
        detail::Packet packet{};
        if (!detail::encode(request, impl_->config.token, packet)) return false;
        return sendto(impl_->socket.handle, reinterpret_cast<const char*>(packet.data()),
            static_cast<int>(packet.size()), 0, reinterpret_cast<sockaddr*>(&peer), peer_size) == static_cast<int>(packet.size());
    } catch (...) { return false; }
}
void SourceContextServer::stop() noexcept { impl_->socket.close(); impl_->sequence = 0; impl_->session = 0; }
std::string SourceContextServer::last_error() const { return impl_->error; }
} // namespace source_context
