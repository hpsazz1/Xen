#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

namespace {
std::atomic<std::uint64_t> received_bytes = 0;
int WSAAPI observed_recv(SOCKET socket, char* buffer, int length, int flags) {
    const int count = ::recv(socket, buffer, length, flags);
    if (count > 0) received_bytes.fetch_add(static_cast<unsigned>(count));
    return count;
}
}
// 原始Receiver/HTTP/状态机不替换；只观察真实recv返回，保证分片确实分次读取。
#define recv observed_recv
#include "../Xen/weapon/weapon.cpp"
#undef recv

namespace {
using namespace std::chrono_literals;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
struct Socket {
    SOCKET value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    Socket() { require(value != INVALID_SOCKET, "创建回环socket"); }
    ~Socket() { closesocket(value); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    void connect_to(std::uint16_t port) {
        DWORD timeout = 1000;
        require(setsockopt(value, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0,
                "设置客户端接收界限");
        require(setsockopt(value, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0,
                "设置客户端发送界限");
        sockaddr_in address{};
        address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
        require(connect(value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "连接自有回环服务");
    }
    void send_all(std::string_view bytes) {
        while (!bytes.empty()) {
            const int count = send(value, bytes.data(), static_cast<int>(bytes.size()), 0);
            require(count > 0, "发送合成HTTP片段");
            bytes.remove_prefix(static_cast<std::size_t>(count));
        }
    }
    bool response_ok() {
        char bytes[256]{};
        const int count = ::recv(value, bytes, sizeof(bytes), 0);
        return count > 0 && std::string_view(bytes, count).starts_with("HTTP/1.1 200 OK");
    }
};
std::uint16_t reserve_port() {
    Socket connection;
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int size = sizeof(address);
    require(bind(connection.value, reinterpret_cast<sockaddr*>(&address), size) == 0 &&
            getsockname(connection.value, reinterpret_cast<sockaddr*>(&address), &size) == 0, "保留临时回环端口");
    return ntohs(address.sin_port);
}
void await_read(std::uint64_t target) {
    const auto deadline = weapon::Clock::now() + 1s;
    while (received_bytes.load() < target && weapon::Clock::now() < deadline) std::this_thread::sleep_for(1ms);
    require(received_bytes.load() >= target, "真实Receiver未读取指定分片");
}
std::int64_t utc_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
nlohmann::json payload(std::int64_t timestamp, int ammo) {
    return {{"provider", {{"appid", 730}, {"steamid", "76561198000000000"}, {"timestamp", timestamp}}},
        {"map", {{"phase", "live"}}}, {"round", {{"phase", "live"}}},
        {"player", {{"steamid", "76561198000000000"}, {"activity", "playing"}, {"team", "CT"},
            {"state", {{"health", 100}}}, {"weapons", {{"weapon_0", {{"name", "weapon_ak47"},
                {"state", "active"}, {"ammo_clip", ammo}, {"ammo_clip_max", 30}, {"ammo_reserve", 90}}}}}}}};
}
std::string request(const nlohmann::json& value) {
    const auto body = value.dump();
    return "POST /gsi HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\n\r\n" + body;
}
struct Fixture {
    weapon::GsiReceiver receiver;
    weapon::GsiConfig config;
    explicit Fixture(int timeout = 2000) {
        config.enabled = true; config.ttl_ms = 10000; config.request_timeout_ms = timeout; config.max_clock_skew_ms = 10000;
        start();
    }
    void start() {
        bool started = false;
        for (int attempt = 0; attempt < 8 && !started; ++attempt) { config.port = reserve_port(); started = receiver.start(config); }
        require(started, "真实Receiver启动");
    }
    void post(int ammo) {
        Socket connection; connection.connect_to(config.port);
        connection.send_all(request(payload(utc_seconds(), ammo)));
        require(connection.response_ok(), "健康请求收到实际HTTP响应");
        const auto state = receiver.snapshot();
        require(state.valid && state.ammo_clip == ammo && !state.source_order_verified, "健康请求更新武器但不宣称源端顺序已证实");
    }
};

void fragmented_requests() {
    Fixture fixture;
    fixture.post(30);
    const auto text = request(payload(utc_seconds(), 29));
    const auto header = text.find("\r\n\r\n");
    const std::size_t cuts[]{1, 18, header - 1, header + 1, header + 3, header + 4, text.size() - 1};
    int ammo = 29;
    for (const auto cut : cuts) {
        const auto current = request(payload(utc_seconds(), ammo));
        const auto before = fixture.receiver.snapshot();
        const auto read_before = received_bytes.load();
        Socket connection; connection.connect_to(fixture.config.port);
        connection.send_all(std::string_view(current).substr(0, cut));
        await_read(read_before + cut);
        require(fixture.receiver.snapshot().revision == before.revision, "头部或body未完整时不能发布半包");
        connection.send_all(std::string_view(current).substr(cut));
        require(connection.response_ok(), "分片完结后正常响应");
        const auto after = fixture.receiver.snapshot();
        require(after.valid && after.ammo_clip == ammo && after.revision == before.revision + 1, "分片请求只完整提交一次");
        --ammo;
    }
    std::cout << "TCP实际分片七例通过\n";
}

void rejected_and_expired_requests() {
    Fixture fixture(100);
    fixture.post(30);
    const std::string prefix = "POST /gsi HTTP/1.1\r\nContent-Type: application/json\r\n";
    const std::string bad[]{
        prefix + "Content-Length: 1\r\nContent-Length: 1\r\n\r\n{",
        prefix + "Content-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n{",
        prefix + "Content-Length: 65537\r\n\r\n",
        prefix + "X-Padding: " + std::string(8192, 'x') + "\r\nContent-Length: 1\r\n\r\n{"
    };
    int ammo = 29;
    for (const auto& text : bad) {
        const auto before = fixture.receiver.snapshot();
        Socket connection; connection.connect_to(fixture.config.port); connection.send_all(text);
        require(!connection.response_ok(), "坏协议/长度边界不能返回成功");
        const auto after = fixture.receiver.snapshot();
        require(after.revision == before.revision && after.valid_until == before.valid_until, "拒绝请求不覆盖快照或延长TTL");
        fixture.post(ammo--);
    }
    for (bool body : {false, true}) {
        const auto before = fixture.receiver.snapshot();
        const auto text = body ? prefix + "Content-Length: 300\r\n\r\n{" : std::string("POST /gsi HTTP/1.1\r\n");
        Socket connection; connection.connect_to(fixture.config.port);
        const auto read_before = received_bytes.load(); connection.send_all(text); await_read(read_before + text.size());
        char byte{};
        const int result = ::recv(connection.value, &byte, 1, 0);
        require(result == 0 || (result == SOCKET_ERROR && WSAGetLastError() == WSAECONNRESET), "半包由服务端请求期限关闭，不能靠客户端超时冒充");
        require(fixture.receiver.snapshot().revision == before.revision, "过期半包不发布状态");
        fixture.post(ammo--);
    }
    std::cout << "四种坏请求与两种半包超时均拒绝，六次后续请求恢复\n";
}

void stop_and_restart() {
    for (bool body : {false, true}) {
        Fixture fixture;
        fixture.post(30);
        const auto before = fixture.receiver.snapshot();
        Socket connection; connection.connect_to(fixture.config.port);
        const auto text = body ? "POST /gsi HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 300\r\n\r\n{" : "POST /gsi";
        const auto read_before = received_bytes.load(); connection.send_all(text); await_read(read_before + std::char_traits<char>::length(text));
        const auto start = weapon::Clock::now(); fixture.receiver.stop();
        const auto elapsed = weapon::Clock::now() - start;
        require(elapsed < 1s, "半包等待时stop不等待完整两秒请求期限");
        require(!fixture.receiver.snapshot().valid && fixture.receiver.snapshot().revision == 0, "stop撤销旧快照且不提交半包");
        auto invalid = fixture.config; invalid.enabled = false;
        require(!fixture.receiver.start(invalid) && !fixture.receiver.snapshot().valid, "无效重启仍关闭信任");
        fixture.start(); fixture.post(29);
        const auto after = fixture.receiver.snapshot();
        require(after.source_epoch > before.source_epoch && after.control_safety_epoch > before.control_safety_epoch,
                "同Receiver重启推进代际，不复用旧会话许可");
        std::cout << (body ? "body" : "header") << "半包stop耗时ms=" << std::chrono::duration<double, std::milli>(elapsed).count() << '\n';
    }
}

void clock_rollback() {
    weapon::detail::GsiState state;
    weapon::GsiConfig config; config.enabled = true;
    const auto now = weapon::Clock::time_point{} + 10s;
    constexpr std::int64_t stamp = 1700000000;
    require(state.ingest(payload(stamp, 30).dump(), config, now, stamp * 1000) == weapon::Status::READY, "时钟对照先建立健康状态");
    const auto before = state.snapshot(now);
    require(state.ingest(payload(stamp, 29).dump(), config, now - 1ms, stamp * 1000) == weapon::Status::COUNTER_EXHAUSTED &&
            !state.snapshot(now).valid, "单调时间回退必须撤销武器可信度");
    require(state.ingest(payload(stamp, 28).dump(), config, now + 1ms, stamp * 1000) == weapon::Status::READY, "后续真实时间恢复");
    const auto after = state.snapshot(now + 1ms);
    require(after.control_safety_epoch > before.control_safety_epoch && after.recoil_safety_epoch > before.recoil_safety_epoch &&
            after.valid_until == before.valid_until, "回退断点持久化且同秒恢复不刷新TTL");
    require(state.ingest(payload(stamp + 100, 27).dump(), config, now + 2ms, stamp * 1000) == weapon::Status::CLOCK_REJECTED,
            "未来墙钟包不获准");
    require(state.ingest(payload(stamp, 26).dump(), config, now + 3ms, stamp * 1000) == weapon::Status::READY,
            "不可信未来时间戳不能污染高水位阻断健康恢复");
    std::cout << "单调回退和未来墙钟拒绝及恢复通过\n";
}
}

int main() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 2;
    struct Cleanup { ~Cleanup() { WSACleanup(); } } cleanup;
    try {
        fragmented_requests(); rejected_and_expired_requests(); stop_and_restart(); clock_rollback();
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
