#ifndef AUTO_STOP_WORKER_H
#define AUTO_STOP_WORKER_H

#include <atomic>
#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <chrono>
#include <condition_variable>
#include <vector>
#include "auto_stop/auto_stop.h"
#include "mouse/mouse.h"
#include "source_context/source_context.h"

enum class OutputArbiterSource { AIM, TRIGGER, RECOIL };
enum class OutputArbiterRejection { NONE, LOCK_BUSY, AUXILIARY_PENDING, OUTPUT_FAULT };
struct OutputArbiterCounters {
    std::uint64_t acquired = 0, lock_busy = 0, auxiliary_pending = 0, output_fault = 0;
};
struct OutputArbiterSnapshot {
    // 下标与OutputArbiterSource一致；并发读取是各计数器的观测值，不宣称同一事务快照。
    std::array<OutputArbiterCounters, 3> sources{};
};

// 仅关联生产观测；零值表示没有该项证据，不按帧率或本机序号补造。
struct AutoStopTargetEvidence {
    std::uint64_t observation_epoch = 0, observation_sequence = 0;
    std::int64_t source_observed_ns = 0;
};
enum class AutoStopExecutionKind {
    RECORDING_STARTED, RECORDING_STOPPED, WORKER_STARTED, WORKER_STOPPED,
    INPUT_CHANGED, INPUT_GAP, BLOCK_CHANGED, REQUEST_STARTED, REQUEST_REJECTED,
    BACKEND_BEGIN, BACKEND_RESULT, ESTIMATED, CANCELED, MOVEMENT_RETURNED,
    CLEANUP_UNKNOWN, RESCUE, MANUAL_FIRE_RETAINED, RESUME_REQUESTED
};
enum class AutoStopBackendAction { NONE, MASK, KEYBOARD, CLEANUP };
const char* AutoStopExecutionKindName(AutoStopExecutionKind kind) noexcept;
const char* AutoStopBackendActionName(AutoStopBackendAction action) noexcept;
inline constexpr std::size_t kAutoStopExecutionCapacity = 2048;
struct AutoStopExecutionEvent {
    std::uint64_t sequence = 0, generation = 0, request_id = 0, command_id = 0, backend_call_id = 0;
    AutoStopExecutionKind kind = AutoStopExecutionKind::INPUT_CHANGED;
    AutoStopBackendAction backend_action = AutoStopBackendAction::NONE;
    // reason仅引用字符串字面量；事件不持有动态资源，也不在热路径格式化或写盘。
    const char* reason = "none";
    AutoStopStatus status = AutoStopStatus::DISABLED;
    AutoStopBlockReason block_reason = AutoStopBlockReason::NONE;
    AutoStopTargetEvidence target;
    std::uint64_t source_session_id = 0, source_sequence = 0, source_safety_epoch = 0;
    std::uint64_t input_epoch = 0, input_sequence = 0, monitor_sequence = 0;
    std::int64_t input_received_ns = 0, observed_at_ns = 0, call_started_ns = 0;
    std::int64_t backend_completed_ns = 0, protocol_ack_received_ns = 0;
    std::int64_t completion_ready_ns = 0;
    // 监听快照、WASD事件和软件输出三者分别保留；不把它们解释为游戏键态或速度。
    std::uint8_t physical_wasd_mask = 0, event_wasd_mask = 0, software_mask = 0, command_mask = 0;
    bool input_valid = false, event_valid = false, input_continuous = false, history_valid = false;
    bool left_button_held = false, activation_held = false, release_held = false, end_held = false;
    bool cleanup_unknown = false, datagram_sent = false, independent = false, manual_release = false;
    InputMonitorStatus input_status = InputMonitorStatus::CLOSED;
    KeyboardDisposition receipt = KeyboardDisposition::UNSUPPORTED;
};
struct AutoStopExecutionLog {
    std::vector<AutoStopExecutionEvent> events;
    // first/last是保留环的水位，即使after_sequence过滤后为空也不改变；查询不消费。
    std::uint64_t dropped_count = 0, first_sequence = 0, last_sequence = 0, generation = 0;
    bool enabled = false;
};
// 同一Runtime的Aim发送和辅助发送共用此门；配置等待不占用后端事务。
class AutoStopOutputArbiter {
public:
    std::unique_lock<std::timed_mutex> try_enter_aim(OutputArbiterSource source = OutputArbiterSource::AIM,
        OutputArbiterRejection* rejection = nullptr) noexcept;
    // Runtime在计算前有限等待短事务；获得门后仍须复核许可，不排队补发旧命令。
    std::unique_lock<std::timed_mutex> enter_aim_until(std::chrono::steady_clock::time_point deadline,
        OutputArbiterRejection* rejection = nullptr) noexcept;
    OutputArbiterSnapshot snapshot() const noexcept;
    // 撤销本owner既有按钮债务不受普通发送门禁阻挡。
    std::unique_lock<std::timed_mutex> try_enter_cleanup() noexcept;
    void latch_output_fault() noexcept;
    // 调用者必须持本arbiter返回的输出锁；所有权与Aim计算/提交在同一边界切换。
    void set_recoil_y_owned(bool owned) noexcept { recoil_y_owned_.store(owned, std::memory_order_release); }
    bool recoil_y_owned() const noexcept { return recoil_y_owned_.load(std::memory_order_acquire); }
    void release_recoil_y() noexcept {
        std::lock_guard lock(mutex_);
        recoil_y_owned_.store(false, std::memory_order_release);
    }
    std::uint64_t aim_skips() const noexcept { return aim_skips_.load(std::memory_order_relaxed); }
private:
    friend class AutoStopWorker;
    void set_auxiliary_pending(bool pending) noexcept;
    std::timed_mutex mutex_;
    // 等待状态锁不跨后端mutex获取；通知与谓词在同一锁下更新，避免丢唤醒。
    std::mutex pending_mutex_;
    std::condition_variable pending_changed_;
    std::atomic<bool> auxiliary_pending_{false};
    std::atomic<bool> faulted_{false};
    std::atomic<bool> recoil_y_owned_{false};
    std::atomic<std::uint64_t> aim_skips_{0};
    struct Counters {
        std::atomic<std::uint64_t> acquired{0}, lock_busy{0}, auxiliary_pending{0}, output_fault{0};
    };
    std::array<Counters, 3> counters_{};
};

class AutoStopWorker {
public:
    // 许可回调应只读取原子安全状态，不能要求Aim hold或持有Runtime生命周期锁。
    AutoStopWorker(std::shared_ptr<IMouseController> mouse,
        std::shared_ptr<AutoStopOutputArbiter> arbiter,
        std::function<bool()> output_permission,
        std::function<std::uint64_t()> allocate_request = {},
        std::function<bool()> focused = {},
        std::function<AutoStopWeaponContext()> weapon_context = {});
    // 有来源身份时直接提供不可变快照；回调应支持并发读取，不能保存消费者会话状态。
    AutoStopWorker(std::shared_ptr<IMouseController> mouse,
        std::shared_ptr<AutoStopOutputArbiter> arbiter,
        std::function<bool()> output_permission,
        std::function<std::uint64_t()> allocate_request,
        std::function<source_context::SourceContextSnapshot()> source_context,
        std::function<AutoStopWeaponContext()> weapon_context);
    ~AutoStopWorker();
    AutoStopWorker(const AutoStopWorker&) = delete;
    AutoStopWorker& operator=(const AutoStopWorker&) = delete;

    bool start(const AutoStopConfig& config, int command_timeout_ms = 300) noexcept;
    void stop() noexcept;
    // 保留显式请求入口；独立目标模式与调用方共用互斥请求槽，取消按 id 隔离。
    // 显式新id在500ms时触发取消，不能续期；归还另受在途ACK与清理耗时约束。
    bool request(std::uint64_t request_id) noexcept;
    void cancel() noexcept;
    void cancel(std::uint64_t request_id) noexcept;
    void set_paused(bool paused) noexcept;
    void set_movement_suspended(bool suspended) noexcept;
    bool movement_suspension_ready() const noexcept;
    AutoStopSnapshot snapshot() const noexcept;
    // 默认关闭；关闭保留既有事件。序号在本worker生命周期内不回退，start不重置偏好。
    // 开启和每次运行递增generation；覆盖只计真实环覆盖，不把关闭期间计为丢失。
    void set_diagnostics_enabled(bool enabled) noexcept;
    AutoStopExecutionLog execution_log(std::uint64_t after_sequence = 0) const;
    // 只返回独立四键接管仍有效的估计完成id，不是观察停稳证明。
    std::uint64_t estimated_completion_id() const noexcept;
    // 只证明无制动事务且新WASD事件没有未处理移动；不把零键态解释为实测停稳。
    bool idle_for_trigger(const InputSnapshot& input) const noexcept;
    // 点射UP确认后投递归还请求；只在worker线程操作设备，not_before前保持真实移动。
    bool resume_movement(std::uint64_t request_id, std::chrono::steady_clock::time_point not_before) noexcept;
    // 人工左键接管当前独立估计完成请求；确认后锁存四键保持，不再依赖激活键或左键。
    // 目标失效、武器上下文变化及公共安全撤销仍清理；保持期间拒绝点射归还。
    bool retain_for_manual_fire(std::uint64_t request_id) noexcept;
    // 独立目标存在事实，仅人工接管保持消费；准星离框不等于目标消失。
    // 无目标/失败发布零期限，陈旧帧不能刷新；期限失效撤销原保持，不自动承接新目标。
    void publish_tracking_target(std::chrono::steady_clock::time_point valid_until,
        AutoStopTargetEvidence evidence = {}) noexcept;
    // 检测线程发布不可变目标事实；无目标/失败发布零期限，旧帧不能自行续期。
    // 目标仅准入；四键接管后制动与保持都锁存至松键或安全撤销，软件制动最多500ms。
    void publish_target(std::chrono::steady_clock::time_point valid_until,
        AutoStopBlockReason reason = AutoStopBlockReason::NO_TARGET,
        AutoStopTargetEvidence evidence = {}) noexcept;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

#endif
