#ifndef SESSION_ARCHIVE_H
#define SESSION_ARCHIVE_H
#include "debug/debug.h"

struct SessionArchiveConfig {
    std::string directory;
    DebugReportConfig report_config;
    std::size_t segment_samples = 1200;
    std::chrono::milliseconds segment_interval{5000};
    // 1..128 批；每批最多 10000 帧及 2048 个 Trigger 事件，整个队列最多 12000 帧。
    std::size_t queue_capacity = 32;
    std::uint64_t trigger_sequence_baseline = 0;
};

struct SessionArchiveStatus {
    bool active = false;
    std::uint64_t accepted_samples = 0, written_samples = 0, dropped_samples = 0;
    std::uint64_t written_segments = 0, trigger_events_dropped = 0, marker_count = 0;
    std::uint64_t dropped_batches = 0;
    std::size_t queued_batches = 0;
    std::string directory, last_error;
};

// App 冷路径唯一生产者；仅后台线程访问磁盘。stop 排空并封尾，析构同样封尾。
class SessionArchive {
public:
    SessionArchive();
    ~SessionArchive();
    SessionArchive(const SessionArchive&) = delete;
    SessionArchive& operator=(const SessionArchive&) = delete;
    bool start(const SessionArchiveConfig&, std::string& error) noexcept;
    bool submit(std::span<const RuntimePipelineSample>, const RuntimeSnapshot&) noexcept;
    // 同时保存接收时刻的 steady/system 时钟；窗口为前后各 30 秒。
    bool mark(const std::string& label = "manual") noexcept;
    // 上游读取失败等未能交付样本的缺口不能伪装成完整会话。
    void note_gap(const std::string& reason) noexcept;
    void stop() noexcept;
    SessionArchiveStatus status() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
#endif
