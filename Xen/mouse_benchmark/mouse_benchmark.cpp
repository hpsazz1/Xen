#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>

#ifdef ERROR
#undef ERROR
#endif

#include "mouse_benchmark/mouse_benchmark.h"

#include "crash/crash.h"
#include "log/log.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>

namespace {

constexpr std::wstring_view kPhysicalOutputConfirmation =
    L"XEN_MOUSE_BENCHMARK_SENDS_REAL_INPUT";
constexpr std::uint64_t kMaximumWarmupPairs = 100000;
constexpr std::uint64_t kMaximumSamplePairs = 500000;

std::atomic<bool> stop_requested{false};

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

bool finite_nonnegative(double value) noexcept {
    return std::isfinite(value) && value >= 0.0;
}

bool valid_kmbox_uuid(std::string_view value) noexcept {
    if (value.size() != 8U) return false;
    return std::all_of(value.begin(), value.end(), [](char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f') ||
               (character >= 'A' && character <= 'F');
    });
}

bool valid_makcu_port(std::string_view value) noexcept {
    if (value.size() < 4U || value.size() > 6U ||
        (value[0] != 'C' && value[0] != 'c') ||
        (value[1] != 'O' && value[1] != 'o') ||
        (value[2] != 'M' && value[2] != 'm') ||
        (value.size() > 4U && value[3] == '0')) {
        return false;
    }
    int port_number = 0;
    for (std::size_t index = 3U; index < value.size(); ++index) {
        const char character = value[index];
        if (character < '0' || character > '9') return false;
        port_number = port_number * 10 + static_cast<int>(character - '0');
    }
    return port_number >= 1 && port_number <= 256;
}

std::string mouse_endpoint(const MouseConfig& config) {
    if (config.backend == MouseBackend::KMBOX_NET) {
        return config.kmbox_ip + ":" + std::to_string(config.kmbox_port);
    }
    if (config.backend == MouseBackend::MAKCU) return config.makcu_port;
    return {};
}

int mouse_connect_timeout_ms(const MouseConfig& config) noexcept {
    if (config.backend == MouseBackend::KMBOX_NET) {
        return config.kmbox_connect_timeout_ms;
    }
    if (config.backend == MouseBackend::MAKCU) {
        return config.makcu_connect_timeout_ms;
    }
    return 0;
}

int mouse_command_timeout_ms(const MouseConfig& config) noexcept {
    if (config.backend == MouseBackend::KMBOX_NET) {
        return config.kmbox_command_timeout_ms;
    }
    if (config.backend == MouseBackend::MAKCU) {
        return config.makcu_command_timeout_ms;
    }
    return 0;
}

double percentile(std::vector<double> values, double quantile) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const double position = quantile *
        static_cast<double>(values.size() - 1U);
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const auto upper = static_cast<std::size_t>(std::ceil(position));
    if (lower == upper) return values[lower];
    const double fraction = position - static_cast<double>(lower);
    return values[lower] * (1.0 - fraction) + values[upper] * fraction;
}

std::string json_escape(std::string_view input) {
    std::string output;
    output.reserve(input.size() + 16U);
    constexpr char hexadecimal[] = "0123456789abcdef";
    for (const unsigned char value : input) {
        switch (value) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (value < 0x20U) {
                    output += "\\u00";
                    output.push_back(hexadecimal[value >> 4U]);
                    output.push_back(hexadecimal[value & 0x0FU]);
                } else {
                    output.push_back(static_cast<char>(value));
                }
                break;
        }
    }
    return output;
}

bool valid_summary(const MouseBenchmarkTimingSummary& summary) noexcept {
    return finite_nonnegative(summary.mean_ms) &&
           finite_nonnegative(summary.p50_ms) &&
           finite_nonnegative(summary.p95_ms) &&
           finite_nonnegative(summary.p99_ms) &&
           finite_nonnegative(summary.max_ms) &&
           summary.p50_ms <= summary.p95_ms &&
           summary.p95_ms <= summary.p99_ms &&
           summary.p99_ms <= summary.max_ms;
}

bool valid_report_data(const MouseBenchmarkOptions& options,
                       const MouseBenchmarkResult& result) noexcept {
    const std::uint64_t expected_formal_commands = options.sample_pairs * 2U;
    const std::uint64_t expected_total_commands =
        (options.warmup_pairs + options.sample_pairs + 1U) * 2U;
    if (!result.complete || result.failed_commands != 0 ||
        result.final_status != MouseStatus::READY ||
        !finite_nonnegative(result.open_ms) ||
        !finite_nonnegative(result.first_command_ms) ||
        !finite_nonnegative(result.first_compensation_ms) ||
        !finite_nonnegative(result.warmup_elapsed_ms) ||
        !finite_nonnegative(result.formal_elapsed_ms) ||
        !finite_nonnegative(result.total_elapsed_ms) ||
        !valid_summary(result.command_latency) ||
        options.sample_pairs >
            std::numeric_limits<std::uint64_t>::max() / 2U ||
        result.samples.size() != expected_formal_commands ||
        result.formal_successful_commands != expected_formal_commands ||
        result.successful_commands != expected_total_commands) {
        return false;
    }
    for (std::size_t index = 0; index < result.samples.size(); ++index) {
        const auto& sample = result.samples[index];
        const int expected_direction = index % 2U == 0 ? 1 : -1;
        if (sample.sequence != index + 1U ||
            sample.pair_index != index / 2U + 1U ||
            sample.direction != expected_direction ||
            sample.dx_counts != options.dx_counts * expected_direction ||
            sample.dy_counts != options.dy_counts * expected_direction ||
            !finite_nonnegative(sample.latency_ms)) {
            return false;
        }
    }
    return true;
}

bool write_json(const std::filesystem::path& path,
                const MouseBenchmarkOptions& options,
                const MouseBenchmarkResult& result) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output << std::setprecision(9)
           << "{\n  \"schema\": 1,\n  \"complete\": true,\n"
           << "  \"backend\": \""
           << MouseBackendName(options.mouse.backend) << "\",\n"
           << "  \"endpoint\": \""
           << json_escape(mouse_endpoint(options.mouse))
           << "\",\n"
           << "  \"authorization\": {\"physical_output\": true, "
              "\"confirmation_token\": true},\n"
           << "  \"command\": {\"dx_counts\": "
           << options.dx_counts << ", \"dy_counts\": "
           << options.dy_counts << ", \"paired_reverse\": true},\n"
           << "  \"configuration\": {\"warmup_pairs\": "
           << options.warmup_pairs << ", \"sample_pairs\": "
           << options.sample_pairs
           << ", \"connect_timeout_ms\": "
           << mouse_connect_timeout_ms(options.mouse)
           << ", \"command_timeout_ms\": "
           << mouse_command_timeout_ms(options.mouse)
           << ", \"baud_rate\": "
           << (options.mouse.backend == MouseBackend::MAKCU
                   ? options.mouse.makcu_baud_rate : 0)
           << ", \"kmbox_uuid_recorded\": false},\n"
           << "  \"stats\": {\"successful_commands\": "
           << result.successful_commands
           << ", \"formal_successful_commands\": "
           << result.formal_successful_commands
           << ", \"failed_commands\": "
           << result.failed_commands << ", \"final_status\": \""
           << MouseStatusName(result.final_status) << "\"},\n"
           << "  \"timing\": {\n"
           << "    \"open_ms\": " << result.open_ms << ",\n"
           << "    \"first_command_ms\": "
           << result.first_command_ms << ",\n"
           << "    \"first_compensation_ms\": "
           << result.first_compensation_ms << ",\n"
           << "    \"warmup_elapsed_ms\": "
           << result.warmup_elapsed_ms << ",\n"
           << "    \"formal_elapsed_ms\": "
           << result.formal_elapsed_ms << ",\n"
           << "    \"total_elapsed_ms\": "
           << result.total_elapsed_ms << ",\n"
           << "    \"command_latency\": {\"mean_ms\": "
           << result.command_latency.mean_ms << ", \"p50_ms\": "
           << result.command_latency.p50_ms << ", \"p95_ms\": "
           << result.command_latency.p95_ms << ", \"p99_ms\": "
           << result.command_latency.p99_ms << ", \"max_ms\": "
           << result.command_latency.max_ms << "}\n  },\n"
           << "  \"samples\": [\n";
    for (std::size_t index = 0; index < result.samples.size(); ++index) {
        const auto& sample = result.samples[index];
        output << "    {\"sequence\": " << sample.sequence
               << ", \"pair_index\": " << sample.pair_index
               << ", \"direction\": " << sample.direction
               << ", \"dx_counts\": " << sample.dx_counts
               << ", \"dy_counts\": " << sample.dy_counts
               << ", \"latency_ms\": " << sample.latency_ms << '}';
        if (index + 1U != result.samples.size()) output << ',';
        output << '\n';
    }
    output << "  ]\n}\n";
    output.flush();
    const bool succeeded = output.good();
    output.close();
    return succeeded;
}

std::string move_failure_message(const char* phase,
                                 std::uint64_t index,
                                 const IMouseController& mouse) {
    return std::string(phase) + "命令失败: index=" +
           std::to_string(index) + ", status=" +
           MouseStatusName(mouse.status()) + ", error=" +
           mouse.last_error();
}

} // namespace

MouseBenchmarkTimingSummary summarize_mouse_benchmark_timings(
        std::span<const double> values) noexcept {
    MouseBenchmarkTimingSummary summary;
    try {
        if (values.empty()) return summary;
        std::vector<double> copy(values.begin(), values.end());
        for (const double value : copy) {
            if (!finite_nonnegative(value)) return {};
        }
        summary.mean_ms = std::accumulate(
            copy.begin(), copy.end(), 0.0) / static_cast<double>(copy.size());
        summary.p50_ms = percentile(copy, 0.50);
        summary.p95_ms = percentile(copy, 0.95);
        summary.p99_ms = percentile(copy, 0.99);
        summary.max_ms = *std::max_element(copy.begin(), copy.end());
    } catch (...) {
        return {};
    }
    return summary;
}

bool validate_mouse_benchmark_options(
        const MouseBenchmarkOptions& options,
        std::string& error) noexcept {
    if (!options.backend_explicit) {
        set_error(error, "必须通过 --backend 显式选择鼠标后端");
        return false;
    }
    if (options.report_path.empty()) {
        set_error(error, "必须提供 --report");
        return false;
    }
    if (!options.allow_physical_output ||
        !options.physical_output_confirmed) {
        set_error(error,
            "鼠标基准会发送真实输入，必须同时提供物理输出开关和确认令牌");
        return false;
    }
    if (options.warmup_pairs > kMaximumWarmupPairs ||
        options.sample_pairs == 0 ||
        options.sample_pairs > kMaximumSamplePairs) {
        set_error(error, "warmup/sample pairs 超出固定容量边界");
        return false;
    }
    if ((options.dx_counts == 0 && options.dy_counts == 0) ||
        options.dx_counts < -32767 || options.dx_counts > 32767 ||
        options.dy_counts < -32767 || options.dy_counts > 32767) {
        set_error(error,
            "位移必须非零且每轴位于 -32767..32767，保证反向命令可表示");
        return false;
    }
    if (options.mouse.backend == MouseBackend::KMBOX_NET) {
        if (options.mouse.kmbox_ip.empty() ||
            options.mouse.kmbox_port <= 0 ||
            options.mouse.kmbox_port > 65535 ||
            !valid_kmbox_uuid(options.mouse.kmbox_uuid) ||
            options.mouse.kmbox_connect_timeout_ms <= 0 ||
            options.mouse.kmbox_connect_timeout_ms > 10000 ||
            options.mouse.kmbox_command_timeout_ms <= 0 ||
            options.mouse.kmbox_command_timeout_ms > 1000) {
            set_error(error, "KMBOX NET 地址、UUID 或超时参数非法");
            return false;
        }
        if (!options.mouse.makcu_port.empty()) {
            set_error(error, "KMBOX NET 后端不得携带 MAKCU 设备参数");
            return false;
        }
    } else if (options.mouse.backend == MouseBackend::MAKCU) {
        if (!valid_makcu_port(options.mouse.makcu_port) ||
            (options.mouse.makcu_baud_rate != 115200 &&
             options.mouse.makcu_baud_rate != 4000000) ||
            options.mouse.makcu_connect_timeout_ms <= 0 ||
            options.mouse.makcu_connect_timeout_ms > 10000 ||
            options.mouse.makcu_command_timeout_ms <= 0 ||
            options.mouse.makcu_command_timeout_ms > 1000) {
            set_error(error, "MAKCU COM 口、波特率或超时参数非法");
            return false;
        }
        if (!options.mouse.kmbox_ip.empty() ||
            options.mouse.kmbox_port != 0 ||
            !options.mouse.kmbox_uuid.empty()) {
            set_error(error, "MAKCU 后端不得携带 KMBOX 设备参数");
            return false;
        }
    } else if (options.mouse.backend == MouseBackend::WIN32_SEND_INPUT) {
        if (!options.mouse.kmbox_ip.empty() ||
            options.mouse.kmbox_port != 0 ||
            !options.mouse.kmbox_uuid.empty() ||
            !options.mouse.makcu_port.empty()) {
            set_error(error, "Win32 后端不得携带物理设备参数");
            return false;
        }
    } else {
        set_error(error, "鼠标基准后端非法");
        return false;
    }
    error.clear();
    return true;
}

MouseBenchmarkParseStatus parse_mouse_benchmark_options(
        std::span<const std::wstring_view> arguments,
        MouseBenchmarkOptions& options,
        std::string& error) noexcept {
    try {
        options = {};
        error.clear();
        bool seen_backend = false;
        bool seen_report = false;
        bool seen_warmup = false;
        bool seen_samples = false;
        bool seen_dx = false;
        bool seen_dy = false;
        bool seen_allow = false;
        bool seen_confirmation = false;
        bool seen_ip = false;
        bool seen_port = false;
        bool seen_uuid = false;
        bool seen_connect_timeout = false;
        bool seen_command_timeout = false;
        bool seen_makcu_port = false;
        bool seen_makcu_baud_rate = false;

        for (std::size_t index = 0; index < arguments.size(); ++index) {
            const auto argument = arguments[index];
            if (argument == L"--help" || argument == L"-h") {
                return MouseBenchmarkParseStatus::HELP;
            }
            if (argument == L"--allow-physical-output") {
                if (seen_allow) {
                    set_error(error, "--allow-physical-output 重复");
                    return MouseBenchmarkParseStatus::INVALID;
                }
                seen_allow = true;
                options.allow_physical_output = true;
                continue;
            }
            if (index + 1U >= arguments.size()) {
                set_error(error, "参数缺少值");
                return MouseBenchmarkParseStatus::INVALID;
            }
            const auto value = arguments[++index];
            const auto duplicate = [&](bool& seen, const char* name) {
                if (!seen) {
                    seen = true;
                    return false;
                }
                set_error(error, std::string(name) + " 重复");
                return true;
            };

            if (argument == L"--backend") {
                if (duplicate(seen_backend, "--backend"))
                    return MouseBenchmarkParseStatus::INVALID;
                if (value == L"win32") {
                    options.mouse.backend = MouseBackend::WIN32_SEND_INPUT;
                } else if (value == L"kmbox_net") {
                    options.mouse.backend = MouseBackend::KMBOX_NET;
                } else if (value == L"makcu") {
                    options.mouse.backend = MouseBackend::MAKCU;
                } else {
                    set_error(error,
                              "--backend 仅支持 win32、kmbox_net 或 makcu");
                    return MouseBenchmarkParseStatus::INVALID;
                }
                options.backend_explicit = true;
            } else if (argument == L"--report") {
                if (duplicate(seen_report, "--report") ||
                    !wide_to_utf8(value, options.report_path)) {
                    if (error.empty()) set_error(error, "--report 不是有效 UTF-8 路径");
                    return MouseBenchmarkParseStatus::INVALID;
                }
            } else if (argument == L"--warmup-pairs") {
                if (duplicate(seen_warmup, "--warmup-pairs") ||
                    !parse_number(value, options.warmup_pairs)) {
                    if (error.empty()) set_error(error, "--warmup-pairs 非法");
                    return MouseBenchmarkParseStatus::INVALID;
                }
            } else if (argument == L"--sample-pairs") {
                if (duplicate(seen_samples, "--sample-pairs") ||
                    !parse_number(value, options.sample_pairs)) {
                    if (error.empty()) set_error(error, "--sample-pairs 非法");
                    return MouseBenchmarkParseStatus::INVALID;
                }
            } else if (argument == L"--dx-counts") {
                if (duplicate(seen_dx, "--dx-counts") ||
                    !parse_number(value, options.dx_counts)) {
                    if (error.empty()) set_error(error, "--dx-counts 非法");
                    return MouseBenchmarkParseStatus::INVALID;
                }
            } else if (argument == L"--dy-counts") {
                if (duplicate(seen_dy, "--dy-counts") ||
                    !parse_number(value, options.dy_counts)) {
                    if (error.empty()) set_error(error, "--dy-counts 非法");
                    return MouseBenchmarkParseStatus::INVALID;
                }
            } else if (argument == L"--confirm-physical-output") {
                if (duplicate(seen_confirmation,
                              "--confirm-physical-output")) {
                    return MouseBenchmarkParseStatus::INVALID;
                }
                options.physical_output_confirmed =
                    value == kPhysicalOutputConfirmation;
                if (!options.physical_output_confirmed) {
                    set_error(error, "物理输出确认令牌不匹配");
                    return MouseBenchmarkParseStatus::INVALID;
                }
            } else if (argument == L"--kmbox-ip") {
                if (duplicate(seen_ip, "--kmbox-ip") ||
                    !wide_to_utf8(value, options.mouse.kmbox_ip)) {
                    if (error.empty()) set_error(error, "--kmbox-ip 非法");
                    return MouseBenchmarkParseStatus::INVALID;
                }
            } else if (argument == L"--kmbox-port") {
                if (duplicate(seen_port, "--kmbox-port") ||
                    !parse_number(value, options.mouse.kmbox_port)) {
                    if (error.empty()) set_error(error, "--kmbox-port 非法");
                    return MouseBenchmarkParseStatus::INVALID;
                }
            } else if (argument == L"--kmbox-uuid") {
                if (duplicate(seen_uuid, "--kmbox-uuid") ||
                    !wide_to_utf8(value, options.mouse.kmbox_uuid)) {
                    if (error.empty()) set_error(error, "--kmbox-uuid 非法");
                    return MouseBenchmarkParseStatus::INVALID;
                }
            } else if (argument == L"--makcu-port") {
                if (duplicate(seen_makcu_port, "--makcu-port") ||
                    !wide_to_utf8(value, options.mouse.makcu_port)) {
                    if (error.empty()) set_error(error, "--makcu-port 非法");
                    return MouseBenchmarkParseStatus::INVALID;
                }
            } else if (argument == L"--makcu-baud-rate") {
                if (duplicate(seen_makcu_baud_rate,
                              "--makcu-baud-rate") ||
                    !parse_number(value, options.mouse.makcu_baud_rate)) {
                    if (error.empty()) {
                        set_error(error, "--makcu-baud-rate 非法");
                    }
                    return MouseBenchmarkParseStatus::INVALID;
                }
            } else if (argument == L"--connect-timeout-ms") {
                if (duplicate(seen_connect_timeout,
                              "--connect-timeout-ms") ||
                    !parse_number(value,
                                  options.mouse.kmbox_connect_timeout_ms)) {
                    if (error.empty()) set_error(error, "--connect-timeout-ms 非法");
                    return MouseBenchmarkParseStatus::INVALID;
                }
                options.mouse.makcu_connect_timeout_ms =
                    options.mouse.kmbox_connect_timeout_ms;
            } else if (argument == L"--command-timeout-ms") {
                if (duplicate(seen_command_timeout,
                              "--command-timeout-ms") ||
                    !parse_number(value,
                                  options.mouse.kmbox_command_timeout_ms)) {
                    if (error.empty()) set_error(error, "--command-timeout-ms 非法");
                    return MouseBenchmarkParseStatus::INVALID;
                }
                options.mouse.makcu_command_timeout_ms =
                    options.mouse.kmbox_command_timeout_ms;
            } else {
                set_error(error, "未知参数");
                return MouseBenchmarkParseStatus::INVALID;
            }
        }
        if (!validate_mouse_benchmark_options(options, error)) {
            return MouseBenchmarkParseStatus::INVALID;
        }
        if (options.mouse.backend == MouseBackend::WIN32_SEND_INPUT &&
            (seen_ip || seen_port || seen_uuid ||
             seen_makcu_port || seen_makcu_baud_rate ||
             seen_connect_timeout || seen_command_timeout)) {
            set_error(error, "Win32 后端不得携带物理设备专属参数");
            return MouseBenchmarkParseStatus::INVALID;
        }
        if (options.mouse.backend == MouseBackend::KMBOX_NET &&
            (seen_makcu_port || seen_makcu_baud_rate)) {
            set_error(error, "KMBOX NET 后端不得携带 MAKCU 专属参数");
            return MouseBenchmarkParseStatus::INVALID;
        }
        if (options.mouse.backend == MouseBackend::MAKCU &&
            (seen_ip || seen_port || seen_uuid)) {
            set_error(error, "MAKCU 后端不得携带 KMBOX 专属参数");
            return MouseBenchmarkParseStatus::INVALID;
        }
        return MouseBenchmarkParseStatus::READY;
    } catch (...) {
        set_error(error, "解析鼠标基准参数时发生未知异常");
        return MouseBenchmarkParseStatus::INVALID;
    }
}

bool write_mouse_benchmark_report(
        const MouseBenchmarkOptions& options,
        const MouseBenchmarkResult& result,
        std::string& error) noexcept {
    std::filesystem::path temporary_path;
    try {
        if (!validate_mouse_benchmark_options(options, error) ||
            !valid_report_data(options, result)) {
            if (error.empty()) set_error(error, "鼠标基准报告参数或样本非法");
            return false;
        }
        const auto final_path = std::filesystem::absolute(
            std::filesystem::u8path(options.report_path));
        if (std::filesystem::exists(final_path)) {
            set_error(error, "鼠标基准报告目标已存在，拒绝覆盖");
            return false;
        }
        const auto parent = final_path.parent_path();
        std::error_code directory_error;
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, directory_error);
        }
        if (directory_error ||
            (!parent.empty() && !std::filesystem::is_directory(parent))) {
            set_error(error, "鼠标基准报告目录创建失败");
            return false;
        }
        temporary_path = final_path;
        temporary_path += L".pending-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64());
        if (std::filesystem::exists(temporary_path)) {
            set_error(error, "鼠标基准报告临时目标已存在，拒绝覆盖");
            temporary_path.clear();
            return false;
        }
        if (!write_json(temporary_path, options, result)) {
            set_error(error, "鼠标基准报告临时文件写入失败");
            std::error_code ignored;
            std::filesystem::remove(temporary_path, ignored);
            temporary_path.clear();
            return false;
        }
        if (!MoveFileExW(temporary_path.c_str(), final_path.c_str(),
                         MOVEFILE_WRITE_THROUGH)) {
            set_error(error, "鼠标基准报告原子发布失败，Win32Error=" +
                std::to_string(GetLastError()));
            std::error_code ignored;
            std::filesystem::remove(temporary_path, ignored);
            temporary_path.clear();
            return false;
        }
        temporary_path.clear();
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        set_error(error, std::string("鼠标基准报告异常: ") +
                          exception.what());
    } catch (...) {
        set_error(error, "鼠标基准报告发生未知异常");
    }
    if (!temporary_path.empty()) {
        std::error_code ignored;
        std::filesystem::remove(temporary_path, ignored);
    }
    return false;
}

bool run_mouse_benchmark(
        const MouseBenchmarkOptions& options,
        MouseBenchmarkResult& result,
        std::string& error) noexcept {
    result = {};
    if (!validate_mouse_benchmark_options(options, error)) return false;
    bool owns_log = false;
    try {
        const auto report_path = std::filesystem::absolute(
            std::filesystem::u8path(options.report_path));
        if (std::filesystem::exists(report_path)) {
            set_error(error, "鼠标基准报告目标已存在，物理输出前拒绝运行");
            return false;
        }
        const auto report_directory = report_path.parent_path();
        std::error_code directory_error;
        if (!report_directory.empty()) {
            std::filesystem::create_directories(
                report_directory, directory_error);
        }
        if (directory_error ||
            (!report_directory.empty() &&
             !std::filesystem::is_directory(report_directory))) {
            set_error(error, "鼠标基准报告目录在物理输出前创建失败");
            return false;
        }
        // 从这里开始属于新会话；随后到达的 Ctrl+C 在设备初始化和握手期间也必须保留。
        stop_requested.store(false, std::memory_order_release);

        owns_log = !Log::initialized();
        if (owns_log) {
            LogConfig log_config;
            log_config.enable_debug_file = false;
            Log::init(log_config);
        }
        if (!Log::initialized()) {
            set_error(error, "Log 初始化失败");
            return false;
        }
        Log::register_module("mouse_benchmark", LogLevel::INFO);
        bool success = false;
        {
            CrashHandler crash_handler;
            if (!crash_handler.install("logs")) {
                set_error(error, "鼠标基准崩溃诊断安装失败");
            } else {
                MouseConfig mouse_config = options.mouse;
                // 只有双重 CLI 授权通过后，才在局部生产配置中打开物理输出门。
                mouse_config.allow_send_input = true;
                auto mouse = MouseDeviceFactory::create(mouse_config);
                if (!mouse) {
                    set_error(error, "创建鼠标后端失败");
                } else {
                    const auto run_started = std::chrono::steady_clock::now();
                    const auto open_started = std::chrono::steady_clock::now();
                    const bool opened = mouse->open();
                    const auto open_finished = std::chrono::steady_clock::now();
                    result.open_ms = std::chrono::duration<double, std::milli>(
                        open_finished - open_started).count();
                    if (!opened) {
                        result.final_status = mouse->status();
                        set_error(error, "鼠标后端打开失败: status=" +
                            std::string(MouseStatusName(mouse->status())) +
                            ", error=" + mouse->last_error());
                    } else {
                        const MouseMoveCommand forward{
                            options.dx_counts, options.dy_counts};
                        const MouseMoveCommand reverse{
                            -options.dx_counts, -options.dy_counts};
                        const auto timed_move = [&](
                                const MouseMoveCommand& command,
                                double& latency_ms) noexcept {
                            const auto started = std::chrono::steady_clock::now();
                            const bool moved = mouse->move(command);
                            const auto finished = std::chrono::steady_clock::now();
                            latency_ms =
                                std::chrono::duration<double, std::milli>(
                                    finished - started).count();
                            return moved;
                        };

                        bool run_ok = !stop_requested.load(
                            std::memory_order_acquire);
                        if (!run_ok) {
                            set_error(error, "鼠标基准在首条命令前被人工中止");
                        }
                        if (run_ok) {
                            run_ok = timed_move(
                                forward, result.first_command_ms);
                            if (run_ok) ++result.successful_commands;
                        }
                        if (!run_ok) {
                            if (mouse->status() != MouseStatus::READY) {
                                ++result.failed_commands;
                                set_error(error, move_failure_message(
                                    "首条", 1, *mouse));
                            }
                        } else if (!timed_move(
                                       reverse,
                                       result.first_compensation_ms)) {
                            ++result.failed_commands;
                            run_ok = false;
                            set_error(error, move_failure_message(
                                "首条补偿", 2, *mouse));
                        } else {
                            ++result.successful_commands;
                        }

                        const auto warmup_started =
                            std::chrono::steady_clock::now();
                        for (std::uint64_t pair = 0;
                             run_ok && pair < options.warmup_pairs; ++pair) {
                            if (stop_requested.load(std::memory_order_acquire)) {
                                set_error(error, "鼠标基准被人工中止");
                                run_ok = false;
                                break;
                            }
                            double ignored = 0.0;
                            if (!timed_move(forward, ignored)) {
                                ++result.failed_commands;
                                set_error(error, move_failure_message(
                                    "预热正向", pair * 2U + 1U, *mouse));
                                run_ok = false;
                            } else {
                                ++result.successful_commands;
                            }
                            if (run_ok && !timed_move(reverse, ignored)) {
                                ++result.failed_commands;
                                set_error(error, move_failure_message(
                                    "预热反向", pair * 2U + 2U, *mouse));
                                run_ok = false;
                            } else if (run_ok) {
                                ++result.successful_commands;
                            }
                        }
                        result.warmup_elapsed_ms =
                            std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() -
                                warmup_started).count();

                        if (run_ok) {
                            result.samples.reserve(static_cast<std::size_t>(
                                options.sample_pairs * 2U));
                        }
                        const auto formal_started =
                            std::chrono::steady_clock::now();
                        for (std::uint64_t pair = 0;
                             run_ok && pair < options.sample_pairs; ++pair) {
                            if (stop_requested.load(std::memory_order_acquire)) {
                                set_error(error, "鼠标基准被人工中止");
                                run_ok = false;
                                break;
                            }
                            for (int direction : {1, -1}) {
                                double latency_ms = 0.0;
                                const auto& command =
                                    direction > 0 ? forward : reverse;
                                if (!timed_move(command, latency_ms)) {
                                    ++result.failed_commands;
                                    set_error(error, move_failure_message(
                                        "正式",
                                        pair * 2U +
                                            (direction > 0 ? 1U : 2U),
                                        *mouse));
                                    run_ok = false;
                                    break;
                                }
                                MouseBenchmarkSample sample;
                                sample.sequence = result.samples.size() + 1U;
                                sample.pair_index = pair + 1U;
                                sample.direction = direction;
                                sample.dx_counts = command.dx_counts;
                                sample.dy_counts = command.dy_counts;
                                sample.latency_ms = latency_ms;
                                result.samples.push_back(sample);
                                ++result.successful_commands;
                                ++result.formal_successful_commands;
                            }
                        }
                        result.formal_elapsed_ms =
                            std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() -
                                formal_started).count();
                        result.total_elapsed_ms =
                            std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() -
                                run_started).count();
                        result.final_status = mouse->status();
                        mouse->close();

                        if (run_ok &&
                            result.samples.size() == options.sample_pairs * 2U &&
                            result.final_status == MouseStatus::READY) {
                            std::vector<double> latencies;
                            latencies.reserve(result.samples.size());
                            for (const auto& sample : result.samples) {
                                latencies.push_back(sample.latency_ms);
                            }
                            result.command_latency =
                                summarize_mouse_benchmark_timings(latencies);
                            result.complete = true;
                            success = write_mouse_benchmark_report(
                                options, result, error);
                            if (success) {
                                LOG_INFO(
                                    "mouse_benchmark",
                                    "鼠标基准完成: backend={}, commands={}, "
                                    "p50={:.3f}ms, p95={:.3f}ms, p99={:.3f}ms",
                                    MouseBackendName(options.mouse.backend),
                                    result.successful_commands,
                                    result.command_latency.p50_ms,
                                    result.command_latency.p95_ms,
                                    result.command_latency.p99_ms);
                            }
                        } else if (run_ok) {
                            set_error(error,
                                "鼠标基准完成计数或最终状态不符合发布条件");
                        }
                    }
                    if (mouse->status() != MouseStatus::CLOSED) mouse->close();
                }
                crash_handler.uninstall();
            }
        }
        if (owns_log) Log::shutdown();
        if (success) error.clear();
        return success;
    } catch (const std::exception& exception) {
        if (owns_log) Log::shutdown();
        set_error(error, std::string("执行鼠标基准异常: ") +
                          exception.what());
        return false;
    } catch (...) {
        if (owns_log) Log::shutdown();
        set_error(error, "执行鼠标基准时发生未知异常");
        return false;
    }
}

std::string mouse_benchmark_usage() {
    return
        "用法: XenMouseBenchmark --backend <win32|kmbox_net|makcu> --report <json>\n"
        "  --allow-physical-output\n"
        "  --confirm-physical-output XEN_MOUSE_BENCHMARK_SENDS_REAL_INPUT\n"
        "  [--warmup-pairs 100] [--sample-pairs 10000]\n"
        "  [--dx-counts 1] [--dy-counts 0]\n"
        "  KMBOX: --kmbox-ip <IPv4> --kmbox-port <1..65535>\n"
        "         --kmbox-uuid <8位十六进制>\n"
        "  MAKCU: --makcu-port <COM1..COM256> [--makcu-baud-rate 115200|4000000]\n"
        "  设备:  [--connect-timeout-ms 1000] [--command-timeout-ms 300]\n"
        "说明: 工具会向系统或设备发送真实相对移动，每组均发送等量反向命令。\n";
}

void request_mouse_benchmark_stop() noexcept {
    stop_requested.store(true, std::memory_order_release);
}
