#ifndef MOUSE_BENCHMARK_H
#define MOUSE_BENCHMARK_H

#include "mouse/mouse.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

enum class MouseBenchmarkParseStatus {
    READY,
    HELP,
    INVALID,
};

enum class MouseBenchmarkCompletionSemantic {
    UNSPECIFIED,
    WINDOWS_INPUT_STREAM_INSERTION,
    KMBOX_MATCHED_UDP_PROTOCOL_ACK,
    MAKCU_MATCHED_SERIAL_DEVICE_STATUS_ACK,
};

enum class MouseBenchmarkPeerTestBoundary {
    UNSPECIFIED,
    LOCAL_OS_API,
    CONFIGURED_EXTERNAL_DEVICE_PEER,
    LOOPBACK_UDP_FAKE,
    IN_MEMORY_SERIAL_FAKE,
};

const char* mouse_benchmark_completion_semantic_name(
    MouseBenchmarkCompletionSemantic semantic) noexcept;
const char* mouse_benchmark_peer_test_boundary_name(
    MouseBenchmarkPeerTestBoundary boundary) noexcept;

struct MouseBenchmarkOptions {
    MouseConfig mouse;
    std::string report_path;
    // 正式脚本传入同一 run UUID；直接入口未传时由 runner 生成并写回 result。
    std::string run_uuid;
    std::uint64_t warmup_pairs = 100;
    std::uint64_t sample_pairs = 10000;
    int dx_counts = 1;
    int dy_counts = 0;
    // 由公有入口或 fake seam 显式声明，writer 不得从 endpoint 反推测试边界。
    MouseBenchmarkPeerTestBoundary peer_test_boundary =
        MouseBenchmarkPeerTestBoundary::UNSPECIFIED;
    bool backend_explicit = false;
    bool allow_physical_output = false;
    bool physical_output_confirmed = false;
};

struct MouseBenchmarkTimingSummary {
    double mean_ms = 0.0;
    double p50_ms = 0.0;
    double p95_ms = 0.0;
    double p99_ms = 0.0;
    double max_ms = 0.0;
};

struct MouseBenchmarkSample {
    std::uint64_t sequence = 0;
    std::uint64_t pair_index = 0;
    int direction = 0;
    int dx_counts = 0;
    int dy_counts = 0;
    double latency_ms = 0.0;
};

struct MouseBenchmarkResult {
    bool complete = false;
    std::string run_uuid;
    MouseBenchmarkCompletionSemantic completion_semantic =
        MouseBenchmarkCompletionSemantic::UNSPECIFIED;
    bool protocol_ack_observed = false;
    bool physical_effect_observed = false;
    double open_ms = 0.0;
    double first_command_ms = 0.0;
    double first_compensation_ms = 0.0;
    double warmup_elapsed_ms = 0.0;
    double formal_elapsed_ms = 0.0;
    double total_elapsed_ms = 0.0;
    // successful_commands 包含首条、补偿、预热和正式命令；正式分位数只使用
    // formal_successful_commands 对应的 samples，避免冷启动和预热污染结果。
    std::uint64_t successful_commands = 0;
    std::uint64_t formal_successful_commands = 0;
    std::uint64_t failed_commands = 0;
    MouseStatus final_status = MouseStatus::CLOSED;
    MouseBenchmarkTimingSummary command_latency;
    std::vector<MouseBenchmarkSample> samples;
};

MouseBenchmarkParseStatus parse_mouse_benchmark_options(
    std::span<const std::wstring_view> arguments,
    MouseBenchmarkOptions& options,
    std::string& error) noexcept;

bool validate_mouse_benchmark_options(
    const MouseBenchmarkOptions& options,
    std::string& error) noexcept;

MouseBenchmarkTimingSummary summarize_mouse_benchmark_timings(
    std::span<const double> values) noexcept;

// 仅完整成功且零失败的样本可以发布；目标已存在时拒绝覆盖。
bool write_mouse_benchmark_report(
    const MouseBenchmarkOptions& options,
    const MouseBenchmarkResult& result,
    std::string& error) noexcept;

bool run_mouse_benchmark(
    const MouseBenchmarkOptions& options,
    MouseBenchmarkResult& result,
    std::string& error) noexcept;

std::string mouse_benchmark_usage();
void request_mouse_benchmark_stop() noexcept;

#endif // MOUSE_BENCHMARK_H
