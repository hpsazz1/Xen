#ifndef AUTO_STOP_H
#define AUTO_STOP_H

#include <cstdint>
#include <array>

// 移动制动配置独立于 Aim；允许键只授予许可，不代表武装或开火请求。
struct AutoStopConfig {
    bool enabled = false;
    int activation_virtual_key = 0;
};

enum class AutoStopStatus {
    DISABLED,
    UNSUPPORTED_BACKEND,
    UNBOUND,
    AWAITING_VALIDATION,
    PAUSED,
    READY, WAITING_INPUT, BRAKING, ESTIMATED, CANCELED, FAULT,
};

const char* AutoStopStatusName(AutoStopStatus status) noexcept;

struct AutoStopSnapshot {
    AutoStopStatus status = AutoStopStatus::DISABLED;
    // 软件协议能力不能证明目标固件行为或角色已经停稳。
    bool device_protocol_available = false;
    bool stop_evidence_available = false;
    bool fire_permitted = false;
    std::uint64_t request_id = 0, requests = 0, completed = 0, canceled = 0, aim_skips = 0;
    std::int64_t max_release_overshoot_ns = 0, max_ack_wait_ns = 0, max_arbiter_wait_ns = 0;
    bool cleanup_unknown = false;
    bool telemetry_available = false;
    std::uint64_t acknowledged_commands = 0, cleanup_attempts = 0, cleanup_failures = 0, release_commands = 0;
    std::uint64_t arbiter_wait_samples = 0;
};

// 首批只发布能力与未满足条件，不产生物理操作。后续以有证据的
// 制动计划替换 AWAITING_VALIDATION，不能用固定等待时间伪造停稳。
AutoStopSnapshot assess_auto_stop_availability(
    const AutoStopConfig& config, bool kmbox_backend,
    bool protocol_available, bool paused) noexcept;

struct WasdMotionIntent {
    bool history_valid = false;
    bool conflicting = false;
    std::uint8_t held_mask = 0;
    int horizontal = 0;
    int longitudinal = 0;
    // W/A/S/D 位与 KMBOX 输入约定一致。时间是本机接收单调时间，
    // 不是游戏执行时间或实际速度；零值表示该键不再按住。
    std::array<std::int64_t, 4> held_since_ns{};
    std::uint64_t epoch = 0;
    std::uint64_t sequence = 0;
    std::int64_t received_at_ns = 0;
};

enum class AutoStopPhase { IDLE, WAITING_ACK, BRAKING, COMPLETE_ESTIMATED, CANCELLED, INVALID };
struct AutoStopDecision {
    AutoStopPhase phase = AutoStopPhase::IDLE;
    std::uint64_t request_id = 0;
    std::uint64_t command_id = 0;
    std::uint8_t desired_mask = 0;
    // 顺序为纵向、横向；未确认报告前为零，不得提前启动制动计时。
    std::array<std::int64_t, 2> axis_deadline_ns{};
    bool estimated = true;
    bool fire_permitted = false;
};

// 纯状态机，不接触设备。候选230/180/110ms是本轮现象拟合，未完成校准。
class AutoStopController {
public:
    AutoStopDecision observe(const WasdMotionIntent& intent, std::int64_t now_ns) noexcept;
    AutoStopDecision request(std::uint64_t request_id, std::int64_t now_ns) noexcept;
    AutoStopDecision cancel(std::uint64_t request_id, std::int64_t now_ns) noexcept;
    AutoStopDecision tick(std::int64_t now_ns) noexcept;
    AutoStopDecision acknowledge(std::uint64_t request_id, std::uint64_t command_id,
        std::uint8_t applied_mask, std::int64_t ack_ns) noexcept;
    AutoStopDecision decision() const noexcept { return decision_; }
private:
    bool advance(std::int64_t now_ns) noexcept;
    void invalidate() noexcept;
    void issue(std::uint8_t mask) noexcept;
    void deadlines(std::int64_t now_ns) noexcept;
    bool active() const noexcept;
    AutoStopDecision decision_;
    WasdMotionIntent input_;
    std::array<double, 2> state_{};
    std::int64_t time_ns_ = 0;
    std::uint64_t request_watermark_ = 0;
    std::uint8_t applied_mask_ = 0;
    bool synchronized_ = false;
    bool output_started_ = false;
};

class WasdInputHistory {
public:
    // 首次观察到的已按住键没有可证明的按下边沿。先收到一次合法
    // 全松开才能建立本代际完整历史；输入缺口后同样重新同步。
    WasdMotionIntent observe(std::uint8_t held_mask, std::uint64_t epoch,
        std::uint64_t sequence, std::int64_t received_at_ns,
        bool input_valid = true, bool sequence_gap = false) noexcept;
    void reset() noexcept;
private:
    WasdMotionIntent state_;
    std::uint64_t epoch_ = 0;
    std::uint64_t sequence_ = 0;
    std::int64_t received_at_ns_ = 0;
    bool synchronized_ = false;
};

#endif // AUTO_STOP_H
