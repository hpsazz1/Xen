#ifndef RUNTIME_WEAPON_CONTEXT_INTERNAL_H
#define RUNTIME_WEAPON_CONTEXT_INTERNAL_H

#include "trigger/trigger.h"
#include "auto_stop/auto_stop.h"
#include "weapon/weapon_catalog.h"
#include "weapon/weapon.h"
#include "weapon/weapon_timing.h"
#include <limits>
#include <string>

namespace runtime::detail {
// 可恢复只限有明确身份、健康、有效期的正常武器状态，不含死亡或未知。
inline bool weapon_session_trusted(const weapon::WeaponSnapshot& current, weapon::Clock::time_point now) noexcept {
    return current.identity_match && current.player_playing && current.player_health && *current.player_health > 0 &&
        current.source_epoch != 0 && current.control_safety_epoch != 0 && current.received_at <= now &&
        current.valid_until > now && !current.canonical_id.empty() &&
        ((current.status == weapon::Status::READY && current.valid) ||
         current.status == weapon::Status::RELOADING || current.status == weapon::Status::EMPTY);
}
inline bool weapon_ready(const weapon::WeaponSnapshot& current, weapon::Clock::time_point now) noexcept {
    return weapon_session_trusted(current, now) && current.valid && current.status == weapon::Status::READY &&
        current.state == weapon::WeaponState::ACTIVE && current.ammo_clip && *current.ammo_clip > 0;
}

// 急停与扳机共用启动时冻结的实际资料；循环无可用点射资料时不得先接管键盘。
inline AutoStopWeaponContext auto_stop_weapon_context(const weapon::WeaponSnapshot& current,
        bool cycle, const weapon::TimingCatalog* catalog, weapon::Clock::time_point now) noexcept {
    const auto id = weapon::normalize_weapon_id(current.canonical_id);
    const auto* timing = catalog ? weapon::find_timing(*catalog, id) : nullptr;
    const bool valid = weapon_ready(current, now) && (!cycle || (timing && timing->enabled));
    return {true, valid, current.source_epoch, id, current.control_safety_epoch, weapon_session_trusted(current, now)};
}

// Runtime与生产GSI组合回归共用适配器，避免测试手写许可。
class TriggerWeaponContext final {
public:
    TriggerContext update(const weapon::WeaponSnapshot& weapon, const weapon::TimingCatalog& catalog,
              TriggerTime now) {
        if (exhausted) return TriggerContext{generation, true, false};
        const std::string_view id = weapon.canonical_id;
        const bool valid = weapon_ready(weapon, now);
        // revision/timestamp 的正常心跳不改变会话；身份、连续性或有效性变化持续增代。
        if (generation == 0 || id != previous_weapon ||
            weapon.source_epoch != previous_epoch || valid != previous_valid ||
            weapon.control_safety_epoch != previous_trust) {
            if (generation == std::numeric_limits<std::uint64_t>::max()) {
                exhausted = true;
                return TriggerContext{generation, true, false};
            }
            previous_weapon = id;
            selected = weapon::find_timing(catalog, id);
            previous_epoch = weapon.source_epoch;
            previous_valid = valid;
            previous_trust = weapon.control_safety_epoch;
            ++generation;
        }
        TriggerContext result{generation, true, valid};
        result.trust_generation = weapon.control_safety_epoch;
        result.session_trusted = weapon_session_trusted(weapon, now);
        result.timing_required = true;
        result.timing_catalog_revision = catalog.revision;
        result.timing_weapon_id = selected ? selected->canonical_id : std::string_view{};
        result.timing_valid = selected && selected->enabled;
        if (result.timing_valid) {
            result.shot_hold_ms = selected->shot_hold_ms;
            result.fire_interval_ms = selected->fire_interval_ms;
        }
        return result;
    }
private:
    const weapon::TimingProfile* selected = nullptr;
    std::string previous_weapon;
    std::uint64_t previous_epoch = 0, previous_trust = 0, generation = 0;
    bool previous_valid = false, exhausted = false;
};
} // namespace runtime::detail
#endif
