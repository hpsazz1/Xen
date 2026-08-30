#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>

#ifdef ERROR
#undef ERROR
#endif

#include "benchmark/benchmark.h"
#include "benchmark/benchmark_internal.h"

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
#include <fstream>
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
    else if (input == L"openvino") output = BackendType::OPENVINO;
    else if (input == L"cpu") output = BackendType::CPU;
    else return false;
    return true;
}

bool parse_openvino_device(
        std::wstring_view input, OpenVinoDevice& output) noexcept {
    if (input == L"gpu") output = OpenVinoDevice::GPU;
    else if (input == L"cpu") output = OpenVinoDevice::CPU;
    else if (input == L"npu") output = OpenVinoDevice::NPU;
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

bool validate_performance_probe_sample(
        const RuntimePipelineSample& sample,
        CaptureBackend capture_backend,
        bool probes_enabled,
        std::string& error) noexcept {
    const auto& capture = sample.capture_stages;
    const auto& service = sample.service;
    if (service.valid != probes_enabled ||
        capture.runtime_handoff_valid != probes_enabled ||
        (capture_backend == CaptureBackend::NDI &&
         capture.ndi_valid != probes_enabled)) {
        set_error(error, "样本性能探针有效位不符合请求");
        return false;
    }
    if (!probes_enabled) return true;

    const auto finite_nonnegative = [](double value) noexcept {
        return std::isfinite(value) && value >= 0.0;
    };
    const double service_values[] = {
        service.preview_ms,
        service.snapshot_ms,
        service.snapshot_lock_wait_ms,
        service.debug_ring_ms,
        service.profile_window_ms,
        service.service_tail_ms,
        service.pipeline_service_ms,
        service.pipeline_complete_ms,
        capture.runtime_capture_grab_ms,
        capture.runtime_queue_publish_ms,
    };
    for (const double value : service_values) {
        if (!finite_nonnegative(value)) {
            set_error(error, "样本 Runtime 收尾探针出现非法耗时");
            return false;
        }
    }
    if (service.snapshot_ms + 1e-6 < service.snapshot_lock_wait_ms ||
        service.snapshot_ms + 1e-6 < service.debug_ring_ms ||
        service.snapshot_ms + 1e-6 < service.profile_window_ms ||
        service.service_tail_ms + 1e-6 < service.preview_ms ||
        service.service_tail_ms + 1e-6 < service.snapshot_ms ||
        std::abs(service.pipeline_complete_ms -
                 (sample.profile.total_ms + service.service_tail_ms)) > 0.01 ||
        std::abs(service.pipeline_service_ms -
                 (service.pipeline_complete_ms - sample.profile.queue_ms)) >
            0.01) {
        set_error(error, "样本 Runtime 收尾探针边界或包含关系不成立");
        return false;
    }
    if (capture_backend == CaptureBackend::NDI) {
        const double ndi_values[] = {
            capture.receive_call_ms,
            capture.metadata_ms,
            capture.geometry_ms,
            capture.pool_acquire_ms,
            capture.color_convert_ms,
            capture.pool_publish_ms,
        };
        for (const double value : ndi_values) {
            if (!finite_nonnegative(value)) {
                set_error(error, "样本 NDI 分段探针出现非法耗时");
                return false;
            }
        }
        if ((capture.performance_query_sampled &&
             !finite_nonnegative(capture.performance_query_ms)) ||
            (capture.queue_depth_sampled &&
             (!finite_nonnegative(capture.queue_query_ms) ||
              capture.queued_video_frames < 0 ||
              capture.queued_audio_frames < 0 ||
              capture.queued_metadata_frames < 0))) {
            set_error(error, "样本 NDI 低频查询探针出现非法结果");
            return false;
        }
    }
    return true;
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
    if (snapshot.d3d11_cuda_interop !=
        options.enable_d3d11_cuda_interop) {
        set_error(error, "Runtime D3D11/CUDA 互操作状态不符合请求");
        return false;
    }
    if (snapshot.d3d11_directml_interop !=
        options.enable_d3d11_directml_interop) {
        set_error(error, "Runtime D3D11/DirectML 互操作状态不符合请求");
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

class ReadyFileGuard {
public:
    explicit ReadyFileGuard(const std::string& path) noexcept : path_(path) {}

    ReadyFileGuard(const ReadyFileGuard&) = delete;
    ReadyFileGuard& operator=(const ReadyFileGuard&) = delete;

    ~ReadyFileGuard() noexcept {
        remove();
    }

    bool publish(const RuntimeSnapshot& snapshot,
                 const AppConfig& config,
                 const BenchmarkOptions& options,
                 std::string& error) noexcept {
        if (path_.empty()) return true;
        try {
            const auto target = std::filesystem::u8path(path_);
            if (std::filesystem::exists(target)) {
                set_error(error, "ready-file 目标已存在，拒绝覆盖: " + path_);
                return false;
            }
            auto temporary = target;
            temporary += ".tmp." + std::to_string(
                static_cast<unsigned long long>(GetCurrentProcessId()));
            if (std::filesystem::exists(temporary)) {
                set_error(error, "ready-file 临时目标已存在，拒绝覆盖");
                return false;
            }

            // ready 只声明接收端已经能够收帧；首帧到达前不能把期望几何
            // 冒充实际帧几何，因此字段明确命名为 expected_geometry。
            const auto& geometry = options.expected_geometry;
            std::ofstream output(
                temporary, std::ios::binary | std::ios::trunc);
            if (!output) {
                set_error(error, "无法创建 ready-file 临时文件: " + path_);
                return false;
            }
            output << "{\n"
                   << "  \"schema\": 1,\n"
                   << "  \"ready\": true,\n"
                   << "  \"pid\": " << GetCurrentProcessId() << ",\n"
                   << "  \"runtime_state\": \""
                   << RuntimeStateName(snapshot.state) << "\",\n"
                   << "  \"provider\": \"" << snapshot.provider << "\",\n"
                   << "  \"capture_backend\": \""
                   << CaptureBackendName(config.capture.backend) << "\",\n"
                   << "  \"expected_geometry\": {"
                   << "\"source_width\": " << geometry.source_width
                   << ", \"source_height\": " << geometry.source_height
                   << ", \"encoded_width\": " << geometry.encoded_width
                   << ", \"encoded_height\": " << geometry.encoded_height
                   << ", \"roi_x\": " << geometry.roi_x
                   << ", \"roi_y\": " << geometry.roi_y
                   << ", \"roi_width\": " << geometry.roi_width
                   << ", \"roi_height\": " << geometry.roi_height
                   << "}\n}\n";
            output.flush();
            const bool write_succeeded = output.good();
            output.close();
            if (!write_succeeded) {
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                set_error(error, "写入 ready-file 临时文件失败: " + path_);
                return false;
            }
            if (!MoveFileExW(
                    temporary.c_str(), target.c_str(),
                    MOVEFILE_WRITE_THROUGH)) {
                const DWORD win32_error = GetLastError();
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                set_error(error, "ready-file 原子发布失败，Win32Error=" +
                    std::to_string(win32_error));
                return false;
            }
            owned_ = true;
            return true;
        } catch (const std::exception& exception) {
            set_error(error, std::string("发布 ready-file 异常: ") +
                              exception.what());
            return false;
        } catch (...) {
            set_error(error, "发布 ready-file 时发生未知异常");
            return false;
        }
    }

    void remove() noexcept {
        if (!owned_ || path_.empty()) return;
        try {
            std::error_code ignored;
            std::filesystem::remove(std::filesystem::u8path(path_), ignored);
        } catch (...) {
        }
        owned_ = false;
    }

private:
    const std::string& path_;
    bool owned_ = false;
};

void remove_benchmark_outputs(
        const std::string& csv_path,
        const std::string& json_path,
        const std::string& csv_staging_path,
        const std::string& json_staging_path,
        const std::string& provider_profile_path) noexcept {
    try {
        std::error_code ignored;
        std::filesystem::remove(csv_path, ignored);
        ignored.clear();
        std::filesystem::remove(json_path, ignored);
        const std::string debug_temporary_suffix = ".tmp." +
            std::to_string(static_cast<unsigned long long>(
                GetCurrentProcessId()));
        const std::string retention_temporary_suffix =
            ".retention.tmp." +
            std::to_string(static_cast<unsigned long long>(
                GetCurrentProcessId()));
        for (const auto* staging_path : {
                 &csv_staging_path, &json_staging_path}) {
            if (staging_path->empty()) continue;
            ignored.clear();
            std::filesystem::remove(*staging_path, ignored);
            ignored.clear();
            std::filesystem::remove(
                *staging_path + debug_temporary_suffix, ignored);
            ignored.clear();
            std::filesystem::remove(
                *staging_path + retention_temporary_suffix, ignored);
        }
        if (!provider_profile_path.empty()) {
            ignored.clear();
            std::filesystem::remove(provider_profile_path, ignored);
        }
    } catch (...) {
        // 失败收口不能覆盖原始错误；PowerShell 外层还会清理 pending 文件。
    }
}

bool read_report_samples_dropped(
        const std::string& path,
        benchmark::detail::ReportFileFormat format,
        std::uint64_t& value,
        std::string& error) noexcept {
    try {
        std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
        if (!input) {
            set_error(error, "无法回读 staging 报告省略计数: " + path);
            return false;
        }
        const std::string_view prefix =
            format == benchmark::detail::ReportFileFormat::CSV
            ? "# report_samples_dropped,"
            : "  \"report_samples_dropped\": ";
        std::size_t matches = 0;
        std::uint64_t parsed_value = 0;
        std::string line;
        while (std::getline(input, line)) {
            if (!line.starts_with(prefix)) continue;
            std::string_view encoded(line);
            encoded.remove_prefix(prefix.size());
            if (format == benchmark::detail::ReportFileFormat::JSON) {
                if (encoded.empty() || encoded.back() != ',') {
                    set_error(error,
                        "staging JSON 省略计数行格式非法");
                    return false;
                }
                encoded.remove_suffix(1);
            }
            std::uint64_t candidate = 0;
            const auto [end, result] = std::from_chars(
                encoded.data(), encoded.data() + encoded.size(), candidate);
            if (result != std::errc{} ||
                end != encoded.data() + encoded.size()) {
                set_error(error, "staging 报告省略计数不是 uint64");
                return false;
            }
            parsed_value = candidate;
            ++matches;
        }
        if (!input.eof() || matches != 1) {
            set_error(error,
                "staging 报告必须且只能包含一条省略计数元数据");
            return false;
        }
        value = parsed_value;
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        set_error(error, std::string("回读 staging 报告异常: ") +
                          exception.what());
        return false;
    } catch (...) {
        set_error(error, "回读 staging 报告时发生未知异常");
        return false;
    }
}

bool rewrite_report_samples_dropped(
        const std::string& path,
        benchmark::detail::ReportFileFormat format,
        std::uint64_t omitted_sample_count,
        std::string& error) noexcept {
    if (omitted_sample_count == 0) return true;
    std::filesystem::path temporary;
    try {
        const auto target = std::filesystem::u8path(path);
        if (!std::filesystem::is_regular_file(target)) {
            set_error(error, "staging 报告不存在: " + path);
            return false;
        }
        temporary = target;
        temporary += ".retention.tmp." +
            std::to_string(static_cast<unsigned long long>(
                GetCurrentProcessId()));
        if (std::filesystem::exists(temporary)) {
            set_error(error, "staging 补写临时目标已存在");
            return false;
        }

        std::ifstream input(target, std::ios::binary);
        std::ofstream output(
            temporary, std::ios::binary | std::ios::trunc);
        if (!input || !output) {
            set_error(error, "无法打开 staging 报告或补写临时文件");
            return false;
        }
        std::size_t matches = 0;
        std::string line;
        std::string rewritten;
        while (std::getline(input, line)) {
            if (benchmark::detail::rewrite_report_samples_dropped_line(
                    line, format, omitted_sample_count, rewritten)) {
                ++matches;
                output << rewritten << '\n';
            } else {
                output << line << '\n';
            }
        }
        output.flush();
        const bool stream_valid = input.eof() && output.good();
        input.close();
        output.close();
        if (!stream_valid || matches != 1) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            set_error(error,
                "staging 报告补写失败或省略计数标记不唯一");
            return false;
        }
        if (!MoveFileExW(
                temporary.c_str(), target.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const DWORD win32_error = GetLastError();
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            set_error(error, "staging 报告补写发布失败，Win32Error=" +
                              std::to_string(win32_error));
            return false;
        }
        temporary.clear();
        std::uint64_t verified = 0;
        if (!read_report_samples_dropped(
                path, format, verified, error) ||
            verified != omitted_sample_count) {
            if (error.empty()) {
                set_error(error, "staging 报告省略计数回读不一致");
            }
            return false;
        }
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        set_error(error, std::string("补写 staging 报告异常: ") +
                          exception.what());
    } catch (...) {
        set_error(error, "补写 staging 报告时发生未知异常");
    }
    if (!temporary.empty()) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
    }
    return false;
}

bool publish_benchmark_reports(
        const std::string& csv_staging_path,
        const std::string& json_staging_path,
        const std::string& csv_path,
        const std::string& json_path,
        std::string& error) noexcept {
    try {
        const auto csv_staging =
            std::filesystem::u8path(csv_staging_path);
        const auto json_staging =
            std::filesystem::u8path(json_staging_path);
        const auto csv_target = std::filesystem::u8path(csv_path);
        const auto json_target = std::filesystem::u8path(json_path);
        if (!std::filesystem::is_regular_file(csv_staging) ||
            !std::filesystem::is_regular_file(json_staging) ||
            std::filesystem::exists(csv_target) ||
            std::filesystem::exists(json_target)) {
            set_error(error, "staging 报告不完整或正式目标已存在");
            return false;
        }
        if (!MoveFileExW(
                csv_staging.c_str(), csv_target.c_str(),
                MOVEFILE_WRITE_THROUGH)) {
            set_error(error, "CSV 正式报告发布失败，Win32Error=" +
                              std::to_string(GetLastError()));
            return false;
        }
        if (!MoveFileExW(
                json_staging.c_str(), json_target.c_str(),
                MOVEFILE_WRITE_THROUGH)) {
            const DWORD win32_error = GetLastError();
            std::error_code ignored;
            std::filesystem::remove(csv_target, ignored);
            set_error(error, "JSON 正式报告发布失败，Win32Error=" +
                              std::to_string(win32_error));
            return false;
        }
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        set_error(error, std::string("发布正式报告异常: ") +
                          exception.what());
        return false;
    } catch (...) {
        set_error(error, "发布正式报告时发生未知异常");
        return false;
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

bool benchmark::detail::finalize_report(
        DebugReport& report,
        const RuntimeSnapshot& final_snapshot,
        const DebugCoverageSummary& coverage,
        const FormalSampleSummary& formal_summary,
        std::uint64_t phase_formal_samples,
        bool performance_probes_enabled,
        CaptureBackend capture_backend,
        std::string& error) noexcept {
    if (!report.finalize(final_snapshot, error, &coverage)) return false;

    const auto& summary = report.summary();
    const std::uint64_t formal_samples =
        formal_summary.formal_sample_count;
    const std::size_t retained_samples =
        formal_summary.retained_sample_count;
    const bool probe_summary_valid = performance_probes_enabled
        ? summary.pipeline_complete.sample_count == retained_samples &&
          summary.runtime_handoff.sample_count == retained_samples &&
          (capture_backend != CaptureBackend::NDI ||
           (summary.ndi_receive_call.sample_count == retained_samples &&
            summary.ndi_video_queue_depth.sample_count > 0))
        : summary.pipeline_complete.sample_count == 0 &&
          summary.runtime_handoff.sample_count == 0 &&
          summary.ndi_receive_call.sample_count == 0 &&
          summary.ndi_video_queue_depth.sample_count == 0;
    if (phase_formal_samples == formal_samples &&
        formal_summary.successful_samples == formal_samples &&
        formal_summary.failed_samples == 0 &&
        static_cast<std::uint64_t>(retained_samples) +
                formal_summary.omitted_sample_count == formal_samples &&
        summary.sample_count == retained_samples &&
        summary.successful_samples == retained_samples &&
        summary.failed_samples == 0 &&
        summary.report_samples_dropped == 0 &&
        summary.runtime_samples_dropped == 0 &&
        summary.coverage.available &&
        summary.coverage.formal.sample_count == formal_samples &&
        probe_summary_valid) {
        return true;
    }
    set_error(error, "staging 报告汇总不符合有界留样契约");
    return false;
}

BenchmarkParseStatus parse_benchmark_options(
        std::span<const std::wstring_view> arguments,
        BenchmarkOptions& options,
        std::string& error) noexcept {
    try {
        BenchmarkOptions parsed;
        bool model_set = false;
        bool report_set = false;
        bool ready_file_set = false;
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
                              "--backend 必须是 tensorrt/cuda/directml/"
                              "openvino/cpu");
                    return BenchmarkParseStatus::INVALID;
                }
                parsed.backend_explicit = true;
            } else if (argument == L"--openvino-device") {
                if (!parse_openvino_device(value, parsed.openvino_device)) {
                    set_error(error,
                              "--openvino-device 必须是 gpu/cpu/npu");
                    return BenchmarkParseStatus::INVALID;
                }
                parsed.openvino_device_explicit = true;
            } else if (argument == L"--report-prefix") {
                if (!wide_to_utf8(value, parsed.report_prefix)) {
                    set_error(error, "--report-prefix 不是合法 UTF-16 路径");
                    return BenchmarkParseStatus::INVALID;
                }
                report_set = true;
            } else if (argument == L"--ready-file") {
                if (ready_file_set ||
                    !wide_to_utf8(value, parsed.ready_file_path) ||
                    parsed.ready_file_path.empty()) {
                    set_error(error,
                              "--ready-file 必须是唯一的合法非空 UTF-16 路径");
                    return BenchmarkParseStatus::INVALID;
                }
                ready_file_set = true;
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
            } else if (argument == L"--performance-probes") {
                if (!parse_switch(
                        value, parsed.enable_performance_probes)) {
                    set_error(error, "--performance-probes 必须是 on/off");
                    return BenchmarkParseStatus::INVALID;
                }
            } else if (argument == L"--d3d11-cuda-interop") {
                if (!parse_switch(
                        value, parsed.enable_d3d11_cuda_interop)) {
                    set_error(error,
                              "--d3d11-cuda-interop 必须是 on/off");
                    return BenchmarkParseStatus::INVALID;
                }
            } else if (argument == L"--d3d11-directml-interop") {
                if (!parse_switch(
                        value, parsed.enable_d3d11_directml_interop)) {
                    set_error(error,
                              "--d3d11-directml-interop 必须是 on/off");
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
        if (options.backend == BackendType::OPENVINO &&
            !options.openvino_device_explicit) {
            set_error(error, "OpenVINO 必须提供 --openvino-device");
            return false;
        }
        if (options.backend != BackendType::OPENVINO &&
            options.openvino_device_explicit) {
            set_error(error,
                      "只有 OpenVINO 后端接受 --openvino-device");
            return false;
        }
        const bool profile_required =
            options.backend == BackendType::TENSORRT ||
            options.backend == BackendType::CUDA ||
            options.backend == BackendType::OPENVINO;
        if (profile_required && options.provider_profile_path.empty()) {
            set_error(error,
                      "TensorRT/CUDA/OpenVINO 必须提供 --provider-profile");
            return false;
        }
        if (!profile_required && !options.provider_profile_path.empty()) {
            set_error(error,
                      "DirectML/CPU 不接受 --provider-profile");
            return false;
        }
        if (options.enable_d3d11_cuda_interop &&
            (options.backend != BackendType::TENSORRT ||
             !options.enable_cuda_graph ||
             !options.enable_gpu_preprocess)) {
            set_error(error,
                "D3D11/CUDA 互操作只支持 TensorRT CUDA Graph GPU 前处理");
            return false;
        }
        if (options.enable_d3d11_directml_interop &&
            options.backend != BackendType::DIRECTML) {
            set_error(error,
                "D3D11/DirectML 互操作只支持严格 DirectML 后端");
            return false;
        }
        if (options.enable_d3d11_cuda_interop &&
            options.enable_d3d11_directml_interop) {
            set_error(error, "两种 D3D11 GPU 互操作模式不能同时启用");
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
        case BackendType::OPENVINO: return "OpenVINOExecutionProvider";
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
        "  --backend NAME           tensorrt/cuda/directml/openvino/cpu\n"
        "  --report-prefix PATH     成功后发布 PATH.csv 和 PATH.json\n\n"
        "进程协调:\n"
        "  --ready-file PATH        Runtime 就绪后原子发布，退出时删除\n\n"
        "Provider 证据:\n"
        "  --provider-profile PATH  TensorRT/CUDA/OpenVINO 必选，独立 ORT trace JSON\n\n"
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
        "  --openvino-device NAME   OpenVINO 必选，gpu/cpu/npu\n"
        "  --fp16 on|off            TensorRT FP16，默认 on\n"
        "  --cuda-graph on|off      TensorRT CUDA Graph，默认 on\n"
        "  --gpu-preprocess on|off  CUDA 前处理，默认 on\n"
        "  --performance-probes on|off  NDI/Runtime 分段探针，默认 off\n"
        "  --d3d11-cuda-interop on|off  DXGI GPU-only 输入，默认 off\n"
        "  --d3d11-directml-interop on|off  DXGI→DML GPU-only 输入，默认 off\n"
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
    std::string csv_staging_path;
    std::string json_staging_path;
    std::string provider_profile_path;
    bool report_outputs_owned = false;
    ReadyFileGuard ready_file(options.ready_file_path);
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
        if (!options.ready_file_path.empty()) {
            const auto ready_path = std::filesystem::absolute(
                std::filesystem::u8path(options.ready_file_path))
                    .lexically_normal();
            const auto same_as_ready = [&](const std::string& path) {
                return !path.empty() &&
                    std::filesystem::absolute(std::filesystem::u8path(path))
                        .lexically_normal() == ready_path;
            };
            if (same_as_ready(csv_path) || same_as_ready(json_path) ||
                same_as_ready(provider_profile_path)) {
                set_error(error,
                    "ready-file 不得与正式报告或 Provider profile 共用路径");
                return false;
            }
            if (std::filesystem::exists(ready_path)) {
                set_error(error, "ready-file 目标已存在，拒绝覆盖: " +
                                  options.ready_file_path);
                return false;
            }
        }
        if (std::filesystem::exists(csv_path) ||
            std::filesystem::exists(json_path) ||
            (!provider_profile_path.empty() &&
             std::filesystem::exists(provider_profile_path))) {
            set_error(error, "报告目标已存在，拒绝覆盖: " +
                              options.report_prefix);
            return false;
        }
        const std::string staging_suffix = ".pending." +
            std::to_string(static_cast<unsigned long long>(
                GetCurrentProcessId())) + '-' +
            std::to_string(static_cast<unsigned long long>(
                GetTickCount64()));
        csv_staging_path = csv_path + staging_suffix;
        json_staging_path = json_path + staging_suffix;
        const std::string debug_temporary_suffix = ".tmp." +
            std::to_string(static_cast<unsigned long long>(
                GetCurrentProcessId()));
        const std::string retention_temporary_suffix =
            ".retention.tmp." +
            std::to_string(static_cast<unsigned long long>(
                GetCurrentProcessId()));
        const auto staging_path_exists = [&](const std::string& path) {
            return std::filesystem::exists(path) ||
                std::filesystem::exists(path + debug_temporary_suffix) ||
                std::filesystem::exists(path + retention_temporary_suffix);
        };
        if (staging_path_exists(csv_staging_path) ||
            staging_path_exists(json_staging_path)) {
            set_error(error, "本轮 staging 报告目标已存在，拒绝覆盖");
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
        config.detector.openvino_device = options.openvino_device;
        config.detector.output_format = options.output_format;
        config.detector.enable_fp16 = options.enable_fp16;
        config.detector.enable_trt_cuda_graph = options.enable_cuda_graph;
        config.detector.enable_gpu_preprocess =
            options.enable_gpu_preprocess;
        config.capture.enable_d3d11_cuda_interop =
            options.enable_d3d11_cuda_interop;
        config.capture.enable_d3d11_directml_interop =
            options.enable_d3d11_directml_interop;
        config.detector.enable_output_fingerprint = false;
        config.detector.enable_ort_profiling = false;
        config.detector.ort_profile_prefix.clear();
        config.runtime.enable_performance_probes =
            options.enable_performance_probes;
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
                    } else if (!ready_file.publish(
                                   running_snapshot, config, options, error)) {
                        runtime.stop();
                    } else {
                        DebugReportConfig report_config;
                        report_config.csv_path = csv_staging_path;
                        report_config.json_path = json_staging_path;
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
                        report_config.performance_probes_enabled =
                            options.enable_performance_probes;
                        {
                            std::vector<RuntimePipelineSample> pending;
                            benchmark::detail::SamplePhaseTracker phase_tracker(
                                options.warmup_samples);
                            benchmark::detail::FormalSampleTracker formal_tracker(
                                kMaximumReportSamples);
                            bool measurement_started = false;
                            auto measurement_started_at = runtime_started;
                            bool gates_satisfied = false;
                            bool run_failed = false;

                            const auto consume_pending = [&]() noexcept {
                                try {
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
                                        if (!validate_performance_probe_sample(
                                                sample, config.capture.backend,
                                                options.enable_performance_probes,
                                                error)) {
                                            return false;
                                        }
                                        const auto& detector_profile =
                                            sample.profile.detector;
                                        if (detector_profile.d3d11_cuda_interop !=
                                                options.enable_d3d11_cuda_interop) {
                                            set_error(error,
                                                "样本 D3D11/CUDA 互操作状态不符合请求");
                                            return false;
                                        }
                                        if (detector_profile.d3d11_directml_interop !=
                                                options.enable_d3d11_directml_interop) {
                                            set_error(error,
                                                "样本 D3D11/DirectML 互操作状态不符合请求");
                                            return false;
                                        }
                                        if (options.enable_d3d11_cuda_interop) {
                                            const std::uint64_t expected_copy =
                                                static_cast<std::uint64_t>(
                                                    sample.geometry.roi_width) *
                                                static_cast<std::uint64_t>(
                                                    sample.geometry.roi_height) * 4U;
                                            if (detector_profile.h2d_ms != 0.0 ||
                                                detector_profile.input_upload_bytes != 0 ||
                                                detector_profile.input_device_copy_bytes !=
                                                    expected_copy ||
                                                !detector_profile.gpu_preprocess ||
                                                !detector_profile.explicit_device_copy) {
                                                set_error(error,
                                                    "互操作样本违反零 host upload 或设备复制台账");
                                                return false;
                                            }
                                        }
                                        if (options.enable_d3d11_directml_interop &&
                                            (detector_profile.h2d_ms != 0.0 ||
                                             detector_profile.input_upload_bytes != 0 ||
                                             detector_profile.input_device_copy_bytes != 0 ||
                                             !detector_profile.gpu_preprocess ||
                                             detector_profile.explicit_device_copy)) {
                                            set_error(error,
                                                "DirectML 互操作样本违反零 host upload/零中间设备复制台账");
                                            return false;
                                        }
                                        benchmark::detail::
                                            SamplePhaseObservation observation;
                                        if (!phase_tracker.observe(
                                                sample, observation, error)) {
                                            return false;
                                        }
                                        if (observation.measurement_begins) {
                                            measurement_started = true;
                                            measurement_started_at =
                                                std::chrono::steady_clock::now();
                                        }
                                        if (observation.phase !=
                                                benchmark::detail::
                                                    CoveragePhase::FORMAL) {
                                            continue;
                                        }
                                        if (!formal_tracker.observe(
                                                sample, error)) {
                                            return false;
                                        }
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
                                        formal_tracker.gates_satisfied(
                                            options.minimum_samples,
                                            measurement_seconds >=
                                                static_cast<long long>(
                                                    options.minimum_seconds));
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
                                formal_tracker.summary().formal_sample_count <
                                    options.minimum_samples) {
                                set_error(error,
                                    "正式成功样本未达到门槛");
                                run_failed = true;
                            }
                            if (!run_failed && !phase_tracker.finish(
                                    final_snapshot, error)) {
                                run_failed = true;
                            }
                            if (!run_failed &&
                                !report.start(report_config, error)) {
                                run_failed = true;
                            }
                            if (!run_failed) {
                                for (const auto retained_span :
                                     formal_tracker.retained_sample_spans()) {
                                    if (!retained_span.empty()) {
                                        report.ingest(retained_span);
                                    }
                                }
                                // DebugReport 已复制尾窗；在构造 CSV/JSON 前释放
                                // tracker 库存，避免序列化阶段继续持有双份样本。
                                formal_tracker.release_retained_storage();
                            }
                            const auto& formal_summary =
                                formal_tracker.summary();
                            if (!run_failed &&
                                benchmark::detail::finalize_report(
                                    report, final_snapshot,
                                    phase_tracker.coverage(),
                                    formal_summary,
                                    phase_tracker.formal_successful(),
                                    options.enable_performance_probes,
                                    config.capture.backend, error)) {
                                const auto& summary = report.summary();
                                const std::uint64_t formal_samples =
                                    formal_summary.formal_sample_count;
                                const std::size_t retained_samples =
                                    formal_summary.retained_sample_count;
                                const auto omitted_samples =
                                    formal_summary.omitted_sample_count;
                                bool metadata_valid = true;
                                if (omitted_samples > 0) {
                                    metadata_valid =
                                        rewrite_report_samples_dropped(
                                            csv_staging_path,
                                            benchmark::detail::
                                                ReportFileFormat::CSV,
                                            omitted_samples, error) &&
                                        rewrite_report_samples_dropped(
                                            json_staging_path,
                                            benchmark::detail::
                                                ReportFileFormat::JSON,
                                            omitted_samples, error);
                                } else {
                                    std::uint64_t csv_omitted = 0;
                                    std::uint64_t json_omitted = 0;
                                    metadata_valid =
                                        read_report_samples_dropped(
                                            csv_staging_path,
                                            benchmark::detail::
                                                ReportFileFormat::CSV,
                                            csv_omitted, error) &&
                                        read_report_samples_dropped(
                                            json_staging_path,
                                            benchmark::detail::
                                                ReportFileFormat::JSON,
                                            json_omitted, error) &&
                                        csv_omitted == 0 &&
                                        json_omitted == 0;
                                }
                                if (!metadata_valid && error.empty()) {
                                    set_error(error,
                                        "CSV/JSON staging 省略计数不一致");
                                }
                                if (metadata_valid &&
                                    publish_benchmark_reports(
                                        csv_staging_path,
                                        json_staging_path,
                                        csv_path, json_path, error)) {
                                    success = true;
                                    LOG_INFO(
                                        "benchmark",
                                        "正式基准完成: startup={}, warmup={}, formal_samples={}, "
                                        "retained_samples={}, omitted_samples={}, "
                                        "retained_total_p50={:.3f}ms, "
                                        "retained_total_p95={:.3f}ms, "
                                        "retained_total_p99={:.3f}ms",
                                        phase_tracker.startup_successful(),
                                        phase_tracker.warmup_successful(),
                                        formal_samples,
                                        retained_samples,
                                        omitted_samples,
                                        summary.total.p50_ms,
                                        summary.total.p95_ms,
                                        summary.total.p99_ms);
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
            // 正式和 staging 目标在入口均确认不存在，因此这里只清理本轮
            // 创建的精确 CSV、JSON、临时文件和 Provider profile。补写或成对
            // 发布失败时不能留下可被误认为有效结果的单个文件。
            remove_benchmark_outputs(
                csv_path, json_path,
                csv_staging_path, json_staging_path,
                provider_profile_path);
        }
        Log::shutdown();
        if (success) error.clear();
        return success;
    } catch (const std::exception& exception) {
        if (report_outputs_owned) {
            remove_benchmark_outputs(
                csv_path, json_path,
                csv_staging_path, json_staging_path,
                provider_profile_path);
        }
        Log::shutdown();
        set_error(error, std::string("执行 Runtime 基准异常: ") +
                          exception.what());
        return false;
    } catch (...) {
        if (report_outputs_owned) {
            remove_benchmark_outputs(
                csv_path, json_path,
                csv_staging_path, json_staging_path,
                provider_profile_path);
        }
        Log::shutdown();
        set_error(error, "执行 Runtime 基准时发生未知异常");
        return false;
    }
}
