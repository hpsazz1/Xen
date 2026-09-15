#ifndef AUTO_STOP_WORKER_H
#define AUTO_STOP_WORKER_H

#include <atomic>
#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <chrono>
#include <condition_variable>
#include "auto_stop/auto_stop.h"
#include "mouse/mouse.h"

enum class OutputArbiterSource { AIM, TRIGGER, RECOIL };
enum class OutputArbiterRejection { NONE, LOCK_BUSY, AUXILIARY_PENDING, OUTPUT_FAULT };
struct OutputArbiterCounters {
    std::uint64_t acquired = 0, lock_busy = 0, auxiliary_pending = 0, output_fault = 0;
};
struct OutputArbiterSnapshot {
    // 下标与OutputArbiterSource一致；并发读取是各计数器的观测值，不宣称同一事务快照。
    std::array<OutputArbiterCounters, 3> sources{};
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
    AutoStopSnapshot snapshot() const noexcept;
    // 只返回独立四键接管仍有效的估计完成id，不是观察停稳证明。
    std::uint64_t estimated_completion_id() const noexcept;
    // 点射UP确认后投递归还请求；只在worker线程操作设备，not_before前保持真实移动。
    bool resume_movement(std::uint64_t request_id, std::chrono::steady_clock::time_point not_before) noexcept;
    // 检测线程发布不可变目标事实；无目标/失败发布零期限，旧帧不能自行续期。
    // 目标仅准入；四键接管后制动与保持都锁存至松键或安全撤销，软件制动最多500ms。
    void publish_target(std::chrono::steady_clock::time_point valid_until,
        AutoStopBlockReason reason = AutoStopBlockReason::NO_TARGET) noexcept;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

#endif
