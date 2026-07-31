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
};

struct DebugTimingSummary {
    std::size_t sample_count = 0;
    double mean_ms = 0.0;
    double p50_ms = 0.0;
    double p95_ms = 0.0;
    double p99_ms = 0.0;
    double max_ms = 0.0;
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
    DebugTimingSummary gpu_preprocess;
    DebugTimingSummary execution;
    DebugTimingSummary d2h;
    DebugTimingSummary postprocess;
    DebugTimingSummary aim;
    DebugTimingSummary mouse;
    DebugTimingSummary total;
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
                  std::string& error) noexcept;

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
