#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

namespace {
std::atomic_int next_accept_error{0}, injected_errors{0};
SOCKET WSAAPI fixture_accept(SOCKET listener, sockaddr* address, int* length) {
    const int error = next_accept_error.exchange(0);
    if (error == WSAEWOULDBLOCK) {
        ++injected_errors; WSASetLastError(error); return INVALID_SOCKET;
    }
    const SOCKET connection = ::accept(listener, address, length);
    if (error && connection != INVALID_SOCKET) {
        // Consume only this test's real loopback connection, then inject the
        // documented accept return. No receiver/parser/state logic is mocked.
        closesocket(connection); ++injected_errors; WSASetLastError(error); return INVALID_SOCKET;
    }
    return connection;
}
}
#define accept fixture_accept
#include "../Xen/weapon/weapon.cpp"
#undef accept

namespace {
using namespace std::chrono_literals;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::uint16_t reserve_port() {
    const SOCKET socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(socket_handle != INVALID_SOCKET, "reserve socket");
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int length = sizeof(address);
    require(bind(socket_handle, reinterpret_cast<sockaddr*>(&address), length) == 0 &&
        getsockname(socket_handle, reinterpret_cast<sockaddr*>(&address), &length) == 0, "reserve loopback port");
    closesocket(socket_handle); return ntohs(address.sin_port);
}
nlohmann::json payload(std::int64_t timestamp, int ammo) {
    return {{"provider", {{"appid", 730}, {"steamid", "76561198000000000"}, {"timestamp", timestamp}}},
        {"map", {{"phase", "live"}}}, {"round", {{"phase", "live"}}},
        {"player", {{"steamid", "76561198000000000"}, {"activity", "playing"}, {"team", "CT"},
            {"state", {{"health", 100}}}, {"weapons", {{"weapon_0", {{"name", "weapon_ak47"},
                {"state", "active"}, {"ammo_clip", ammo}, {"ammo_clip_max", 30}, {"ammo_reserve", 90}}}}}}}};
}
bool post(std::uint16_t port, const nlohmann::json& value) {
    const SOCKET connection = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(connection != INVALID_SOCKET, "post socket");
    struct Close { SOCKET value; ~Close() { closesocket(value); } } close{connection};
    DWORD timeout = 250;
    require(setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0,
        "post receive bound");
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
    if (connect(connection, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) return false;
    const auto body = value.dump();
    const auto request = "POST / HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    std::size_t sent = 0;
    while (sent < request.size()) {
        const int size = send(connection, request.data() + sent, static_cast<int>(request.size() - sent), 0);
        if (size <= 0) return false; sent += size;
    }
    char response[256]{};
    const int size = recv(connection, response, sizeof(response), 0);
    return size > 0 && std::string_view(response, size).starts_with("HTTP/1.1 200 OK");
}
bool run_case(const char* name, int error, bool recoverable) {
    weapon::GsiReceiver receiver; weapon::GsiConfig config;
    config.enabled = true; config.ttl_ms = 1000; config.request_timeout_ms = 100;
    bool started = false;
    for (int attempt = 0; attempt < 8 && !started; ++attempt) { config.port = reserve_port(); started = receiver.start(config); }
    require(started, "actual receiver start");
    const auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    require(post(config.port, payload(timestamp, 30)), "initial actual HTTP request");
    const auto initial = receiver.snapshot(); require(initial.valid && initial.ammo_clip == 30, "initial state");
    const int before = injected_errors.load(); next_accept_error = error;
    const bool interrupted_response = post(config.port, payload(timestamp, 29));
    require(injected_errors == before + (error ? 1 : 0), "exactly one requested accept failure");
    if (!recoverable) {
        const auto deadline = weapon::Clock::now() + 1s;
        while (receiver.last_error().empty() && weapon::Clock::now() < deadline) std::this_thread::sleep_for(1ms);
        const bool passed = !interrupted_response && !receiver.last_error().empty() && !receiver.snapshot().valid;
        std::cout << name << ": service_error=" << !receiver.last_error().empty() << " passed=" << passed << '\n';
        receiver.stop(); return passed;
    }
    const bool resumed = post(config.port, payload(timestamp, 28));
    const auto after = receiver.snapshot();
    bool passed = resumed && receiver.last_error().empty() && after.valid && after.ammo_clip == 28 &&
        after.source_epoch == initial.source_epoch && after.team_epoch == initial.team_epoch &&
        after.recoil_safety_epoch == initial.recoil_safety_epoch && after.control_safety_epoch == initial.control_safety_epoch &&
        after.valid_until == initial.valid_until;
    if (resumed) {
        require(post(config.port, payload(timestamp - 1, 27)), "replay transport");
        require(receiver.snapshot().revision == after.revision && receiver.snapshot().ammo_clip == after.ammo_clip, "high-water replay rejected");
        require(post(config.port, payload(timestamp, 30)), "duplicate transport");
        require(receiver.snapshot().revision == after.revision && receiver.snapshot().valid_until == initial.valid_until, "duplicate cannot renew TTL");
        // Expiration never clears timestamp high-water or revives same-second data.
        std::this_thread::sleep_until(initial.valid_until + 2ms);
        require(post(config.port, payload(timestamp - 1, 26)), "expired replay transport");
        require(receiver.snapshot().revision == after.revision && !receiver.snapshot().valid, "expired high-water retained");
    }
    std::cout << name << ": resumed=" << resumed << " valid=" << after.valid
        << " service_error=" << !receiver.last_error().empty() << " passed=" << passed << '\n';
    receiver.stop(); return passed;
}
}
int main() {
    WSADATA data{}; if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 2;
    try {
        bool passed = true;
        passed &= run_case("healthy", 0, true);
        passed &= run_case("connection-reset", WSAECONNRESET, true);
        passed &= run_case("would-block", WSAEWOULDBLOCK, true);
        passed &= run_case("invalid-listener", WSAENOTSOCK, false);
        passed &= run_case("network-down", WSAENETDOWN, false);
        WSACleanup(); return passed ? 0 : 1;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; WSACleanup(); return 1; }
}
