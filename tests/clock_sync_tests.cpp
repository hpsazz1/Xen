#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <WinSock2.h>
#include <WS2tcpip.h>

#include "clock_sync/clock_sync.h"
#include "clock_sync/clock_sync_internal.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << message << '\n';
}

unsigned short reserve_loopback_port() noexcept {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 0;
    const SOCKET socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle == INVALID_SOCKET) {
        WSACleanup();
        return 0;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    unsigned short port = 0;
    if (bind(socket_handle, reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) != SOCKET_ERROR) {
        int size = sizeof(address);
        if (getsockname(socket_handle, reinterpret_cast<sockaddr*>(&address),
                        &size) != SOCKET_ERROR) {
            port = ntohs(address.sin_port);
        }
    }
    closesocket(socket_handle);
    WSACleanup();
    return port;
}

void test_protocol_round_trip() {
    clock_sync::detail::Request request;
    request.request_id = 42;
    request.requester_send_steady_ns = 8'000'000'000ULL;
    std::array<std::uint8_t, clock_sync::detail::kRequestBytes> request_packet{};
    clock_sync::detail::Request parsed_request;
    expect(clock_sync::detail::serialize_request(request, request_packet) &&
               clock_sync::detail::parse_request(
                   request_packet, parsed_request) &&
               parsed_request.request_id == request.request_id &&
               parsed_request.requester_send_steady_ns ==
                   request.requester_send_steady_ns,
           "时钟请求必须固定长度并完整往返 request id 与本地发送时刻");

    clock_sync::detail::Response response;
    response.request_id = request.request_id;
    response.requester_send_steady_ns = request.requester_send_steady_ns;
    response.source_session_id = 99;
    response.source_receive_utc_ns = 1'700'000'000'001'000'000ULL;
    response.source_send_utc_ns = 1'700'000'000'001'100'000ULL;
    std::array<std::uint8_t, clock_sync::detail::kResponseBytes> response_packet{};
    clock_sync::detail::Response parsed_response;
    expect(clock_sync::detail::serialize_response(response, response_packet) &&
               clock_sync::detail::parse_response(
                   response_packet, parsed_response) &&
               parsed_response.source_session_id ==
                   response.source_session_id &&
               parsed_response.source_receive_utc_ns ==
                   response.source_receive_utc_ns &&
               parsed_response.source_send_utc_ns ==
                   response.source_send_utc_ns,
           "时钟响应必须保留源会话、UTC receive 与 UTC send");

    response_packet[0] = 'B';
    expect(!clock_sync::detail::parse_response(
               response_packet, parsed_response),
           "时钟协议必须拒绝 magic 损坏的响应");
}

void test_affine_mapping_and_expiry() {
    constexpr double kClockRate = 1.00002;
    constexpr double kClockOffsetNs = -1.69e18;
    constexpr std::uint64_t kSourceOrigin =
        1'700'000'000'000'000'000ULL;
    constexpr std::uint64_t kForwardDelayNs = 1'000'000ULL;
    constexpr std::uint64_t kReverseDelayNs = 3'000'000ULL;
    constexpr std::uint64_t kProcessingNs = 100'000ULL;
    const auto local_from_source = [=](std::uint64_t source_ns) {
        return static_cast<std::uint64_t>(std::llround(
            kClockRate * static_cast<double>(source_ns) + kClockOffsetNs));
    };

    clock_sync::detail::AffineMapper mapper;
    std::uint64_t last_local_receive = 0;
    for (std::uint64_t index = 0; index < 5; ++index) {
        const std::uint64_t source_at_request =
            kSourceOrigin + index * 250'000'000ULL;
        clock_sync::detail::Sample sample;
        sample.source_session_id = 7;
        sample.requester_send_steady_ns =
            local_from_source(source_at_request);
        sample.source_receive_utc_ns =
            source_at_request + kForwardDelayNs;
        sample.source_send_utc_ns =
            sample.source_receive_utc_ns + kProcessingNs;
        sample.requester_receive_steady_ns = local_from_source(
            sample.source_send_utc_ns + kReverseDelayNs);
        last_local_receive = sample.requester_receive_steady_ns;
        expect(mapper.add_sample(sample),
               "同一 source session 的合法四时间戳样本必须进入映射窗口");
    }

    const std::uint64_t source_frame_ns =
        kSourceOrigin + 700'000'000ULL;
    const auto local_now = std::chrono::steady_clock::time_point(
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::nanoseconds(last_local_receive + 10'000'000ULL)));
    const auto mapped = mapper.map_utc_ns(
        source_frame_ns, local_now, std::chrono::seconds(1));
    const auto expected_local = std::chrono::steady_clock::time_point(
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::nanoseconds(local_from_source(source_frame_ns))));
    const double mapping_error_ms = std::abs(
        std::chrono::duration<double, std::milli>(
            mapped.local_time - expected_local).count());
    expect(mapped.valid && mapped.status == clock_sync::MappingStatus::VALID &&
               mapped.sample_count == 5 &&
               std::abs(mapped.clock_rate - kClockRate) < 1e-6 &&
               mapped.round_trip_ms > 3.9 &&
               mapping_error_ms <= mapped.uncertainty_ms,
           "重复双向交换必须拟合 affine rate，并用 RTT 覆盖路径非对称误差");

    const auto stale = mapper.map_utc_ns(
        source_frame_ns, local_now + std::chrono::seconds(2),
        std::chrono::seconds(1));
    expect(!stale.valid && stale.status == clock_sync::MappingStatus::STALE,
           "映射超过显式最大年龄后必须变为 STALE，不能沿用旧 offset");

    clock_sync::detail::Sample restarted;
    restarted.source_session_id = 8;
    restarted.requester_send_steady_ns = last_local_receive + 20'000'000ULL;
    restarted.source_receive_utc_ns = source_frame_ns + 20'000'000ULL;
    restarted.source_send_utc_ns =
        restarted.source_receive_utc_ns + 50'000ULL;
    restarted.requester_receive_steady_ns =
        restarted.requester_send_steady_ns + 2'000'000ULL;
    expect(mapper.add_sample(restarted),
           "source process 重启后的首个合法样本必须开启新 session");
    const auto warming = mapper.map_utc_ns(
        restarted.source_send_utc_ns,
        std::chrono::steady_clock::time_point(
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::nanoseconds(
                    restarted.requester_receive_steady_ns + 1'000'000ULL))),
        std::chrono::seconds(1));
    expect(!warming.valid &&
               warming.status == clock_sync::MappingStatus::WARMING &&
               warming.sample_count == 1 &&
               warming.source_session_id == restarted.source_session_id,
           "source session 改变必须清空旧映射并重新 WARMING");
}

void test_client_server_loopback() {
    const unsigned short port = reserve_loopback_port();
    expect(port != 0, "时钟回环测试必须取得临时 UDP 端口");
    if (port == 0) return;

    clock_sync::Server server;
    clock_sync::ServerConfig server_config;
    server_config.bind_url =
        "udp://127.0.0.1:" + std::to_string(port);
    expect(server.open(server_config),
           "生产源机时钟服务必须能监听回环端口: " +
               server.last_error());
    std::atomic<bool> stop{false};
    std::atomic<bool> server_ok{true};
    std::thread server_thread([&] {
        while (!stop.load(std::memory_order_acquire)) {
            if (!server.serve_once(50)) {
                server_ok.store(false, std::memory_order_release);
                break;
            }
        }
    });

    clock_sync::Client client;
    clock_sync::ClientConfig client_config;
    client_config.source_url = server_config.bind_url;
    client_config.exchange_interval_ms = 50;
    client_config.response_timeout_ms = 100;
    client_config.maximum_mapping_age_ms = 500;
    expect(client.open(client_config),
           "生产 NDI 时钟客户端必须能连接回环端点: " +
               client.last_error());

    clock_sync::MappingResult mapped;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto source_100ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count() /
            100;
        mapped = client.map_utc_100ns(
            source_100ns, std::chrono::steady_clock::now());
        if (mapped.valid) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    expect(mapped.valid && mapped.sample_count >= 3 &&
               mapped.source_session_id != 0 &&
               mapped.uncertainty_ms >= 0.0,
           "真实 UDP 回环必须建立带 session、RTT 与 uncertainty 的有效映射");

    client.close();
    stop.store(true, std::memory_order_release);
    server_thread.join();
    server.close();
    expect(server_ok.load(std::memory_order_acquire),
           "源机时钟服务回环期间不得发生协议或套接字失败");
}

} // namespace

int main() {
    test_protocol_round_trip();
    test_affine_mapping_and_expiry();
    test_client_server_loopback();
    if (failures != 0) {
        std::cerr << "时钟同步测试失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "时钟同步测试全部通过。\n";
    return 0;
}
