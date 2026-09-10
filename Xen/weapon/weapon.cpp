#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <WS2tcpip.h>
#include "weapon/weapon_internal.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace weapon {
namespace {
using Json = nlohmann::json;
bool digits(std::string_view text) noexcept {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}
bool same_secret(std::string_view a, std::string_view b) noexcept {
    std::size_t diff = a.size() ^ b.size();
    for (std::size_t i = 0; i < b.size(); ++i) diff |= static_cast<unsigned char>(i < a.size() ? a[i] : 0) ^ static_cast<unsigned char>(b[i]);
    return diff == 0;
}
std::string lower(std::string_view text) {
    std::string result(text);
    for (char& c : result) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return result;
}
std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}
std::optional<int> integer(const Json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number_integer()) return {};
    if (it->is_number_unsigned()) {
        const auto value = it->get<std::uint64_t>();
        if (value > 100000) return {};
        return static_cast<int>(value);
    }
    const auto value = it->get<std::int64_t>();
    if (value < 0 || value > 100000) return {};
    return static_cast<int>(value);
}
std::string fingerprint(const WeaponSnapshot& s) {
    const auto value = [](std::optional<int> v) { return v ? std::to_string(*v) : "?"; };
    return s.raw_name + ":" + std::to_string(static_cast<int>(s.state)) + ":" +
        value(s.ammo_clip) + ":" + value(s.ammo_clip_max) + ":" + value(s.ammo_reserve) + ":" +
        std::to_string(static_cast<int>(s.status));
}
} // namespace

const char* status_name(Status status) noexcept {
    switch (status) {
#define WEAPON_STATUS(v) case Status::v: return #v
        WEAPON_STATUS(DISABLED); WEAPON_STATUS(UNAVAILABLE); WEAPON_STATUS(READY); WEAPON_STATUS(EXPIRED);
        WEAPON_STATUS(AUTH_REJECTED); WEAPON_STATUS(INVALID_PAYLOAD); WEAPON_STATUS(IDENTITY_MISMATCH);
        WEAPON_STATUS(PLAYER_INACTIVE); WEAPON_STATUS(UNKNOWN_WEAPON); WEAPON_STATUS(RELOADING);
        WEAPON_STATUS(EMPTY); WEAPON_STATUS(CLOCK_REJECTED); WEAPON_STATUS(OUT_OF_ORDER);
        WEAPON_STATUS(DUPLICATE); WEAPON_STATUS(COUNTER_EXHAUSTED);
#undef WEAPON_STATUS
    }
    return "UNKNOWN";
}
bool valid_config(const GsiConfig& c) noexcept {
    IN_ADDR bind{}, peer{};
    return c.enabled && c.port != 0 && c.token.size() >= 32 && c.token.size() <= 1024 &&
        c.expected_player_id.size() <= 32 && digits(c.expected_player_id) &&
        c.ttl_ms >= 100 && c.ttl_ms <= 10000 && c.request_timeout_ms >= 20 && c.request_timeout_ms <= 2000 &&
        c.max_body_bytes >= 256 && c.max_body_bytes <= 65536 && c.max_clock_skew_ms >= 1000 && c.max_clock_skew_ms <= 10000 &&
        InetPtonA(AF_INET, c.bind_address.c_str(), &bind) == 1 &&
        (c.allowed_peer_ipv4.empty() ? c.bind_address == "127.0.0.1" : InetPtonA(AF_INET, c.allowed_peer_ipv4.c_str(), &peer) == 1);
}
std::string canonical_weapon_id(const std::string& name) {
    // 精确映射协议武器名，不由子串或RCS是否有曲线猜类型/射击模式。
    static constexpr std::pair<const char*, const char*> names[] = {
        {"weapon_ak47", "ak47"}, {"weapon_m4a1", "m4a4"}, {"weapon_m4a1_silencer", "m4a1_s"},
        {"weapon_aug", "aug"}, {"weapon_sg556", "sg553"}, {"weapon_famas", "famas"}, {"weapon_galilar", "galil"},
        {"weapon_bizon", "bizon"}, {"weapon_mac10", "mac10"}, {"weapon_mp5sd", "mp5sd"}, {"weapon_mp7", "mp7"},
        {"weapon_mp9", "mp9"}, {"weapon_p90", "p90"}, {"weapon_ump45", "ump45"}, {"weapon_m249", "m249"},
        {"weapon_negev", "negev"}, {"weapon_cz75a", "cz75"}, {"weapon_glock", "glock"},
        {"weapon_hkp2000", "p2000"}, {"weapon_usp_silencer", "usp_s"}, {"weapon_p250", "p250"},
        {"weapon_fiveseven", "fiveseven"}, {"weapon_tec9", "tec9"}, {"weapon_elite", "dual_berettas"},
        {"weapon_deagle", "deagle"}, {"weapon_revolver", "revolver"}, {"weapon_awp", "awp"},
        {"weapon_ssg08", "ssg08"}, {"weapon_scar20", "scar20"}, {"weapon_g3sg1", "g3sg1"},
        {"weapon_nova", "nova"}, {"weapon_mag7", "mag7"}, {"weapon_sawedoff", "sawedoff"}, {"weapon_xm1014", "xm1014"}
    };
    for (const auto& [raw, canonical] : names) if (name == raw) return canonical;
    return {};
}

namespace detail {
bool http_body_length(std::string_view header, std::size_t limit, std::size_t& length) noexcept {
    try {
        if (header.size() > kMaximumHeaders || !header.ends_with("\r\n\r\n")) return false;
        const auto first = header.find("\r\n");
        if (first == std::string_view::npos) return false;
        const auto line = header.substr(0, first);
        if (line != "POST / HTTP/1.1" && line != "POST /gsi HTTP/1.1") return false;
        bool content_length = false, content_type = false;
        std::size_t pos = first + 2;
        while (pos + 2 < header.size()) {
            const auto end = header.find("\r\n", pos);
            if (end == std::string_view::npos) return false;
            if (end == pos) break;
            const auto entry = header.substr(pos, end - pos);
            const auto colon = entry.find(':');
            if (colon == std::string_view::npos || colon == 0 || entry.front() == ' ' || entry.front() == '\t') return false;
            const auto key = lower(entry.substr(0, colon));
            const auto value = trim(entry.substr(colon + 1));
            if (key == "transfer-encoding" || key == "expect" || key == "content-encoding") return false;
            if (key == "content-length") {
                if (content_length || !digits(value)) return false;
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), length);
                if (parsed.ec != std::errc{} || length == 0 || length > limit) return false;
                content_length = true;
            } else if (key == "content-type") {
                if (content_type || lower(value.substr(0, value.find(';'))) != "application/json") return false;
                content_type = true;
            }
            pos = end + 2;
        }
        return content_length && content_type;
    } catch (...) { return false; }
}

WeaponSnapshot parse_payload(std::string_view body, const GsiConfig& config, std::int64_t local_utc_ms) noexcept {
    WeaponSnapshot result;
    result.status = Status::INVALID_PAYLOAD;
    try {
        if (body.empty() || body.size() > config.max_body_bytes || !valid_config(config)) return result;
        const auto json = Json::parse(body.begin(), body.end(), [&](int depth, Json::parse_event_t, Json&) {
            // 回调返回false只丢弃值，不会停止递归；主动终止才真正限制解析深度。
            if (depth > 16) throw std::runtime_error("JSON depth limit");
            return true;
        }, false);
        if (json.is_discarded() || !json.is_object()) return result;
        if (!json.contains("auth") || !json["auth"].is_object() || !json["auth"].contains("token") ||
            !json["auth"]["token"].is_string() || !same_secret(json["auth"]["token"].get_ref<const std::string&>(), config.token)) {
            result.status = Status::AUTH_REJECTED; return result;
        }
        if (!json.contains("provider") || !json["provider"].is_object() ||
            !json.contains("player") || !json["player"].is_object()) return result;
        const auto& provider = json["provider"];
        const auto& player = json["player"];
        if (integer(provider, "appid") != 730) return result;
        if (!provider.contains("steamid") || !provider["steamid"].is_string() ||
            !player.contains("steamid") || !player["steamid"].is_string() ||
            provider["steamid"].get_ref<const std::string&>() != config.expected_player_id ||
            player["steamid"].get_ref<const std::string&>() != config.expected_player_id) {
            result.status = Status::IDENTITY_MISMATCH; return result;
        }
        result.identity_match = true;
        if (!provider.contains("timestamp") || !provider["timestamp"].is_number_integer()) return result;
        if (provider["timestamp"].is_number_unsigned() && provider["timestamp"].get<std::uint64_t>() >
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / 1000)) {
            result.status = Status::CLOCK_REJECTED; return result;
        }
        const auto timestamp = provider["timestamp"].get<std::int64_t>();
        if (timestamp <= 0 || timestamp > std::numeric_limits<std::int64_t>::max() / 1000 || local_utc_ms < 0) {
            result.status = Status::CLOCK_REJECTED; return result;
        }
        const auto source_ms = timestamp * 1000;
        const auto difference = source_ms > local_utc_ms ? source_ms - local_utc_ms : local_utc_ms - source_ms;
        if (difference > config.max_clock_skew_ms) { result.status = Status::CLOCK_REJECTED; return result; }
        result.provider_timestamp_seconds = static_cast<std::uint64_t>(timestamp);
        if (!player.contains("activity") || !player["activity"].is_string() || player["activity"] != "playing" ||
            !player.contains("state") || !player["state"].is_object() ||
            !integer(player["state"], "health") || *integer(player["state"], "health") == 0) {
            result.status = Status::PLAYER_INACTIVE; return result;
        }
        if (!player.contains("weapons") || !player["weapons"].is_object() || player["weapons"].size() > 64) return result;
        const Json* selected = nullptr;
        for (const auto& value : player["weapons"].items()) {
            if (!value.value().is_object()) return result;
            const auto& w = value.value();
            if (!w.contains("state") || !w["state"].is_string()) return result;
            if (w["state"] == "active" || w["state"] == "reloading") {
                if (selected) return result;
                selected = &w;
            }
        }
        if (!selected || !selected->contains("name") || !(*selected)["name"].is_string()) return result;
        result.raw_name = (*selected)["name"].get<std::string>();
        if (result.raw_name.size() > 64 || !std::all_of(result.raw_name.begin(), result.raw_name.end(), [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; })) {
            result.raw_name.clear(); return result;
        }
        result.canonical_id = canonical_weapon_id(result.raw_name);
        result.state = (*selected)["state"] == "active" ? WeaponState::ACTIVE : WeaponState::RELOADING;
        result.ammo_clip = integer(*selected, "ammo_clip");
        result.ammo_clip_max = integer(*selected, "ammo_clip_max");
        result.ammo_reserve = integer(*selected, "ammo_reserve");
        if (result.canonical_id.empty()) { result.status = Status::UNKNOWN_WEAPON; return result; }
        if (!result.ammo_clip || !result.ammo_clip_max || !result.ammo_reserve || *result.ammo_clip > *result.ammo_clip_max) return result;
        if (result.state == WeaponState::RELOADING) { result.status = Status::RELOADING; return result; }
        if (*result.ammo_clip == 0) { result.status = Status::EMPTY; return result; }
        result.valid = true;
        result.status = Status::READY;
        return result;
    } catch (...) { result.valid = false; result.status = Status::INVALID_PAYLOAD; return result; }
}

void GsiState::reset() noexcept {
    current_ = {}; timestamp_ = 0; last_now_ = {}; timestamp_deadline_ = {}; seen_states_.clear();
    if (epoch_ != std::numeric_limits<std::uint64_t>::max()) ++epoch_;
}
Status GsiState::ingest(std::string_view body, const GsiConfig& config,
                      Clock::time_point now, std::int64_t local_utc_ms) noexcept {
    try {
        auto incoming = parse_payload(body, config, local_utc_ms);
        if (epoch_ == 0) epoch_ = 1;
        if (incoming.status == Status::AUTH_REJECTED) return incoming.status;
        if (now < last_now_ || epoch_ == std::numeric_limits<std::uint64_t>::max() || revision_ == std::numeric_limits<std::uint64_t>::max()) {
            current_.valid = false; current_.status = Status::COUNTER_EXHAUSTED; return current_.status;
        }
        last_now_ = now;
        if (incoming.provider_timestamp_seconds) {
            const auto ts = *incoming.provider_timestamp_seconds;
            if (ts < timestamp_) return Status::OUT_OF_ORDER;
            if (ts > timestamp_) {
                timestamp_ = ts;
                timestamp_deadline_ = now + std::chrono::milliseconds(config.ttl_ms);
                seen_states_.clear();
            }
            const auto key = fingerprint(incoming);
            if (std::find(seen_states_.begin(), seen_states_.end(), key) != seen_states_.end()) return Status::DUPLICATE;
            if (seen_states_.size() >= 128) { current_.valid = false; current_.status = Status::INVALID_PAYLOAD; return current_.status; }
            seen_states_.push_back(key);
            // 同秒不同完整状态正常发布，但期限锁定到该timestamp首次接收，不能靠重放续命。
            incoming.valid_until = timestamp_deadline_;
            if (now >= timestamp_deadline_) { incoming.valid = false; incoming.status = Status::EXPIRED; }
        }
        if (current_.valid && (!incoming.valid || now >= current_.valid_until)) ++epoch_;
        incoming.source_epoch = epoch_;
        incoming.revision = ++revision_;
        incoming.received_at = now;
        current_ = std::move(incoming);
        return current_.status;
    } catch (...) { current_.valid = false; current_.status = Status::INVALID_PAYLOAD; return current_.status; }
}
WeaponSnapshot GsiState::snapshot(Clock::time_point now) const {
    auto result = current_;
    if (result.valid && (now < result.received_at || now >= result.valid_until)) { result.valid = false; result.status = Status::EXPIRED; }
    return result;
}
} // namespace detail

struct GsiReceiver::Impl {
    GsiConfig config;
    SOCKET listener = INVALID_SOCKET;
    bool winsock = false;
    std::atomic<bool> stopping{false};
    std::thread worker;
    mutable std::mutex mutex;
    detail::GsiState state;
    std::string error;
    bool wait_socket(SOCKET socket, Clock::time_point deadline) const {
        while (!stopping.load() && Clock::now() < deadline) {
            fd_set read;
            FD_ZERO(&read); FD_SET(socket, &read);
            timeval timeout{0, 10000};
            const auto ready = select(0, &read, nullptr, nullptr, &timeout);
            if (ready == SOCKET_ERROR) throw std::runtime_error("GSI socket unavailable");
            if (ready > 0) return true;
        }
        return false;
    }
    void handle(SOCKET connection) {
        const auto deadline = Clock::now() + std::chrono::milliseconds(config.request_timeout_ms);
        std::string request;
        std::size_t header_end = std::string::npos, content_length = 0;
        while (wait_socket(connection, deadline)) {
            char bytes[4096];
            const int size = recv(connection, bytes, sizeof(bytes), 0);
            if (size == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
            if (size <= 0) return;
            request.append(bytes, static_cast<std::size_t>(size));
            if (request.size() > detail::kMaximumHeaders + config.max_body_bytes) return;
            if (header_end == std::string::npos) {
                const auto found = request.find("\r\n\r\n");
                if (found == std::string::npos) { if (request.size() > detail::kMaximumHeaders) return; continue; }
                header_end = found + 4;
                if (!detail::http_body_length(std::string_view(request).substr(0, header_end), config.max_body_bytes, content_length)) return;
            }
            if (request.size() > header_end + content_length) return;
            if (request.size() != header_end + content_length) continue;
            if (stopping.load() || Clock::now() >= deadline) return;
            const auto utc = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            Status status;
            { std::lock_guard lock(mutex); status = state.ingest(std::string_view(request).substr(header_end), config, Clock::now(), utc); }
            const char* response = status == Status::AUTH_REJECTED
                ? "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
                : "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send(connection, response, static_cast<int>(std::char_traits<char>::length(response)), 0);
            return;
        }
    }
    void run() noexcept {
        try {
            while (!stopping.load()) {
                if (!wait_socket(listener, Clock::now() + std::chrono::milliseconds(20))) continue;
                sockaddr_in peer{}; int size = sizeof(peer);
                const SOCKET connection = accept(listener, reinterpret_cast<sockaddr*>(&peer), &size);
                if (connection == INVALID_SOCKET) break;
                u_long nonblocking = 1;
                if (ioctlsocket(connection, FIONBIO, &nonblocking) == SOCKET_ERROR) {
                    closesocket(connection); continue;
                }
                char address[INET_ADDRSTRLEN]{};
                const bool allowed = InetNtopA(AF_INET, &peer.sin_addr, address, sizeof(address)) &&
                    (config.allowed_peer_ipv4.empty() ? std::string_view(address) == "127.0.0.1" : std::string_view(address) == config.allowed_peer_ipv4);
                DWORD timeout = 20;
                setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
                try { if (allowed) handle(connection); } catch (...) {}
                closesocket(connection);
            }
        } catch (...) {}
        std::lock_guard lock(mutex);
        state.reset();
        if (!stopping.load()) error = "GSI接收线程停止";
    }
};
GsiReceiver::GsiReceiver() : impl_(std::make_unique<Impl>()) {}
GsiReceiver::~GsiReceiver() { stop(); }
bool GsiReceiver::start(const GsiConfig& config) noexcept {
    stop();
    try {
        if (!valid_config(config)) { std::lock_guard lock(impl_->mutex); impl_->error = "GSI配置未启用或不完整"; return false; }
        impl_->config = config;
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
        impl_->winsock = true;
        impl_->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (impl_->listener == INVALID_SOCKET) { stop(); return false; }
        BOOL exclusive = TRUE;
        if (setsockopt(impl_->listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR) {
            stop(); return false;
        }
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(config.port);
        InetPtonA(AF_INET, config.bind_address.c_str(), &address.sin_addr);
        if (bind(impl_->listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR || listen(impl_->listener, 4) == SOCKET_ERROR) {
            stop(); std::lock_guard lock(impl_->mutex); impl_->error = "GSI监听绑定失败"; return false;
        }
        { std::lock_guard lock(impl_->mutex); impl_->state.reset(); impl_->error.clear(); }
        impl_->stopping.store(false);
        impl_->worker = std::thread([this] { impl_->run(); });
        return true;
    } catch (...) { stop(); return false; }
}
void GsiReceiver::stop() noexcept {
    impl_->stopping.store(true);
    if (impl_->worker.joinable()) impl_->worker.join();
    if (impl_->listener != INVALID_SOCKET) closesocket(impl_->listener);
    impl_->listener = INVALID_SOCKET;
    if (impl_->winsock) WSACleanup();
    impl_->winsock = false;
    std::lock_guard lock(impl_->mutex); impl_->state.reset();
}
WeaponSnapshot GsiReceiver::snapshot() const { std::lock_guard lock(impl_->mutex); return impl_->state.snapshot(Clock::now()); }
std::string GsiReceiver::last_error() const { std::lock_guard lock(impl_->mutex); return impl_->error; }
} // namespace weapon
