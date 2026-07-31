#ifndef CAPTURE_XUDP_INTERNAL_H
#define CAPTURE_XUDP_INTERNAL_H

#include "capture/capture.h"
#include "capture/network_internal.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace capture::detail {

inline constexpr std::size_t kXudpHeaderBytes = 124;
inline constexpr std::size_t kXudpSha256Bytes = 32;
inline constexpr std::size_t kXudpMaxFrameBytes = 8 * 1024 * 1024;
inline constexpr std::uint16_t kXudpMaxFragments = 4096;

struct XudpFrameDescriptor {
    std::uint64_t stream_id = 0;
    std::uint64_t frame_id = 0;
    std::uint32_t frame_size = 0;
    std::uint32_t encoded_width = 0;
    std::uint32_t encoded_height = 0;
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
    std::uint32_t source_roi_x = 0;
    std::uint32_t source_roi_y = 0;
    std::uint32_t source_roi_width = 0;
    std::uint32_t source_roi_height = 0;
    std::uint32_t frame_rate_n = 0;
    std::uint32_t frame_rate_d = 0;
    // 发送端单调时钟纳秒值；跨机器没有时钟同步时只能作为源端诊断。
    std::uint64_t sent_timestamp_ns = 0;
};

struct XudpPacketHeader {
    XudpFrameDescriptor frame;
    std::uint16_t fragment_index = 0;
    std::uint16_t fragment_count = 0;
    std::uint32_t fragment_offset = 0;
    std::uint32_t fragment_payload_size = 0;
    std::array<std::uint8_t, kXudpSha256Bytes> frame_sha256{};
};

enum class XudpConsumeResult {
    IGNORED,
    INCOMPLETE,
    FRAME,
    INVALID_PACKET,
};

struct XudpCompletedFrame {
    XudpFrameDescriptor descriptor;
    // 仅在下一次 consume_packet()/reset() 前有效；调用方必须立即解码。
    std::span<const std::uint8_t> jpeg;
    std::chrono::steady_clock::time_point started_at{};
    std::uint64_t source_received_frames = 0;
    std::uint64_t transport_dropped_frames = 0;
    std::uint64_t transport_invalid_packets = 0;
};

bool compute_xudp_frame_sha256(
    const XudpFrameDescriptor& descriptor,
    std::span<const std::uint8_t> frame_payload,
    std::array<std::uint8_t, kXudpSha256Bytes>& sha256) noexcept;

bool serialize_xudp_packet(
    const XudpPacketHeader& header,
    std::span<const std::uint8_t> fragment_payload,
    std::vector<std::uint8_t>& packet) noexcept;

bool parse_xudp_packet(
    std::span<const std::uint8_t> packet,
    XudpPacketHeader& header,
    std::span<const std::uint8_t>& fragment_payload) noexcept;

// 生产发送端复用同一个 CNG 哈希句柄和单个数据报缓冲。prepare_frame() 只
// 计算一次帧级 SHA-256，随后按索引顺序序列化互不重叠的连续分片。
class XudpFramePacketizer {
public:
    XudpFramePacketizer() noexcept;
    ~XudpFramePacketizer();

    XudpFramePacketizer(const XudpFramePacketizer&) = delete;
    XudpFramePacketizer& operator=(const XudpFramePacketizer&) = delete;

    bool prepare_frame(
        const XudpFrameDescriptor& descriptor,
        std::span<const std::uint8_t> frame_payload,
        std::size_t max_datagram_bytes) noexcept;
    std::size_t fragment_count() const noexcept;
    bool serialize_fragment(
        std::size_t fragment_index,
        std::span<const std::uint8_t> frame_payload,
        std::vector<std::uint8_t>& packet) const noexcept;
    void reset() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class XudpFrameAssembler {
public:
    XudpFrameAssembler();
    ~XudpFrameAssembler();

    XudpFrameAssembler(const XudpFrameAssembler&) = delete;
    XudpFrameAssembler& operator=(const XudpFrameAssembler&) = delete;

    XudpConsumeResult consume_packet(
        std::span<const std::uint8_t> packet,
        std::chrono::steady_clock::time_point received_at,
        XudpCompletedFrame& completed) noexcept;
    void reset() noexcept;

    std::uint64_t source_received_frames() const noexcept;
    std::uint64_t transport_dropped_frames() const noexcept;
    std::uint64_t transport_invalid_packets() const noexcept;
    // 完整分片通过协议校验后，若 JPEG 解码或尺寸契约仍失败，由生产后端补记。
    void record_invalid_frame() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

bool resolve_xudp_frame_geometry(
    const CaptureConfig& config,
    const XudpFrameDescriptor& descriptor,
    NetworkFrameGeometry& geometry) noexcept;

std::unique_ptr<ICapture> create_xudp_capture(
    const CaptureConfig& config) noexcept;

} // namespace capture::detail

#endif // CAPTURE_XUDP_INTERNAL_H
