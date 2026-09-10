#ifndef WEAPON_INTERNAL_H
#define WEAPON_INTERNAL_H
#include "weapon/weapon.h"
#include <string_view>
#include <vector>

namespace weapon::detail {
inline constexpr std::size_t kMaximumHeaders = 8192;
bool http_body_length(std::string_view header, std::size_t limit, std::size_t& length) noexcept;
WeaponSnapshot parse_payload(std::string_view body, const GsiConfig& config,
                             std::int64_t local_utc_ms) noexcept;
class GsiState final {
public:
    void reset() noexcept;
    Status ingest(std::string_view body, const GsiConfig& config,
                  Clock::time_point now, std::int64_t local_utc_ms) noexcept;
    WeaponSnapshot snapshot(Clock::time_point now) const;
private:
    WeaponSnapshot current_;
    std::uint64_t epoch_ = 0, revision_ = 0, timestamp_ = 0;
    Clock::time_point last_now_{}, timestamp_deadline_{};
    std::vector<std::string> seen_states_;
};
} // namespace weapon::detail
#endif
