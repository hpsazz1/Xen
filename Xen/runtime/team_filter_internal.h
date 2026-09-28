#ifndef RUNTIME_TEAM_FILTER_INTERNAL_H
#define RUNTIME_TEAM_FILTER_INTERNAL_H

#include "runtime/weapon_context_internal.h"
#include "config/config.h"
#include <algorithm>
#include <span>

namespace runtime::detail {
inline bool team_context_valid(const weapon::WeaponSnapshot& current, weapon::Clock::time_point now) noexcept {
    return current.team_epoch != 0 &&
        (current.game_phase == weapon::GamePhase::PREPARATION ||
         (current.game_phase == weapon::GamePhase::ACTIVE &&
          (current.team == weapon::Team::CT || current.team == weapon::Team::T))) &&
        weapon_session_trusted(current, now);
}

// 只构造控制候选；调用方保留原始检测用于采集和预览，不将友军误当背景。
inline std::vector<Detection> enemy_detections(std::span<const Detection> original,
        const TeamFilterConfig& policy, weapon::Team team, weapon::GamePhase phase, bool valid) {
    if (!policy.enabled) return {original.begin(), original.end()};
    std::vector<Detection> result;
    if (!valid) return result;
    if (phase == weapon::GamePhase::PREPARATION) return {original.begin(), original.end()};
    if (phase != weapon::GamePhase::ACTIVE || (team != weapon::Team::CT && team != weapon::Team::T)) return result;
    const auto& allowed = team == weapon::Team::CT ? policy.t_class_ids : policy.ct_class_ids;
    result.reserve(original.size());
    for (const auto& box : original) {
        if (std::find(allowed.begin(), allowed.end(), box.class_id) != allowed.end()) result.push_back(box);
    }
    return result;
}
} // namespace runtime::detail
#endif
