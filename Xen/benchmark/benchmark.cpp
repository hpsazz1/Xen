#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>

#ifdef ERROR
#undef ERROR
#endif

#include "benchmark/benchmark.h"

#include "capture/capture.h"
#include "config/config.h"
#include "crash/crash.h"
#include "debug/debug.h"
#include "log/log.h"
#include "mouse/mouse.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t kMaximumReportSamples = 100000;
constexpr std::uint64_t kMaximumBenchmarkSeconds = 86400;
constexpr auto kPollInterval = std::chrono::milliseconds(2);

std::atomic<bool> benchmark_stop_requested{false};

void set_error(std::string& output, const std::string& value) noexcept {
    try {
        output = value;
    } catch (...) {
    }
}

bool wide_to_utf8(std::wstring_view input, std::string& output) noexcept {
    try {
        if (input.empty()) {
            output.clear();
            return true;
        }
        if (input.size() > static_cast<std::size_t>(
                std::numeric_limits<int>::max())) {
            return false;
        }
        const int required = WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
            static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
        if (required <= 0) return false;
        output.resize(static_cast<std::size_t>(required));
        return WideCharToMultiByte(
                   CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
                   static_cast<int>(input.size()), output.data(), required,
                   nullptr, nullptr) == required;
    } catch (...) {
        return false;
    }
}

template<typename Number>
bool parse_number(std::wstring_view input, Number& output) noexcept {
    std::string utf8;
    if (!wide_to_utf8(input, utf8) || utf8.empty()) return false;
    Number parsed{};
    const auto [end, result] = std::from_chars(
        utf8.data(), utf8.data() + utf8.size(), parsed);
    if (result != std::errc{} || end != utf8.data() + utf8.size()) {
        return false;
    }
    output = parsed;
    return true;
}

bool parse_pair(std::wstring_view input, wchar_t separator,
                std::wstring_view& first,
                std::wstring_view& second) noexcept {
    const std::size_t position = input.find(separator);
    if (position == std::wstring_view::npos || position == 0 ||
        position + 1 >= input.size() ||
        input.find(separator, position + 1) != std::wstring_view::npos) {
        return false;
    }
    first = input.substr(0, position);
    second = input.substr(position + 1);
    return true;
}

bool parse_dimensions(std::wstring_view input, int& width,
                      int& height) noexcept {
    std::wstring_view first;
    std::wstring_view second;
    std::size_t position = input.find(L'x');
    if (position == std::wstring_view::npos) position = input.find(L'X');
    if (position == std::wstring_view::npos || position == 0 ||
        position + 1 >= input.size()) {
        return false;
    }
    first = input.substr(0, position);
    second = input.substr(position + 1);
    return parse_number(first, width) && parse_number(second, height);
}

bool parse_roi(std::wstring_view input,
               BenchmarkExpectedGeometry& geometry) noexcept {
    int values[4]{};
    std::size_t begin = 0;
    for (int index = 0; index < 4; ++index) {
        const std::size_t end = input.find(L',', begin);
        if ((index < 3 && end == std::wstring_view::npos) ||
            (index == 3 && end != std::wstring_view::npos)) {
            return false;
        }
        const std::size_t length = end == std::wstring_view::npos
            ? input.size() - begin : end - begin;
        if (length == 0 || !parse_number(
                input.substr(begin, length), values[index])) {
            return false;
        }
        begin = end == std::wstring_view::npos ? input.size() : end + 1;
    }
    geometry.roi_x = values[0];
    geometry.roi_y = values[1];
    geometry.roi_width = values[2];
    geometry.roi_height = values[3];
    return true;
}

bool parse_scale(std::wstring_view input,
                 BenchmarkExpectedGeometry& geometry) noexcept {
    std::wstring_view first;
    std::wstring_view second;
    if (!parse_pair(input, L',', first, second)) return false;
    return parse_number(first, geometry.source_pixels_per_pixel_x) &&
           parse_number(second, geometry.source_pixels_per_pixel_y);
}

bool parse_switch(std::wstring_view input, bool& output) noexcept {
    if (input == L"on") {
        output = true;
        return true;
    }
    if (input == L"off") {
        output = false;
        return true;
    }
    return false;
}

bool parse_backend(std::wstring_view input, BackendType& output) noexcept {
    if (input == L"tensorrt") output = BackendType::TENSORRT;
    else if (input == L"cuda") output = BackendType::CUDA;
    else if (input == L"directml") output = BackendType::DIRECTML;
    else if (input == L"cpu") output = BackendType::CPU;
    else return false;
    return true;
}

bool parse_output_format(std::wstring_view input,
                         OutputFormat& output) noexcept {
    if (input == L"auto") output = OutputFormat::AUTO;
    else if (input == L"channel_first") {
        output = OutputFormat::CHANNEL_FIRST;
    } else if (input == L"objectness") {
        output = OutputFormat::ANCHOR_FIRST_OBJECTNESS;
    } else if (input == L"end_to_end") {
        output = OutputFormat::END_TO_END;
    } else {
        return false;
    }
    return true;
}

RuntimeFrameGeometry snapshot_geometry(
        const RuntimeSnapshot& snapshot) noexcept {
    RuntimeFrameGeometry geometry;
    geometry.encoded_width = snapshot.encoded_width;
    geometry.encoded_height = snapshot.encoded_height;
    geometry.source_width = snapshot.source_width;
    geometry.source_height = snapshot.source_height;
    geometry.roi_width = snapshot.capture_roi_width;
    geometry.roi_height = snapshot.capture_roi_height;
    geometry.roi_x = snapshot.capture_roi_x;
    geometry.roi_y = snapshot.capture_roi_y;
    geometry.source_pixels_per_pixel_x =
        snapshot.source_pixels_per_pixel_x;
    geometry.source_pixels_per_pixel_y =
        snapshot.source_pixels_per_pixel_y;
    return geometry;
}

bool same_double(double first, double second) noexcept {
    return std::isfinite(first) && std::isfinite(second) &&
           std::abs(first - second) <= 1e-6;
}

bool validate_runtime_snapshot(
        const RuntimeSnapshot& snapshot,
        const BenchmarkOptions& options,
        bool require_frame,
        std::string& error) noexcept {
    const char* expected_provider = expected_provider_name(options.backend);
    if (snapshot.provider != expected_provider) {
        set_error(error, "实际 Provider 不符合请求: expected=" +
            std::string(expected_provider) + ", actual=" + snapshot.provider);
        return false;
    }
    if (snapshot.failed_frames != 0) {
        set_error(error, "Runtime 已记录失败帧: " +
            std::to_string(snapshot.failed_frames));
        return false;
    }
    if (snapshot.debug_samples_dropped != 0) {
        set_error(error, "Runtime 诊断环已丢弃样本: " +
            std::to_string(snapshot.debug_samples_dropped));
        return false;
    }
    if (snapshot.output_allowed_by_config || snapshot.output_armed ||
        snapshot.mouse_commands != 0) {
        set_error(error, "无界面基准的物理输入隔离被破坏");
        return false;
    }
    if (!snapshot.last_error.empty()) {
        set_error(error, "Runtime 报告错误: " + snapshot.last_error);
        return false;
    }
    if (require_frame && snapshot.processed_frames == 0) {
        set_error(error, "Runtime 未处理任何帧");
        return false;
    }
    if (snapshot.processed_frames != 0 &&
        !validate_benchmark_geometry(
            snapshot_geometry(snapshot), options.expected_geometry, error)) {
        return false;
    }
    return true;
}

void remove_benchmark_outputs(
        const std::string& csv_path,
        const std::string& json_path,
        const std::string& provider_profile_path) noexcept {
    try {
        std::error_code ignored;
        std::filesystem::remove(csv_path, ignored);
        ignored.clear();
        std::filesystem::remove(json_path, ignored);
        const std::string temporary_suffix = ".tmp." +
            std::to_string(static_cast<unsigned long long>(
                GetCurrentProcessId()));
        ignored.clear();
        std::filesystem::remove(csv_path + temporary_suffix, ignored);
        ignored.clear();
        std::filesystem::remove(json_path + temporary_suffix, ignored);
        if (!provider_profile_path.empty()) {
            ignored.clear();
            std::filesystem::remove(provider_profile_path, ignored);
        }
    } catch (...) {
        // 失败收口不能覆盖原始错误；PowerShell 外层还会清理 pending 文件。
    }
}

bool generate_provider_profile(
        const DetectorConfig& runtime_config,
        const std::string& output_path,
        const char* expected_provider,
        std::string& error) noexcept {
    std::filesystem::path generated_path;
    try {
        const auto final_path = std::filesystem::u8path(output_path);
        if (output_path.empty() || std::filesystem::exists(final_path)) {
            set_error(error, "Provider profile 目标为空或已存在: " +
                              output_path);
            return false;
        }

        DetectorConfig profile_config = runtime_config;
        profile_config.enable_ort_profiling = true;
        profile_config.ort_profile_prefix = output_path + ".ort";
        profile_config.enable_output_fingerprint = false;
        Detector detector(profile_config);
        if (!detector.load()) {
            set_error(error, "Provider profiling Detector 加载失败");
            return false;
        }
        bool inference_succeeded = true;
        if (detector.backend_name() != expected_provider) {
            set_error(error, "Provider profiling 实际后端不符合请求");
            inference_succeeded = false;
        }

        // 两张内容不同的输入走同一个诊断 Session。这里不统计耗时，也不把
        // trace Session 复用于后续正式样本。
        for (int index = 0; inference_succeeded && index < 2; ++index) {
            const unsigned char value = index == 0 ? 0U : 255U;
            cv::Mat input(
                detector.input_height(), detector.input_width(), CV_8UC3,
                cv::Scalar(value, value, value));
            (void)detector.detect(input);
            const InferenceProfile profile = detector.profile();
            if (profile.status != DetectionStatus::SUCCESS) {
                set_error(error,
                    "Provider profiling 推理失败: status=" +
                    std::string(DetectionStatusName(profile.status)));
                inference_succeeded = false;
            }
        }

        std::string generated;
        const bool profiling_ended = detector.end_profiling(generated);
        detector.reset();
        if (!generated.empty()) {
            generated_path = std::filesystem::u8path(generated);
        }
        if (!inference_succeeded) {
            if (!generated_path.empty()) {
                std::error_code ignored;
                std::filesystem::remove(generated_path, ignored);
                generated_path.clear();
            }
            return false;
        }
        if (!profiling_ended) {
            set_error(error, "ORT EndProfiling 未返回证据文件");
            return false;
        }
        if (!std::filesystem::is_regular_file(generated_path)) {
            set_error(error, "ORT profile 文件不存在: " + generated);
            return false;
        }
        if (!MoveFileExW(
                generated_path.c_str(), final_path.c_str(),
                MOVEFILE_WRITE_THROUGH)) {
            set_error(error,
                "ORT profile 原子发布失败，Win32Error=" +
                std::to_string(GetLastError()));
            std::error_code ignored;
            std::filesystem::remove(generated_path, ignored);
            generated_path.clear();
            return false;
        }
        generated_path.clear();
        if (!std::filesystem::is_regular_file(final_path)) {
            set_error(error, "Provider profile 发布后不存在");
            std::error_code ignored;
            std::filesystem::remove(final_path, ignored);
            return false;
        }
        LOG_INFO("benchmark", "Provider profiling 已生成: {}",
                 output_path);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        set_error(error, std::string("生成 Provider profile 异常: ") +
                          exception.what());
    } catch (...) {
        set_error(error, "生成 Provider profile 时发生未知异常");
    }
    if (!generated_path.empty()) {
        std::error_code ignored;
        std::filesystem::remove(generated_path, ignored);
    }
    return false;
}

} // namespace

BenchmarkParseStatus parse_benchmark_options(
        std::span<const std::wstring_view> arguments,
        BenchmarkOptions& options,
        std::string& error) noexcept {
    try {
        BenchmarkOptions parsed;
        bool model_set = false;
        bool report_set = false;
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            const std::wstring_view argument = arguments[index];
            if (argument == L"--help" || argument == L"-h") {
                options = std::move(parsed);
                error.clear();
                return BenchmarkParseStatus::HELP;
            }
            if (index + 1 >= arguments.size()) {
                set_error(error, "参数缺少值");
                return BenchmarkParseStatus::INVALID;
            }
            const std::wstring_view value = arguments[++index];
            if (argument == L"--model") {
                if (!wide_to_utf8(value, parsed.model_path)) {
                    set_error(error, "--model 不是合法 UTF-16 路径");
                    return BenchmarkParseStatus::INVALID;
                }
                model_set = true;
            } else if (argument == L"--config") {
                if (!wide_to_utf8(value, parsed.config_path)) {
                    set_error(error, "--config 不是合法 UTF-16 路径");
                    return BenchmarkParseStatus::INVALID;
                }
            } else if (argument == L"--backend") {
                if (!parse_backend(value, parsed.backend)) {
                    set_error(error,
                              "--backend 必须是 tensorrt/cuda/directml/cpu");
                    return BenchmarkParseStatus::INVALID;
                }
                parsed.backend_explicit = true;
            } else if (argument == L"--report-prefix") {
                if (!wide_to_utf8(value, parsed.report_prefix)) {
                    set_error(error, "--report-prefix 不是合法 UTF-16 路径");
                    return BenchmarkParseStatus::INVALID;
                }
                report_set = true;
            } else if (argument == L"--provider-profile") {
                if (!wide_to_utf8(value, parsed.provider_profile_path)) {
                    set_error(error,
                              "--provider-profile 不是合法 UTF-16 路径");
                    return BenchmarkParseStatus::INVALID;
                }
            } else if (argument == L"--output-format") {
                if (!parse_output_format(value, parsed.output_format)) {
                    set_error(error,
                        "--output-format 必须是 auto/channel_first/"
                        "objectness/end_to_end");
                    return BenchmarkParseStatus::INVALID;
                }
            } else if (argument == L"--warmup-samples") {
                if (!parse_number(value, parsed.warmup_samples)) {
                    set_error(error, "--warmup-samples 必须是非负整数");
                    return BenchmarkParseStatus::INVALID;
                }
            } else if (argument == L"--minimum-samples") {
                if (!parse_number(value, parsed.minimum_samples)) {
                    set_error(error, "--minimum-samples 必须是正整数");
                    return BenchmarkParseStatus::INVALID;
                }
            } else if (argument == L"--minimum-seconds") {
                if (!parse_number(value, parsed.minimum_seconds)) {
                    set_error(error, "--minimum-seconds 必须是非负整数");
                    return BenchmarkParseStatus::INVALID;
                }
            } else if (argument == L"--maximum-seconds") {
                if (!parse_number(value, parsed.maximum_seconds)) {
                    set_error(error, "--maximum-seconds 必须是正整数");
                    return BenchmarkParseStatus::INVALID;
                }
            } else if (argument == L"--expect-source") {
                if (!parse_dimensions(
                        value, parsed.expected_geometry.source_width,
                        parsed.expected_geometry.source_height)) {
                    set_error(error, "--expect-source 格式必须是 WIDTHxHEIGHT");
                    return BenchmarkParseStatus::INVALID;
                }
            } else if (argument == L"--expect-encoded") {
                if (!parse_dimensions(
                        value, parsed.expected_geometry.encoded_width,
                        parsed.expected_geometry.encoded_height)) {
                    set_error(error, "--expect-encoded 格式必须是 WIDTHxHEIGHT");
                    return BenchmarkParseStatus::INVALID;
                }
            } else if (argument == L"--expect-roi") {
                if (!parse_roi(value, parsed.expected_geometry)) {
                    set_error(error, "--expect-roi 格式必须是 X,Y,WIDTH,HEIGHT");
                    return BenchmarkParseStatus::INVALID;
                }
            } else if (argument == L"--expect-scale") {
                if (!parse_scale(value, parsed.expected_geometry)) {
                    set_error(error, "--expect-scale 格式必须是 X,Y");
                    return BenchmarkParseStatus::INVALID;
                }
            } else if (argument == L"--fp16") {
                if (!parse_switch(value, parsed.enable_fp16)) {
                    set_error(error, "--fp16 必须是 on/off");
                    return BenchmarkParseStatus::INVALID;
                }
            } else if (argument == L"--cuda-graph") {
                if (!parse_switch(value, parsed.enable_cuda_graph)) {
                    set_error(error, "--cuda-graph 必须是 on/off");
                    return BenchmarkParseStatus::INVALID;
                }
            } else if (argument == L"--gpu-preprocess") {
                if (!parse_switch(value, parsed.enable_gpu_preprocess)) {
                    set_error(error, "--gpu-preprocess 必须是 on/off");
                    return BenchmarkParseStatus::INVALID;
                }
            } else {
                std::string unknown;
                wide_to_utf8(argument, unknown);
                set_error(error, "未知参数: " + unknown);
                return BenchmarkParseStatus::INVALID;
            }
        }
        if (!model_set) parsed.model_path.clear();
        if (!report_set) parsed.report_prefix.clear();
        if (!validate_benchmark_options(parsed, error)) {
            return BenchmarkParseStatus::INVALID;
        }
        options = std::move(parsed);
        error.clear();
        return BenchmarkParseStatus::READY;
    } catch (...) {
        set_error(error, "解析基准命令行时发生未知异常");
        return BenchmarkParseStatus::INVALID;
    }
}

bool validate_benchmark_options(
        const BenchmarkOptions& options,
        std::string& error) noexcept {
    try {
        const auto& geometry = options.expected_geometry;
        if (options.model_path.empty()) {
            set_error(error, "必须提供 --model");
            return false;
        }
        if (!options.backend_explicit) {
            set_error(error, "必须提供 --backend");
            return false;
        }
        if (options.report_prefix.empty()) {
            set_error(error, "必须提供 --report-prefix");
            return false;
        }
        const bool gpu_backend = options.backend == BackendType::TENSORRT ||
            options.backend == BackendType::CUDA;
        if (gpu_backend && options.provider_profile_path.empty()) {
            set_error(error,
                      "TensorRT/CUDA 必须提供 --provider-profile");
            return false;
        }
        if (!gpu_backend && !options.provider_profile_path.empty()) {
            set_error(error,
                      "DirectML/CPU 不接受 --provider-profile");
            return false;
        }
        if (options.warmup_samples > kMaximumReportSamples ||
            options.minimum_samples == 0 ||
            options.minimum_samples > kMaximumReportSamples ||
            options.minimum_seconds > kMaximumBenchmarkSeconds ||
            options.maximum_seconds == 0 ||
            options.maximum_seconds > kMaximumBenchmarkSeconds ||
            options.maximum_seconds < options.minimum_seconds) {
            set_error(error, "样本数或时长门槛超出允许范围");
            return false;
        }
        if (geometry.source_width <= 0 || geometry.source_height <= 0 ||
            geometry.encoded_width <= 0 || geometry.encoded_height <= 0 ||
            geometry.roi_x < 0 || geometry.roi_y < 0 ||
            geometry.roi_width <= 0 || geometry.roi_height <= 0 ||
            geometry.roi_x > geometry.source_width ||
            geometry.roi_y > geometry.source_height ||
            geometry.roi_width > geometry.source_width - geometry.roi_x ||
            geometry.roi_height > geometry.source_height - geometry.roi_y ||
            !std::isfinite(geometry.source_pixels_per_pixel_x) ||
            !std::isfinite(geometry.source_pixels_per_pixel_y) ||
            geometry.source_pixels_per_pixel_x <= 0.0 ||
            geometry.source_pixels_per_pixel_y <= 0.0) {
            set_error(error, "期望几何超出允许范围");
            return false;
        }
        error.clear();
        return true;
    } catch (...) {
        set_error(error, "校验基准参数时发生未知异常");
        return false;
    }
}

bool validate_benchmark_geometry(
        const RuntimeFrameGeometry& actual,
        const BenchmarkExpectedGeometry& expected,
        std::string& error) noexcept {
    try {
        if (actual.source_width == expected.source_width &&
            actual.source_height == expected.source_height &&
            actual.encoded_width == expected.encoded_width &&
            actual.encoded_height == expected.encoded_height &&
            actual.roi_width == expected.roi_width &&
            actual.roi_height == expected.roi_height &&
            same_double(actual.roi_x, static_cast<double>(expected.roi_x)) &&
            same_double(actual.roi_y, static_cast<double>(expected.roi_y)) &&
            same_double(actual.source_pixels_per_pixel_x,
                        expected.source_pixels_per_pixel_x) &&
            same_double(actual.source_pixels_per_pixel_y,
                        expected.source_pixels_per_pixel_y)) {
            error.clear();
            return true;
        }
        std::ostringstream message;
        message << "帧几何不符合契约: expected=source "
                << expected.source_width << 'x' << expected.source_height
                << ", encoded " << expected.encoded_width << 'x'
                << expected.encoded_height << ", roi=(" << expected.roi_x
                << ',' << expected.roi_y << ',' << expected.roi_width << 'x'
                << expected.roi_height << "), scale=("
                << expected.source_pixels_per_pixel_x << ','
                << expected.source_pixels_per_pixel_y << "), actual=source "
                << actual.source_width << 'x' << actual.source_height
                << ", encoded " << actual.encoded_width << 'x'
                << actual.encoded_height << ", roi=(" << actual.roi_x << ','
                << actual.roi_y << ',' << actual.roi_width << 'x'
                << actual.roi_height << "), scale=("
                << actual.source_pixels_per_pixel_x << ','
                << actual.source_pixels_per_pixel_y << ')';
        set_error(error, message.str());
        return false;
    } catch (...) {
        set_error(error, "校验帧几何时发生未知异常");
        return false;
    }
}

const char* expected_provider_name(BackendType backend) noexcept {
    switch (backend) {
        case BackendType::TENSORRT: return "TensorrtExecutionProvider";
        case BackendType::CUDA: return "CUDAExecutionProvider";
        case BackendType::DIRECTML: return "DmlExecutionProvider";
        case BackendType::CPU: return "CPUExecutionProvider";
    }
    return "UnknownExecutionProvider";
}

std::string benchmark_usage() {
    return
        "XenBenchmark - 无界面 Runtime 正式基准入口\n\n"
        "用法:\n"
        "  XenBenchmark.exe --model <path> --backend <name> "
        "--report-prefix <path> [选项]\n\n"
        "必选:\n"
        "  --model PATH             ONNX 模型路径\n"
        "  --backend NAME           tensorrt/cuda/directml/cpu\n"
        "  --report-prefix PATH     成功后发布 PATH.csv 和 PATH.json\n\n"
        "Provider 证据:\n"
        "  --provider-profile PATH  TensorRT/CUDA 必选，独立 ORT trace JSON\n\n"
        "运行门槛:\n"
        "  --config PATH            可选 AppConfig INI\n"
        "  --output-format NAME     auto/channel_first/objectness/end_to_end\n"
        "  --warmup-samples N       warmup 成功样本数，默认 100\n"
        "  --minimum-samples N      正式成功样本数，默认 10000\n"
        "  --minimum-seconds N      正式最小时长，默认 300\n"
        "  --maximum-seconds N      启动后最大时限，默认 600\n\n"
        "几何契约:\n"
        "  --expect-source WxH      主机 FOV，默认 2560x1440\n"
        "  --expect-encoded WxH     编码帧，DXGI 默认 2560x1440\n"
        "  --expect-roi X,Y,W,H     主机 ROI，默认 1120,560,320,320\n"
        "  --expect-scale X,Y       ROI 像素到主机像素比例，默认 1,1\n\n"
        "推理开关:\n"
        "  --fp16 on|off            TensorRT FP16，默认 on\n"
        "  --cuda-graph on|off      TensorRT CUDA Graph，默认 on\n"
        "  --gpu-preprocess on|off  CUDA 前处理，默认 on\n"
        "  --help                   显示帮助\n";
}

void request_benchmark_stop() noexcept {
    benchmark_stop_requested.store(true, std::memory_order_release);
}

bool run_runtime_benchmark(
        const BenchmarkOptions& options,
        std::string& error) noexcept {
    std::string csv_path;
    std::string json_path;
    std::string provider_profile_path;
    bool report_outputs_owned = false;
    try {
        if (!validate_benchmark_options(options, error)) return false;
        const std::filesystem::path model_path(options.model_path);
        if (!std::filesystem::is_regular_file(model_path)) {
            set_error(error, "模型文件不存在: " + options.model_path);
            return false;
        }
        csv_path = options.report_prefix + ".csv";
        json_path = options.report_prefix + ".json";
        provider_profile_path = options.provider_profile_path;
        if (std::filesystem::exists(csv_path) ||
            std::filesystem::exists(json_path) ||
            (!provider_profile_path.empty() &&
             std::filesystem::exists(provider_profile_path))) {
            set_error(error, "报告目标已存在，拒绝覆盖: " +
                              options.report_prefix);
            return false;
        }
        report_outputs_owned = true;

        AppConfig config;
        config.detector.model_path = options.model_path;
        if (!options.config_path.empty() &&
            !load_app_config(options.config_path, config, error)) {
            return false;
        }
        config.detector.model_path = options.model_path;
        config.detector.backend = options.backend;
        config.detector.output_format = options.output_format;
        config.detector.enable_fp16 = options.enable_fp16;
        config.detector.enable_trt_cuda_graph = options.enable_cuda_graph;
        config.detector.enable_gpu_preprocess =
            options.enable_gpu_preprocess;
        config.detector.enable_output_fingerprint = false;
        config.detector.enable_ort_profiling = false;
        config.detector.ort_profile_prefix.clear();
        // 基准从不武装 SafetyGate，并强制使用禁用的 Win32 后端。即使配置文件
        // 原本允许 KMBOX/SendInput，也不会打开设备连接或发送物理输入。
        config.mouse.backend = MouseBackend::WIN32_SEND_INPUT;
        config.mouse.allow_send_input = false;
        if (!validate_app_config(config, error)) return false;

        Log::init(config.log);
        if (!Log::initialized()) {
            set_error(error, "Log 初始化失败");
            return false;
        }
        Log::register_module("benchmark", LogLevel::INFO);
        bool success = false;
        {
            CrashHandler crash_handler;
            const std::string crash_log_dir = config.log.log_dir.empty()
                ? "logs" : config.log.log_dir;
            if (!crash_handler.install(crash_log_dir)) {
                set_error(error, "崩溃诊断安装失败");
            } else {
                const bool profile_ready = provider_profile_path.empty() ||
                    generate_provider_profile(
                        config.detector, provider_profile_path,
                        expected_provider_name(options.backend), error);
                if (profile_ready) {
                Runtime runtime;
                DebugReport report;
                benchmark_stop_requested.store(false,
                                                std::memory_order_release);
                if (!runtime.start(config)) {
                    const RuntimeSnapshot snapshot = runtime.snapshot();
                    set_error(error, snapshot.last_error.empty()
                        ? "Runtime 启动失败" : snapshot.last_error);
                } else {
                    const auto runtime_started =
                        std::chrono::steady_clock::now();
                    RuntimeSnapshot running_snapshot = runtime.snapshot();
                    if (!validate_runtime_snapshot(
                            running_snapshot, options, false, error)) {
                        runtime.stop();
                    } else {
                        DebugReportConfig report_config;
                        report_config.csv_path = csv_path;
                        report_config.json_path = json_path;
                        report_config.session_id =
                            std::to_string(GetCurrentProcessId()) + "-" +
                            std::to_string(GetTickCount64());
                        report_config.model_path = options.model_path;
                        report_config.provider = running_snapshot.provider;
                        report_config.capture_backend =
                            CaptureBackendName(config.capture.backend);
                        report_config.mouse_backend =
                            MouseBackendName(config.mouse.backend);
                        report_config.max_samples = kMaximumReportSamples;
                        if (!report.start(report_config, error)) {
                            runtime.stop();
                        } else {
                            std::vector<RuntimePipelineSample> pending;
                            std::uint64_t warmup_successful = 0;
                            std::uint64_t formal_successful = 0;
                            std::uint64_t formal_samples = 0;
                            bool measurement_started =
                                options.warmup_samples == 0;
                            auto measurement_started_at = runtime_started;
                            bool gates_satisfied = false;
                            bool run_failed = false;

                            const auto consume_pending = [&]() noexcept {
                                try {
                                    std::size_t formal_begin = pending.size();
                                    for (std::size_t index = 0;
                                         index < pending.size(); ++index) {
                                        const auto& sample = pending[index];
                                        if (!validate_benchmark_geometry(
                                                sample.geometry,
                                                options.expected_geometry,
                                                error)) {
                                            return false;
                                        }
                                        if (!debug_sample_succeeded(sample)) {
                                            set_error(
                                                error,
                                                "Pipeline 样本失败: sequence=" +
                                                std::to_string(sample.sequence) +
                                                ", detection=" +
                                                DetectionStatusName(
                                                    sample.detection_status) +
                                                ", aim=" +
                                                AimStatusName(sample.aim_status));
                                            return false;
                                        }
                                        if (sample.mouse_sent) {
                                            set_error(error,
                                                "无界面基准出现 Mouse 发送记录");
                                            return false;
                                        }
                                        if (warmup_successful <
                                                options.warmup_samples) {
                                            ++warmup_successful;
                                            if (warmup_successful ==
                                                    options.warmup_samples) {
                                                measurement_started = true;
                                                measurement_started_at =
                                                    std::chrono::steady_clock::now();
                                            }
                                            continue;
                                        }
                                        if (formal_begin == pending.size()) {
                                            formal_begin = index;
                                        }
                                        ++formal_samples;
                                        ++formal_successful;
                                    }
                                    if (formal_begin < pending.size()) {
                                        if (formal_samples >
                                                kMaximumReportSamples) {
                                            set_error(error,
                                                "正式样本超过报告固定容量");
                                            return false;
                                        }
                                        report.ingest(std::span(
                                            pending).subspan(formal_begin));
                                    }
                                    return true;
                                } catch (...) {
                                    set_error(error,
                                              "消费 Runtime 样本时发生未知异常");
                                    return false;
                                }
                            };

                            while (!gates_satisfied && !run_failed) {
                                if (benchmark_stop_requested.load(
                                        std::memory_order_acquire)) {
                                    set_error(error, "基准被人工中止");
                                    run_failed = true;
                                    break;
                                }
                                if (!runtime.drain_pipeline_samples(pending)) {
                                    set_error(error,
                                              "读取 Runtime 诊断样本失败");
                                    run_failed = true;
                                    break;
                                }
                                if (!consume_pending()) {
                                    run_failed = true;
                                    break;
                                }
                                pending.clear();

                                running_snapshot = runtime.snapshot();
                                if (running_snapshot.state !=
                                        RuntimeState::RUNNING) {
                                    set_error(error, "Runtime 提前退出: state=" +
                                        std::string(RuntimeStateName(
                                            running_snapshot.state)));
                                    run_failed = true;
                                    break;
                                }
                                if (!validate_runtime_snapshot(
                                        running_snapshot, options,
                                        false, error)) {
                                    run_failed = true;
                                    break;
                                }

                                const auto now =
                                    std::chrono::steady_clock::now();
                                const auto runtime_seconds =
                                    std::chrono::duration_cast<
                                        std::chrono::seconds>(
                                            now - runtime_started).count();
                                if (runtime_seconds >= static_cast<long long>(
                                        options.maximum_seconds)) {
                                    set_error(error,
                                        "达到最大时限但未满足正式基准门槛");
                                    run_failed = true;
                                    break;
                                }
                                if (measurement_started) {
                                    const auto measurement_seconds =
                                        std::chrono::duration_cast<
                                            std::chrono::seconds>(
                                                now - measurement_started_at)
                                            .count();
                                    gates_satisfied =
                                        formal_successful >=
                                            options.minimum_samples &&
                                        measurement_seconds >=
                                            static_cast<long long>(
                                                options.minimum_seconds);
                                }
                                if (!gates_satisfied) {
                                    std::this_thread::sleep_for(kPollInterval);
                                }
                            }

                            runtime.stop();
                            if (!run_failed) {
                                if (!runtime.drain_pipeline_samples(pending) ||
                                    !consume_pending()) {
                                    run_failed = true;
                                }
                                pending.clear();
                            }
                            const RuntimeSnapshot final_snapshot =
                                runtime.snapshot();
                            if (!run_failed && final_snapshot.state !=
                                    RuntimeState::STOPPED) {
                                set_error(error,
                                    "Runtime 停止后状态不是 STOPPED");
                                run_failed = true;
                            }
                            if (!run_failed &&
                                !validate_runtime_snapshot(
                                    final_snapshot, options, true, error)) {
                                run_failed = true;
                            }
                            if (!run_failed &&
                                (formal_successful < options.minimum_samples ||
                                 formal_samples != formal_successful)) {
                                set_error(error,
                                    "正式样本门槛或成功计数不一致");
                                run_failed = true;
                            }
                            if (!run_failed && report.finalize(
                                    final_snapshot, error)) {
                                const auto& summary = report.summary();
                                if (summary.sample_count == formal_samples &&
                                    summary.successful_samples == formal_samples &&
                                    summary.failed_samples == 0 &&
                                    summary.report_samples_dropped == 0 &&
                                    summary.runtime_samples_dropped == 0) {
                                    success = true;
                                    LOG_INFO(
                                        "benchmark",
                                        "正式基准完成: warmup={}, samples={}, "
                                        "total_p50={:.3f}ms, total_p95={:.3f}ms, "
                                        "total_p99={:.3f}ms",
                                        warmup_successful, formal_samples,
                                        summary.total.p50_ms,
                                        summary.total.p95_ms,
                                        summary.total.p99_ms);
                                } else {
                                    set_error(error,
                                        "发布后的报告汇总不符合零失败契约");
                                }
                            }
                        }
                    }
                }
                }
                crash_handler.uninstall();
            }
        }
        if (!success && report_outputs_owned) {
            // 报告目标在入口已确认不存在，因此这里只清理本轮创建的精确
            // CSV、JSON 和 Provider profile。即使 JSON 原子发布或发布后汇总
            // 校验失败，也不能留下可被误认为有效结果的单个文件。
            remove_benchmark_outputs(
                csv_path, json_path, provider_profile_path);
        }
        Log::shutdown();
        if (success) error.clear();
        return success;
    } catch (const std::exception& exception) {
        if (report_outputs_owned) {
            remove_benchmark_outputs(
                csv_path, json_path, provider_profile_path);
        }
        Log::shutdown();
        set_error(error, std::string("执行 Runtime 基准异常: ") +
                          exception.what());
        return false;
    } catch (...) {
        if (report_outputs_owned) {
            remove_benchmark_outputs(
                csv_path, json_path, provider_profile_path);
        }
        Log::shutdown();
        set_error(error, "执行 Runtime 基准时发生未知异常");
        return false;
    }
}
