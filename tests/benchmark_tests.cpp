#include "benchmark/benchmark.h"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << message << '\n';
}

BenchmarkParseStatus parse(
        const std::vector<std::wstring_view>& arguments,
        BenchmarkOptions& options,
        std::string& error) {
    return parse_benchmark_options(arguments, options, error);
}

void test_main_machine_defaults() {
    const std::vector<std::wstring_view> arguments{
        L"--model", L"model.onnx",
        L"--backend", L"tensorrt",
        L"--report-prefix", L"reports/runtime",
        L"--provider-profile", L"reports/runtime.provider-profile.json"};
    BenchmarkOptions options;
    std::string error;
    expect(parse(arguments, options, error) == BenchmarkParseStatus::READY,
           "完整必选参数应解析成功: " + error);
    const auto& geometry = options.expected_geometry;
    expect(geometry.source_width == 2560 &&
               geometry.source_height == 1440 &&
               geometry.encoded_width == 2560 &&
               geometry.encoded_height == 1440 &&
               geometry.roi_x == 1120 && geometry.roi_y == 560 &&
               geometry.roi_width == 320 && geometry.roi_height == 320 &&
               geometry.source_pixels_per_pixel_x == 1.0 &&
               geometry.source_pixels_per_pixel_y == 1.0,
           "默认几何必须是 2560x1440 主机中心 320x320 ROI");
    expect(options.warmup_samples == 100 &&
               options.minimum_samples == 10000 &&
               options.minimum_seconds == 300 &&
               options.maximum_seconds == 600 && options.enable_fp16 &&
               options.enable_cuda_graph &&
               options.enable_gpu_preprocess &&
               options.provider_profile_path ==
                   "reports/runtime.provider-profile.json",
            "正式门槛和 TensorRT 优化默认值必须稳定");
}

void test_network_encoded_override() {
    const std::vector<std::wstring_view> arguments{
        L"--model", L"model.onnx",
        L"--backend", L"directml",
        L"--report-prefix", L"reports/network",
        L"--output-format", L"channel_first",
        L"--expect-source", L"2560x1440",
        L"--expect-encoded", L"320x320",
        L"--expect-roi", L"1120,560,320,320",
        L"--expect-scale", L"1,1",
        L"--warmup-samples", L"0",
        L"--minimum-samples", L"1",
        L"--minimum-seconds", L"0",
        L"--maximum-seconds", L"10",
        L"--fp16", L"off",
        L"--cuda-graph", L"off",
        L"--gpu-preprocess", L"off"};
    BenchmarkOptions options;
    std::string error;
    expect(parse(arguments, options, error) == BenchmarkParseStatus::READY,
           "网络几何和短冒烟门槛应解析成功: " + error);
    expect(options.backend == BackendType::DIRECTML &&
               options.output_format == OutputFormat::CHANNEL_FIRST &&
               options.expected_geometry.encoded_width == 320 &&
               options.expected_geometry.encoded_height == 320 &&
               options.warmup_samples == 0 &&
               options.minimum_samples == 1 &&
               options.minimum_seconds == 0 &&
               options.maximum_seconds == 10 &&
               !options.enable_fp16 && !options.enable_cuda_graph &&
               !options.enable_gpu_preprocess,
           "辅机运行必须只覆盖编码尺寸，不改变主机 FOV/ROI 契约");
}

void test_invalid_options() {
    BenchmarkOptions options;
    std::string error;
    expect(parse({L"--model", L"model.onnx",
                  L"--report-prefix", L"report"}, options, error) ==
               BenchmarkParseStatus::INVALID &&
               error.find("--backend") != std::string::npos,
           "正式基准必须显式选择后端");
    expect(parse({L"--model", L"model.onnx",
                  L"--backend", L"cuda",
                  L"--report-prefix", L"report",
                  L"--provider-profile", L"report.provider.json",
                  L"--minimum-seconds", L"20",
                  L"--maximum-seconds", L"10"}, options, error) ==
               BenchmarkParseStatus::INVALID,
           "最大时限短于最小时长必须拒绝");
    expect(parse({L"--model", L"model.onnx",
                  L"--backend", L"cuda",
                  L"--report-prefix", L"report",
                  L"--provider-profile", L"report.provider.json",
                  L"--warmup-samples", L"100001"}, options, error) ==
               BenchmarkParseStatus::INVALID,
           "warmup 超过固定容量上限必须拒绝");
    expect(parse({L"--model", L"model.onnx",
                  L"--backend", L"cuda",
                  L"--report-prefix", L"report",
                  L"--provider-profile", L"report.provider.json",
                  L"--maximum-seconds", L"86401"}, options, error) ==
               BenchmarkParseStatus::INVALID,
           "最大运行时长超过一天必须拒绝");
    expect(parse({L"--model", L"model.onnx",
                  L"--backend", L"cuda",
                  L"--report-prefix", L"report",
                  L"--provider-profile", L"report.provider.json",
                  L"--output-format", L"unknown",
                  L"--maximum-seconds", L"10"}, options, error) ==
               BenchmarkParseStatus::INVALID,
           "未知模型输出契约必须拒绝");
    expect(parse({L"--model", L"model.onnx",
                  L"--backend", L"cuda",
                  L"--report-prefix", L"report",
                  L"--provider-profile", L"report.provider.json",
                  L"--expect-roi", L"2500,1400,320,320"}, options,
                 error) == BenchmarkParseStatus::INVALID,
           "越过主机 FOV 的 ROI 必须拒绝");
    expect(parse({L"--model", L"model.onnx",
                  L"--backend", L"cuda",
                  L"--report-prefix", L"report"}, options, error) ==
               BenchmarkParseStatus::INVALID &&
               error.find("--provider-profile") != std::string::npos,
           "TensorRT/CUDA 缺少节点级 Provider profile 必须拒绝");
    expect(parse({L"--model", L"model.onnx",
                  L"--backend", L"directml",
                  L"--report-prefix", L"report",
                  L"--provider-profile", L"unexpected.json"}, options,
                 error) == BenchmarkParseStatus::INVALID,
           "严格 DirectML 不应接受多余的 GPU Provider profile 参数");
    expect(parse({L"--help"}, options, error) ==
               BenchmarkParseStatus::HELP,
           "--help 不应要求其他必选参数");
}

void test_provider_mapping() {
    expect(std::string(expected_provider_name(BackendType::TENSORRT)) ==
               "TensorrtExecutionProvider" &&
               std::string(expected_provider_name(BackendType::CUDA)) ==
               "CUDAExecutionProvider" &&
               std::string(expected_provider_name(BackendType::DIRECTML)) ==
               "DmlExecutionProvider" &&
               std::string(expected_provider_name(BackendType::CPU)) ==
               "CPUExecutionProvider",
           "请求后端必须映射到 ORT 实际 Provider 名称");
}

void test_per_frame_geometry_validation() {
    BenchmarkExpectedGeometry expected;
    RuntimeFrameGeometry actual;
    actual.source_width = 2560;
    actual.source_height = 1440;
    actual.encoded_width = 2560;
    actual.encoded_height = 1440;
    actual.roi_x = 1120.0;
    actual.roi_y = 560.0;
    actual.roi_width = 320;
    actual.roi_height = 320;
    actual.source_pixels_per_pixel_x = 1.0;
    actual.source_pixels_per_pixel_y = 1.0;
    std::string error;
    expect(validate_benchmark_geometry(actual, expected, error),
           "主机默认逐帧几何应通过: " + error);

    actual.encoded_width = 1920;
    actual.encoded_height = 1080;
    expect(!validate_benchmark_geometry(actual, expected, error) &&
               error.find("actual=source 2560x1440") != std::string::npos,
           "辅机显示分辨率不得被误写成编码或主机 FOV 几何");

    actual.encoded_width = 2560;
    actual.encoded_height = 1440;
    actual.source_pixels_per_pixel_x = 4.0;
    expect(!validate_benchmark_geometry(actual, expected, error),
           "任一帧比例变化都必须被正式基准拒绝");
}

} // namespace

int main() {
    test_main_machine_defaults();
    test_network_encoded_override();
    test_invalid_options();
    test_provider_mapping();
    test_per_frame_geometry_validation();
    if (failures != 0) {
        std::cerr << "Benchmark 测试失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "Benchmark 测试全部通过。\n";
    return 0;
}
