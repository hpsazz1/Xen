#ifndef MOTION_LEDGER_H
#define MOTION_LEDGER_H
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>
#include "mouse/mouse.h"
#include "aim/external_motion.h"
// 调用方持有唯一输出arbiter；账本自己的锁只保护短时间快照，不覆盖设备等待。
class MotionLedger {
public:
    void reset(double limit, int window_ms, std::chrono::steady_clock::time_point now);
    bool permits(const MouseMoveCommand& command, std::chrono::steady_clock::time_point now);
    bool record(const MouseMoveCommand& command, const MouseMoveReceipt& receipt, bool external);
    ExternalMotionWindow snapshot(std::chrono::steady_clock::time_point now) const;
    std::uint64_t revision() const;
private:
    mutable std::mutex mutex_;
    struct BudgetEntry { std::chrono::steady_clock::time_point at; double magnitude; };
    std::deque<BudgetEntry> budget_;
    std::deque<ExternalMotionEvent> external_;
    double limit_ = 0;
    int window_ms_ = 16;
    bool fault_ = false;
    std::uint64_t revision_ = 0;
    std::chrono::steady_clock::time_point covered_from_{};
    std::chrono::steady_clock::time_point last_completed_{};
};
#endif
