#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>

#ifdef ERROR
#undef ERROR
#endif

#include "sender/report.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <string_view>

namespace sender::detail {
namespace {

struct TimingSummary {
    double mean_ms = 0.0;
    double p50_ms = 0.0;
    double p95_ms = 0.0;
    double p99_ms = 0.0;
    double max_ms = 0.0;
};

void set_error(std::string& output, const std::string& value) noexcept {
    try {
        output = value;
    } catch (...) {
    }
}

bool finite_nonnegative(double value) noexcept {
    return std::isfinite(value) && value >= 0.0;
}

bool valid_geometry(const SenderRunGeometry& geometry) noexcept {
    return geometry.source_width > 0 && geometry.source_height > 0 &&
           geometry.encoded_width > 0 && geometry.encoded_height > 0 &&
           geometry.roi_x >= 0 && geometry.roi_y >= 0 &&
           geometry.roi_width > 0 && geometry.roi_height > 0 &&
           geometry.roi_x + geometry.roi_width <= geometry.source_width &&
           geometry.roi_y + geometry.roi_height <= geometry.source_height &&
           std::isfinite(geometry.source_pixels_per_pixel_x) &&
           std::isfinite(geometry.source_pixels_per_pixel_y) &&
           geometry.source_pixels_per_pixel_x > 0.0 &&
           geometry.source_pixels_per_pixel_y > 0.0;
}

bool valid_stop_reason(std::string_view value) noexcept {
    return value == "duration" || value == "frame_limit" ||
           value == "signal";
}

bool valid_report(const SenderRunReport& report) noexcept {
    if (report.destination_url.empty() ||
        !valid_stop_reason(report.stop_reason) ||
        report.adapter_index < 0 || report.output_index < 0 ||
        report.jpeg_quality < 1 || report.jpeg_quality > 100 ||
        report.max_datagram_bytes <= 124 || report.fps == 0 ||
        !finite_nonnegative(report.elapsed_seconds) ||
        !valid_geometry(report.geometry) ||
        report.stats.frames_sent == 0 || report.stats.frames_failed != 0 ||
        report.samples_dropped != 0 ||
        report.stats.frames_sent !=
            report.samples.size() + report.samples_dropped ||
        report.stats.largest_datagram_bytes >
            static_cast<std::uint64_t>(report.max_datagram_bytes)) {
        return false;
    }
    if (report.stop_reason == "duration" &&
        (report.maximum_seconds == 0 ||
         report.elapsed_seconds <
             static_cast<double>(report.maximum_seconds))) {
        return false;
    }
    if (report.stop_reason == "frame_limit" &&
        (report.maximum_frames == 0 ||
         report.stats.frames_sent < report.maximum_frames)) {
        return false;
    }
    std::uint64_t previous_frame_id = 0;
    std::uint64_t datagrams = 0;
    std::uint64_t jpeg_bytes = 0;
    std::uint64_t wire_bytes = 0;
    const auto add_checked = [](std::uint64_t& total,
                                std::uint64_t value) noexcept {
        if (value > std::numeric_limits<std::uint64_t>::max() - total) {
            return false;
        }
        total += value;
        return true;
    };
    for (const auto& sample : report.samples) {
        if (sample.frame_id <= previous_frame_id ||
            !finite_nonnegative(sample.capture_ms) ||
            !finite_nonnegative(sample.encode_ms) ||
            !finite_nonnegative(sample.packetize_ms) ||
            !finite_nonnegative(sample.send_ms) ||
            !finite_nonnegative(sample.sender_total_ms) ||
            sample.datagrams == 0 || sample.jpeg_bytes == 0 ||
            sample.wire_bytes <= sample.jpeg_bytes ||
            !add_checked(datagrams, sample.datagrams) ||
            !add_checked(jpeg_bytes, sample.jpeg_bytes) ||
            !add_checked(wire_bytes, sample.wire_bytes)) {
            return false;
        }
        previous_frame_id = sample.frame_id;
    }
    return previous_frame_id == report.stats.last_frame_id &&
           datagrams == report.stats.datagrams_sent &&
           jpeg_bytes == report.stats.jpeg_bytes_sent &&
           wire_bytes == report.stats.wire_bytes_sent &&
           report.samples.back().datagrams ==
               report.stats.last_frame_datagrams &&
           report.samples.back().jpeg_bytes ==
               report.stats.last_frame_jpeg_bytes &&
           report.samples.back().wire_bytes ==
               report.stats.last_frame_wire_bytes;
}

std::string json_escape(std::string_view input) {
    std::string output;
    output.reserve(input.size() + 16U);
    constexpr char hexadecimal[] = "0123456789abcdef";
    for (const unsigned char value : input) {
        switch (value) {
            case '\"': output += "\\\""; break;
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

TimingSummary summarize(const std::vector<double>& values) {
    TimingSummary result;
    if (values.empty()) return result;
    result.mean_ms = std::accumulate(
        values.begin(), values.end(), 0.0) /
        static_cast<double>(values.size());
    result.p50_ms = percentile(values, 0.50);
    result.p95_ms = percentile(values, 0.95);
    result.p99_ms = percentile(values, 0.99);
    result.max_ms = *std::max_element(values.begin(), values.end());
    return result;
}

void write_timing(std::ostream& output, std::string_view name,
                  const TimingSummary& summary, bool trailing_comma) {
    output << "    \"" << name << "\": {"
           << "\"mean_ms\": " << summary.mean_ms
           << ", \"p50_ms\": " << summary.p50_ms
           << ", \"p95_ms\": " << summary.p95_ms
           << ", \"p99_ms\": " << summary.p99_ms
           << ", \"max_ms\": " << summary.max_ms << '}';
    if (trailing_comma) output << ',';
    output << '\n';
}

bool write_json(const std::filesystem::path& path,
                const SenderRunReport& report) {
    std::vector<double> capture;
    std::vector<double> encode;
    std::vector<double> packetize;
    std::vector<double> send;
    std::vector<double> sender_total;
    std::vector<double> capture_to_send;
    const std::size_t count = report.samples.size();
    capture.reserve(count);
    encode.reserve(count);
    packetize.reserve(count);
    send.reserve(count);
    sender_total.reserve(count);
    capture_to_send.reserve(count);
    for (const auto& sample : report.samples) {
        capture.push_back(sample.capture_ms);
        encode.push_back(sample.encode_ms);
        packetize.push_back(sample.packetize_ms);
        send.push_back(sample.send_ms);
        sender_total.push_back(sample.sender_total_ms);
        capture_to_send.push_back(
            sample.capture_ms + sample.sender_total_ms);
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output << std::setprecision(9)
           << "{\n  \"schema\": 1,\n  \"complete\": true,\n"
           << "  \"destination_url\": \""
           << json_escape(report.destination_url) << "\",\n"
           << "  \"stop_reason\": \""
           << json_escape(report.stop_reason) << "\",\n"
           << "  \"elapsed_seconds\": " << report.elapsed_seconds
           << ",\n  \"limits\": {\"maximum_frames\": "
           << report.maximum_frames << ", \"maximum_seconds\": "
           << report.maximum_seconds << "},\n"
           << "  \"capture\": {\"adapter_index\": "
           << report.adapter_index << ", \"output_index\": "
           << report.output_index << "},\n"
           << "  \"sender\": {\"jpeg_quality\": "
           << report.jpeg_quality << ", \"max_datagram_bytes\": "
           << report.max_datagram_bytes << ", \"fps\": "
           << report.fps << "},\n"
           << "  \"geometry\": {\"source_width\": "
           << report.geometry.source_width
           << ", \"source_height\": " << report.geometry.source_height
           << ", \"encoded_width\": " << report.geometry.encoded_width
           << ", \"encoded_height\": " << report.geometry.encoded_height
           << ", \"roi_x\": " << report.geometry.roi_x
           << ", \"roi_y\": " << report.geometry.roi_y
           << ", \"roi_width\": " << report.geometry.roi_width
           << ", \"roi_height\": " << report.geometry.roi_height
           << ", \"source_pixels_per_pixel_x\": "
           << report.geometry.source_pixels_per_pixel_x
           << ", \"source_pixels_per_pixel_y\": "
           << report.geometry.source_pixels_per_pixel_y << "},\n"
           << "  \"stats\": {\"stream_id\": "
           << report.stats.stream_id << ", \"last_frame_id\": "
           << report.stats.last_frame_id << ", \"frames_sent\": "
           << report.stats.frames_sent << ", \"frames_failed\": "
           << report.stats.frames_failed << ", \"datagrams_sent\": "
           << report.stats.datagrams_sent << ", \"jpeg_bytes_sent\": "
           << report.stats.jpeg_bytes_sent << ", \"wire_bytes_sent\": "
           << report.stats.wire_bytes_sent
           << ", \"largest_datagram_bytes\": "
           << report.stats.largest_datagram_bytes
           << ", \"samples_dropped\": " << report.samples_dropped
           << "},\n  \"timing\": {\n";
    write_timing(output, "capture", summarize(capture), true);
    write_timing(output, "encode", summarize(encode), true);
    write_timing(output, "packetize", summarize(packetize), true);
    write_timing(output, "send", summarize(send), true);
    write_timing(output, "sender_total", summarize(sender_total), true);
    write_timing(
        output, "capture_to_send", summarize(capture_to_send), false);
    output << "  },\n  \"samples\": [\n";
    for (std::size_t index = 0; index < count; ++index) {
        const auto& sample = report.samples[index];
        output << "    {\"frame_id\": " << sample.frame_id
               << ", \"capture_ms\": " << sample.capture_ms
               << ", \"encode_ms\": " << sample.encode_ms
               << ", \"packetize_ms\": " << sample.packetize_ms
               << ", \"send_ms\": " << sample.send_ms
               << ", \"sender_total_ms\": " << sample.sender_total_ms
               << ", \"capture_to_send_ms\": "
               << sample.capture_ms + sample.sender_total_ms
               << ", \"datagrams\": " << sample.datagrams
               << ", \"jpeg_bytes\": " << sample.jpeg_bytes
               << ", \"wire_bytes\": " << sample.wire_bytes << '}';
        if (index + 1U != count) output << ',';
        output << '\n';
    }
    output << "  ]\n}\n";
    output.flush();
    return output.good();
}

} // namespace

bool write_sender_run_report(
        const std::string& path,
        const SenderRunReport& report,
        std::string& error) noexcept {
    std::filesystem::path temporary_path;
    try {
        if (path.empty() || !valid_report(report)) {
            set_error(error, "XUDP Sender 报告参数或样本非法");
            return false;
        }
        const auto final_path = std::filesystem::absolute(
            std::filesystem::u8path(path));
        if (std::filesystem::exists(final_path)) {
            set_error(error, "XUDP Sender 报告目标已存在，拒绝覆盖");
            return false;
        }
        const auto parent = final_path.parent_path();
        std::error_code directory_error;
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, directory_error);
        }
        if (directory_error ||
            (!parent.empty() && !std::filesystem::is_directory(parent))) {
            set_error(error, "XUDP Sender 报告目录创建失败");
            return false;
        }
        temporary_path = final_path;
        temporary_path += L".pending-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64());
        if (!write_json(temporary_path, report)) {
            set_error(error, "XUDP Sender 报告临时文件写入失败");
            std::error_code ignored;
            std::filesystem::remove(temporary_path, ignored);
            temporary_path.clear();
            return false;
        }
        if (!MoveFileExW(
                temporary_path.c_str(), final_path.c_str(),
                MOVEFILE_WRITE_THROUGH)) {
            set_error(error, "XUDP Sender 报告原子发布失败，Win32Error=" +
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
        set_error(error, std::string("XUDP Sender 报告异常: ") +
                          exception.what());
    } catch (...) {
        set_error(error, "XUDP Sender 报告发生未知异常");
    }
    if (!temporary_path.empty()) {
        std::error_code ignored;
        std::filesystem::remove(temporary_path, ignored);
    }
    return false;
}

} // namespace sender::detail
