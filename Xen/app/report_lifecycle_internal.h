#ifndef APP_REPORT_LIFECYCLE_INTERNAL_H
#define APP_REPORT_LIFECYCLE_INTERNAL_H

#include "runtime/runtime.h"
#include <algorithm>
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

} // namespace app::detail
#endif
