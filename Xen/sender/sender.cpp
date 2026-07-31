#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <bcrypt.h>

#ifdef ERROR
#undef ERROR
#endif

#include "sender/sender.h"

#include "capture/udp_internal.h"
#include "capture/xudp_internal.h"
#include "log/log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>

namespace {

constexpr int kMinimumSendBufferBytes = 64 * 1024;
constexpr int kMaximumSendBufferBytes = 16 * 1024 * 1024;
constexpr std::uint32_t kMaximumFrameRatePart = 1'000'000;
constexpr int kMaximumDimension = 16'384;

void saturating_increment(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) ++value;
}

void saturating_add(std::uint64_t& value, std::uint64_t amount) noexcept {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    value = amount > maximum - value ? maximum : value + amount;
}

bool exact_nonnegative_integer(double value,
                               std::uint32_t& converted) noexcept {
    if (!std::isfinite(value) || value < 0.0 ||
        value > static_cast<double>(
            std::numeric_limits<std::uint32_t>::max())) {
        return false;
    }
    const double rounded = std::round(value);
    if (std::abs(value - rounded) > 1e-6) return false;
    converted = static_cast<std::uint32_t>(rounded);
    return true;
}

bool terminal_socket_error(int error) noexcept {
    return error != WSAEWOULDBLOCK && error != WSAETIMEDOUT &&
           error != WSAENOBUFS;
}

} // namespace

struct XudpSender::Impl {
    bool open(const XudpSenderConfig& requested) noexcept {
        close();
        Log::register_module("sender", LogLevel::INFO);
        try {
            std::string destination_host;
            std::uint16_t destination_port = 0;
            if (!valid_config(requested) ||
                !capture::detail::parse_udp_url(
                    requested.destination_url, destination_host,
                    destination_port) ||
                destination_host == "0.0.0.0" || destination_host == "*") {
                return fail_open(
                    XudpSenderStatus::INVALID_CONFIG,
                    "XUDP Sender 配置非法");
            }
            if (!open_socket(
                    destination_host, destination_port, requested)) {
                return false;
            }

            std::uint64_t generated_stream_id = 0;
            if (BCryptGenRandom(
                    nullptr,
                    reinterpret_cast<PUCHAR>(&generated_stream_id),
                    sizeof(generated_stream_id),
                    BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 ||
                generated_stream_id == 0U) {
                release_socket();
                return fail_open(
                    XudpSenderStatus::FAILURE,
                    "生成 XUDP stream_id 失败");
            }

            config = requested;
            stream_id = generated_stream_id;
            next_frame_id = 0;
            stats_value = {};
            stats_value.stream_id = stream_id;
            jpeg.clear();
            jpeg.reserve(512U * 1024U);
            packet.clear();
            packet.reserve(static_cast<std::size_t>(
                config.max_datagram_bytes));
            jpeg_parameters = {
                cv::IMWRITE_JPEG_QUALITY, config.jpeg_quality,
            };
            packetizer.reset();
            set_error({});
            status_value.store(
                XudpSenderStatus::READY, std::memory_order_release);
            LOG_INFO(
                "sender",
                "XUDP Sender 已连接: destination={}, quality={}, "
                "datagram={}B, fps={}/{}",
                config.destination_url, config.jpeg_quality,
                config.max_datagram_bytes, config.frame_rate_n,
                config.frame_rate_d);
            return true;
        } catch (...) {
            release_socket();
            return fail_open(
                XudpSenderStatus::FAILURE,
                "打开 XUDP Sender 时发生未知异常");
        }
    }

    bool send_frame(const CapturedFrame& frame) noexcept {
        if (status_value.load(std::memory_order_acquire) !=
                XudpSenderStatus::READY ||
            socket_handle == INVALID_SOCKET) {
            set_error("XUDP Sender 尚未打开");
            return false;
        }

        const auto started = std::chrono::steady_clock::now();
        capture::detail::XudpFrameDescriptor descriptor;
        if (!resolve_descriptor_geometry(frame, descriptor)) {
            record_failure(0, started, started, started, started);
            set_error("待发送帧的主机几何或 BGR 格式非法");
            return false;
        }
        if (next_frame_id == std::numeric_limits<std::uint64_t>::max()) {
            record_failure(0, started, started, started, started);
            set_error("XUDP frame_id 已耗尽，需要重建发送会话");
            return false;
        }

        const std::uint64_t frame_id = ++next_frame_id;
        descriptor.stream_id = stream_id;
        descriptor.frame_id = frame_id;
        descriptor.frame_rate_n = config.frame_rate_n;
        descriptor.frame_rate_d = config.frame_rate_d;
        stats_value.last_frame_id = frame_id;

        try {
            jpeg.clear();
            if (!cv::imencode(
                    ".jpg", frame.bgr, jpeg, jpeg_parameters) ||
                jpeg.empty() ||
                jpeg.size() > capture::detail::kXudpMaxFrameBytes ||
                jpeg.size() > std::numeric_limits<std::uint32_t>::max()) {
                const auto failed_at = std::chrono::steady_clock::now();
                record_failure(frame_id, started, failed_at,
                               failed_at, failed_at);
                set_error("JPEG 编码失败或结果超过 XUDP 帧上限");
                return false;
            }
            const auto encoded_at = std::chrono::steady_clock::now();
            descriptor.frame_size = static_cast<std::uint32_t>(jpeg.size());
            const auto sent_timestamp =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    encoded_at.time_since_epoch()).count();
            if (sent_timestamp < 0) {
                record_failure(frame_id, started, encoded_at,
                               encoded_at, encoded_at);
                set_error("发送端单调时钟不能表示为 XUDP 时间戳");
                return false;
            }
            descriptor.sent_timestamp_ns =
                static_cast<std::uint64_t>(sent_timestamp);

            const std::span<const std::uint8_t> payload(jpeg);
            if (!packetizer.prepare_frame(
                    descriptor, payload,
                    static_cast<std::size_t>(config.max_datagram_bytes))) {
                const auto failed_at = std::chrono::steady_clock::now();
                record_failure(frame_id, started, encoded_at,
                               failed_at, failed_at);
                set_error("XUDP 帧哈希或分片准备失败");
                return false;
            }
            const auto packetized_at = std::chrono::steady_clock::now();
            const std::size_t fragment_count =
                packetizer.fragment_count();
            std::uint64_t sent_datagrams = 0;
            std::uint64_t sent_wire_bytes = 0;
            for (std::size_t index = 0; index < fragment_count; ++index) {
                if (!packetizer.serialize_fragment(index, payload, packet) ||
                    packet.empty() ||
                    packet.size() > static_cast<std::size_t>(
                        config.max_datagram_bytes)) {
                    const auto failed_at = std::chrono::steady_clock::now();
                    record_failure(frame_id, started, encoded_at,
                                   packetized_at, failed_at);
                    set_error("XUDP 分片序列化失败");
                    return false;
                }

                const int sent = ::send(
                    socket_handle,
                    reinterpret_cast<const char*>(packet.data()),
                    static_cast<int>(packet.size()), 0);
                if (sent != static_cast<int>(packet.size())) {
                    const int error = WSAGetLastError();
                    const auto failed_at = std::chrono::steady_clock::now();
                    record_failure(frame_id, started, encoded_at,
                                   packetized_at, failed_at);
                    set_error(
                        "XUDP 数据报发送失败，Winsock 错误码: " +
                        std::to_string(error));
                    if (terminal_socket_error(error)) {
                        status_value.store(
                            XudpSenderStatus::FAILURE,
                            std::memory_order_release);
                    }
                    return false;
                }
                ++sent_datagrams;
                saturating_add(
                    sent_wire_bytes,
                    static_cast<std::uint64_t>(packet.size()));
                saturating_increment(stats_value.datagrams_sent);
                saturating_add(
                    stats_value.wire_bytes_sent,
                    static_cast<std::uint64_t>(packet.size()));
                stats_value.largest_datagram_bytes = std::max(
                    stats_value.largest_datagram_bytes,
                    static_cast<std::uint64_t>(packet.size()));
            }

            const auto finished = std::chrono::steady_clock::now();
            saturating_increment(stats_value.frames_sent);
            saturating_add(
                stats_value.jpeg_bytes_sent,
                static_cast<std::uint64_t>(jpeg.size()));
            stats_value.last_frame_datagrams = sent_datagrams;
            stats_value.last_frame_jpeg_bytes =
                static_cast<std::uint64_t>(jpeg.size());
            stats_value.last_frame_wire_bytes = sent_wire_bytes;
            update_timings(
                started, encoded_at, packetized_at, finished);
            set_error({});
            return true;
        } catch (...) {
            const auto failed_at = std::chrono::steady_clock::now();
            record_failure(frame_id, started, failed_at,
                           failed_at, failed_at);
            set_error("发送 XUDP 帧时发生未知异常");
            return false;
        }
    }

    void close() noexcept {
        release_socket();
        packetizer.reset();
        jpeg.clear();
        packet.clear();
        jpeg_parameters.clear();
        stream_id = 0;
        next_frame_id = 0;
        status_value.store(
            XudpSenderStatus::CLOSED, std::memory_order_release);
    }

    bool valid_config(const XudpSenderConfig& candidate) const noexcept {
        return !candidate.destination_url.empty() &&
               candidate.jpeg_quality >= 1 &&
               candidate.jpeg_quality <= 100 &&
               candidate.max_datagram_bytes >
                   static_cast<int>(capture::detail::kXudpHeaderBytes) &&
               candidate.max_datagram_bytes <= 65507 &&
               candidate.socket_send_buffer_bytes >=
                   kMinimumSendBufferBytes &&
               candidate.socket_send_buffer_bytes <=
                   kMaximumSendBufferBytes &&
               candidate.socket_send_timeout_ms > 0 &&
               candidate.socket_send_timeout_ms <= 1000 &&
               candidate.frame_rate_n > 0 &&
               candidate.frame_rate_n <= kMaximumFrameRatePart &&
               candidate.frame_rate_d > 0 &&
               candidate.frame_rate_d <= kMaximumFrameRatePart;
    }

    bool open_socket(const std::string& host, std::uint16_t port,
                     const XudpSenderConfig& requested) noexcept {
        WSADATA winsock_data{};
        if (WSAStartup(MAKEWORD(2, 2), &winsock_data) != 0) {
            return fail_open(
                XudpSenderStatus::FAILURE,
                "初始化 Winsock 失败");
        }
        winsock_ready = true;

        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;
        addrinfo* addresses = nullptr;
        const std::string port_text = std::to_string(port);
        const int resolve_result = getaddrinfo(
            host.c_str(), port_text.c_str(), &hints, &addresses);
        if (resolve_result != 0 || !addresses) {
            release_socket();
            return fail_open(
                XudpSenderStatus::INVALID_CONFIG,
                "无法解析 XUDP 目的地址: " + host);
        }

        for (const addrinfo* address = addresses; address != nullptr;
             address = address->ai_next) {
            const SOCKET candidate = socket(
                address->ai_family, address->ai_socktype,
                address->ai_protocol);
            if (candidate == INVALID_SOCKET) continue;
            const DWORD timeout =
                static_cast<DWORD>(requested.socket_send_timeout_ms);
            const int send_buffer = requested.socket_send_buffer_bytes;
            const bool configured =
                setsockopt(candidate, SOL_SOCKET, SO_SNDTIMEO,
                           reinterpret_cast<const char*>(&timeout),
                           sizeof(timeout)) != SOCKET_ERROR &&
                setsockopt(candidate, SOL_SOCKET, SO_SNDBUF,
                           reinterpret_cast<const char*>(&send_buffer),
                           sizeof(send_buffer)) != SOCKET_ERROR;
            if (configured &&
                connect(candidate, address->ai_addr,
                        static_cast<int>(address->ai_addrlen)) !=
                    SOCKET_ERROR) {
                socket_handle = candidate;
                break;
            }
            closesocket(candidate);
        }
        freeaddrinfo(addresses);
        if (socket_handle == INVALID_SOCKET) {
            release_socket();
            return fail_open(
                XudpSenderStatus::FAILURE,
                "创建或连接 XUDP UDP 套接字失败");
        }
        return true;
    }

    void release_socket() noexcept {
        if (socket_handle != INVALID_SOCKET) {
            closesocket(socket_handle);
            socket_handle = INVALID_SOCKET;
        }
        if (winsock_ready) {
            WSACleanup();
            winsock_ready = false;
        }
    }

    bool resolve_descriptor_geometry(
        const CapturedFrame& frame,
        capture::detail::XudpFrameDescriptor& descriptor) const noexcept {
        if (frame.bgr.empty() || frame.bgr.type() != CV_8UC3 ||
            frame.bgr.cols <= 0 || frame.bgr.rows <= 0 ||
            frame.bgr.cols > kMaximumDimension ||
            frame.bgr.rows > kMaximumDimension ||
            frame.source_width <= 0 || frame.source_height <= 0 ||
            frame.source_width > kMaximumDimension ||
            frame.source_height > kMaximumDimension ||
            !std::isfinite(frame.source_pixels_per_pixel_x) ||
            !std::isfinite(frame.source_pixels_per_pixel_y) ||
            frame.source_pixels_per_pixel_x <= 0.0 ||
            frame.source_pixels_per_pixel_y <= 0.0) {
            return false;
        }

        std::uint32_t roi_x = 0;
        std::uint32_t roi_y = 0;
        std::uint32_t roi_width = 0;
        std::uint32_t roi_height = 0;
        // XUDP v1 几何字段是整数。不能用强制转换静默截断小数坐标或
        // 非整数缩放结果，否则辅机还原出的主机准星会发生系统性偏移。
        if (!exact_nonnegative_integer(frame.roi_x, roi_x) ||
            !exact_nonnegative_integer(frame.roi_y, roi_y) ||
            !exact_nonnegative_integer(
                frame.bgr.cols * frame.source_pixels_per_pixel_x,
                roi_width) ||
            !exact_nonnegative_integer(
                frame.bgr.rows * frame.source_pixels_per_pixel_y,
                roi_height) ||
            roi_width == 0 || roi_height == 0 ||
            roi_x > static_cast<std::uint32_t>(frame.source_width) ||
            roi_y > static_cast<std::uint32_t>(frame.source_height) ||
            roi_width > static_cast<std::uint32_t>(frame.source_width) -
                            roi_x ||
            roi_height > static_cast<std::uint32_t>(frame.source_height) -
                             roi_y) {
            return false;
        }

        descriptor.encoded_width =
            static_cast<std::uint32_t>(frame.bgr.cols);
        descriptor.encoded_height =
            static_cast<std::uint32_t>(frame.bgr.rows);
        descriptor.source_width =
            static_cast<std::uint32_t>(frame.source_width);
        descriptor.source_height =
            static_cast<std::uint32_t>(frame.source_height);
        descriptor.source_roi_x = roi_x;
        descriptor.source_roi_y = roi_y;
        descriptor.source_roi_width = roi_width;
        descriptor.source_roi_height = roi_height;
        return true;
    }

    void update_timings(
        std::chrono::steady_clock::time_point started,
        std::chrono::steady_clock::time_point encoded,
        std::chrono::steady_clock::time_point packetized,
        std::chrono::steady_clock::time_point finished) noexcept {
        stats_value.last_encode_ms =
            std::chrono::duration<double, std::milli>(
                encoded - started).count();
        stats_value.last_packetize_ms =
            std::chrono::duration<double, std::milli>(
                packetized - encoded).count();
        stats_value.last_send_ms =
            std::chrono::duration<double, std::milli>(
                finished - packetized).count();
        stats_value.last_total_ms =
            std::chrono::duration<double, std::milli>(
                finished - started).count();
    }

    void record_failure(
        std::uint64_t frame_id,
        std::chrono::steady_clock::time_point started,
        std::chrono::steady_clock::time_point encoded,
        std::chrono::steady_clock::time_point packetized,
        std::chrono::steady_clock::time_point finished) noexcept {
        saturating_increment(stats_value.frames_failed);
        // 几何校验等前置失败尚未分配 frame_id，不得抹掉最后一个已分配帧号。
        if (frame_id != 0) stats_value.last_frame_id = frame_id;
        stats_value.last_frame_datagrams = 0;
        stats_value.last_frame_jpeg_bytes = 0;
        stats_value.last_frame_wire_bytes = 0;
        update_timings(started, encoded, packetized, finished);
    }

    bool fail_open(XudpSenderStatus failure_status,
                   const std::string& message) noexcept {
        status_value.store(failure_status, std::memory_order_release);
        set_error(message);
        LOG_ERROR("sender", "{}", message);
        return false;
    }

    void set_error(const std::string& message) noexcept {
        try {
            last_error_value = message;
        } catch (...) {
        }
    }

    XudpSenderConfig config;
    SOCKET socket_handle = INVALID_SOCKET;
    bool winsock_ready = false;
    capture::detail::XudpFramePacketizer packetizer;
    std::vector<std::uint8_t> jpeg;
    std::vector<std::uint8_t> packet;
    std::vector<int> jpeg_parameters;
    std::atomic<XudpSenderStatus> status_value{XudpSenderStatus::CLOSED};
    std::string last_error_value;
    XudpSenderStats stats_value;
    std::uint64_t stream_id = 0;
    std::uint64_t next_frame_id = 0;
};

const char* XudpSenderStatusName(XudpSenderStatus status) noexcept {
    switch (status) {
        case XudpSenderStatus::CLOSED: return "CLOSED";
        case XudpSenderStatus::READY: return "READY";
        case XudpSenderStatus::INVALID_CONFIG: return "INVALID_CONFIG";
        case XudpSenderStatus::FAILURE: return "FAILURE";
    }
    return "UNKNOWN";
}

XudpSender::XudpSender() noexcept {
    try {
        impl_ = std::make_unique<Impl>();
    } catch (...) {
    }
}

XudpSender::~XudpSender() {
    close();
}

bool XudpSender::open(const XudpSenderConfig& config) noexcept {
    return impl_ && impl_->open(config);
}

bool XudpSender::send_frame(const CapturedFrame& frame) noexcept {
    return impl_ && impl_->send_frame(frame);
}

void XudpSender::close() noexcept {
    if (impl_) impl_->close();
}

XudpSenderStatus XudpSender::status() const noexcept {
    return impl_
        ? impl_->status_value.load(std::memory_order_acquire)
        : XudpSenderStatus::FAILURE;
}

XudpSenderStats XudpSender::stats() const noexcept {
    return impl_ ? impl_->stats_value : XudpSenderStats{};
}

std::string XudpSender::last_error() const {
    try {
        return impl_ ? impl_->last_error_value : "XUDP Sender 初始化失败";
    } catch (...) {
        return "读取 XUDP Sender 错误信息失败";
    }
}
