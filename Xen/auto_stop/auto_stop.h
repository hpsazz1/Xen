#ifndef AUTO_STOP_H
#define AUTO_STOP_H

#include <cstdint>
#include <array>
#include <vector>

// 移动制动配置独立于 Aim；允许键与有效目标可发起制动，不代表武装或开火请求。
struct AutoStopConfig {
    bool enabled = false;
    int activation_virtual_key = 0;
    std::vector<int> release_virtual_keys{0x31, 0x32, 0x33, 0x34, 0x35, 0x51};
};

enum class AutoStopStatus {
    DISABLED,
    UNSUPPORTED_BACKEND,
    UNBOUND,
    AWAITING_VALIDATION,
    PAUSED,
    READY, WAITING_INPUT, BRAKING, ESTIMATED, CANCELED, FAULT,
};

enum class AutoStopBlockReason {
    NONE, SOURCE_FOCUS, RELEASE_REQUIRED, INPUT_UNAVAILABLE, INPUT_HISTORY,
    ACTIVATION_NOT_HELD, SAFETY_PERMISSION, PAUSED, MOTION_UNAVAILABLE,
    CONTINUOUS_REQUEST_CONSUMED, NO_TARGET, SOURCE_TIMING_INVALID,
    SOURCE_UNCERTAINTY, TARGET_STALE, OUTPUT_FAULT, CROSSHAIR_OUTSIDE_TARGET
};
inline const char* AutoStopBlockReasonName(AutoStopBlockReason reason) noexcept {
    switch (reason) {
    case AutoStopBlockReason::NONE: return "条件满足";
    case AutoStopBlockReason::SOURCE_FOCUS: return "源程序未聚焦或桥接不可用";
    case AutoStopBlockReason::RELEASE_REQUIRED: return "切回后请松开允许键再按下";
    case AutoStopBlockReason::INPUT_UNAVAILABLE: return "设备按键监听不可用";
    case AutoStopBlockReason::INPUT_HISTORY: return "等待WASD全部释放以同步历史";
    case AutoStopBlockReason::ACTIVATION_NOT_HELD: return "未按住允许键";
    case AutoStopBlockReason::SAFETY_PERMISSION: return "物理输出未获安全许可";
    case AutoStopBlockReason::PAUSED: return "急停已暂停";
    case AutoStopBlockReason::MOTION_UNAVAILABLE: return "未移动或方向冲突";
    case AutoStopBlockReason::CONTINUOUS_REQUEST_CONSUMED: return "本次连续按住已执行，等待重新触发";
    case AutoStopBlockReason::NO_TARGET: return "未识别到配置目标";
    case AutoStopBlockReason::SOURCE_TIMING_INVALID: return "源帧时钟映射无效";
    case AutoStopBlockReason::SOURCE_UNCERTAINTY: return "源时钟不确定度超界";
    case AutoStopBlockReason::TARGET_STALE: return "目标帧已超过50ms有效期";
    case AutoStopBlockReason::OUTPUT_FAULT: return "设备输出故障";
    case AutoStopBlockReason::CROSSHAIR_OUTSIDE_TARGET: return "准星未进入人物范围";
    }
    return "未知阻断原因";
}

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
    bool independent_trigger_enabled = false;
    bool target_available = false;
    bool source_focused = false;
    bool focus_required = false;
    bool release_required = false;
    AutoStopBlockReason block_reason = AutoStopBlockReason::NONE;
    std::uint64_t rescue_attempts = 0, rescue_succeeded = 0, rescue_failed = 0;
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
    // 调用方须证明完整屏蔽、正常释放与清理ACK，并保持真实监听连续；
    // 此接口只承接估计模型，不创建物理释放事件或停稳证据。
    bool resume_after_masked_hold(const WasdMotionIntent& intent, std::int64_t released_at_ns) noexcept;
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
    bool masked_hold_model_valid_ = false;
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
