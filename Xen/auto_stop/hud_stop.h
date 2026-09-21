#ifndef XEN_HUD_STOP_H
#define XEN_HUD_STOP_H

#include "auto_stop/auto_stop.h"

struct HudStopTelemetry {
    // 纵向、横向；均为软件模型归一量，不是游戏速度。
    std::array<double, 2> velocity{};
    std::array<double, 2> planned_ms{};
    bool seeded = false;
};

// HUD 运动积分 + Xen ACK/清理适配。与现有控制器互斥选择，永不提供实际停稳证据。
class HudStopController {
public:
    explicit HudStopController(const AutoStopConfig& config = {}) noexcept;
    AutoStopDecision observe(const WasdMotionIntent&, std::int64_t now_ns) noexcept;
    AutoStopDecision request(std::uint64_t request_id, std::int64_t now_ns) noexcept;
    AutoStopDecision request_manual_release(std::uint64_t request_id,
        std::uint8_t released_mask, std::int64_t now_ns) noexcept;
    AutoStopDecision cancel(std::uint64_t request_id, std::int64_t now_ns) noexcept;
    AutoStopDecision tick(std::int64_t now_ns) noexcept;
    bool resume_after_masked_hold(const WasdMotionIntent&, std::int64_t released_at_ns) noexcept;
    bool restart_after_cleanup(const WasdMotionIntent&, std::int64_t cleanup_ns) noexcept;
    AutoStopDecision acknowledge(std::uint64_t request_id, std::uint64_t command_id,
        std::uint8_t applied_mask, std::int64_t ack_ns) noexcept;
    AutoStopDecision decision() const noexcept { return decision_; }
    HudStopTelemetry telemetry() const noexcept { return telemetry_; }
private:
    bool active() const noexcept;
    bool advance(std::int64_t now_ns) noexcept;
    void invalidate() noexcept;
    void issue(std::uint8_t mask) noexcept;
    bool valid_intent(const WasdMotionIntent&, std::int64_t now_ns) const noexcept;
    bool ordered(const WasdMotionIntent&) const noexcept;
    void remember(const WasdMotionIntent&) noexcept;
    std::uint8_t reverse_mask() const noexcept;
    AutoStopDecision decision_;
    HudStopTelemetry telemetry_;
    WasdMotionIntent input_;
    std::array<std::int64_t, 2> deadlines_{};
    std::int64_t time_ns_ = 0, after_release_ns_ = 18000000;
    std::uint64_t request_watermark_ = 0, seen_epoch_ = 0, seen_sequence_ = 0;
    std::int64_t seen_time_ns_ = 0;
    std::uint8_t applied_mask_ = 0;
    bool synchronized_ = false, output_started_ = false, initial_zero_ = false;
    bool timing_valid_ = true;
};
#endif
