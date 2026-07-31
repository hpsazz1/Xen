#ifndef SENDER_H
#define SENDER_H

#include "capture/capture.h"

#include <cstdint>
#include <memory>
#include <string>

enum class XudpSenderStatus {
    CLOSED,
    READY,
    INVALID_CONFIG,
    FAILURE,
};

const char* XudpSenderStatusName(XudpSenderStatus status) noexcept;

struct XudpSenderConfig {
    // 辅机 XUDP Capture 的 IPv4 地址；0.0.0.0 仅可用于接收绑定，不可作为目的地址。
    std::string destination_url = "udp://127.0.0.1:5000";
    int jpeg_quality = 85;
    // 包含 124 字节 XUDP 头。默认 1400，避免常见 1500 MTU 下的 IP 分片。
    int max_datagram_bytes = 1400;
    int socket_send_buffer_bytes = 4 * 1024 * 1024;
    int socket_send_timeout_ms = 10;
    std::uint32_t frame_rate_n = 240;
    std::uint32_t frame_rate_d = 1;
};

struct XudpSenderStats {
    std::uint64_t stream_id = 0;
    std::uint64_t last_frame_id = 0;
    std::uint64_t frames_sent = 0;
    std::uint64_t frames_failed = 0;
    std::uint64_t datagrams_sent = 0;
    std::uint64_t jpeg_bytes_sent = 0;
    std::uint64_t wire_bytes_sent = 0;
    std::uint64_t largest_datagram_bytes = 0;
    std::uint64_t last_frame_datagrams = 0;
    std::uint64_t last_frame_jpeg_bytes = 0;
    double last_encode_ms = 0.0;
    double last_packetize_ms = 0.0;
    double last_send_ms = 0.0;
    double last_total_ms = 0.0;
};

// 同步单线程发送器：调用线程完成 JPEG、帧哈希、分片和 send()。对象不在内部
// 建线程，也不排队旧帧；调用方应直接提供 Capture 最新帧。
class XudpSender {
public:
    XudpSender() noexcept;
    ~XudpSender();

    XudpSender(const XudpSender&) = delete;
    XudpSender& operator=(const XudpSender&) = delete;

    bool open(const XudpSenderConfig& config) noexcept;
    bool send_frame(const CapturedFrame& frame) noexcept;
    void close() noexcept;

    XudpSenderStatus status() const noexcept;
    XudpSenderStats stats() const noexcept;
    std::string last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif // SENDER_H
