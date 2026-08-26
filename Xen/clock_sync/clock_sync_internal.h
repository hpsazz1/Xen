#ifndef CLOCK_SYNC_INTERNAL_H
#define CLOCK_SYNC_INTERNAL_H

#include "clock_sync/clock_sync.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

namespace clock_sync::detail {

inline constexpr std::size_t kRequestBytes = 40;
inline constexpr std::size_t kResponseBytes = 64;

struct Request {
    std::uint64_t request_id = 0;
    std::uint64_t requester_send_steady_ns = 0;
};

struct Response {
    std::uint64_t request_id = 0;
    std::uint64_t requester_send_steady_ns = 0;
    std::uint64_t source_session_id = 0;
    std::uint64_t source_receive_utc_ns = 0;
    std::uint64_t source_send_utc_ns = 0;
};

struct Sample {
    std::uint64_t source_session_id = 0;
    std::uint64_t requester_send_steady_ns = 0;
    std::uint64_t source_receive_utc_ns = 0;
    std::uint64_t source_send_utc_ns = 0;
    std::uint64_t requester_receive_steady_ns = 0;
};

bool serialize_request(
    const Request& request,
    std::array<std::uint8_t, kRequestBytes>& packet) noexcept;
bool parse_request(
    std::span<const std::uint8_t> packet,
    Request& request) noexcept;
bool serialize_response(
    const Response& response,
    std::array<std::uint8_t, kResponseBytes>& packet) noexcept;
bool parse_response(
    std::span<const std::uint8_t> packet,
    Response& response) noexcept;

class AffineMapper final {
public:
    bool add_sample(const Sample& sample) noexcept;
    MappingResult map_utc_ns(
        std::uint64_t source_utc_ns,
        std::chrono::steady_clock::time_point local_now,
        std::chrono::steady_clock::duration maximum_mapping_age) const noexcept;
    void reset(MappingStatus status = MappingStatus::UNSYNCHRONIZED) noexcept;

private:
    struct SamplePoint {
        double source_midpoint_ns = 0.0;
        double local_midpoint_ns = 0.0;
        double round_trip_ns = 0.0;
        std::uint64_t local_received_ns = 0;
    };

    static constexpr std::size_t kSampleCapacity = 8;
    static constexpr std::size_t kMinimumValidSamples = 3;
    std::array<SamplePoint, kSampleCapacity> samples_{};
    std::size_t sample_count_ = 0;
    std::size_t next_sample_ = 0;
    std::uint64_t source_session_id_ = 0;
    MappingStatus reset_status_ = MappingStatus::UNSYNCHRONIZED;
};

} // namespace clock_sync::detail

#endif // CLOCK_SYNC_INTERNAL_H
