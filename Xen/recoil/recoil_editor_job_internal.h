#ifndef RECOIL_EDITOR_JOB_INTERNAL_H
#define RECOIL_EDITOR_JOB_INTERNAL_H

#include <atomic>
#include <memory>
#include <thread>
#include <utility>

namespace recoil_editor_detail {
// 单个按需任务持有独立数据；页面销毁不会等待 future，也不留下页面引用。
// 已进入同步存储/分析的任务必须完成。调用方关闭前持续 poll，不能把请求取消当成回滚。
template<class Value>
class Job {
public:
    enum class State { QUEUED, RUNNING, CANCELED, FINISHED };
    Value value;

    template<class Function>
    static std::shared_ptr<Job> start(Value value, Function function) {
        auto job = std::shared_ptr<Job>(new Job(std::move(value)));
        std::thread([job, function = std::move(function)]() mutable {
            State expected = State::QUEUED;
            if (job->state_.compare_exchange_strong(expected, State::RUNNING)) {
                try { function(job->value); }
                catch (...) { job->failed_.store(true, std::memory_order_relaxed); }
            }
            job->state_.store(State::FINISHED, std::memory_order_release);
        }).detach();
        return job;
    }
    bool ready() const noexcept { return state_.load(std::memory_order_acquire) == State::FINISHED; }
    bool failed() const noexcept { return failed_.load(std::memory_order_relaxed); }
    bool cancel() noexcept {
        State expected = State::QUEUED;
        return state_.compare_exchange_strong(expected, State::CANCELED);
    }
private:
    explicit Job(Value initial) : value(std::move(initial)) {}
    std::atomic<State> state_{State::QUEUED};
    std::atomic<bool> failed_{false};
};
}
#endif
