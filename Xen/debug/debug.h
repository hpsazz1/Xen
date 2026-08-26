#ifndef DEBUG_H
#define DEBUG_H

#include "runtime/runtime.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

struct DebugReportConfig {
    std::string csv_path = "cache/runtime/latest.csv";
    std::string json_path = "cache/runtime/latest.json";
    std::string session_id;
    std::string model_path;
    std::string provider;
    std::string capture_backend;
    std::string mouse_backend;
    std::size_t max_samples = 10000;
    bool performance_probes_enabled = false;
};

struct DebugTimingSummary {
    std::size_t sample_count = 0;
    double mean_ms = 0.0;
    double p50_ms = 0.0;
    double p95_ms = 0.0;
    double p99_ms = 0.0;
    double max_ms = 0.0;
};

struct DebugQueueDepthSummary {
    std::size_t sample_count = 0;
    double mean_frames = 0.0;
    double p50_frames = 0.0;
    double p95_frames = 0.0;
    double p99_frames = 0.0;
    int max_frames = 0;
};

struct DebugCoveragePhaseSummary {
    std::uint64_t sample_count = 0;
    std::uint64_t first_sequence = 0;
    std::uint64_t last_sequence = 0;
    std::uint64_t runtime_overwritten_frames = 0;
    std::uint64_t sequence_gaps = 0;
    // formal 最后一个样本到 Runtime stop 封口之间没有后继 sequence 可用于
    // 交叉核对，单独保存这段 Runtime 覆盖尾差；其他阶段保持 0。
    std::uint64_t trailing_runtime_overwritten_frames = 0;
    bool counter_matches_sequence_gaps = false;
};

struct DebugCoverageSummary {
    bool available = false;
    std::uint64_t warmup_start_overwritten_frames = 0;
    std::uint64_t warmup_end_overwritten_frames = 0;
    std::uint64_t formal_end_overwritten_frames = 0;
    DebugCoveragePhaseSummary startup;
    DebugCoveragePhaseSummary warmup;
    DebugCoveragePhaseSummary formal;
};

struct DebugReportSummary {
    std::size_t sample_count = 0;
    std::size_t successful_samples = 0;
    std::size_t failed_samples = 0;
    std::uint64_t report_samples_dropped = 0;
    std::uint64_t runtime_samples_dropped = 0;
    DebugTimingSummary capture;
    DebugTimingSummary queue;
    DebugTimingSummary preprocess;
    DebugTimingSummary inference;
    DebugTimingSummary h2d;
    DebugTimingSummary d3d11_to_cuda;
    DebugTimingSummary d3d11_to_directml;
    DebugTimingSummary gpu_preprocess;
    DebugTimingSummary execution;
    DebugTimingSummary d2h;
    DebugTimingSummary postprocess;
    DebugTimingSummary aim;
    DebugTimingSummary mouse;
    DebugTimingSummary total;
    DebugTimingSummary source_to_capture;
    DebugTimingSummary source_to_control;
    DebugTimingSummary source_clock_uncertainty;
    DebugTimingSummary source_clock_round_trip;
    DebugTimingSummary source_clock_mapping_age;
    DebugTimingSummary capture_to_control;
    DebugTimingSummary control_to_mouse_backend_completion;
    DebugTimingSummary capture_to_mouse_backend_completion;
    DebugTimingSummary source_to_mouse_backend_completion;
    DebugTimingSummary control_to_mouse_protocol_ack;
    DebugTimingSummary capture_to_mouse_protocol_ack;
    DebugTimingSummary source_to_mouse_protocol_ack;
    DebugTimingSummary control_to_mouse_physical_effect;
    DebugTimingSummary capture_to_mouse_physical_effect;
    DebugTimingSummary source_to_mouse_physical_effect;
    DebugTimingSummary ndi_receive_call;
    DebugTimingSummary ndi_metadata;
    DebugTimingSummary ndi_geometry;
    DebugTimingSummary ndi_pool_acquire;
    DebugTimingSummary ndi_color_convert;
    DebugTimingSummary ndi_performance_query;
    DebugTimingSummary ndi_queue_query;
    DebugTimingSummary ndi_pool_publish;
    DebugTimingSummary runtime_capture_grab;
    DebugTimingSummary runtime_queue_publish;
    DebugTimingSummary runtime_handoff;
    DebugTimingSummary preview;
    DebugTimingSummary snapshot;
    DebugTimingSummary snapshot_lock_wait;
    DebugTimingSummary debug_ring;
    DebugTimingSummary profile_window;
    DebugTimingSummary service_tail;
    DebugTimingSummary pipeline_service;
    DebugTimingSummary pipeline_complete;
    DebugQueueDepthSummary ndi_video_queue_depth;
    DebugCoverageSummary coverage;
};

// Debug 报告、正式基准入口和测试共用同一成功语义。合法空检测仍是成功；
// 只有实际尝试发送 Mouse 命令时才要求后端处于 READY。
bool debug_sample_succeeded(
    const RuntimePipelineSample& sample) noexcept;

class DebugReport {
public:
    DebugReport();
    ~DebugReport() = default;

    DebugReport(const DebugReport&) = delete;
    DebugReport& operator=(const DebugReport&) = delete;

    bool start(const DebugReportConfig& config,
               std::string& error) noexcept;
    void ingest(std::span<const RuntimePipelineSample> samples) noexcept;
    bool finalize(const RuntimeSnapshot& final_snapshot,
                  std::string& error,
                  const DebugCoverageSummary* coverage = nullptr) noexcept;

    bool active() const noexcept;
    const DebugReportSummary& summary() const noexcept;
    std::string last_error() const;

private:
    DebugReportConfig config_;
    std::vector<RuntimePipelineSample> samples_;
    DebugReportSummary summary_;
    std::uint64_t report_samples_dropped_ = 0;
    bool active_ = false;
    std::string last_error_;
};

#endif // DEBUG_H
