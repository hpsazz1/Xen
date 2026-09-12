#ifndef TRIGGER_WORKER_H
#define TRIGGER_WORKER_H
#include <atomic>
#include <functional>
#include <memory>
#include <vector>
#include "trigger/trigger.h"
struct TriggerFiringSignal {
    bool confirmed_down = false;
    std::uint64_t id = 0;
    TriggerTime started_at{};
    TriggerTime call_started_at{}, backend_completed_at{}, protocol_ack_received_at{}, observed_at{};
    // 命令调用开始到后端完成的区间宽度，不是游戏发射时刻的置信界。
    std::chrono::nanoseconds uncertainty{};
};
struct TriggerExecutionEvent {
    std::uint64_t sequence = 0;
    TriggerSnapshot snapshot;
    TriggerButtonAction button_action = TriggerButtonAction::NONE;
    TriggerStopAction stop_action = TriggerStopAction::NONE;
    std::uint64_t stop_request_id = 0;
    TriggerReceiptStatus receipt_status = TriggerReceiptStatus::NOT_SENT;
    bool backend_called = false;
    const char* rejection_reason = "none";
    // planned_at是决策消费时刻，不是控制器预定唤醒时间；不能据此声称调度迟到。
    TriggerTime planned_at{}, call_started_at{}, backend_completed_at{}, protocol_ack_received_at{}, observed_at{}, source_observed_at{};
};
struct TriggerExecutionLog {
    std::vector<TriggerExecutionEvent> events;
    std::uint64_t dropped_count = 0, first_sequence = 0, last_sequence = 0;
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
        std::function<void(std::uint64_t)> cancel_stop,
        std::function<TriggerContext()> context = {});
    ~TriggerWorker();
    // 停止清理的锁准入预算；已在途设备调用另受后端command timeout约束。
    bool start(const TriggerConfig& config, int cleanup_budget_ms = 1000) noexcept;
    void publish(std::shared_ptr<const TriggerObservation> observation) noexcept;
    void cancel() noexcept;
    void stop() noexcept;
    TriggerSnapshot snapshot() const noexcept;
    TriggerFiringSignal firing_signal() const noexcept;
    // 冷路径复制有界事件环；不依赖普通Log等级，不清空其他消费者的证据。
    TriggerExecutionLog execution_log() const;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
#endif
