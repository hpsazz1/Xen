#ifndef APP_REPORT_LIFECYCLE_INTERNAL_H
#define APP_REPORT_LIFECYCLE_INTERNAL_H

#include "runtime/runtime.h"
#include <algorithm>
#include <chrono>
#include <future>
#include <optional>
#include <utility>

namespace app::detail {

// App 冷路径持有分段水位；后台封尾结束前，主线程不得再次访问。
struct ReportBoundary {
    std::uint64_t trigger_after_event = 0;
    std::uint64_t archive_after_event = 0;
    std::uint64_t recoil_after_command = 0;

    template<class Source>
    void begin_segment(Source& source) {
        // 所有开段入口共用当前生产水位，包含模型加载成功/失败后的恢复。
        trigger_after_event = archive_after_event = source.trigger_execution_log().last_sequence;
        const auto recoil = source.recoil_execution_log();
        recoil_after_command = recoil.records.empty() ? 0 : recoil.records.back().intent.command_id;
    }

    void filter_final(RuntimeSnapshot& snapshot) {
        auto& events = snapshot.trigger_execution_log.events;
        std::erase_if(events, [&](const auto& event) { return event.sequence <= trigger_after_event; });
        if (!events.empty()) trigger_after_event = events.back().sequence;
        snapshot.trigger_execution_log.first_sequence = events.empty() ? 0 : events.front().sequence;
        snapshot.trigger_execution_log.last_sequence = events.empty() ? 0 : events.back().sequence;
        auto& records = snapshot.recoil_execution_log.records;
        std::erase_if(records, [&](const auto& record) { return record.intent.command_id <= recoil_after_command; });
        if (!records.empty()) recoil_after_command = records.back().intent.command_id;
    }
};

// 封尾入口由 App 与回归共同使用；所有访问均在既有 future 屏障内。
class ReportFinalization {
public:
    bool pending() const noexcept { return frozen_.has_value(); }
    bool failed() const noexcept { return !error_.empty(); }
    const std::string& error() const noexcept { return error_; }

    template<class Report, class Archive, class Freeze, class Drain>
    bool finish(bool& report_active, bool& archive_active, ReportBoundary& boundary,
                Report& report, Archive& archive, Freeze&& freeze, Drain&& drain) noexcept {
        try {
            if (!frozen_) {
                if (!report_active && !archive_active) return true;
                frozen_ = std::forward<Freeze>(freeze)();
                std::forward<Drain>(drain)(*frozen_);
                archive.stop();
                archive_active = false;
                if (report_active) boundary.filter_final(*frozen_);
            }
            // 发布失败不能把 ready 当成功，也不能再次过滤已推进水位的旧快照。
            if (report_active && !report.finalize(*frozen_, error_)) return false;
            report_active = false;
            frozen_.reset();
            error_.clear();
            return true;
        } catch (...) {
            error_ = "报告封尾异常，旧段尚未确认保存";
            return false;
        }
    }
private:
    std::optional<RuntimeSnapshot> frozen_;
    std::string error_;
};

template<class Source, class Finish>
auto finish_reports_after_archive(Source& source, RuntimeSnapshot frozen, Finish&& finish) {
    source.set_recoil_archive({});
    // 执行事件保留关闭水位；只补入等待封尾后才确定的写盘结果。
    frozen.recoil_archive = source.snapshot().recoil_archive;
    return std::forward<Finish>(finish)(std::move(frozen));
}

struct ReportResumeResult {
    bool reload_finished = false;
    bool report_started = false;
};

// 主循环直接使用这条路由；测试驱动同一 LOADING/恢复/取消决策，不另写状态机副本。
class ReportResume {
public:
    void diagnostics_changed(bool enabled) noexcept { start_pending_ = enabled; }
    void reload_requested(bool accepted) noexcept { reload_pending_ = accepted; }
    void cancel() noexcept { start_pending_ = reload_pending_ = false; }

    template<class Start>
    ReportResumeResult poll(const RuntimeSnapshot& snapshot, bool busy, Start&& start) {
        if (busy) return {};
        if (reload_pending_) {
            if (snapshot.state != RuntimeState::RUNNING) {
                cancel();
                return {};
            }
            if (snapshot.detector_reload_state == DetectorReloadState::LOADING) return {};
            cancel();
            return {true, std::forward<Start>(start)(snapshot)};
        }
        if (std::exchange(start_pending_, false)) std::forward<Start>(start)(snapshot);
        return {};
    }
private:
    bool start_pending_ = false;
    bool reload_pending_ = false;
};

enum class ReportRestartTarget { NONE, RUNTIME, APPLICATION };

// 两种重启共用成功门槛；仅主线程在 future.get() 回收结果后消费请求。
class ReportRestart {
public:
    template<class Stop>
    bool defer_if_active(bool active, Stop&& stop) {
        if (!active) return false;
        std::forward<Stop>(stop)();
        pending_ = ReportRestartTarget::RUNTIME;
        return true;
    }

    template<class Stop>
    void request_application(Stop&& stop) {
        std::forward<Stop>(stop)();
        pending_ = ReportRestartTarget::APPLICATION;
    }
    ReportRestartTarget take_ready(bool busy, bool cancelled = false) noexcept {
        if (cancelled) cancel();
        return busy ? ReportRestartTarget::NONE : std::exchange(pending_, ReportRestartTarget::NONE);
    }
    void finish_completed(bool succeeded) noexcept { if (!succeeded) cancel(); }
    void cancel() noexcept { pending_ = ReportRestartTarget::NONE; }
private:
    ReportRestartTarget pending_ = ReportRestartTarget::NONE;
};

enum class ReportStopRequest { IDLE, STARTED, DEFERRED, BUSY };
enum class ReportJobKind { FINISH, STOP };

// 主循环与测试共用两阶段调度：回收封尾结果后才停止，回收停止结果后才允许重启。
// future 及请求只由主线程访问；报告活动标志在相应 future 回收后读取。
class ReportLifecycle {
public:
    ReportLifecycle(const bool& report_active, const bool& archive_active,
                    const ReportFinalization& finalization)
        : report_active_(report_active), archive_active_(archive_active), finalization_(finalization) {}
    ReportLifecycle(const ReportLifecycle&) = delete;
    ReportLifecycle& operator=(const ReportLifecycle&) = delete;

    bool finishing() const noexcept { return finish_job_.valid(); }
    bool stopping() const noexcept { return stop_job_.valid(); }
    bool stop_pending() const noexcept { return stop_pending_; }
    bool busy() const noexcept { return finishing() || stopping(); }
    void defer_stop() noexcept { stop_pending_ = true; }
    void cancel_restart() noexcept { restart_.cancel(); }

    template<class Work>
    void finish_async(Work&& work) {
        if (busy()) return;
        finish_job_ = std::async(std::launch::async, std::forward<Work>(work));
    }

    template<class Runtime, class Finish>
    ReportStopRequest stop(Runtime& runtime, Finish finish, bool retry_report = true) {
        if (stopping()) return ReportStopRequest::BUSY;
        if (finishing()) {
            runtime.post_intent({RuntimeIntentType::DISARM_OUTPUT, true});
            defer_stop();
            return ReportStopRequest::DEFERRED;
        }
        if (runtime.snapshot().state == RuntimeState::STOPPED &&
            ((!report_active_ && !archive_active_) || (!retry_report && finalization_.failed())))
            return ReportStopRequest::IDLE;
        runtime.post_intent({RuntimeIntentType::DISARM_OUTPUT, true});
        stop_job_ = std::async(std::launch::async, [this, &runtime, finish = std::move(finish), retry_report]() mutable {
            runtime.stop();
            // 自动收尾仍停止设备，但不在磁盘持续失败时按 UI 帧率重试。
            if (!retry_report && finalization_.failed()) return false;
            return finish();
        });
        return ReportStopRequest::STARTED;
    }

    template<class Stop>
    bool defer_runtime_restart(Stop&& stop) {
        return restart_.defer_if_active(busy() || report_active_ || archive_active_, std::forward<Stop>(stop));
    }
    template<class Stop>
    void request_application_restart(Stop&& stop) {
        restart_.request_application(std::forward<Stop>(stop));
    }

    template<class Stop, class Collect>
    void poll(bool stop_blocked, Stop&& stop, Collect&& collect) {
        collect_ready(finish_job_, ReportJobKind::FINISH, collect);
        collect_ready(stop_job_, ReportJobKind::STOP, collect);
        if (stop_pending_ && !stop_blocked && !finishing()) {
            stop_pending_ = false;
            stop(false);
        }
    }

    // 必须在本帧输入处理之后消费；外部忙碌不能吞掉待处理的取消。
    ReportRestartTarget take_restart(bool external_busy, bool cancelled = false) noexcept {
        return restart_.take_ready(busy() || stop_pending_ || external_busy, cancelled);
    }

private:
    template<class Collect>
    void collect_ready(std::future<bool>& job, ReportJobKind kind, Collect& collect) {
        if (!job.valid() || job.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
        const bool succeeded = job.get();
        restart_.finish_completed(succeeded);
        collect(succeeded, kind);
    }

    const bool& report_active_;
    const bool& archive_active_;
    const ReportFinalization& finalization_;
    ReportRestart restart_;
    bool stop_pending_ = false;
    std::future<bool> stop_job_;
    std::future<bool> finish_job_;
};

} // namespace app::detail
#endif
