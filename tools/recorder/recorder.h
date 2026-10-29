// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef XEN_RECORDER_RECORDER_H
#define XEN_RECORDER_RECORDER_H

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace xen::recorder {

struct Config {
    std::filesystem::path obs_root;
    std::filesystem::path directory;
    // 零尺寸取主显示器原生像素；合成测试允许显式指定小尺寸。
    std::uint32_t width = 0, height = 0, fps = 240, bitrate_kbps = 60000;
    // audio控制系统回环；synthetic始终不打开设备。muxer所需静音AAC轨仍保留。
    bool audio = false, synthetic = false;
    // 只在调用方明确选择时允许软件编码；Snapshot始终公开实际编码器。
    bool allow_software_encoder = false;
};

struct EncodedMarker {
    std::string id;
    std::int64_t render_begin_steady_ns = 0, render_end_steady_ns = 0;
    std::int64_t render_begin_system_unix_ns = 0, render_end_system_unix_ns = 0;
    std::uint64_t render_frame_time_ns = 0;
    std::int64_t video_pts = 0;
    std::int32_t timebase_num = 0, timebase_den = 0;
    bool matched = false;
    // 未渲染、编码帧缺失或有限队列缺口不能用请求时间冒充PTS。
    std::string error;
};

struct Snapshot {
    bool running = false, stopping = false, stop_completed = true, software_fallback = false;
    std::uint32_t width = 0, height = 0, fps = 0;
    std::uint64_t total_frames = 0, encoded_packets = 0, bytes_written = 0;
    std::uint64_t output_dropped_frames = 0, render_total_frames = 0, render_lagged_frames = 0;
    std::uint64_t encoder_total_frames = 0, encoder_skipped_frames = 0;
    double active_render_fps = 0.0;
    std::uint64_t average_render_ns = 0;
    std::uint64_t markers_requested = 0, markers_matched = 0, markers_dropped = 0, markers_pending = 0;
    std::uint64_t render_queue_gaps = 0, packet_queue_gaps = 0;
    std::string encoder_id, last_error;
    std::filesystem::path video_path;
};

// 独立进程中唯一libobs实例；生命周期与查询走冷路径，回调只写有限内存队列。
class Recorder final {
public:
    Recorder();
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;
    bool start(const Config& config, std::string& error) noexcept;
    void stop() noexcept;
    bool mark(const std::string& id) noexcept;
    Snapshot snapshot() const;
    // 调用方录制期间定期消费（建议100ms内）；停止后再消费一次，收回全部未匹配编号。
    std::vector<EncodedMarker> drain_markers();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace xen::recorder
#endif
