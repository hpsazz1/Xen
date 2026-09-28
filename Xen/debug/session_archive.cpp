#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#ifdef ERROR
#undef ERROR
#endif
#include "debug/session_archive.h"
#include "debug/session_archive_internal.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <algorithm>

namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
std::int64_t steady_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
std::int64_t system_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
void publish(const std::filesystem::path& path, const Json& data) {
    const auto temp = std::filesystem::path(path.string() + ".tmp");
    std::ofstream output(temp, std::ios::binary | std::ios::trunc);
    output << data.dump(2); output.flush();
    if (!output) throw std::runtime_error("归档文件写入失败: " + path.string());
    output.close();
    if (!output) throw std::runtime_error("归档文件关闭失败: " + path.string());
    // 唯一段文件从不替换；状态文件用 Windows 原子替换。
    if (std::filesystem::exists(path)) {
        if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("归档状态发布失败");
    } else std::filesystem::rename(temp, path);
}
}

struct SessionArchive::Impl {
    struct Batch {
        std::vector<RuntimePipelineSample> samples;
        RuntimeSnapshot snapshot;
        bool marker = false;
        std::string label;
        std::int64_t steady = 0, system = 0;
    };
    SessionArchiveConfig config;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<Batch> queue;
    std::size_t queued_samples = 0;
    std::thread worker;
    SessionArchiveStatus state;
    std::atomic<std::uint64_t> rejected{0};
    std::atomic<std::uint64_t> rejected_batches{0};
    bool stopping = false;
    std::int64_t started = 0, ended = 0, started_system = 0, ended_system = 0;
    std::uint64_t trigger_sequence = 0, segment_number = 0;
    std::vector<RuntimePipelineSample> pending;
    RuntimeSnapshot latest;
    std::vector<TriggerExecutionEvent> events;
    std::function<void()> before_flush;
    Clock::time_point segment_started;

    void error(const std::string& message) {
        std::lock_guard lock(mutex); state.last_error = message;
    }
    SessionArchiveStatus status() const {
        std::lock_guard lock(mutex);
        auto result = state; result.dropped_samples += rejected.load();
        result.dropped_batches += rejected_batches.load();
        result.queued_batches = queue.size(); return result;
    }
    void manifest(bool closed) {
        const auto s = status();
        publish(std::filesystem::path(config.directory) / "manifest.json", {
            {"schema",1}, {"session_id",config.report_config.session_id}, {"closed",closed},
            {"started_steady_ns",std::to_string(started)}, {"ended_steady_ns",std::to_string(ended)},
            {"started_system_unix_ns",std::to_string(started_system)}, {"ended_system_unix_ns",std::to_string(ended_system)},
            {"queue_capacity_batches",config.queue_capacity}, {"max_batch_samples",10000},
            {"max_queued_samples",12000},
            {"accepted_samples",s.accepted_samples}, {"written_samples",s.written_samples},
            {"dropped_samples",s.dropped_samples}, {"written_segments",s.written_segments},
            {"dropped_batches",s.dropped_batches},
            {"coalesced_batches",s.coalesced_batches}, {"queue_capacity_rejections",s.queue_capacity_rejections},
            {"sample_capacity_rejections",s.sample_capacity_rejections},
            {"trigger_events_dropped",s.trigger_events_dropped}, {"marker_count",s.marker_count},
            {"runtime_samples_dropped",latest.debug_samples_dropped},
            {"last_error",s.last_error}, {"complete",closed && !s.dropped_batches && !s.dropped_samples && !s.trigger_events_dropped && !latest.debug_samples_dropped && s.last_error.empty()},
            {"segments","segment-*.meta.json"}, {"markers","marker-*.json"},
            {"clock_domain","local_steady_clock_nanoseconds"}, {"physical_effect_verified",false}});
    }
    void flush() {
        if (pending.empty() && events.empty()) return;
        std::function<void()> callback;
        { std::lock_guard lock(mutex); callback=before_flush; }
        if (callback) callback();
        const auto number = ++segment_number;
        const auto base = std::filesystem::path(config.directory) / ("segment-" + std::to_string(number));
        auto report_config = config.report_config;
        report_config.csv_path = base.string() + ".csv";
        report_config.json_path = base.string() + ".json";
        report_config.max_samples = std::max<std::size_t>(1, pending.size());
        report_config.include_json_samples = false;
        report_config.enable_lock_marker = false;
        latest.trigger_execution_log.events = events;
        latest.trigger_execution_log.first_sequence = events.empty() ? 0 : events.front().sequence;
        latest.trigger_execution_log.last_sequence = events.empty() ? 0 : events.back().sequence;
        latest.trigger_execution_log.dropped_count = status().trigger_events_dropped;
        try {
            DebugReport report; std::string message;
            if (!report.start(report_config, message)) throw std::runtime_error(message);
            report.ingest(pending);
            if (!report.finalize(latest, message)) throw std::runtime_error(message);
            publish(base.string() + ".meta.json", {
                {"schema",1}, {"segment",number}, {"csv",base.filename().string()+".csv"},
                {"json",base.filename().string()+".json"}, {"sample_count",pending.size()},
                {"first_sequence", pending.empty()?0:pending.front().sequence},
                {"last_sequence", pending.empty()?0:pending.back().sequence},
                {"first_control_steady_ns",std::to_string(pending.empty()?0:pending.front().frame_timing.control_steady_ns)},
                {"last_control_steady_ns",std::to_string(pending.empty()?0:pending.back().frame_timing.control_steady_ns)},
                {"first_trigger_steady_ns",std::to_string(events.empty()?0:std::chrono::duration_cast<std::chrono::nanoseconds>(events.front().observed_at.time_since_epoch()).count())},
                {"last_trigger_steady_ns",std::to_string(events.empty()?0:std::chrono::duration_cast<std::chrono::nanoseconds>(events.back().observed_at.time_since_epoch()).count())},
                {"trigger_event_count",events.size()}, {"published",true}});
            std::lock_guard lock(mutex); ++state.written_segments; state.written_samples += pending.size();
        } catch (const std::exception& ex) {
            std::lock_guard lock(mutex); state.last_error = ex.what();
            state.dropped_samples += pending.size(); state.trigger_events_dropped += events.size();
        }
        pending.clear(); events.clear(); segment_started = Clock::now();
        manifest(false);
    }
    void marker(const Batch& batch) {
        std::uint64_t number;
        { std::lock_guard lock(mutex); number = ++state.marker_count; }
        constexpr std::int64_t window = 30000000000LL;
        publish(std::filesystem::path(config.directory) / ("marker-" + std::to_string(number) + ".json"), {
            {"schema",1}, {"label",batch.label}, {"steady_ns",std::to_string(batch.steady)},
            {"system_unix_ns",std::to_string(batch.system)}, {"window_start_steady_ns",std::to_string(batch.steady-window)},
            {"window_end_steady_ns",std::to_string(batch.steady+window)},
            {"pre_truncated",batch.steady-window < started}, {"post_truncated",nullptr},
            {"segment_reference","segment-*.meta.json; select overlapping control/trigger steady timestamps"},
            {"coverage_manifest","manifest.json"}, {"closed",false}});
    }
    void close_markers() {
        for (const auto& entry : std::filesystem::directory_iterator(config.directory)) {
            const auto name = entry.path().filename().string();
            if (!name.starts_with("marker-") || entry.path().extension() != ".json") continue;
            std::ifstream input(entry.path()); Json value; input >> value; input.close();
            value["post_truncated"] = ended < std::stoll(value["window_end_steady_ns"].get<std::string>());
            value["closed"] = true; publish(entry.path(), value);
        }
    }
    void run() noexcept {
        segment_started = Clock::now();
        try {
            manifest(false);
            for (;;) {
                Batch batch;
                {
                    std::unique_lock lock(mutex);
                    wake.wait_for(lock, config.segment_interval, [&]{ return stopping || !queue.empty(); });
                    if (queue.empty()) {
                        if (stopping) break;
                        lock.unlock(); flush(); continue;
                    }
                    batch = std::move(queue.front()); queue.pop_front(); queued_samples -= batch.samples.size();
                }
                if (batch.marker) { marker(batch); continue; }
                ended = batch.steady; ended_system = batch.system;
                for (const auto& event : batch.snapshot.trigger_execution_log.events) {
                    if (event.sequence <= trigger_sequence) continue;
                    if (event.sequence > trigger_sequence + 1) {
                        std::lock_guard lock(mutex); state.trigger_events_dropped += event.sequence-trigger_sequence-1;
                    }
                    trigger_sequence = event.sequence; events.push_back(event);
                }
                batch.snapshot.trigger_execution_log.events.clear();
                latest = std::move(batch.snapshot);
                for (const auto& sample : batch.samples) {
                    pending.push_back(sample);
                    if (pending.size() >= config.segment_samples) flush();
                }
                if (events.size() >= 2048 || Clock::now()-segment_started >= config.segment_interval) flush();
            }
            flush(); close_markers(); manifest(true);
        } catch (const std::exception& ex) { error(ex.what()); }
          catch (...) { error("归档后台未知异常"); }
        {
            std::lock_guard lock(mutex);
            state.dropped_samples += pending.size();
            state.trigger_events_dropped += events.size();
            for (const auto& batch : queue) {
                state.dropped_samples += batch.samples.size();
                state.trigger_events_dropped += batch.snapshot.trigger_execution_log.events.size();
                ++state.dropped_batches;
            }
            pending.clear(); events.clear(); queue.clear(); queued_samples=0; state.active = false;
            state.dropped_samples = std::max(state.dropped_samples,
                state.accepted_samples-state.written_samples);
        }
        try { manifest(true); } catch (...) { /* 保留旧清单 closed=false，不能伪造完整。 */ }
    }
};

SessionArchive::SessionArchive() : impl_(std::make_unique<Impl>()) {}
SessionArchive::~SessionArchive() { stop(); }
bool SessionArchive::start(const SessionArchiveConfig& config, std::string& error) noexcept {
    stop();
    try {
        if (config.directory.empty() || !config.queue_capacity || config.queue_capacity>128 || !config.segment_samples ||
            config.segment_samples > 100000 || config.segment_interval.count() <= 0)
            throw std::runtime_error("归档配置无效");
        const std::filesystem::path directory(config.directory);
        if (!directory.parent_path().empty()) std::filesystem::create_directories(directory.parent_path());
        if (!std::filesystem::create_directory(directory)) throw std::runtime_error("归档目录已存在，拒绝覆盖旧会话");
        impl_ = std::make_unique<Impl>(); impl_->config = config;
        impl_->trigger_sequence = config.trigger_sequence_baseline;
        impl_->state.directory = config.directory; impl_->state.active = true; impl_->started = steady_ns();
        impl_->started_system = system_ns();
        impl_->worker = std::thread([p=impl_.get()]{ p->run(); }); error.clear(); return true;
    } catch (const std::exception& ex) {
        try { error = ex.what(); impl_->error(error); } catch (...) {}
        std::lock_guard lock(impl_->mutex); impl_->state.active=false; return false;
    } catch (...) {
        std::lock_guard lock(impl_->mutex); impl_->state.active=false; return false;
    }
}
bool SessionArchive::submit(std::span<const RuntimePipelineSample> samples, const RuntimeSnapshot& snapshot) noexcept {
    try {
        if (samples.size()>10000 || snapshot.trigger_execution_log.events.size()>2048) {
            impl_->rejected += samples.size(); ++impl_->rejected_batches; return false;
        }
        Impl::Batch batch; batch.steady=steady_ns(); batch.system=system_ns();
        batch.samples.assign(samples.begin(), samples.end()); batch.snapshot = snapshot;
        // App 冷路径只等待内存移交；消费者在该锁外序列化、写盘和运行测试回调。
        std::unique_lock lock(impl_->mutex);
        if (!impl_->state.active || impl_->stopping || impl_->queued_samples+samples.size()>12000) {
            if (impl_->queued_samples+samples.size()>12000) ++impl_->state.sample_capacity_rejections;
            impl_->rejected += samples.size(); ++impl_->rejected_batches; return false;
        }
        const std::size_t merge_limit=std::min<std::size_t>(1200,impl_->config.segment_samples);
        const bool merge=!impl_->queue.empty() && !impl_->queue.back().marker &&
            impl_->queue.back().samples.size()+samples.size()<=merge_limit &&
            impl_->queue.back().snapshot.trigger_execution_log.events.size()+snapshot.trigger_execution_log.events.size()<=2048;
        if (merge) {
            auto& tail=impl_->queue.back();
            // 先完成所有可能分配的操作，再改变尾批；失败不能部分写入后又计作丢弃。
            auto combined_events=tail.snapshot.trigger_execution_log.events;
            combined_events.insert(combined_events.end(),snapshot.trigger_execution_log.events.begin(),snapshot.trigger_execution_log.events.end());
            const auto needed=tail.samples.size()+samples.size();
            if (needed>tail.samples.capacity()) tail.samples.reserve(std::max(needed,
                std::min(merge_limit,tail.samples.capacity()*2)));
            tail.samples.insert(tail.samples.end(),batch.samples.begin(),batch.samples.end());
            batch.snapshot.trigger_execution_log.events=std::move(combined_events);
            tail.snapshot=std::move(batch.snapshot); tail.steady=batch.steady; tail.system=batch.system;
            ++impl_->state.coalesced_batches;
        } else {
            if (impl_->queue.size()>=impl_->config.queue_capacity) {
                ++impl_->state.queue_capacity_rejections;
                impl_->rejected += samples.size(); ++impl_->rejected_batches; return false;
            }
            impl_->queue.push_back(std::move(batch));
        }
        impl_->state.accepted_samples += samples.size();
        impl_->queued_samples += samples.size();
        lock.unlock(); impl_->wake.notify_one(); return true;
    } catch (...) { impl_->rejected += samples.size(); ++impl_->rejected_batches; return false; }
}
bool SessionArchive::mark(const std::string& label) noexcept {
    try {
        Impl::Batch batch; batch.marker=true; batch.label=label.substr(0,256); batch.steady=steady_ns(); batch.system=system_ns();
        std::unique_lock lock(impl_->mutex, std::try_to_lock);
        if (!lock || !impl_->state.active || impl_->stopping || impl_->queue.size()>=impl_->config.queue_capacity) return false;
        impl_->queue.push_back(std::move(batch)); lock.unlock(); impl_->wake.notify_one(); return true;
    } catch (...) { return false; }
}
void SessionArchive::stop() noexcept {
    { std::lock_guard lock(impl_->mutex); impl_->stopping=true; }
    impl_->wake.notify_one(); if (impl_->worker.joinable()) impl_->worker.join();
}
void SessionArchive::note_gap(const std::string& reason) noexcept {
    try { impl_->error(reason.empty() ? "上游遥测缺口" : reason); } catch (...) {}
}
SessionArchiveStatus SessionArchive::status() const { return impl_->status(); }
void xen::debug::detail::SessionArchiveTestAccess::before_flush(
        SessionArchive& archive, std::function<void()> callback) {
    std::lock_guard lock(archive.impl_->mutex);
    archive.impl_->before_flush=std::move(callback);
}
