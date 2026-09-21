#ifndef AUTO_STOP_H
#define AUTO_STOP_H

#include <cstdint>
#include <array>
#include <vector>
#include <string_view>

// 移动制动配置独立于 Aim；允许键与有效目标可发起制动，不代表武装或开火请求。
struct AutoStopConfig {
    bool enabled = false;
    int activation_virtual_key = 0;
    std::vector<int> release_virtual_keys{0x31, 0x32, 0x33, 0x34, 0x35, 0x51};
    // 正式入口统一采用已验收时序；false 仅保留给旧模型回归。
    bool use_counterpulse_timing = true;
    int counter_hold_ms = 40, shot_after_release_ms = 18;
    bool cycle_enabled = false;
};

enum class AutoStopStatus {
    DISABLED,
    UNSUPPORTED_BACKEND,
    UNBOUND,
    AWAITING_VALIDATION,
    PAUSED,
    READY, WAITING_INPUT, BRAKING, ESTIMATED, CANCELED, FAULT, MASKED,
};

enum class AutoStopBlockReason {
    NONE, SOURCE_FOCUS, RELEASE_REQUIRED, INPUT_UNAVAILABLE, INPUT_HISTORY,
    ACTIVATION_NOT_HELD, SAFETY_PERMISSION, PAUSED, MOTION_UNAVAILABLE,
    CONTINUOUS_REQUEST_CONSUMED, NO_TARGET, SOURCE_TIMING_INVALID,
    SOURCE_UNCERTAINTY, TARGET_STALE, OUTPUT_FAULT, CROSSHAIR_OUTSIDE_TARGET, WEAPON_CONTEXT
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
    case AutoStopBlockReason::WEAPON_CONTEXT: return "GSI武器或点射资料不可用";
    }
    return "未知阻断原因";
}

const char* AutoStopStatusName(AutoStopStatus status) noexcept;

// 名称只引用共享武器目录的静态存储；有效性由提供方按实时新鲜度判断。
// 这是武器上下文，不是停稳、开火或逐发时间证明。
struct AutoStopWeaponContext {
    bool required = false, valid = false;
    std::uint64_t generation = 0;
    std::string_view canonical_id;
    // 普通武器暂停只在同一可信来源代际内恢复；默认保持旧提供方保守语义。
    std::uint64_t trust_generation = 0;
    bool session_trusted = false;
};

struct AutoStopSnapshot {
    AutoStopWeaponContext weapon_context;
    bool cycle_moving = false;
    std::uint64_t cycle_count = 0;
    bool use_counterpulse_timing = false;
    int counter_hold_ms = 0, shot_after_release_ms = 0;
    std::int64_t counter_release_ack_ns = 0, completion_ready_ns = 0;
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

// 能力检查不产生物理操作；时序估计与严格停稳观察保持独立。
AutoStopSnapshot assess_auto_stop_availability(
    const AutoStopConfig& config, bool kmbox_backend,
    bool protocol_available, bool paused) noexcept;

struct WasdMotionIntent {
    // 原始事件连续不等于运动可估计；相反键重叠只破坏后者。
    bool input_continuous = false;
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

// 仅在连续真实事件的WASD全部松开时返回紧邻前态方向；部分释放不累计。
std::uint8_t WasdReleasedAxes(const WasdMotionIntent& previous,
                             const WasdMotionIntent& current) noexcept;

enum class AutoStopPhase { IDLE, WAITING_ACK, BRAKING, COMPLETE_ESTIMATED, CANCELLED, INVALID, SETTLING };
struct AutoStopDecision {
    AutoStopPhase phase = AutoStopPhase::IDLE;
    std::uint64_t request_id = 0;
    std::uint64_t command_id = 0;
    std::uint8_t desired_mask = 0;
    // 顺序为纵向、横向；未确认报告前为零，不得提前启动制动计时。
    std::array<std::int64_t, 2> axis_deadline_ns{};
    bool estimated = true;
    bool fire_permitted = false;
    std::int64_t completion_ready_ns = 0;
};

// 纯状态机，不接触设备。旧230/180/110ms模型保留回退；H40仅迁移ACK时序，均非速度观察。
class AutoStopController {
public:
    AutoStopController() noexcept : AutoStopController(AutoStopConfig{}) {}
    explicit AutoStopController(const AutoStopConfig& config) noexcept;
    AutoStopDecision observe(const WasdMotionIntent& intent, std::int64_t now_ns) noexcept;
    AutoStopDecision request(std::uint64_t request_id, std::int64_t now_ns) noexcept;
    // released_mask由真实释放边沿取得；不改写当前input，不使用旧模型估计速度。
    AutoStopDecision request_manual_release(std::uint64_t request_id,
        std::uint8_t released_mask, std::int64_t now_ns) noexcept;
    AutoStopDecision cancel(std::uint64_t request_id, std::int64_t now_ns) noexcept;
    AutoStopDecision tick(std::int64_t now_ns) noexcept;
    // 调用方须证明完整屏蔽、正常释放与清理ACK，并保持真实监听连续；
    // 此接口只承接估计模型，不创建物理释放事件或停稳证据。
    bool resume_after_masked_hold(const WasdMotionIntent& intent, std::int64_t released_at_ns) noexcept;
    // 仅固定反向时序：清理ACK且真实监听连续后建立新计划，不承接旧运动模型。
    bool restart_after_cleanup(const WasdMotionIntent& intent, std::int64_t cleanup_ns) noexcept;
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
    bool counterpulse_ = false, timing_valid_ = true, initial_zero_ = false;
    std::uint8_t counter_mask_ = 0;
    std::int64_t counter_hold_ns_ = 40000000, after_release_ns_ = 18000000;
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
