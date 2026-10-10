#ifndef EXTERNAL_MOTION_H
#define EXTERNAL_MOTION_H
#include <chrono>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>
#include <cmath>

struct ExternalMotionEvent {
    std::uint64_t id = 0;
    std::chrono::steady_clock::time_point completed_at{};
    int dx_counts = 0, dy_counts = 0;
    // 控制规划时刻C；完成B仅是回执事实。Aim用C+D预计作用，不能用B再加D。
    std::chrono::steady_clock::time_point issued_at{};
};
struct ExternalMotionWindow {
    bool enabled = false, complete = false;
    std::uint64_t revision = 0;
    std::chrono::steady_clock::time_point covered_from{}, covered_until{};
    std::vector<ExternalMotionEvent> events;
};
// 区间为(from,to]；没有外部模块保留Aim-only零增量，启用但缺失不能当零。
namespace aim::detail {
template<bool ByIssue>
inline std::optional<std::pair<double, double>> external_motion_interval_sum(
    const ExternalMotionWindow& window, std::chrono::steady_clock::time_point from,
    std::chrono::steady_clock::time_point to) noexcept {
    if (!window.enabled) return std::pair{0.0, 0.0};
    if (!window.complete || from > to || window.covered_from > window.covered_until ||
        from < window.covered_from || to > window.covered_until || window.events.size() > 4096 ||
        (window.events.empty() && window.revision != 0)) return {};
    double x = 0, y = 0;
    std::uint64_t previous_id = 0;
    auto previous_at = window.covered_from;
    for (const auto& entry : window.events) {
        if (entry.id <= previous_id || (previous_id != 0 && entry.id - previous_id != 1) ||
            entry.id > window.revision || entry.completed_at < previous_at ||
            entry.completed_at > window.covered_until) return {};
        previous_id = entry.id; previous_at = entry.completed_at;
        if constexpr (ByIssue) {
            if (entry.issued_at == std::chrono::steady_clock::time_point{} ||
                entry.issued_at > entry.completed_at) return {};
        }
        const auto stamp = ByIssue ? entry.issued_at : entry.completed_at;
        if (stamp > from && stamp <= to) { x += entry.dx_counts; y += entry.dy_counts; }
    }
    if (previous_id != window.revision) return {};
    if (!std::isfinite(x) || !std::isfinite(y)) return {};
    return std::pair{x, y};
}
}
inline std::optional<std::pair<double, double>> external_motion_sum(
    const ExternalMotionWindow& window, std::chrono::steady_clock::time_point from,
    std::chrono::steady_clock::time_point to) noexcept {
    return aim::detail::external_motion_interval_sum<false>(window, from, to);
}
inline bool external_motion_proven_zero_x(const ExternalMotionWindow& window,
    std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to) noexcept {
    if (!external_motion_sum(window, from, to)) return false;
    if (!window.enabled) return true;
    for (const auto& entry : window.events)
        if (entry.completed_at > from && entry.completed_at <= to && entry.dx_counts != 0) return false;
    return true;
}

// 模型区间仍传入(from-D,to-D]或(from-D,current_control]，但按控制起点筛选。
// 完成事件覆盖是保守水位：C<=B，因此B<=from的已逐出事件不可能仍有C>from。
inline std::optional<std::pair<double, double>> external_motion_issued_sum(
    const ExternalMotionWindow& window, std::chrono::steady_clock::time_point from,
    std::chrono::steady_clock::time_point to) noexcept {
    // 与完成查询共用一次验证/累加遍历，不在每个模型查询中重复扫描账本。
    return aim::detail::external_motion_interval_sum<true>(window, from, to);
}
inline bool external_motion_issued_proven_zero_x(const ExternalMotionWindow& window,
    std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to) noexcept {
    if (!external_motion_issued_sum(window, from, to)) return false;
    if (!window.enabled) return true;
    for (const auto& entry : window.events)
        if (entry.issued_at > from && entry.issued_at <= to && entry.dx_counts != 0) return false;
    return true;
}
#endif
