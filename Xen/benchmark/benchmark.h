#ifndef BENCHMARK_H
#define BENCHMARK_H

#include "detector/detector.h"
#include "runtime/runtime.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

struct BenchmarkExpectedGeometry {
    int source_width = 2560;
    int source_height = 1440;
    int encoded_width = 2560;
    int encoded_height = 1440;
    int roi_x = 1120;
    int roi_y = 560;
    int roi_width = 320;
    int roi_height = 320;
    double source_pixels_per_pixel_x = 1.0;
    double source_pixels_per_pixel_y = 1.0;
};

struct BenchmarkOptions {
    std::string model_path;
    std::string config_path;
    std::string report_prefix;
    // TensorRT/CUDA 正式基准必须先由独立诊断 Session 生成该 ORT profile。
    // DirectML 已禁用 CPU 回退，CPU 后端不需要该证据文件。
    std::string provider_profile_path;
    BackendType backend = BackendType::TENSORRT;
    OutputFormat output_format = OutputFormat::AUTO;
    std::uint64_t warmup_samples = 100;
    std::uint64_t minimum_samples = 10000;
    std::uint64_t minimum_seconds = 300;
    std::uint64_t maximum_seconds = 600;
    bool enable_fp16 = true;
    bool enable_cuda_graph = true;
    bool enable_gpu_preprocess = true;
    bool backend_explicit = false;
    BenchmarkExpectedGeometry expected_geometry;
};

enum class BenchmarkParseStatus {
    READY,
    HELP,
    INVALID,
};

BenchmarkParseStatus parse_benchmark_options(
    std::span<const std::wstring_view> arguments,
    BenchmarkOptions& options,
    std::string& error) noexcept;

bool validate_benchmark_options(
    const BenchmarkOptions& options,
    std::string& error) noexcept;

bool validate_benchmark_geometry(
    const RuntimeFrameGeometry& actual,
    const BenchmarkExpectedGeometry& expected,
    std::string& error) noexcept;

const char* expected_provider_name(BackendType backend) noexcept;
std::string benchmark_usage();
void request_benchmark_stop() noexcept;

// 仅在全部 warmup、时长、样本、状态、Provider 和逐帧几何门槛通过后
// 发布 <report_prefix>.csv/.json；失败或人工中止不会发布半份报告。
bool run_runtime_benchmark(
    const BenchmarkOptions& options,
    std::string& error) noexcept;

#endif // BENCHMARK_H
