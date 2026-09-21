#ifndef TRIGGER_H
#define TRIGGER_H

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>
#include "detector/detector.h"

enum class TriggerFireMode { SINGLE, AUTOMATIC };
enum class TriggerRegion { NONE, HEAD, BODY, GENERAL };
enum class TriggerButtonAction { NONE, DOWN, UP };
enum class TriggerStopAction { NONE, REQUEST, CANCEL };
enum class TriggerReceiptStatus { ACKNOWLEDGED, NOT_SENT, UNKNOWN };
enum class TriggerPhase { DISABLED, WAITING, QUALIFYING, WAIT_STOP, DOWN_PENDING, HELD, UP_PENDING, COOLDOWN, FAULT };
enum class TriggerPermissionBlock { NONE, INPUT_UNHEALTHY, NOT_FOCUSED, NOT_ARMED, MANUAL_FIRE };
enum class TriggerReason {
    NONE, DISABLED, INVALID_CONFIG, WAIT_RELEASE, PERMISSION, INVALID_OBSERVATION,
    TIMING_UNAVAILABLE, STALE, NO_CANDIDATE, TARGET_CHANGED, DELAY, COOLDOWN,
    WAIT_NEW_FRAME, STOP_UNVERIFIED, STOP_EXPIRED, COMMAND_PENDING, RELEASED,
    UNKNOWN_RECEIPT, CANCELED, COUNTER_EXHAUSTED, CONTEXT_CHANGED, CONTEXT_UNAVAILABLE, FIRE_DISABLED
};

using TriggerClock = std::chrono::steady_clock;
using TriggerTime = TriggerClock::time_point;

struct TriggerConfig {
    bool enabled = false;
    bool fire_enabled = true;
    int hold_virtual_key = 0;
    float range_percent = 100.0f;
    float head_width_percent = 60.0f, head_height_percent = 60.0f;
    float body_width_percent = 50.0f, body_height_percent = 60.0f;
    float general_width_percent = 50.0f, general_height_percent = 50.0f;
    float min_confidence = 0.0f;
    int fire_delay_ms = 20, shot_interval_ms = 120, press_duration_ms = 20;
    int max_hold_ms = 300, max_observation_age_ms = 50;
    bool require_stop = false;
    // 显式估计策略不替代严格观察资格，也不创建观察租约。
    bool allow_estimated_stop = false;
    TriggerFireMode fire_mode = TriggerFireMode::SINGLE;
    std::vector<int> person_class_ids{0}, head_class_ids{1}, general_class_ids;
};

bool valid_trigger_config(const TriggerConfig& config) noexcept;
const char* TriggerReasonName(TriggerReason reason) noexcept;

struct TriggerObservation {
    std::vector<Detection> detections;
    float center_x = 0.0f, center_y = 0.0f;
    int roi_width = 0, roi_height = 0;
    std::uint64_t epoch = 0, sequence = 0;
    TriggerTime observed_at{};
    std::chrono::nanoseconds uncertainty{};
    bool valid = false, timing_valid = false;
};

// 可选的持续上下文；generation 由来源 owner 在身份/连续性/有效性变化时递增。
// required=false 保留显式手动模式；上下文不授予开火或观察停稳资格。
struct TriggerContext {
    std::uint64_t generation = 0;
    bool required = false, valid = false;
    // 点射资料与来源上下文使用同一代际；连续扫射不读取此节奏。
    bool timing_required = false, timing_valid = false;
    int shot_hold_ms = 0, fire_interval_ms = 0;
    // 运行期间不可变；武器名仅引用共享目录的静态ID，不持有临时字符串。
    std::uint64_t timing_catalog_revision = 0;
    std::string_view timing_weapon_id;
    // 0/false保留未提供可信分类的旧调用方保守语义。
    std::uint64_t trust_generation = 0;
    bool session_trusted = false;
};

struct TriggerPermit {
    bool enabled = false, held = false, healthy = false, focused = false, armed = false;
    bool physical_left_down = false;
    TriggerContext context;
    // Runtime 分配全会话递增的急停 id；仅 REQUEST 消费此值，不能逐帧续租。
    std::uint64_t next_stop_request_id = 0;
    std::uint64_t stop_request_id = 0, stop_observation_epoch = 0;
    bool stop_observed_qualified = false;
    bool stop_estimated_qualified = false;
    // 无移动键且急停所有者空闲；只免除联动等待，不声明游戏内已停稳。
    bool stop_not_needed = false;
    std::uint64_t estimated_stop_request_id = 0;
    TriggerTime stop_expires_at{};
    // Runtime 已预留清理预算的期限；并非物理硬实时归还保证。
    TriggerTime stop_release_deadline{};
};

struct TriggerSnapshot {
    bool stop_not_needed = false;
    TriggerPhase phase = TriggerPhase::DISABLED;
    TriggerReason reason = TriggerReason::DISABLED;
    TriggerPermissionBlock permission_block = TriggerPermissionBlock::NONE;
    TriggerRegion region = TriggerRegion::NONE;
    std::uint64_t candidate_id = 0, observation_epoch = 0, observation_sequence = 0;
    std::uint64_t command_id = 0, stop_request_id = 0;
    std::uint64_t estimated_stop_request_id = 0;
    float normalized_margin = 0.0f;
    bool button_may_be_down = false, faulted = false;
    TriggerContext context;
    // DOWN决策锁存，后续上下文变化导致UP时仍可追溯原发参数。
    TriggerContext firing_context;
    bool firing_context_available = false;
};

struct TriggerDecision {
    TriggerButtonAction button_action = TriggerButtonAction::NONE;
    std::uint64_t command_id = 0;
    TriggerStopAction stop_action = TriggerStopAction::NONE;
    std::uint64_t stop_request_id = 0;
    TriggerTime next_deadline{};
    TriggerSnapshot snapshot;
};

struct TriggerReceipt {
    std::uint64_t command_id = 0;
    TriggerButtonAction action = TriggerButtonAction::NONE;
    TriggerReceiptStatus status = TriggerReceiptStatus::UNKNOWN;
    TriggerTime completed_at{};
    // 实际后端提交时刻；旧调用未提供时以 ACK 保守起算间隔。
    TriggerTime submitted_at{};
    // 点射按住从协议ACK起算；旧回执缺省时使用后端完成时刻保守兼容。
    TriggerTime protocol_ack_received_at{};
};

// 单线程纯状态机。所有方法的 now 必须处于同一单调时间域；不访问设备或睡眠。
// observe 只接收新发布版本，重复读取同一快照应调用 tick，不重新发布旧帧。
class TriggerController {
public:
    // 有待处理按钮/急停债务时拒绝改配置，调用方先 cancel 并完成清理。
    bool configure(const TriggerConfig& config) noexcept;
    TriggerDecision observe(const TriggerObservation& observation, const TriggerPermit& permit, TriggerTime now) noexcept;
    TriggerDecision tick(const TriggerPermit& permit, TriggerTime now) noexcept;
    TriggerDecision acknowledge(const TriggerReceipt& receipt, TriggerTime now) noexcept;
    // 仅由确认尚未调用后端的owner撤销待发DOWN；兼容已生成但未发送的UP，不创建设备回执。
    // 已ACK、UNKNOWN或不匹配的命令不能通过此接口清债。
    bool withdraw_unsent(std::uint64_t down_command_id, TriggerTime now) noexcept;
    TriggerDecision cancel(TriggerReason reason, TriggerTime now) noexcept;
    TriggerSnapshot snapshot() const noexcept { return state_; }
private:
    TriggerDecision result(TriggerTime now) const noexcept;
    TriggerDecision release(TriggerReason reason, TriggerTime now) noexcept;
    std::optional<TriggerDecision> check_context(const TriggerPermit& permit, TriggerTime now) noexcept;
    std::optional<TriggerDecision> check_permission(const TriggerPermit& permit, TriggerTime now) noexcept;
    bool select_candidate(const TriggerObservation& observation, TriggerTime now) noexcept;
    TriggerConfig config_;
    TriggerSnapshot state_;
    Detection anchor_;
    TriggerRegion anchor_region_ = TriggerRegion::NONE;
    TriggerTime qualified_at_{}, observation_expires_{}, held_until_{}, cooldown_until_{}, pending_at_{}, last_now_{};
    TriggerTime stop_expires_{}, stop_release_deadline_{};
    std::uint64_t next_command_id_ = 0, next_candidate_id_ = 0, last_stop_id_ = 0;
    std::uint64_t last_down_epoch_ = 0, last_down_sequence_ = 0;
    TriggerButtonAction pending_ = TriggerButtonAction::NONE;
    TriggerReason observation_failure_ = TriggerReason::NONE;
    bool config_valid_ = true, candidate_valid_ = false, release_seen_ = false, unconfirmed_down_ = false;
    float center_x_ = 0.0f, center_y_ = 0.0f;
    int roi_width_ = 0, roi_height_ = 0;
    // 当前 DOWN 的不可变快照，直到其 UP 清理结束仍用于保守冷却。
    int active_hold_ms_ = 0, active_interval_ms_ = 0;
};

#endif
