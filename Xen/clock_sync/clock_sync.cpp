#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <bcrypt.h>

#include "clock_sync/clock_sync_internal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace clock_sync {
namespace {

constexpr std::array<std::uint8_t, 4> kMagic{{'X', 'C', 'L', 'K'}};
constexpr std::uint16_t kVersion = 1;
constexpr std::uint16_t kRequestType = 1;
constexpr std::uint16_t kResponseType = 2;

std::uint16_t read_u16(std::span<const std::uint8_t> bytes,
                       std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[offset]) << 8U) |
        static_cast<std::uint16_t>(bytes[offset + 1U]));
}

std::uint32_t read_u32(std::span<const std::uint8_t> bytes,
                       std::size_t offset) noexcept {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) |
           static_cast<std::uint32_t>(bytes[offset + 3U]);
}

std::uint64_t read_u64(std::span<const std::uint8_t> bytes,
                       std::size_t offset) noexcept {
    return (static_cast<std::uint64_t>(read_u32(bytes, offset)) << 32U) |
           static_cast<std::uint64_t>(read_u32(bytes, offset + 4U));
}

void write_u16(std::span<std::uint8_t> bytes,
               std::size_t offset,
               std::uint16_t value) noexcept {
    bytes[offset] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    bytes[offset + 1U] = static_cast<std::uint8_t>(value & 0xffU);
}

void write_u32(std::span<std::uint8_t> bytes,
               std::size_t offset,
               std::uint32_t value) noexcept {
    bytes[offset] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
    bytes[offset + 1U] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
    bytes[offset + 2U] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    bytes[offset + 3U] = static_cast<std::uint8_t>(value & 0xffU);
}

void write_u64(std::span<std::uint8_t> bytes,
               std::size_t offset,
               std::uint64_t value) noexcept {
    write_u32(bytes, offset, static_cast<std::uint32_t>(value >> 32U));
    write_u32(bytes, offset + 4U, static_cast<std::uint32_t>(value));
}

template <std::size_t Size>
void write_prefix(std::array<std::uint8_t, Size>& packet,
                  std::uint16_t type) noexcept {
    packet.fill(0U);
    std::span<std::uint8_t> bytes(packet);
    std::copy(kMagic.begin(), kMagic.end(), bytes.begin());
    write_u16(bytes, 4, kVersion);
    write_u16(bytes, 6, type);
    write_u32(bytes, 8, static_cast<std::uint32_t>(Size));
}

bool valid_prefix(std::span<const std::uint8_t> packet,
                  std::size_t size,
                  std::uint16_t type) noexcept {
    return packet.size() == size &&
           std::equal(kMagic.begin(), kMagic.end(), packet.begin()) &&
           read_u16(packet, 4) == kVersion &&
           read_u16(packet, 6) == type &&
           read_u32(packet, 8) == size;
}

bool parse_udp_url(const std::string& url,
                   std::string& host,
                   std::uint16_t& port) noexcept {
    constexpr std::string_view kPrefix = "udp://";
    if (!url.starts_with(kPrefix)) return false;
    const std::string_view endpoint(url.data() + kPrefix.size(),
                                    url.size() - kPrefix.size());
    const std::size_t colon = endpoint.rfind(':');
    if (colon == std::string_view::npos || colon == 0 ||
        colon + 1U >= endpoint.size()) {
        return false;
    }
    host.assign(endpoint.substr(0, colon));
    const std::string port_text(endpoint.substr(colon + 1U));
    try {
        std::size_t consumed = 0;
        const unsigned long parsed = std::stoul(port_text, &consumed);
        if (consumed != port_text.size() || parsed == 0 || parsed > 65535) {
            return false;
        }
        port = static_cast<std::uint16_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

std::uint64_t steady_now_ns() noexcept {
    const auto value = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return value > 0 ? static_cast<std::uint64_t>(value) : 0U;
}

std::uint64_t utc_now_ns() noexcept {
    const auto value = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return value > 0 ? static_cast<std::uint64_t>(value) : 0U;
}

} // namespace

namespace detail {

bool serialize_request(
        const Request& request,
        std::array<std::uint8_t, kRequestBytes>& packet) noexcept {
    if (request.request_id == 0 || request.requester_send_steady_ns == 0) {
        return false;
    }
    write_prefix(packet, kRequestType);
    std::span<std::uint8_t> bytes(packet);
    write_u64(bytes, 16, request.request_id);
    write_u64(bytes, 24, request.requester_send_steady_ns);
    return true;
}

bool parse_request(std::span<const std::uint8_t> packet,
                   Request& request) noexcept {
    if (!valid_prefix(packet, kRequestBytes, kRequestType)) return false;
    Request parsed;
    parsed.request_id = read_u64(packet, 16);
    parsed.requester_send_steady_ns = read_u64(packet, 24);
    if (parsed.request_id == 0 || parsed.requester_send_steady_ns == 0) {
        return false;
    }
    request = parsed;
    return true;
}

bool serialize_response(
        const Response& response,
        std::array<std::uint8_t, kResponseBytes>& packet) noexcept {
    if (response.request_id == 0 ||
        response.requester_send_steady_ns == 0 ||
        response.source_session_id == 0 ||
        response.source_receive_utc_ns == 0 ||
        response.source_send_utc_ns < response.source_receive_utc_ns) {
        return false;
    }
    write_prefix(packet, kResponseType);
    std::span<std::uint8_t> bytes(packet);
    write_u64(bytes, 16, response.request_id);
    write_u64(bytes, 24, response.requester_send_steady_ns);
    write_u64(bytes, 32, response.source_session_id);
    write_u64(bytes, 40, response.source_receive_utc_ns);
    write_u64(bytes, 48, response.source_send_utc_ns);
    return true;
}

bool parse_response(std::span<const std::uint8_t> packet,
                    Response& response) noexcept {
    if (!valid_prefix(packet, kResponseBytes, kResponseType)) return false;
    Response parsed;
    parsed.request_id = read_u64(packet, 16);
    parsed.requester_send_steady_ns = read_u64(packet, 24);
    parsed.source_session_id = read_u64(packet, 32);
    parsed.source_receive_utc_ns = read_u64(packet, 40);
    parsed.source_send_utc_ns = read_u64(packet, 48);
    if (parsed.request_id == 0 || parsed.requester_send_steady_ns == 0 ||
        parsed.source_session_id == 0 ||
        parsed.source_receive_utc_ns == 0 ||
        parsed.source_send_utc_ns < parsed.source_receive_utc_ns) {
        return false;
    }
    response = parsed;
    return true;
}

bool AffineMapper::add_sample(const Sample& sample) noexcept {
    if (sample.source_session_id == 0 ||
        sample.requester_send_steady_ns == 0 ||
        sample.source_receive_utc_ns == 0 ||
        sample.source_send_utc_ns < sample.source_receive_utc_ns ||
        sample.requester_receive_steady_ns <
            sample.requester_send_steady_ns) {
        reset(MappingStatus::INVALID);
        return false;
    }
    const std::uint64_t local_round_trip =
        sample.requester_receive_steady_ns -
        sample.requester_send_steady_ns;
    const std::uint64_t source_processing =
        sample.source_send_utc_ns - sample.source_receive_utc_ns;
    if (source_processing > local_round_trip) {
        reset(MappingStatus::INVALID);
        return false;
    }
    if (source_session_id_ != 0 &&
        source_session_id_ != sample.source_session_id) {
        reset(MappingStatus::WARMING);
    }
    source_session_id_ = sample.source_session_id;
    reset_status_ = MappingStatus::WARMING;

    SamplePoint point;
    point.source_midpoint_ns =
        static_cast<double>(sample.source_receive_utc_ns) +
        static_cast<double>(source_processing) * 0.5;
    point.local_midpoint_ns =
        static_cast<double>(sample.requester_send_steady_ns) +
        static_cast<double>(local_round_trip) * 0.5;
    point.round_trip_ns =
        static_cast<double>(local_round_trip - source_processing);
    point.local_received_ns = sample.requester_receive_steady_ns;
    samples_[next_sample_] = point;
    next_sample_ = (next_sample_ + 1U) % samples_.size();
    sample_count_ = std::min(sample_count_ + 1U, samples_.size());
    return true;
}

MappingResult AffineMapper::map_utc_ns(
        std::uint64_t source_utc_ns,
        std::chrono::steady_clock::time_point local_now,
        std::chrono::steady_clock::duration maximum_mapping_age) const noexcept {
    MappingResult result;
    result.status = reset_status_;
    result.sample_count = sample_count_;
    result.source_session_id = source_session_id_;
    if (source_utc_ns == 0 || source_session_id_ == 0 || sample_count_ == 0) {
        return result;
    }

    const auto local_now_count =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            local_now.time_since_epoch()).count();
    const auto maximum_age_count =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            maximum_mapping_age).count();
    if (local_now_count <= 0 || maximum_age_count <= 0) {
        result.status = MappingStatus::INVALID;
        return result;
    }

    const SamplePoint* newest = nullptr;
    const SamplePoint* best = nullptr;
    double source_mean = 0.0;
    double local_mean = 0.0;
    for (std::size_t index = 0; index < sample_count_; ++index) {
        const SamplePoint& sample = samples_[index];
        if (!newest || sample.local_received_ns > newest->local_received_ns) {
            newest = &sample;
        }
        if (!best || sample.round_trip_ns < best->round_trip_ns) best = &sample;
        source_mean += sample.source_midpoint_ns;
        local_mean += sample.local_midpoint_ns;
    }
    if (!newest || !best ||
        static_cast<std::uint64_t>(local_now_count) < newest->local_received_ns) {
        result.status = MappingStatus::INVALID;
        return result;
    }
    const std::uint64_t mapping_age_ns =
        static_cast<std::uint64_t>(local_now_count) -
        newest->local_received_ns;
    result.mapping_age_ms = static_cast<double>(mapping_age_ns) / 1.0e6;
    if (mapping_age_ns > static_cast<std::uint64_t>(maximum_age_count)) {
        result.status = MappingStatus::STALE;
        return result;
    }
    if (sample_count_ < kMinimumValidSamples) {
        result.status = MappingStatus::WARMING;
        return result;
    }

    source_mean /= static_cast<double>(sample_count_);
    local_mean /= static_cast<double>(sample_count_);
    double covariance = 0.0;
    double source_variance = 0.0;
    for (std::size_t index = 0; index < sample_count_; ++index) {
        const double source_delta =
            samples_[index].source_midpoint_ns - source_mean;
        covariance += source_delta *
            (samples_[index].local_midpoint_ns - local_mean);
        source_variance += source_delta * source_delta;
    }
    if (source_variance < 1.0e12) {
        result.status = MappingStatus::WARMING;
        return result;
    }
    const double clock_rate = covariance / source_variance;
    // system_clock 与 steady_clock 都以纳秒归一化；百分之一以上的频率差
    // 表示源 UTC 跳变、会话串线或损坏样本，不能继续外推。
    if (!std::isfinite(clock_rate) || clock_rate < 0.99 ||
        clock_rate > 1.01) {
        result.status = MappingStatus::INVALID;
        return result;
    }
    const double mapped_ns = best->local_midpoint_ns + clock_rate *
        (static_cast<double>(source_utc_ns) - best->source_midpoint_ns);
    if (!std::isfinite(mapped_ns) || mapped_ns <= 0.0 ||
        mapped_ns > static_cast<double>(
            std::numeric_limits<std::int64_t>::max())) {
        result.status = MappingStatus::INVALID;
        return result;
    }
    double maximum_residual_ns = 0.0;
    for (std::size_t index = 0; index < sample_count_; ++index) {
        const double estimated = best->local_midpoint_ns + clock_rate *
            (samples_[index].source_midpoint_ns -
             best->source_midpoint_ns);
        maximum_residual_ns = std::max(
            maximum_residual_ns,
            std::abs(samples_[index].local_midpoint_ns - estimated));
    }

    result.status = MappingStatus::VALID;
    result.valid = true;
    result.local_time = std::chrono::steady_clock::time_point(
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::nanoseconds(
                static_cast<std::int64_t>(std::llround(mapped_ns)))));
    result.uncertainty_ms =
        (best->round_trip_ns * 0.5 + maximum_residual_ns) / 1.0e6;
    result.round_trip_ms = best->round_trip_ns / 1.0e6;
    result.clock_rate = clock_rate;
    return result;
}

void AffineMapper::reset(MappingStatus status) noexcept {
    samples_ = {};
    sample_count_ = 0;
    next_sample_ = 0;
    source_session_id_ = 0;
    reset_status_ = status;
}

} // namespace detail

struct Client::Impl {
    bool open(const ClientConfig& requested) noexcept {
        close();
        config = requested;
        if (config.source_url.empty()) return true;
        if (config.exchange_interval_ms < 50 ||
            config.exchange_interval_ms > 10000 ||
            config.response_timeout_ms < 10 ||
            config.response_timeout_ms > 5000 ||
            config.maximum_mapping_age_ms < config.exchange_interval_ms ||
            config.maximum_mapping_age_ms > 60000) {
            return set_error("NDI 时钟同步参数非法");
        }
        std::string host;
        std::uint16_t port = 0;
        if (!parse_udp_url(config.source_url, host, port) ||
            host == "0.0.0.0" || host == "*") {
            return set_error("NDI 时钟源地址格式非法");
        }
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            return set_error("初始化 NDI 时钟同步 Winsock 失败");
        }
        winsock_ready = true;
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;
        addrinfo* addresses = nullptr;
        const std::string port_text = std::to_string(port);
        if (getaddrinfo(host.c_str(), port_text.c_str(), &hints, &addresses) != 0 ||
            !addresses) {
            close_socket();
            return set_error("无法解析 NDI 时钟源地址");
        }
        for (const addrinfo* address = addresses; address;
             address = address->ai_next) {
            const SOCKET candidate = socket(
                address->ai_family, address->ai_socktype,
                address->ai_protocol);
            if (candidate == INVALID_SOCKET) continue;
            const DWORD receive_timeout = 50;
            if (setsockopt(candidate, SOL_SOCKET, SO_RCVTIMEO,
                           reinterpret_cast<const char*>(&receive_timeout),
                           sizeof(receive_timeout)) != SOCKET_ERROR &&
                connect(candidate, address->ai_addr,
                        static_cast<int>(address->ai_addrlen)) != SOCKET_ERROR) {
                socket_handle = candidate;
                break;
            }
            closesocket(candidate);
        }
        freeaddrinfo(addresses);
        if (socket_handle == INVALID_SOCKET) {
            close_socket();
            return set_error("连接 NDI 时钟源 UDP 端点失败");
        }
        stop_requested.store(false, std::memory_order_release);
        request_id = 0;
        {
            std::lock_guard<std::mutex> lock(mapping_mutex);
            mapper.reset();
        }
        set_error({});
        try {
            worker = std::thread([this] { run(); });
        } catch (...) {
            close_socket();
            return set_error("创建 NDI 时钟同步线程失败");
        }
        return true;
    }

    void run() noexcept {
        while (!stop_requested.load(std::memory_order_acquire)) {
            detail::Request request;
            request.request_id = ++request_id;
            request.requester_send_steady_ns = steady_now_ns();
            std::array<std::uint8_t, detail::kRequestBytes> request_packet{};
            const bool request_valid =
                detail::serialize_request(request, request_packet);
            const int sent = request_valid
                ? send(socket_handle,
                       reinterpret_cast<const char*>(request_packet.data()),
                       static_cast<int>(request_packet.size()), 0)
                : SOCKET_ERROR;
            if (sent == static_cast<int>(request_packet.size())) {
                const auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(config.response_timeout_ms);
                while (!stop_requested.load(std::memory_order_acquire) &&
                       std::chrono::steady_clock::now() < deadline) {
                    std::array<std::uint8_t, detail::kResponseBytes> response_packet{};
                    const int received = recv(
                        socket_handle,
                        reinterpret_cast<char*>(response_packet.data()),
                        static_cast<int>(response_packet.size()), 0);
                    if (received == SOCKET_ERROR) {
                        const int error = WSAGetLastError();
                        if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK) {
                            continue;
                        }
                        break;
                    }
                    const std::uint64_t received_at = steady_now_ns();
                    detail::Response response;
                    if (received == static_cast<int>(response_packet.size()) &&
                        detail::parse_response(response_packet, response) &&
                        response.request_id == request.request_id &&
                        response.requester_send_steady_ns ==
                            request.requester_send_steady_ns) {
                        detail::Sample sample;
                        sample.source_session_id = response.source_session_id;
                        sample.requester_send_steady_ns =
                            request.requester_send_steady_ns;
                        sample.source_receive_utc_ns =
                            response.source_receive_utc_ns;
                        sample.source_send_utc_ns = response.source_send_utc_ns;
                        sample.requester_receive_steady_ns = received_at;
                        std::lock_guard<std::mutex> lock(mapping_mutex);
                        mapper.add_sample(sample);
                        break;
                    }
                }
            }
            std::unique_lock<std::mutex> lock(wait_mutex);
            wake.wait_for(
                lock, std::chrono::milliseconds(config.exchange_interval_ms),
                [this] {
                    return stop_requested.load(std::memory_order_acquire);
                });
        }
    }

    MappingResult map(std::int64_t timestamp_100ns,
                      std::chrono::steady_clock::time_point local_now) const noexcept {
        if (timestamp_100ns <= 0 ||
            timestamp_100ns > std::numeric_limits<std::int64_t>::max() / 100) {
            MappingResult result;
            result.status = MappingStatus::INVALID;
            return result;
        }
        const std::uint64_t source_utc_ns =
            static_cast<std::uint64_t>(timestamp_100ns) * 100U;
        std::lock_guard<std::mutex> lock(mapping_mutex);
        return mapper.map_utc_ns(
            source_utc_ns, local_now,
            std::chrono::milliseconds(config.maximum_mapping_age_ms));
    }

    void close() noexcept {
        stop_requested.store(true, std::memory_order_release);
        wake.notify_all();
        try {
            if (worker.joinable()) worker.join();
        } catch (...) {
        }
        close_socket();
        {
            std::lock_guard<std::mutex> lock(mapping_mutex);
            mapper.reset();
        }
        request_id = 0;
    }

    void close_socket() noexcept {
        if (socket_handle != INVALID_SOCKET) {
            closesocket(socket_handle);
            socket_handle = INVALID_SOCKET;
        }
        if (winsock_ready) {
            WSACleanup();
            winsock_ready = false;
        }
    }

    bool set_error(const std::string& message) noexcept {
        try {
            std::lock_guard<std::mutex> lock(error_mutex);
            error = message;
        } catch (...) {
        }
        return false;
    }

    std::string get_error() const {
        std::lock_guard<std::mutex> lock(error_mutex);
        return error;
    }

    ClientConfig config;
    SOCKET socket_handle = INVALID_SOCKET;
    bool winsock_ready = false;
    std::thread worker;
    std::atomic<bool> stop_requested{false};
    std::condition_variable wake;
    std::mutex wait_mutex;
    mutable std::mutex mapping_mutex;
    detail::AffineMapper mapper;
    std::uint64_t request_id = 0;
    mutable std::mutex error_mutex;
    std::string error;
};

struct Server::Impl {
    bool open(const ServerConfig& requested) noexcept {
        close();
        std::string host;
        std::uint16_t port = 0;
        if (!parse_udp_url(requested.bind_url, host, port)) {
            return set_error("源机时钟监听地址格式非法");
        }
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            return set_error("初始化源机时钟服务 Winsock 失败");
        }
        winsock_ready = true;
        socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_handle == INVALID_SOCKET) {
            close_socket();
            return set_error("创建源机时钟 UDP 套接字失败");
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        if (host == "0.0.0.0" || host == "*") {
            address.sin_addr.s_addr = htonl(INADDR_ANY);
        } else if (InetPtonA(AF_INET, host.c_str(), &address.sin_addr) != 1) {
            close_socket();
            return set_error("源机时钟监听地址必须是 IPv4 地址");
        }
        if (bind(socket_handle, reinterpret_cast<sockaddr*>(&address),
                 sizeof(address)) == SOCKET_ERROR) {
            close_socket();
            return set_error("绑定源机时钟 UDP 端点失败");
        }
        if (BCryptGenRandom(
                nullptr, reinterpret_cast<PUCHAR>(&session_id),
                sizeof(session_id), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 ||
            session_id == 0) {
            close_socket();
            return set_error("生成源机时钟 session_id 失败");
        }
        set_error({});
        return true;
    }

    bool serve_once(int timeout_ms) noexcept {
        if (socket_handle == INVALID_SOCKET || timeout_ms < 0 ||
            timeout_ms > 5000) {
            return set_error("源机时钟服务尚未打开或等待参数非法");
        }
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(socket_handle, &read_set);
        timeval timeout{};
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_usec = (timeout_ms % 1000) * 1000;
        const int ready = select(0, &read_set, nullptr, nullptr, &timeout);
        if (ready == 0) return true;
        if (ready == SOCKET_ERROR) {
            return set_error("源机时钟服务等待请求失败");
        }
        std::array<std::uint8_t, detail::kRequestBytes> request_packet{};
        sockaddr_in source{};
        int source_size = sizeof(source);
        const int received = recvfrom(
            socket_handle, reinterpret_cast<char*>(request_packet.data()),
            static_cast<int>(request_packet.size()), 0,
            reinterpret_cast<sockaddr*>(&source), &source_size);
        const std::uint64_t source_received_at = utc_now_ns();
        if (received != static_cast<int>(request_packet.size()) ||
            source_size != sizeof(source)) {
            return true;
        }
        detail::Request request;
        if (!detail::parse_request(request_packet, request)) return true;

        detail::Response response;
        response.request_id = request.request_id;
        response.requester_send_steady_ns =
            request.requester_send_steady_ns;
        response.source_session_id = session_id;
        response.source_receive_utc_ns = source_received_at;
        response.source_send_utc_ns = utc_now_ns();
        std::array<std::uint8_t, detail::kResponseBytes> response_packet{};
        if (!detail::serialize_response(response, response_packet)) {
            return set_error("源机时钟响应序列化失败");
        }
        const int sent = sendto(
            socket_handle,
            reinterpret_cast<const char*>(response_packet.data()),
            static_cast<int>(response_packet.size()), 0,
            reinterpret_cast<const sockaddr*>(&source), source_size);
        return sent == static_cast<int>(response_packet.size()) ||
               set_error("源机时钟响应发送失败");
    }

    void close() noexcept {
        close_socket();
        session_id = 0;
    }

    void close_socket() noexcept {
        if (socket_handle != INVALID_SOCKET) {
            closesocket(socket_handle);
            socket_handle = INVALID_SOCKET;
        }
        if (winsock_ready) {
            WSACleanup();
            winsock_ready = false;
        }
    }

    bool set_error(const std::string& message) noexcept {
        try {
            std::lock_guard<std::mutex> lock(error_mutex);
            error = message;
        } catch (...) {
        }
        return false;
    }

    std::string get_error() const {
        std::lock_guard<std::mutex> lock(error_mutex);
        return error;
    }

    SOCKET socket_handle = INVALID_SOCKET;
    bool winsock_ready = false;
    std::uint64_t session_id = 0;
    mutable std::mutex error_mutex;
    std::string error;
};

const char* MappingStatusName(MappingStatus status) noexcept {
    switch (status) {
        case MappingStatus::UNSYNCHRONIZED: return "UNSYNCHRONIZED";
        case MappingStatus::WARMING: return "WARMING";
        case MappingStatus::VALID: return "VALID";
        case MappingStatus::STALE: return "STALE";
        case MappingStatus::INVALID: return "INVALID";
    }
    return "UNKNOWN";
}

Client::Client() noexcept {
    try {
        impl_ = std::make_unique<Impl>();
    } catch (...) {
    }
}

Client::~Client() { close(); }

bool Client::open(const ClientConfig& config) noexcept {
    return impl_ && impl_->open(config);
}

MappingResult Client::map_utc_100ns(
        std::int64_t source_timestamp_100ns,
        std::chrono::steady_clock::time_point local_now) const noexcept {
    if (!impl_) return {};
    return impl_->map(source_timestamp_100ns, local_now);
}

void Client::close() noexcept {
    if (impl_) impl_->close();
}

std::string Client::last_error() const {
    return impl_ ? impl_->get_error() : "NDI 时钟客户端未创建";
}

Server::Server() noexcept {
    try {
        impl_ = std::make_unique<Impl>();
    } catch (...) {
    }
}

Server::~Server() { close(); }

bool Server::open(const ServerConfig& config) noexcept {
    return impl_ && impl_->open(config);
}

bool Server::serve_once(int timeout_ms) noexcept {
    return impl_ && impl_->serve_once(timeout_ms);
}

void Server::close() noexcept {
    if (impl_) impl_->close();
}

std::string Server::last_error() const {
    return impl_ ? impl_->get_error() : "源机时钟服务未创建";
}

} // namespace clock_sync
