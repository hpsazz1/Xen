#ifndef AUTO_STOP_WORKER_H
#define AUTO_STOP_WORKER_H

#include <atomic>
#include <array>
#include <functional>
#include <memory>
#include <mutex>
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
// 同一Runtime的Aim发送和辅助发送共用此门。Aim只尝试，不等待辅助事务。
class AutoStopOutputArbiter {
public:
    std::unique_lock<std::timed_mutex> try_enter_aim(OutputArbiterSource source = OutputArbiterSource::AIM,
        OutputArbiterRejection* rejection = nullptr) noexcept;
    OutputArbiterSnapshot snapshot() const noexcept;
    // 撤销本owner既有按钮债务不受普通发送门禁阻挡。
    std::unique_lock<std::timed_mutex> try_enter_cleanup() noexcept;
    void latch_output_fault() noexcept { faulted_.store(true, std::memory_order_release); }
    std::uint64_t aim_skips() const noexcept { return aim_skips_.load(std::memory_order_relaxed); }
private:
    friend class AutoStopWorker;
    std::timed_mutex mutex_;
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
        std::function<bool()> output_permission);
    ~AutoStopWorker();
    AutoStopWorker(const AutoStopWorker&) = delete;
    AutoStopWorker& operator=(const AutoStopWorker&) = delete;

    bool start(const AutoStopConfig& config, int command_timeout_ms = 300) noexcept;
    void stop() noexcept;
    // 请求只由自动扳机等调用方显式提交；允许键本身绝不生成请求。
    // 每个新id在500ms时触发取消，不能续期；归还另受在途ACK与清理耗时约束。
    bool request(std::uint64_t request_id) noexcept;
    void cancel() noexcept;
    void cancel(std::uint64_t request_id) noexcept;
    void set_paused(bool paused) noexcept;
    AutoStopSnapshot snapshot() const noexcept;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

#endif
