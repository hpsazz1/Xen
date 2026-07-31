#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>
#include <bcrypt.h>

#include "capture/xudp_internal.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>

namespace capture::detail {

namespace {

constexpr std::array<std::uint8_t, 4> kMagic{{'X', 'U', 'D', 'P'}};
constexpr std::uint16_t kVersion = 1;
constexpr std::uint32_t kCodecJpeg = 1;
constexpr std::uint32_t kMaxDimension = 16384;
constexpr std::uint32_t kMaxFrameRateNumerator = 1000000;
constexpr std::size_t kHashDescriptorBytes = 78;
constexpr std::size_t kAssemblyCount = 3;
constexpr std::size_t kRetiredStreamCount = 8;

bool nt_success(NTSTATUS status) noexcept {
    return status >= 0;
}

std::uint16_t read_u16(std::span<const std::uint8_t> bytes,
                       std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[offset]) << 8) |
        static_cast<std::uint16_t>(bytes[offset + 1]));
}

std::uint32_t read_u32(std::span<const std::uint8_t> bytes,
                       std::size_t offset) noexcept {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

std::uint64_t read_u64(std::span<const std::uint8_t> bytes,
                       std::size_t offset) noexcept {
    return (static_cast<std::uint64_t>(read_u32(bytes, offset)) << 32) |
           static_cast<std::uint64_t>(read_u32(bytes, offset + 4));
}

void write_u16(std::span<std::uint8_t> bytes,
               std::size_t offset,
               std::uint16_t value) noexcept {
    bytes[offset] = static_cast<std::uint8_t>((value >> 8) & 0xff);
    bytes[offset + 1] = static_cast<std::uint8_t>(value & 0xff);
}

void write_u32(std::span<std::uint8_t> bytes,
               std::size_t offset,
               std::uint32_t value) noexcept {
    bytes[offset] = static_cast<std::uint8_t>((value >> 24) & 0xff);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 16) & 0xff);
    bytes[offset + 2] = static_cast<std::uint8_t>((value >> 8) & 0xff);
    bytes[offset + 3] = static_cast<std::uint8_t>(value & 0xff);
}

void write_u64(std::span<std::uint8_t> bytes,
               std::size_t offset,
               std::uint64_t value) noexcept {
    write_u32(bytes, offset, static_cast<std::uint32_t>(value >> 32));
    write_u32(bytes, offset + 4, static_cast<std::uint32_t>(value));
}

bool valid_descriptor(const XudpFrameDescriptor& frame) noexcept {
    return frame.stream_id != 0 && frame.frame_id != 0 &&
           frame.frame_size > 0 && frame.frame_size <= kXudpMaxFrameBytes &&
           frame.encoded_width > 0 && frame.encoded_height > 0 &&
           frame.encoded_width <= kMaxDimension &&
           frame.encoded_height <= kMaxDimension &&
           frame.source_width > 0 && frame.source_height > 0 &&
           frame.source_width <= kMaxDimension &&
           frame.source_height <= kMaxDimension &&
           frame.source_roi_width > 0 && frame.source_roi_height > 0 &&
           frame.source_roi_x <= frame.source_width &&
           frame.source_roi_y <= frame.source_height &&
           frame.source_roi_width <= frame.source_width - frame.source_roi_x &&
           frame.source_roi_height <= frame.source_height - frame.source_roi_y &&
           frame.frame_rate_n > 0 &&
           frame.frame_rate_n <= kMaxFrameRateNumerator &&
           frame.frame_rate_d > 0 &&
           frame.sent_timestamp_ns <=
               static_cast<std::uint64_t>(
                   std::numeric_limits<std::int64_t>::max());
}

bool valid_packet_header(const XudpPacketHeader& header,
                         std::size_t payload_size) noexcept {
    return valid_descriptor(header.frame) &&
           header.fragment_count > 0 &&
           header.fragment_count <= kXudpMaxFragments &&
           header.fragment_index < header.fragment_count &&
           header.fragment_payload_size == payload_size && payload_size > 0 &&
           header.fragment_offset <= header.frame.frame_size &&
           payload_size <=
               header.frame.frame_size - header.fragment_offset;
}

void serialize_hash_descriptor(
    const XudpFrameDescriptor& frame,
    std::array<std::uint8_t, kHashDescriptorBytes>& bytes) noexcept {
    std::copy(kMagic.begin(), kMagic.end(), bytes.begin());
    write_u16(bytes, 4, kVersion);
    write_u32(bytes, 6, kCodecJpeg);
    write_u64(bytes, 10, frame.stream_id);
    write_u64(bytes, 18, frame.frame_id);
    write_u32(bytes, 26, frame.frame_size);
    write_u32(bytes, 30, frame.encoded_width);
    write_u32(bytes, 34, frame.encoded_height);
    write_u32(bytes, 38, frame.source_width);
    write_u32(bytes, 42, frame.source_height);
    write_u32(bytes, 46, frame.source_roi_x);
    write_u32(bytes, 50, frame.source_roi_y);
    write_u32(bytes, 54, frame.source_roi_width);
    write_u32(bytes, 58, frame.source_roi_height);
    write_u32(bytes, 62, frame.frame_rate_n);
    write_u32(bytes, 66, frame.frame_rate_d);
    write_u64(bytes, 70, frame.sent_timestamp_ns);
}

class Sha256Hasher {
public:
    Sha256Hasher() noexcept {
        DWORD object_bytes = 0;
        DWORD result_bytes = 0;
        if (!nt_success(BCryptOpenAlgorithmProvider(
                &algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) ||
            !nt_success(BCryptGetProperty(
                algorithm_, BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&object_bytes),
                sizeof(object_bytes), &result_bytes, 0)) ||
            object_bytes == 0) {
            close();
            return;
        }
        try {
            hash_object_.resize(object_bytes);
        } catch (...) {
            close();
            return;
        }
        if (!create_hash()) {
            close();
        }
    }

    ~Sha256Hasher() { close(); }

    Sha256Hasher(const Sha256Hasher&) = delete;
    Sha256Hasher& operator=(const Sha256Hasher&) = delete;

    bool compute(
        const XudpFrameDescriptor& descriptor,
        std::span<const std::uint8_t> payload,
        std::array<std::uint8_t, kXudpSha256Bytes>& output) noexcept {
        if (!hash_ || !valid_descriptor(descriptor) ||
            payload.size() != descriptor.frame_size ||
            payload.size() > std::numeric_limits<ULONG>::max()) {
            return false;
        }
        std::array<std::uint8_t, kHashDescriptorBytes> descriptor_bytes{};
        serialize_hash_descriptor(descriptor, descriptor_bytes);
        const bool succeeded = nt_success(BCryptHashData(
            hash_, descriptor_bytes.data(),
            static_cast<ULONG>(descriptor_bytes.size()), 0)) &&
            nt_success(BCryptHashData(
                hash_, const_cast<PUCHAR>(payload.data()),
                static_cast<ULONG>(payload.size()), 0)) &&
            nt_success(BCryptFinishHash(
                hash_, output.data(), static_cast<ULONG>(output.size()), 0));
        if (!succeeded) {
            // 可复用哈希若在中途失败，状态不再可信；重建句柄避免污染后续帧。
            BCryptDestroyHash(hash_);
            hash_ = nullptr;
            create_hash();
        }
        return succeeded;
    }

private:
    bool create_hash() noexcept {
        return algorithm_ && !hash_object_.empty() &&
               nt_success(BCryptCreateHash(
                   algorithm_, &hash_, hash_object_.data(),
                   static_cast<ULONG>(hash_object_.size()), nullptr, 0,
                   BCRYPT_HASH_REUSABLE_FLAG));
    }

    void close() noexcept {
        if (hash_) {
            BCryptDestroyHash(hash_);
            hash_ = nullptr;
        }
        if (algorithm_) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
            algorithm_ = nullptr;
        }
        hash_object_.clear();
    }

    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    std::vector<std::uint8_t> hash_object_;
};

bool same_frame(const XudpPacketHeader& left,
                const XudpPacketHeader& right) noexcept {
    const auto& a = left.frame;
    const auto& b = right.frame;
    return a.stream_id == b.stream_id && a.frame_id == b.frame_id &&
           a.frame_size == b.frame_size &&
           a.encoded_width == b.encoded_width &&
           a.encoded_height == b.encoded_height &&
           a.source_width == b.source_width &&
           a.source_height == b.source_height &&
           a.source_roi_x == b.source_roi_x &&
           a.source_roi_y == b.source_roi_y &&
           a.source_roi_width == b.source_roi_width &&
           a.source_roi_height == b.source_roi_height &&
           a.frame_rate_n == b.frame_rate_n &&
           a.frame_rate_d == b.frame_rate_d &&
           a.sent_timestamp_ns == b.sent_timestamp_ns &&
           left.fragment_count == right.fragment_count &&
           left.frame_sha256 == right.frame_sha256;
}

void saturating_add(std::uint64_t& target, std::uint64_t value) noexcept {
    const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    target = value > maximum - target ? maximum : target + value;
}

} // namespace

bool compute_xudp_frame_sha256(
    const XudpFrameDescriptor& descriptor,
    std::span<const std::uint8_t> frame_payload,
    std::array<std::uint8_t, kXudpSha256Bytes>& sha256) noexcept {
    Sha256Hasher hasher;
    return hasher.compute(descriptor, frame_payload, sha256);
}

bool serialize_xudp_packet(
    const XudpPacketHeader& header,
    std::span<const std::uint8_t> fragment_payload,
    std::vector<std::uint8_t>& packet) noexcept {
    try {
        if (!valid_packet_header(header, fragment_payload.size()) ||
            fragment_payload.size() > 65507 - kXudpHeaderBytes) {
            return false;
        }
        packet.resize(kXudpHeaderBytes + fragment_payload.size());
        std::span<std::uint8_t> bytes(packet);
        std::copy(kMagic.begin(), kMagic.end(), bytes.begin());
        write_u16(bytes, 4, kVersion);
        write_u16(bytes, 6, static_cast<std::uint16_t>(kXudpHeaderBytes));
        write_u32(bytes, 8, kCodecJpeg);
        write_u64(bytes, 12, header.frame.stream_id);
        write_u64(bytes, 20, header.frame.frame_id);
        write_u16(bytes, 28, header.fragment_index);
        write_u16(bytes, 30, header.fragment_count);
        write_u32(bytes, 32, header.fragment_offset);
        write_u32(bytes, 36, header.fragment_payload_size);
        write_u32(bytes, 40, header.frame.frame_size);
        write_u32(bytes, 44, header.frame.encoded_width);
        write_u32(bytes, 48, header.frame.encoded_height);
        write_u32(bytes, 52, header.frame.source_width);
        write_u32(bytes, 56, header.frame.source_height);
        write_u32(bytes, 60, header.frame.source_roi_x);
        write_u32(bytes, 64, header.frame.source_roi_y);
        write_u32(bytes, 68, header.frame.source_roi_width);
        write_u32(bytes, 72, header.frame.source_roi_height);
        write_u32(bytes, 76, header.frame.frame_rate_n);
        write_u32(bytes, 80, header.frame.frame_rate_d);
        write_u64(bytes, 84, header.frame.sent_timestamp_ns);
        std::copy(header.frame_sha256.begin(), header.frame_sha256.end(),
                  bytes.begin() + 92);
        std::copy(fragment_payload.begin(), fragment_payload.end(),
                  bytes.begin() + kXudpHeaderBytes);
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_xudp_packet(
    std::span<const std::uint8_t> packet,
    XudpPacketHeader& header,
    std::span<const std::uint8_t>& fragment_payload) noexcept {
    try {
        if (packet.size() < kXudpHeaderBytes ||
            !std::equal(kMagic.begin(), kMagic.end(), packet.begin()) ||
            read_u16(packet, 4) != kVersion ||
            read_u16(packet, 6) != kXudpHeaderBytes ||
            read_u32(packet, 8) != kCodecJpeg) {
            return false;
        }
        XudpPacketHeader parsed;
        parsed.frame.stream_id = read_u64(packet, 12);
        parsed.frame.frame_id = read_u64(packet, 20);
        parsed.fragment_index = read_u16(packet, 28);
        parsed.fragment_count = read_u16(packet, 30);
        parsed.fragment_offset = read_u32(packet, 32);
        parsed.fragment_payload_size = read_u32(packet, 36);
        parsed.frame.frame_size = read_u32(packet, 40);
        parsed.frame.encoded_width = read_u32(packet, 44);
        parsed.frame.encoded_height = read_u32(packet, 48);
        parsed.frame.source_width = read_u32(packet, 52);
        parsed.frame.source_height = read_u32(packet, 56);
        parsed.frame.source_roi_x = read_u32(packet, 60);
        parsed.frame.source_roi_y = read_u32(packet, 64);
        parsed.frame.source_roi_width = read_u32(packet, 68);
        parsed.frame.source_roi_height = read_u32(packet, 72);
        parsed.frame.frame_rate_n = read_u32(packet, 76);
        parsed.frame.frame_rate_d = read_u32(packet, 80);
        parsed.frame.sent_timestamp_ns = read_u64(packet, 84);
        std::copy_n(packet.begin() + 92, kXudpSha256Bytes,
                    parsed.frame_sha256.begin());
        const std::size_t payload_size = packet.size() - kXudpHeaderBytes;
        if (!valid_packet_header(parsed, payload_size)) return false;
        header = parsed;
        fragment_payload = packet.subspan(kXudpHeaderBytes);
        return true;
    } catch (...) {
        return false;
    }
}

struct XudpFrameAssembler::Impl {
    struct FragmentRange {
        std::uint32_t offset = 0;
        std::uint32_t size = 0;
        bool received = false;
    };

    struct Assembly {
        bool active = false;
        XudpPacketHeader header;
        std::vector<std::uint8_t> bytes;
        std::vector<FragmentRange> fragments;
        std::size_t received_fragments = 0;
        std::chrono::steady_clock::time_point started_at{};
    };

    void reset_stream(std::uint64_t stream_id) noexcept {
        for (auto& assembly : assemblies) assembly.active = false;
        current_stream_id = stream_id;
        last_published_frame_id = 0;
        rejected_through_frame_id = 0;
    }

    bool is_retired_stream(std::uint64_t stream_id) const noexcept {
        return std::find(retired_stream_ids.begin(), retired_stream_ids.end(),
                         stream_id) != retired_stream_ids.end();
    }

    void retire_current_stream() noexcept {
        if (current_stream_id == 0) return;
        retired_stream_ids[next_retired_stream] = current_stream_id;
        next_retired_stream =
            (next_retired_stream + 1) % retired_stream_ids.size();
    }

    Assembly* find(std::uint64_t frame_id) noexcept {
        for (auto& assembly : assemblies) {
            if (assembly.active && assembly.header.frame.frame_id == frame_id) {
                return &assembly;
            }
        }
        return nullptr;
    }

    Assembly* acquire(const XudpPacketHeader& header,
                      std::chrono::steady_clock::time_point received_at) {
        Assembly* selected = nullptr;
        for (auto& assembly : assemblies) {
            if (!assembly.active) {
                selected = &assembly;
                break;
            }
        }
        if (!selected) {
            auto oldest = std::min_element(
                assemblies.begin(), assemblies.end(),
                [](const Assembly& left, const Assembly& right) {
                    return left.header.frame.frame_id <
                           right.header.frame.frame_id;
                });
            // 三槽都被更新帧占用时，迟到旧帧不得挤掉较新的在途帧。
            if (header.frame.frame_id < oldest->header.frame.frame_id) {
                return nullptr;
            }
            selected = &*oldest;
        }
        selected->active = true;
        selected->header = header;
        selected->bytes.resize(header.frame.frame_size);
        selected->fragments.clear();
        selected->fragments.resize(header.fragment_count);
        selected->received_fragments = 0;
        selected->started_at = received_at;
        return selected;
    }

    bool ranges_cover_frame(Assembly& assembly) noexcept {
        std::sort(
            assembly.fragments.begin(), assembly.fragments.end(),
            [](const FragmentRange& left, const FragmentRange& right) {
                return left.offset < right.offset;
            });
        std::uint32_t expected_offset = 0;
        for (const auto& fragment : assembly.fragments) {
            if (!fragment.received || fragment.offset != expected_offset ||
                fragment.size > assembly.header.frame.frame_size -
                                    expected_offset) {
                return false;
            }
            expected_offset += fragment.size;
        }
        return expected_offset == assembly.header.frame.frame_size;
    }

    XudpConsumeResult consume(
        std::span<const std::uint8_t> packet,
        std::chrono::steady_clock::time_point received_at,
        XudpCompletedFrame& completed) noexcept {
        XudpPacketHeader header;
        std::span<const std::uint8_t> payload;
        if (!parse_xudp_packet(packet, header, payload)) {
            saturating_add(invalid_packets, 1);
            return XudpConsumeResult::INVALID_PACKET;
        }
        if (current_stream_id != header.frame.stream_id) {
            // 发送端重启换 stream_id 后，旧会话迟到包不得把接收器切回旧流。
            if (is_retired_stream(header.frame.stream_id)) {
                return XudpConsumeResult::IGNORED;
            }
            retire_current_stream();
            reset_stream(header.frame.stream_id);
        }
        if (header.frame.frame_id <= last_published_frame_id) {
            return XudpConsumeResult::IGNORED;
        }
        if (header.frame.frame_id <= rejected_through_frame_id) {
            return XudpConsumeResult::IGNORED;
        }

        Assembly* assembly = find(header.frame.frame_id);
        if (!assembly) {
            try {
                assembly = acquire(header, received_at);
            } catch (...) {
                saturating_add(invalid_packets, 1);
                return XudpConsumeResult::INVALID_PACKET;
            }
            if (!assembly) return XudpConsumeResult::IGNORED;
        } else if (!same_frame(assembly->header, header)) {
            assembly->active = false;
            rejected_through_frame_id = std::max(
                rejected_through_frame_id, header.frame.frame_id);
            saturating_add(invalid_packets, 1);
            return XudpConsumeResult::INVALID_PACKET;
        }

        auto& fragment = assembly->fragments[header.fragment_index];
        if (fragment.received) {
            const bool same_range =
                fragment.offset == header.fragment_offset &&
                fragment.size == header.fragment_payload_size;
            const bool same_payload = same_range && std::equal(
                payload.begin(), payload.end(),
                assembly->bytes.begin() + fragment.offset);
            if (!same_payload) {
                assembly->active = false;
                rejected_through_frame_id = std::max(
                    rejected_through_frame_id, header.frame.frame_id);
                saturating_add(invalid_packets, 1);
                return XudpConsumeResult::INVALID_PACKET;
            }
            return XudpConsumeResult::IGNORED;
        }

        std::copy(payload.begin(), payload.end(),
                  assembly->bytes.begin() + header.fragment_offset);
        fragment.offset = header.fragment_offset;
        fragment.size = header.fragment_payload_size;
        fragment.received = true;
        ++assembly->received_fragments;
        if (assembly->received_fragments != header.fragment_count) {
            return XudpConsumeResult::INCOMPLETE;
        }
        if (!ranges_cover_frame(*assembly)) {
            assembly->active = false;
            rejected_through_frame_id = std::max(
                rejected_through_frame_id, header.frame.frame_id);
            saturating_add(invalid_packets, 1);
            return XudpConsumeResult::INVALID_PACKET;
        }

        std::array<std::uint8_t, kXudpSha256Bytes> actual_sha256{};
        if (!hasher.compute(assembly->header.frame, assembly->bytes,
                            actual_sha256) ||
            actual_sha256 != assembly->header.frame_sha256) {
            assembly->active = false;
            rejected_through_frame_id = std::max(
                rejected_through_frame_id, header.frame.frame_id);
            saturating_add(invalid_packets, 1);
            return XudpConsumeResult::INVALID_PACKET;
        }

        const std::uint64_t frame_id = assembly->header.frame.frame_id;
        if (last_published_frame_id != 0 &&
            frame_id > last_published_frame_id + 1) {
            saturating_add(
                dropped_frames, frame_id - last_published_frame_id - 1);
        }
        last_published_frame_id = frame_id;
        saturating_add(received_frames, 1);
        completed.descriptor = assembly->header.frame;
        completed.jpeg = std::span<const std::uint8_t>(assembly->bytes);
        completed.started_at = assembly->started_at;
        completed.source_received_frames = received_frames;
        completed.transport_dropped_frames = dropped_frames;
        completed.transport_invalid_packets = invalid_packets;

        for (auto& candidate : assemblies) {
            if (candidate.active &&
                candidate.header.frame.frame_id <= frame_id) {
                candidate.active = false;
            }
        }
        return XudpConsumeResult::FRAME;
    }

    std::array<Assembly, kAssemblyCount> assemblies;
    Sha256Hasher hasher;
    std::uint64_t current_stream_id = 0;
    std::uint64_t last_published_frame_id = 0;
    std::uint64_t rejected_through_frame_id = 0;
    std::array<std::uint64_t, kRetiredStreamCount> retired_stream_ids{};
    std::size_t next_retired_stream = 0;
    std::uint64_t received_frames = 0;
    std::uint64_t dropped_frames = 0;
    std::uint64_t invalid_packets = 0;
};

XudpFrameAssembler::XudpFrameAssembler()
    : impl_(std::make_unique<Impl>()) {}

XudpFrameAssembler::~XudpFrameAssembler() = default;

XudpConsumeResult XudpFrameAssembler::consume_packet(
    std::span<const std::uint8_t> packet,
    std::chrono::steady_clock::time_point received_at,
    XudpCompletedFrame& completed) noexcept {
    if (!impl_) return XudpConsumeResult::INVALID_PACKET;
    return impl_->consume(packet, received_at, completed);
}

void XudpFrameAssembler::reset() noexcept {
    if (!impl_) return;
    try {
        impl_->reset_stream(0);
        impl_->received_frames = 0;
        impl_->dropped_frames = 0;
        impl_->invalid_packets = 0;
        impl_->retired_stream_ids.fill(0);
        impl_->next_retired_stream = 0;
        for (auto& assembly : impl_->assemblies) {
            assembly.bytes.clear();
            assembly.fragments.clear();
        }
    } catch (...) {
    }
}

std::uint64_t XudpFrameAssembler::source_received_frames() const noexcept {
    return impl_ ? impl_->received_frames : 0;
}

std::uint64_t XudpFrameAssembler::transport_dropped_frames() const noexcept {
    return impl_ ? impl_->dropped_frames : 0;
}

std::uint64_t XudpFrameAssembler::transport_invalid_packets() const noexcept {
    return impl_ ? impl_->invalid_packets : 0;
}

void XudpFrameAssembler::record_invalid_frame() noexcept {
    if (impl_) saturating_add(impl_->invalid_packets, 1);
}

bool resolve_xudp_frame_geometry(
    const CaptureConfig& config,
    const XudpFrameDescriptor& descriptor,
    NetworkFrameGeometry& geometry) noexcept {
    try {
        if (!valid_descriptor(descriptor) || config.roi_width <= 0 ||
            config.roi_height <= 0) {
            return false;
        }
        NetworkFrameGeometry resolved;
        resolved.encoded_width = static_cast<int>(descriptor.encoded_width);
        resolved.encoded_height = static_cast<int>(descriptor.encoded_height);
        if (config.center_roi) {
            resolved.decoded_roi_width = std::min(
                config.roi_width, resolved.encoded_width);
            resolved.decoded_roi_height = std::min(
                config.roi_height, resolved.encoded_height);
            resolved.decoded_roi_x =
                (resolved.encoded_width - resolved.decoded_roi_width) / 2;
            resolved.decoded_roi_y =
                (resolved.encoded_height - resolved.decoded_roi_height) / 2;
        } else {
            resolved.decoded_roi_x = config.roi_x;
            resolved.decoded_roi_y = config.roi_y;
            resolved.decoded_roi_width = config.roi_width;
            resolved.decoded_roi_height = config.roi_height;
        }
        if (resolved.decoded_roi_x < 0 || resolved.decoded_roi_y < 0 ||
            resolved.decoded_roi_x + resolved.decoded_roi_width >
                resolved.encoded_width ||
            resolved.decoded_roi_y + resolved.decoded_roi_height >
                resolved.encoded_height) {
            return false;
        }
        resolved.source_width = static_cast<int>(descriptor.source_width);
        resolved.source_height = static_cast<int>(descriptor.source_height);
        resolved.source_pixels_per_pixel_x =
            static_cast<double>(descriptor.source_roi_width) /
            descriptor.encoded_width;
        resolved.source_pixels_per_pixel_y =
            static_cast<double>(descriptor.source_roi_height) /
            descriptor.encoded_height;
        resolved.source_roi_x = descriptor.source_roi_x +
            resolved.decoded_roi_x * resolved.source_pixels_per_pixel_x;
        resolved.source_roi_y = descriptor.source_roi_y +
            resolved.decoded_roi_y * resolved.source_pixels_per_pixel_y;
        geometry = resolved;
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace capture::detail
