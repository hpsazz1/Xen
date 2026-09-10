#ifndef TRIGGER_WORKER_H
#define TRIGGER_WORKER_H
#include <atomic>
#include <functional>
#include <memory>
#include "trigger/trigger.h"
struct TriggerFiringSignal {
    bool confirmed_down = false;
    std::uint64_t id = 0;
    TriggerTime started_at{};
};
#include "auto_stop/auto_stop_worker.h"

// 单owner执行器；图像发布不等待网络，取消/按键释放不等待下一图像。
class TriggerWorker {
public:
    TriggerWorker(std::shared_ptr<IMouseController> mouse,
        std::shared_ptr<AutoStopOutputArbiter> arbiter,
        std::function<bool()> permission, std::function<bool()> focused,
        std::function<std::uint64_t()> next_stop_id,
        std::function<bool(std::uint64_t)> request_stop,
        std::function<void(std::uint64_t)> cancel_stop);
    ~TriggerWorker();
    // 停止清理的锁准入预算；已在途设备调用另受后端command timeout约束。
    bool start(const TriggerConfig& config, int cleanup_budget_ms = 1000) noexcept;
    void publish(std::shared_ptr<const TriggerObservation> observation) noexcept;
    void cancel() noexcept;
    void stop() noexcept;
    TriggerSnapshot snapshot() const noexcept;
    TriggerFiringSignal firing_signal() const noexcept;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
#endif
