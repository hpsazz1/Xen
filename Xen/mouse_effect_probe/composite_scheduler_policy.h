#ifndef XEN_MOUSE_EFFECT_PROBE_COMPOSITE_SCHEDULER_POLICY_H
#define XEN_MOUSE_EFFECT_PROBE_COMPOSITE_SCHEDULER_POLICY_H

#include <cstdint>
#include <string_view>

namespace mouse_effect_probe {

enum class CompositePhaseSchedulerPolicy { LEGACY, ACTIVE_1MS_V1 };

namespace detail {

// timer_mode 同时绑定固定资源策略；只允许整组枚举，不暴露可调预算。
struct CompositeSchedulerParameters {
    std::string_view timer_mode;
    std::uint64_t active_guard_ns;
    std::uint64_t max_wake_lateness_ns;
    std::uint64_t max_event_interval_width_ns;
    std::uint64_t max_active_wait_ns_per_event;
    std::uint64_t max_active_wait_ns_total;
};

inline constexpr bool composite_scheduler_parameters(
        CompositePhaseSchedulerPolicy policy, CompositeSchedulerParameters& parameters) noexcept {
    switch (policy) {
    case CompositePhaseSchedulerPolicy::LEGACY:
        parameters = {"HIGH_RESOLUTION_ONE_SHOT_OR_FAIL", 300000, 150000, 100000, 350000, 14700000};
        return true;
    case CompositePhaseSchedulerPolicy::ACTIVE_1MS_V1:
        parameters = {"HIGH_RESOLUTION_ONE_SHOT_ACTIVE_1MS_V1", 1000000, 150000, 100000, 1000000, 42000000};
        return true;
    }
    return false;
}

inline constexpr bool parse_composite_scheduler_timer_mode(
        std::string_view timer_mode, CompositePhaseSchedulerPolicy& policy) noexcept {
    if (timer_mode == "HIGH_RESOLUTION_ONE_SHOT_OR_FAIL") {
        policy = CompositePhaseSchedulerPolicy::LEGACY;
        return true;
    }
    if (timer_mode == "HIGH_RESOLUTION_ONE_SHOT_ACTIVE_1MS_V1") {
        policy = CompositePhaseSchedulerPolicy::ACTIVE_1MS_V1;
        return true;
    }
    return false;
}

} // namespace detail
} // namespace mouse_effect_probe

#endif
