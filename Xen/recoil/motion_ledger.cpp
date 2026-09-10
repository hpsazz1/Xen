#include "recoil/motion_ledger.h"
#include <cmath>
#include <limits>

void MotionLedger::reset(double limit, int window_ms, std::chrono::steady_clock::time_point now) {
    std::lock_guard lock(mutex_);
    budget_.clear(); external_.clear(); revision_ = 0; covered_from_ = last_completed_ = now;
    limit_ = limit; window_ms_ = window_ms;
    fault_ = !std::isfinite(limit) || limit <= 0 || window_ms <= 0;
}
bool MotionLedger::permits(const MouseMoveCommand& command, std::chrono::steady_clock::time_point now) {
    std::lock_guard lock(mutex_);
    if (fault_ || now < covered_from_) return false;
    const auto cutoff = now - std::chrono::milliseconds(window_ms_);
    while (!budget_.empty() && budget_.front().at <= cutoff) budget_.pop_front();
    double used = std::hypot(static_cast<double>(command.dx_counts), static_cast<double>(command.dy_counts));
    for (const auto& item : budget_) used += item.magnitude;
    return used <= limit_;
}
bool MotionLedger::record(const MouseMoveCommand& command, const MouseMoveReceipt& receipt, bool external) {
    std::lock_guard lock(mutex_);
    if (fault_ || !receipt.succeeded || receipt.backend_completed_at == std::chrono::steady_clock::time_point{} ||
        receipt.backend_completed_at < last_completed_) {
        fault_ = true;
        if (revision_ != std::numeric_limits<std::uint64_t>::max()) ++revision_;
        return false;
    }
    last_completed_ = receipt.backend_completed_at;
    budget_.push_back({receipt.backend_completed_at,
        std::hypot(static_cast<double>(command.dx_counts), static_cast<double>(command.dy_counts))});
    if (!external) return true;
    if (revision_ == std::numeric_limits<std::uint64_t>::max()) { fault_ = true; return false; }
    external_.push_back({++revision_, receipt.backend_completed_at, command.dx_counts, command.dy_counts});
    while (external_.size() > 4096) {
        covered_from_ = external_.front().completed_at;
        external_.pop_front();
    }
    return true;
}
ExternalMotionWindow MotionLedger::snapshot(std::chrono::steady_clock::time_point now) const {
    std::lock_guard lock(mutex_);
    return {true, !fault_, revision_, covered_from_, now, {external_.begin(), external_.end()}};
}
std::uint64_t MotionLedger::revision() const { std::lock_guard lock(mutex_); return revision_; }
