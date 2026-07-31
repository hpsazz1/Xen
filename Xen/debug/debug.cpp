#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>

#ifdef ERROR
#undef ERROR
#endif

#include "debug/debug.h"

#include "log/log.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <vector>

namespace {

struct TimingValues {
    std::vector<double> capture;
    std::vector<double> queue;
    std::vector<double> preprocess;
    std::vector<double> inference;
    std::vector<double> h2d;
    std::vector<double> gpu_preprocess;
    std::vector<double> execution;
    std::vector<double> d2h;
    std::vector<double> postprocess;
    std::vector<double> aim;
    std::vector<double> mouse;
    std::vector<double> total;
};

void set_error(std::string& output, const std::string& value) noexcept {
    try {
        output = value;
    } catch (...) {
    }
}

double percentile(std::vector<double> values, double quantile) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const double position = quantile *
        static_cast<double>(values.size() - 1);
    const std::size_t lower = static_cast<std::size_t>(std::floor(position));
    const std::size_t upper = static_cast<std::size_t>(std::ceil(position));
    if (lower == upper) return values[lower];
    const double fraction = position - static_cast<double>(lower);
    return values[lower] * (1.0 - fraction) + values[upper] * fraction;
}

DebugTimingSummary summarize(const std::vector<double>& values) {
    DebugTimingSummary result;
    result.sample_count = values.size();
    if (values.empty()) return result;
    result.mean_ms = std::accumulate(values.begin(), values.end(), 0.0) /
                    static_cast<double>(values.size());
    result.p50_ms = percentile(values, 0.50);
    result.p95_ms = percentile(values, 0.95);
    result.p99_ms = percentile(values, 0.99);
    result.max_ms = *std::max_element(values.begin(), values.end());
    return result;
}

void collect_timing(TimingValues& values,
                    const RuntimePipelineSample& sample) {
    values.capture.push_back(sample.profile.capture_ms);
    values.queue.push_back(sample.profile.queue_ms);
    values.preprocess.push_back(sample.profile.detector.preprocess_ms);
    values.inference.push_back(sample.profile.detector.inference_ms);
    values.h2d.push_back(sample.profile.detector.h2d_ms);
    values.gpu_preprocess.push_back(
        sample.profile.detector.gpu_preprocess_ms);
    values.execution.push_back(sample.profile.detector.execution_ms);
    values.d2h.push_back(sample.profile.detector.d2h_ms);
    values.postprocess.push_back(sample.profile.detector.postprocess_ms);
    values.aim.push_back(sample.profile.aim.total_ms);
    values.mouse.push_back(sample.profile.mouse_ms);
    values.total.push_back(sample.profile.total_ms);
}

DebugReportSummary make_summary(
        const std::vector<RuntimePipelineSample>& samples,
        std::uint64_t report_dropped,
        std::uint64_t runtime_dropped) {
    DebugReportSummary result;
    result.sample_count = samples.size();
    result.report_samples_dropped = report_dropped;
    result.runtime_samples_dropped = runtime_dropped;
    TimingValues values;
    std::size_t successful_count = 0;
    for (const auto& sample : samples) {
        if (!debug_sample_succeeded(sample)) continue;
        ++successful_count;
        collect_timing(values, sample);
    }
    result.successful_samples = successful_count;
    result.failed_samples = result.sample_count - successful_count;
    result.capture = summarize(values.capture);
    result.queue = summarize(values.queue);
    result.preprocess = summarize(values.preprocess);
    result.inference = summarize(values.inference);
    result.h2d = summarize(values.h2d);
    result.gpu_preprocess = summarize(values.gpu_preprocess);
    result.execution = summarize(values.execution);
    result.d2h = summarize(values.d2h);
    result.postprocess = summarize(values.postprocess);
    result.aim = summarize(values.aim);
    result.mouse = summarize(values.mouse);
    result.total = summarize(values.total);
    return result;
}

std::string csv_escape(const std::string& value) {
    std::string escaped = "\"";
    for (const char ch : value) {
        if (ch == '"') escaped += "\"\"";
        else escaped += ch;
    }
    escaped += '"';
    return escaped;
}

std::string json_escape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (ch < 0x20) escaped += '?';
                else escaped += static_cast<char>(ch);
                break;
        }
    }
    return escaped;
}

const char* bool_name(bool value) noexcept {
    return value ? "true" : "false";
}

const char* runtime_state_name(RuntimeState state) noexcept {
    switch (state) {
        case RuntimeState::STOPPED: return "STOPPED";
        case RuntimeState::STARTING: return "STARTING";
        case RuntimeState::RUNNING: return "RUNNING";
        case RuntimeState::STOPPING: return "STOPPING";
        case RuntimeState::FAILED: return "FAILED";
    }
    return "UNKNOWN";
}

const char* capture_status_name(CaptureStatus status) noexcept {
    switch (status) {
        case CaptureStatus::CLOSED: return "CLOSED";
        case CaptureStatus::READY: return "READY";
        case CaptureStatus::FRAME: return "FRAME";
        case CaptureStatus::NO_FRAME: return "NO_FRAME";
        case CaptureStatus::ACCESS_LOST: return "ACCESS_LOST";
        case CaptureStatus::INVALID_CONFIG: return "INVALID_CONFIG";
        case CaptureStatus::UNSUPPORTED: return "UNSUPPORTED";
        case CaptureStatus::FAILURE: return "FAILURE";
    }
    return "UNKNOWN";
}

void append_json_timing(std::ostringstream& output,
                        const char* name,
                        const DebugTimingSummary& timing,
                        bool last) {
    output << "    \"" << name << "\": {\"sample_count\": "
           << timing.sample_count
           << ", \"mean_ms\": " << timing.mean_ms
           << ", \"p50_ms\": " << timing.p50_ms
           << ", \"p95_ms\": " << timing.p95_ms
           << ", \"p99_ms\": " << timing.p99_ms
           << ", \"max_ms\": " << timing.max_ms << "}";
    output << (last ? '\n' : ',');
}

void append_csv_timing(std::ostringstream& output,
                       const char* name,
                       const DebugTimingSummary& timing) {
    output << "# timing," << name << ',' << timing.sample_count << ','
           << timing.mean_ms << ',' << timing.p50_ms << ','
           << timing.p95_ms << ',' << timing.p99_ms << ','
           << timing.max_ms << '\n';
}

void append_csv_snapshot(std::ostringstream& output,
                         const RuntimeSnapshot& snapshot) {
    output
        << "# final_runtime_state," << runtime_state_name(snapshot.state) << '\n'
        << "# final_capture_status,"
        << capture_status_name(snapshot.capture_status) << '\n'
        << "# final_detection_status,"
        << DetectionStatusName(snapshot.detection_status) << '\n'
        << "# final_aim_status," << AimStatusName(snapshot.aim_status) << '\n'
        << "# final_mouse_status,"
        << MouseStatusName(snapshot.mouse_status) << '\n'
        << "# final_provider," << csv_escape(snapshot.provider) << '\n'
        << "# final_active_model_path,"
        << csv_escape(snapshot.active_model_path) << '\n'
        << "# final_last_error," << csv_escape(snapshot.last_error) << '\n'
        << "# final_detector_reload_error,"
        << csv_escape(snapshot.detector_reload_error) << '\n'
        << "# final_detector_generation,"
        << snapshot.detector_generation << '\n'
        << "# final_captured_frames," << snapshot.captured_frames << '\n'
        << "# final_processed_frames," << snapshot.processed_frames << '\n'
        << "# final_failed_frames," << snapshot.failed_frames << '\n'
        << "# final_source_dropped_frames,"
        << snapshot.source_dropped_frames << '\n'
        << "# final_transport_dropped_frames,"
        << snapshot.transport_dropped_frames << '\n'
        << "# final_transport_invalid_packets,"
        << snapshot.transport_invalid_packets << '\n'
        << "# final_source_received_frames,"
        << snapshot.source_received_frames << '\n'
        << "# final_overwritten_frames,"
        << snapshot.overwritten_frames << '\n'
        << "# final_mouse_commands," << snapshot.mouse_commands << '\n'
        << "# final_debug_samples_dropped,"
        << snapshot.debug_samples_dropped << '\n'
        << "# final_last_sequence," << snapshot.last_sequence << '\n'
        << "# final_encoded_width," << snapshot.encoded_width << '\n'
        << "# final_encoded_height," << snapshot.encoded_height << '\n'
        << "# final_source_width," << snapshot.source_width << '\n'
        << "# final_source_height," << snapshot.source_height << '\n'
        << "# final_roi_x," << snapshot.capture_roi_x << '\n'
        << "# final_roi_y," << snapshot.capture_roi_y << '\n'
        << "# final_roi_width," << snapshot.capture_roi_width << '\n'
        << "# final_roi_height," << snapshot.capture_roi_height << '\n'
        << "# final_source_pixels_per_pixel_x,"
        << snapshot.source_pixels_per_pixel_x << '\n'
        << "# final_source_pixels_per_pixel_y,"
        << snapshot.source_pixels_per_pixel_y << '\n'
        << "# final_capture_fps," << snapshot.capture_fps << '\n'
        << "# final_source_fps," << snapshot.source_fps << '\n'
        << "# final_output_allowed_by_config,"
        << bool_name(snapshot.output_allowed_by_config) << '\n'
        << "# final_output_armed," << bool_name(snapshot.output_armed) << '\n'
        << "# final_emergency_stopped,"
        << bool_name(snapshot.emergency_stopped) << '\n';
}

void append_json_snapshot(std::ostringstream& output,
                          const RuntimeSnapshot& snapshot) {
    output
        << "  \"final_snapshot\": {\n"
        << "    \"runtime_state\": \"" << runtime_state_name(snapshot.state)
        << "\",\n    \"capture_status\": \""
        << capture_status_name(snapshot.capture_status)
        << "\",\n    \"detection_status\": \""
        << DetectionStatusName(snapshot.detection_status)
        << "\",\n    \"aim_status\": \""
        << AimStatusName(snapshot.aim_status)
        << "\",\n    \"mouse_status\": \""
        << MouseStatusName(snapshot.mouse_status)
        << "\",\n    \"provider\": \"" << json_escape(snapshot.provider)
        << "\",\n    \"active_model_path\": \""
        << json_escape(snapshot.active_model_path)
        << "\",\n    \"last_error\": \""
        << json_escape(snapshot.last_error)
        << "\",\n    \"detector_reload_error\": \""
        << json_escape(snapshot.detector_reload_error) << "\",\n"
        << "    \"detector_generation\": "
        << snapshot.detector_generation << ",\n"
        << "    \"captured_frames\": " << snapshot.captured_frames << ",\n"
        << "    \"processed_frames\": " << snapshot.processed_frames << ",\n"
        << "    \"failed_frames\": " << snapshot.failed_frames << ",\n"
        << "    \"source_dropped_frames\": "
        << snapshot.source_dropped_frames << ",\n"
        << "    \"transport_dropped_frames\": "
        << snapshot.transport_dropped_frames << ",\n"
        << "    \"transport_invalid_packets\": "
        << snapshot.transport_invalid_packets << ",\n"
        << "    \"source_received_frames\": "
        << snapshot.source_received_frames << ",\n"
        << "    \"overwritten_frames\": "
        << snapshot.overwritten_frames << ",\n"
        << "    \"mouse_commands\": " << snapshot.mouse_commands << ",\n"
        << "    \"debug_samples_dropped\": "
        << snapshot.debug_samples_dropped << ",\n"
        << "    \"last_sequence\": " << snapshot.last_sequence << ",\n"
        << "    \"encoded_width\": " << snapshot.encoded_width << ",\n"
        << "    \"encoded_height\": " << snapshot.encoded_height << ",\n"
        << "    \"source_width\": " << snapshot.source_width << ",\n"
        << "    \"source_height\": " << snapshot.source_height << ",\n"
        << "    \"roi_x\": " << snapshot.capture_roi_x << ",\n"
        << "    \"roi_y\": " << snapshot.capture_roi_y << ",\n"
        << "    \"roi_width\": " << snapshot.capture_roi_width << ",\n"
        << "    \"roi_height\": " << snapshot.capture_roi_height << ",\n"
        << "    \"source_pixels_per_pixel_x\": "
        << snapshot.source_pixels_per_pixel_x << ",\n"
        << "    \"source_pixels_per_pixel_y\": "
        << snapshot.source_pixels_per_pixel_y << ",\n"
        << "    \"capture_fps\": " << snapshot.capture_fps << ",\n"
        << "    \"source_fps\": " << snapshot.source_fps << ",\n"
        << "    \"output_allowed_by_config\": "
        << bool_name(snapshot.output_allowed_by_config) << ",\n"
        << "    \"output_armed\": " << bool_name(snapshot.output_armed)
        << ",\n    \"emergency_stopped\": "
        << bool_name(snapshot.emergency_stopped) << "\n  },\n";
}

bool write_atomically(const std::string& path,
                      const std::string& content,
                      std::string& error) noexcept {
    try {
        if (path.empty()) {
            set_error(error, "Debug 报告路径不能为空");
            return false;
        }
        const std::filesystem::path target(path);
        const auto parent = target.parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);

        std::filesystem::path temporary = target;
        temporary += ".tmp." +
            std::to_string(static_cast<unsigned long long>(
                GetCurrentProcessId()));
        {
            std::ofstream stream(temporary,
                                 std::ios::binary | std::ios::trunc);
            if (!stream) {
                set_error(error, "无法创建 Debug 报告临时文件: " +
                                  temporary.string());
                return false;
            }
            stream.write(content.data(),
                         static_cast<std::streamsize>(content.size()));
            stream.flush();
            if (!stream) {
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                set_error(error, "Debug 报告临时文件写入失败: " +
                                  temporary.string());
                return false;
            }
        }
        const std::wstring source_name = temporary.wstring();
        const std::wstring target_name = target.wstring();
        if (!MoveFileExW(source_name.c_str(), target_name.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const DWORD code = GetLastError();
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            set_error(error, "Debug 报告原子发布失败，Win32Error=" +
                              std::to_string(code));
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        set_error(error, std::string("Debug 报告发布异常: ") +
                          exception.what());
        return false;
    } catch (...) {
        set_error(error, "Debug 报告发布时发生未知异常");
        return false;
    }
}

} // namespace

bool debug_sample_succeeded(
        const RuntimePipelineSample& sample) noexcept {
    if (sample.detection_status != DetectionStatus::SUCCESS ||
        sample.aim_status != AimStatus::SUCCESS) {
        return false;
    }
    return !sample.mouse_sent || sample.mouse_status == MouseStatus::READY;
}

DebugReport::DebugReport() {
    Log::register_module("debug", LogLevel::INFO);
}

bool DebugReport::start(const DebugReportConfig& config,
                        std::string& error) noexcept {
    try {
        if (config.csv_path.empty() || config.json_path.empty()) {
            set_error(error, "Debug 报告 CSV/JSON 路径不能为空");
            return false;
        }
        if (config.max_samples == 0 || config.max_samples > 100000) {
            set_error(error, "Debug 报告样本容量必须在 1..100000");
            return false;
        }
        config_ = config;
        samples_.clear();
        samples_.reserve(config.max_samples);
        summary_ = {};
        report_samples_dropped_ = 0;
        last_error_.clear();
        active_ = true;
        error.clear();
        LOG_INFO("debug", "Debug 报告已开始: csv={}, json={}, capacity={}",
                 config_.csv_path, config_.json_path, config_.max_samples);
        return true;
    } catch (...) {
        set_error(error, "Debug 报告初始化时发生未知异常");
        return false;
    }
}

void DebugReport::ingest(
        std::span<const RuntimePipelineSample> samples) noexcept {
    if (!active_) return;
    try {
        for (const auto& sample : samples) {
            if (samples_.size() == config_.max_samples) {
                samples_.erase(samples_.begin());
                ++report_samples_dropped_;
            }
            samples_.push_back(sample);
        }
    } catch (...) {
        // 诊断报告不能影响 Runtime；本批次剩余样本直接丢弃。
    }
}

bool DebugReport::finalize(const RuntimeSnapshot& final_snapshot,
                           std::string& error) noexcept {
    if (!active_) {
        set_error(error, "Debug 报告尚未开始");
        return false;
    }
    try {
        summary_ = make_summary(
            samples_, report_samples_dropped_,
            final_snapshot.debug_samples_dropped);
        std::ostringstream csv;
        csv << "# Xen Runtime Debug Report v3\n"
            << "# session_id," << csv_escape(config_.session_id) << '\n'
            << "# model_path," << csv_escape(config_.model_path) << '\n'
            << "# provider," << csv_escape(config_.provider) << '\n'
            << "# capture_backend," << csv_escape(config_.capture_backend)
            << '\n'
            << "# mouse_backend," << csv_escape(config_.mouse_backend)
            << '\n'
            << "# sample_count," << summary_.sample_count << '\n'
            << "# successful_samples," << summary_.successful_samples << '\n'
            << "# failed_samples," << summary_.failed_samples << '\n'
            << "# report_samples_dropped," <<
                summary_.report_samples_dropped << '\n'
            << "# runtime_samples_dropped," <<
                summary_.runtime_samples_dropped << '\n';
        append_csv_snapshot(csv, final_snapshot);
        append_csv_timing(csv, "capture", summary_.capture);
        append_csv_timing(csv, "queue", summary_.queue);
        append_csv_timing(csv, "preprocess", summary_.preprocess);
        append_csv_timing(csv, "inference", summary_.inference);
        append_csv_timing(csv, "h2d", summary_.h2d);
        append_csv_timing(
            csv, "gpu_preprocess", summary_.gpu_preprocess);
        append_csv_timing(csv, "execution", summary_.execution);
        append_csv_timing(csv, "d2h", summary_.d2h);
        append_csv_timing(csv, "postprocess", summary_.postprocess);
        append_csv_timing(csv, "aim", summary_.aim);
        append_csv_timing(csv, "mouse", summary_.mouse);
        append_csv_timing(csv, "total", summary_.total);
        csv << "sequence,capture_ms,queue_ms,preprocess_ms,inference_ms,"
               "h2d_ms,gpu_preprocess_ms,execution_ms,d2h_ms,"
               "postprocess_ms,aim_ms,mouse_ms,"
               "total_ms,detection_status,aim_status,mouse_status,mouse_sent,"
               "explicit_device_copy,gpu_preprocess,input_upload_bytes,"
               "encoded_width,"
               "encoded_height,source_width,source_height,roi_x,roi_y,"
               "roi_width,roi_height,source_pixels_per_pixel_x,"
               "source_pixels_per_pixel_y,success\n";
        csv << std::setprecision(9);
        for (const auto& sample : samples_) {
            csv << sample.sequence << ',' << sample.profile.capture_ms << ','
                << sample.profile.queue_ms << ','
                << sample.profile.detector.preprocess_ms << ','
                << sample.profile.detector.inference_ms << ','
                << sample.profile.detector.h2d_ms << ','
                << sample.profile.detector.gpu_preprocess_ms << ','
                << sample.profile.detector.execution_ms << ','
                << sample.profile.detector.d2h_ms << ','
                << sample.profile.detector.postprocess_ms << ','
                << sample.profile.aim.total_ms << ','
                << sample.profile.mouse_ms << ','
                << sample.profile.total_ms << ','
                << DetectionStatusName(sample.detection_status) << ','
                << AimStatusName(sample.aim_status) << ','
                << MouseStatusName(sample.mouse_status) << ','
                << bool_name(sample.mouse_sent) << ','
                << bool_name(
                    sample.profile.detector.explicit_device_copy) << ','
                << bool_name(sample.profile.detector.gpu_preprocess) << ','
                << sample.profile.detector.input_upload_bytes << ','
                << sample.geometry.encoded_width << ','
                << sample.geometry.encoded_height << ','
                << sample.geometry.source_width << ','
                << sample.geometry.source_height << ','
                << sample.geometry.roi_x << ','
                << sample.geometry.roi_y << ','
                << sample.geometry.roi_width << ','
                << sample.geometry.roi_height << ','
                << sample.geometry.source_pixels_per_pixel_x << ','
                << sample.geometry.source_pixels_per_pixel_y << ','
                << bool_name(debug_sample_succeeded(sample)) << '\n';
        }

        std::ostringstream json;
        json << std::setprecision(9)
             << "{\n  \"schema\": 3,\n"
             << "  \"session_id\": \"" << json_escape(config_.session_id)
             << "\",\n  \"model_path\": \""
             << json_escape(config_.model_path) << "\",\n"
             << "  \"provider\": \"" << json_escape(config_.provider)
             << "\",\n  \"capture_backend\": \""
             << json_escape(config_.capture_backend)
             << "\",\n  \"mouse_backend\": \""
             << json_escape(config_.mouse_backend) << "\",\n"
             << "  \"sample_count\": " << summary_.sample_count << ",\n"
             << "  \"successful_samples\": "
             << summary_.successful_samples << ",\n"
             << "  \"failed_samples\": " << summary_.failed_samples << ",\n"
             << "  \"report_samples_dropped\": "
             << summary_.report_samples_dropped << ",\n"
             << "  \"runtime_samples_dropped\": "
             << summary_.runtime_samples_dropped << ",\n";
        append_json_snapshot(json, final_snapshot);
        json << "  \"timing\": {\n";
        append_json_timing(json, "capture", summary_.capture, false);
        append_json_timing(json, "queue", summary_.queue, false);
        append_json_timing(json, "preprocess", summary_.preprocess, false);
        append_json_timing(json, "inference", summary_.inference, false);
        append_json_timing(json, "h2d", summary_.h2d, false);
        append_json_timing(
            json, "gpu_preprocess", summary_.gpu_preprocess, false);
        append_json_timing(json, "execution", summary_.execution, false);
        append_json_timing(json, "d2h", summary_.d2h, false);
        append_json_timing(json, "postprocess", summary_.postprocess, false);
        append_json_timing(json, "aim", summary_.aim, false);
        append_json_timing(json, "mouse", summary_.mouse, false);
        append_json_timing(json, "total", summary_.total, true);
        json << "  },\n  \"samples\": [\n";
        for (std::size_t index = 0; index < samples_.size(); ++index) {
            const auto& sample = samples_[index];
            json << "    {\"sequence\": " << sample.sequence
                 << ", \"detection_status\": \""
                 << DetectionStatusName(sample.detection_status)
                 << "\", \"aim_status\": \""
                 << AimStatusName(sample.aim_status)
                 << "\", \"mouse_status\": \""
                 << MouseStatusName(sample.mouse_status)
                 << "\", \"mouse_sent\": "
                 << bool_name(sample.mouse_sent)
                 << ", \"explicit_device_copy\": "
                 << bool_name(
                     sample.profile.detector.explicit_device_copy)
                 << ", \"gpu_preprocess\": "
                 << bool_name(sample.profile.detector.gpu_preprocess)
                 << ", \"input_upload_bytes\": "
                 << sample.profile.detector.input_upload_bytes
                 << ", \"encoded_width\": "
                 << sample.geometry.encoded_width
                 << ", \"encoded_height\": "
                 << sample.geometry.encoded_height
                 << ", \"source_width\": "
                 << sample.geometry.source_width
                 << ", \"source_height\": "
                 << sample.geometry.source_height
                 << ", \"roi_x\": " << sample.geometry.roi_x
                 << ", \"roi_y\": " << sample.geometry.roi_y
                 << ", \"roi_width\": " << sample.geometry.roi_width
                 << ", \"roi_height\": " << sample.geometry.roi_height
                 << ", \"source_pixels_per_pixel_x\": "
                 << sample.geometry.source_pixels_per_pixel_x
                 << ", \"source_pixels_per_pixel_y\": "
                 << sample.geometry.source_pixels_per_pixel_y
                 << ", \"success\": "
                 << bool_name(debug_sample_succeeded(sample))
                 << ", \"total_ms\": " << sample.profile.total_ms << "}"
                 << (index + 1 == samples_.size() ? '\n' : ',');
        }
        json << "  ]\n}\n";

        std::string publish_error;
        const bool csv_ok = write_atomically(
            config_.csv_path, csv.str(), publish_error);
        if (!csv_ok) {
            set_error(error, publish_error);
            set_error(last_error_, publish_error);
            return false;
        }
        const bool json_ok = write_atomically(
            config_.json_path, json.str(), publish_error);
        if (!json_ok) {
            set_error(error, publish_error);
            set_error(last_error_, publish_error);
            return false;
        }
        active_ = false;
        error.clear();
        last_error_.clear();
        LOG_INFO("debug", "Debug 报告已发布: samples={}, success={}, failed={}",
                 summary_.sample_count, summary_.successful_samples,
                 summary_.failed_samples);
        return true;
    } catch (const std::exception& exception) {
        set_error(error, std::string("Debug 报告生成异常: ") +
                          exception.what());
        set_error(last_error_, error);
        return false;
    } catch (...) {
        set_error(error, "Debug 报告生成时发生未知异常");
        set_error(last_error_, error);
        return false;
    }
}

bool DebugReport::active() const noexcept {
    return active_;
}

const DebugReportSummary& DebugReport::summary() const noexcept {
    return summary_;
}

std::string DebugReport::last_error() const {
    return last_error_;
}
