#ifndef APP_REPORT_LIFECYCLE_INTERNAL_H
#define APP_REPORT_LIFECYCLE_INTERNAL_H

#include "runtime/runtime.h"
#include <algorithm>
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

template<class Source, class Finish>
void finish_reports_after_archive(Source& source, RuntimeSnapshot frozen, Finish&& finish) {
    source.set_recoil_archive({});
    // 执行事件保留关闭水位；只补入等待封尾后才确定的写盘结果。
    frozen.recoil_archive = source.snapshot().recoil_archive;
    std::forward<Finish>(finish)(std::move(frozen));
}

// 仅 App 主线程访问；后台任务经 future.get() 回收后才消费重启请求。
class ReportRestart {
public:
    template<class Stop>
    bool defer_if_active(bool active, Stop&& stop) {
        if (!active) return false;
        std::forward<Stop>(stop)();
        pending_ = true;
        return true;
    }

    bool take_ready(bool busy, bool cancelled = false) noexcept {
        if (cancelled) cancel();
        return !busy && std::exchange(pending_, false);
    }
    void cancel() noexcept { pending_ = false; }
private:
    bool pending_ = false;
};

} // namespace app::detail
#endif
