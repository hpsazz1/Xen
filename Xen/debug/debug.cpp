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
    std::vector<double> d3d11_to_cuda;
    std::vector<double> d3d11_to_directml;
    std::vector<double> gpu_preprocess;
    std::vector<double> execution;
    std::vector<double> d2h;
    std::vector<double> postprocess;
    std::vector<double> aim;
    std::vector<double> mouse;
    std::vector<double> total;
    std::vector<double> ndi_receive_call;
    std::vector<double> ndi_metadata;
    std::vector<double> ndi_geometry;
    std::vector<double> ndi_pool_acquire;
    std::vector<double> ndi_color_convert;
    std::vector<double> ndi_performance_query;
    std::vector<double> ndi_queue_query;
    std::vector<double> ndi_pool_publish;
    std::vector<double> runtime_capture_grab;
    std::vector<double> runtime_queue_publish;
    std::vector<double> runtime_handoff;
    std::vector<double> preview;
    std::vector<double> snapshot;
    std::vector<double> snapshot_lock_wait;
    std::vector<double> debug_ring;
    std::vector<double> profile_window;
    std::vector<double> service_tail;
    std::vector<double> pipeline_service;
    std::vector<double> pipeline_complete;
    std::vector<double> ndi_video_queue_depth;
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

DebugQueueDepthSummary summarize_queue_depth(
        const std::vector<double>& values) {
    DebugQueueDepthSummary result;
    result.sample_count = values.size();
    if (values.empty()) return result;
    result.mean_frames =
        std::accumulate(values.begin(), values.end(), 0.0) /
        static_cast<double>(values.size());
    result.p50_frames = percentile(values, 0.50);
    result.p95_frames = percentile(values, 0.95);
    result.p99_frames = percentile(values, 0.99);
    result.max_frames = static_cast<int>(
        *std::max_element(values.begin(), values.end()));
    return result;
}

void collect_timing(TimingValues& values,
                    const RuntimePipelineSample& sample) {
    values.capture.push_back(sample.profile.capture_ms);
    values.queue.push_back(sample.profile.queue_ms);
    values.preprocess.push_back(sample.profile.detector.preprocess_ms);
    values.inference.push_back(sample.profile.detector.inference_ms);
    values.h2d.push_back(sample.profile.detector.h2d_ms);
    values.d3d11_to_cuda.push_back(
        sample.profile.detector.d3d11_to_cuda_ms);
    values.d3d11_to_directml.push_back(
        sample.profile.detector.d3d11_to_directml_ms);
    values.gpu_preprocess.push_back(
        sample.profile.detector.gpu_preprocess_ms);
    values.execution.push_back(sample.profile.detector.execution_ms);
    values.d2h.push_back(sample.profile.detector.d2h_ms);
    values.postprocess.push_back(sample.profile.detector.postprocess_ms);
    values.aim.push_back(sample.profile.aim.total_ms);
    values.mouse.push_back(sample.profile.mouse_ms);
    values.total.push_back(sample.profile.total_ms);
    const auto& capture = sample.capture_stages;
    if (capture.ndi_valid) {
        values.ndi_receive_call.push_back(capture.receive_call_ms);
        values.ndi_metadata.push_back(capture.metadata_ms);
        values.ndi_geometry.push_back(capture.geometry_ms);
        values.ndi_pool_acquire.push_back(capture.pool_acquire_ms);
        values.ndi_color_convert.push_back(capture.color_convert_ms);
        values.ndi_pool_publish.push_back(capture.pool_publish_ms);
        if (capture.performance_query_sampled) {
            values.ndi_performance_query.push_back(
                capture.performance_query_ms);
        }
        if (capture.queue_depth_sampled) {
            values.ndi_queue_query.push_back(capture.queue_query_ms);
            values.ndi_video_queue_depth.push_back(
                static_cast<double>(capture.queued_video_frames));
        }
    }
    if (capture.runtime_handoff_valid) {
        values.runtime_capture_grab.push_back(
            capture.runtime_capture_grab_ms);
        values.runtime_queue_publish.push_back(
            capture.runtime_queue_publish_ms);
        values.runtime_handoff.push_back(
            capture.runtime_capture_grab_ms +
            capture.runtime_queue_publish_ms);
    }
    if (sample.service.valid) {
        values.preview.push_back(sample.service.preview_ms);
        values.snapshot.push_back(sample.service.snapshot_ms);
        values.snapshot_lock_wait.push_back(
            sample.service.snapshot_lock_wait_ms);
        values.debug_ring.push_back(sample.service.debug_ring_ms);
        values.profile_window.push_back(sample.service.profile_window_ms);
        values.service_tail.push_back(sample.service.service_tail_ms);
        values.pipeline_service.push_back(
            sample.service.pipeline_service_ms);
        values.pipeline_complete.push_back(
            sample.service.pipeline_complete_ms);
    }
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
    result.d3d11_to_cuda = summarize(values.d3d11_to_cuda);
    result.d3d11_to_directml = summarize(
        values.d3d11_to_directml);
    result.gpu_preprocess = summarize(values.gpu_preprocess);
    result.execution = summarize(values.execution);
    result.d2h = summarize(values.d2h);
    result.postprocess = summarize(values.postprocess);
    result.aim = summarize(values.aim);
    result.mouse = summarize(values.mouse);
    result.total = summarize(values.total);
    result.ndi_receive_call = summarize(values.ndi_receive_call);
    result.ndi_metadata = summarize(values.ndi_metadata);
    result.ndi_geometry = summarize(values.ndi_geometry);
    result.ndi_pool_acquire = summarize(values.ndi_pool_acquire);
    result.ndi_color_convert = summarize(values.ndi_color_convert);
    result.ndi_performance_query = summarize(
        values.ndi_performance_query);
    result.ndi_queue_query = summarize(values.ndi_queue_query);
    result.ndi_pool_publish = summarize(values.ndi_pool_publish);
    result.runtime_capture_grab = summarize(values.runtime_capture_grab);
    result.runtime_queue_publish = summarize(
        values.runtime_queue_publish);
    result.runtime_handoff = summarize(values.runtime_handoff);
    result.preview = summarize(values.preview);
    result.snapshot = summarize(values.snapshot);
    result.snapshot_lock_wait = summarize(values.snapshot_lock_wait);
    result.debug_ring = summarize(values.debug_ring);
    result.profile_window = summarize(values.profile_window);
    result.service_tail = summarize(values.service_tail);
    result.pipeline_service = summarize(values.pipeline_service);
    result.pipeline_complete = summarize(values.pipeline_complete);
    result.ndi_video_queue_depth = summarize_queue_depth(
        values.ndi_video_queue_depth);
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

const char* track_state_name(TrackState state) noexcept {
    switch (state) {
        case TrackState::TENTATIVE: return "TENTATIVE";
        case TrackState::CONFIRMED: return "CONFIRMED";
        case TrackState::LOST: return "LOST";
    }
    return "UNKNOWN";
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

void append_csv_queue_depth(
        std::ostringstream& output,
        const DebugQueueDepthSummary& summary) {
    output << "# ndi_video_queue_depth," << summary.sample_count << ','
           << summary.mean_frames << ',' << summary.p50_frames << ','
           << summary.p95_frames << ',' << summary.p99_frames << ','
           << summary.max_frames << '\n';
}

void append_csv_coverage_phase(
        std::ostringstream& output,
        const char* name,
        const DebugCoveragePhaseSummary& phase) {
    output << "# coverage_phase," << name << ',' << phase.sample_count << ','
           << phase.first_sequence << ',' << phase.last_sequence << ','
           << phase.runtime_overwritten_frames << ',' << phase.sequence_gaps
           << ',' << phase.trailing_runtime_overwritten_frames << ','
           << bool_name(phase.counter_matches_sequence_gaps) << '\n';
}

void append_csv_coverage(
        std::ostringstream& output,
        const DebugCoverageSummary& coverage) {
    output << "# coverage_available," << bool_name(coverage.available) << '\n'
           << "# warmup_start_overwritten_frames,"
           << coverage.warmup_start_overwritten_frames << '\n'
           << "# warmup_end_overwritten_frames,"
           << coverage.warmup_end_overwritten_frames << '\n'
           << "# formal_end_overwritten_frames,"
           << coverage.formal_end_overwritten_frames << '\n';
    append_csv_coverage_phase(output, "startup", coverage.startup);
    append_csv_coverage_phase(output, "warmup", coverage.warmup);
    append_csv_coverage_phase(output, "formal", coverage.formal);
}

void append_json_queue_depth(
        std::ostringstream& output,
        const DebugQueueDepthSummary& summary) {
    output << "  \"ndi_video_queue_depth\": {\"sample_count\": "
           << summary.sample_count
           << ", \"mean_frames\": " << summary.mean_frames
           << ", \"p50_frames\": " << summary.p50_frames
           << ", \"p95_frames\": " << summary.p95_frames
           << ", \"p99_frames\": " << summary.p99_frames
           << ", \"max_frames\": " << summary.max_frames << "},\n";
}

void append_json_coverage_phase(
        std::ostringstream& output,
        const char* name,
        const DebugCoveragePhaseSummary& phase,
        bool last) {
    output << "    \"" << name << "\": {\"sample_count\": "
           << phase.sample_count
           << ", \"first_sequence\": " << phase.first_sequence
           << ", \"last_sequence\": " << phase.last_sequence
           << ", \"runtime_overwritten_frames\": "
           << phase.runtime_overwritten_frames
           << ", \"sequence_gaps\": " << phase.sequence_gaps
           << ", \"trailing_runtime_overwritten_frames\": "
           << phase.trailing_runtime_overwritten_frames
           << ", \"counter_matches_sequence_gaps\": "
           << bool_name(phase.counter_matches_sequence_gaps) << "}"
           << (last ? '\n' : ',');
}

void append_json_coverage(
        std::ostringstream& output,
        const DebugCoverageSummary& coverage) {
    output << "  \"coverage\": {\n"
           << "    \"available\": " << bool_name(coverage.available)
           << ",\n    \"warmup_start_overwritten_frames\": "
           << coverage.warmup_start_overwritten_frames
           << ",\n    \"warmup_end_overwritten_frames\": "
           << coverage.warmup_end_overwritten_frames
           << ",\n    \"formal_end_overwritten_frames\": "
           << coverage.formal_end_overwritten_frames << ",\n";
    append_json_coverage_phase(output, "startup", coverage.startup, false);
    append_json_coverage_phase(output, "warmup", coverage.warmup, false);
    append_json_coverage_phase(output, "formal", coverage.formal, true);
    output << "  },\n";
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
        << "# final_duplication_recoveries,"
        << snapshot.duplication_recoveries << '\n'
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
        << "# final_preview_enabled,"
        << bool_name(snapshot.preview_enabled) << '\n'
        << "# final_preview_sampled_frames,"
        << snapshot.preview_sampled_frames << '\n'
        << "# final_preview_dropped_frames,"
        << snapshot.preview_dropped_frames << '\n'
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
        << "# final_d3d11_cuda_interop,"
        << bool_name(snapshot.d3d11_cuda_interop) << '\n'
        << "# final_d3d11_directml_interop,"
        << bool_name(snapshot.d3d11_directml_interop) << '\n'
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
        << "    \"duplication_recoveries\": "
        << snapshot.duplication_recoveries << ",\n"
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
        << "    \"preview_enabled\": "
        << bool_name(snapshot.preview_enabled) << ",\n"
        << "    \"preview_sampled_frames\": "
        << snapshot.preview_sampled_frames << ",\n"
        << "    \"preview_dropped_frames\": "
        << snapshot.preview_dropped_frames << ",\n"
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
        << "    \"d3d11_cuda_interop\": "
        << bool_name(snapshot.d3d11_cuda_interop) << ",\n"
        << "    \"d3d11_directml_interop\": "
        << bool_name(snapshot.d3d11_directml_interop) << ",\n"
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
                           std::string& error,
                           const DebugCoverageSummary* coverage) noexcept {
    if (!active_) {
        set_error(error, "Debug 报告尚未开始");
        return false;
    }
    try {
        summary_ = make_summary(
            samples_, report_samples_dropped_,
            final_snapshot.debug_samples_dropped);
        if (coverage) summary_.coverage = *coverage;
        std::ostringstream csv;
        csv << "# Xen Runtime Debug Report v8\n"
            << "# session_id," << csv_escape(config_.session_id) << '\n'
            << "# model_path," << csv_escape(config_.model_path) << '\n'
            << "# provider," << csv_escape(config_.provider) << '\n'
            << "# capture_backend," << csv_escape(config_.capture_backend)
            << '\n'
            << "# mouse_backend," << csv_escape(config_.mouse_backend)
            << '\n'
            << "# performance_probes_enabled,"
            << bool_name(config_.performance_probes_enabled) << '\n'
            << "# sample_count," << summary_.sample_count << '\n'
            << "# successful_samples," << summary_.successful_samples << '\n'
            << "# failed_samples," << summary_.failed_samples << '\n'
            << "# report_samples_dropped," <<
                summary_.report_samples_dropped << '\n'
            << "# runtime_samples_dropped," <<
                summary_.runtime_samples_dropped << '\n';
        append_csv_snapshot(csv, final_snapshot);
        append_csv_coverage(csv, summary_.coverage);
        append_csv_timing(csv, "capture", summary_.capture);
        append_csv_timing(csv, "queue", summary_.queue);
        append_csv_timing(csv, "preprocess", summary_.preprocess);
        append_csv_timing(csv, "inference", summary_.inference);
        append_csv_timing(csv, "h2d", summary_.h2d);
        append_csv_timing(
            csv, "d3d11_to_cuda", summary_.d3d11_to_cuda);
        append_csv_timing(
            csv, "d3d11_to_directml", summary_.d3d11_to_directml);
        append_csv_timing(
            csv, "gpu_preprocess", summary_.gpu_preprocess);
        append_csv_timing(csv, "execution", summary_.execution);
        append_csv_timing(csv, "d2h", summary_.d2h);
        append_csv_timing(csv, "postprocess", summary_.postprocess);
        append_csv_timing(csv, "aim", summary_.aim);
        append_csv_timing(csv, "mouse", summary_.mouse);
        append_csv_timing(csv, "total", summary_.total);
        append_csv_timing(
            csv, "ndi_receive_call", summary_.ndi_receive_call);
        append_csv_timing(csv, "ndi_metadata", summary_.ndi_metadata);
        append_csv_timing(csv, "ndi_geometry", summary_.ndi_geometry);
        append_csv_timing(
            csv, "ndi_pool_acquire", summary_.ndi_pool_acquire);
        append_csv_timing(
            csv, "ndi_color_convert", summary_.ndi_color_convert);
        append_csv_timing(
            csv, "ndi_performance_query", summary_.ndi_performance_query);
        append_csv_timing(
            csv, "ndi_queue_query", summary_.ndi_queue_query);
        append_csv_timing(
            csv, "ndi_pool_publish", summary_.ndi_pool_publish);
        append_csv_timing(
            csv, "runtime_capture_grab", summary_.runtime_capture_grab);
        append_csv_timing(
            csv, "runtime_queue_publish", summary_.runtime_queue_publish);
        append_csv_timing(
            csv, "runtime_handoff", summary_.runtime_handoff);
        append_csv_timing(csv, "preview", summary_.preview);
        append_csv_timing(csv, "snapshot", summary_.snapshot);
        append_csv_timing(
            csv, "snapshot_lock_wait", summary_.snapshot_lock_wait);
        append_csv_timing(csv, "debug_ring", summary_.debug_ring);
        append_csv_timing(
            csv, "profile_window", summary_.profile_window);
        append_csv_timing(csv, "service_tail", summary_.service_tail);
        append_csv_timing(
            csv, "pipeline_service", summary_.pipeline_service);
        append_csv_timing(
            csv, "pipeline_complete", summary_.pipeline_complete);
        append_csv_queue_depth(csv, summary_.ndi_video_queue_depth);
        csv << "sequence,capture_ms,queue_ms,preprocess_ms,inference_ms,"
               "h2d_ms,d3d11_to_cuda_ms,d3d11_to_directml_ms,"
               "gpu_preprocess_ms,execution_ms,d2h_ms,"
               "postprocess_ms,aim_ms,mouse_ms,"
               "total_ms,detection_status,aim_status,mouse_status,mouse_sent,"
               "aim_has_target,aim_has_command,aim_track_id,aim_track_state,"
               "aim_track_predicted,aim_lead_active,"
               "aim_base_point_inside_box,aim_prediction_point_outside_box,"
               "aim_command_toward_target,aim_control_center_x,"
               "aim_control_center_y,aim_acquisition_range_radius,"
               "aim_active_range_radius,aim_range_locked,"
               "aim_range_allows_control,aim_box_x1,aim_box_y1,aim_box_x2,"
               "aim_box_y2,aim_base_x,aim_base_y,aim_final_x,aim_final_y,"
               "aim_velocity_x,aim_velocity_y,aim_lead_x,aim_lead_y,"
               "aim_observation_age_ms,aim_command_dx_counts,"
               "aim_command_dy_counts,"
               "person_detection_count,head_detection_count,"
               "max_person_confidence,max_head_confidence,"
               "detection_count_by_class,max_confidence_by_class,"
               "explicit_device_copy,gpu_preprocess,d3d11_cuda_interop,"
               "d3d11_directml_interop,"
               "input_upload_bytes,input_device_copy_bytes,"
               "encoded_width,"
               "encoded_height,source_width,source_height,roi_x,roi_y,"
               "roi_width,roi_height,source_pixels_per_pixel_x,"
               "source_pixels_per_pixel_y,success,performance_probes,"
               "ndi_probe_valid,runtime_handoff_valid,"
               "ndi_receive_call_ms,ndi_metadata_ms,ndi_geometry_ms,"
               "ndi_pool_acquire_ms,ndi_color_convert_ms,"
               "ndi_performance_query_sampled,ndi_performance_query_ms,"
               "ndi_queue_depth_sampled,ndi_queue_query_ms,"
               "ndi_queued_video_frames,ndi_queued_audio_frames,"
               "ndi_queued_metadata_frames,ndi_pool_publish_ms,"
               "runtime_capture_grab_ms,runtime_queue_publish_ms,"
               "preview_attempted,preview_published,preview_ms,"
               "snapshot_ms,snapshot_lock_wait_ms,debug_ring_ms,"
               "profile_window_ms,service_tail_ms,pipeline_service_ms,"
               "pipeline_complete_ms,source_dropped_frames,"
               "transport_dropped_frames,transport_invalid_packets,"
               "runtime_overwritten_frames\n";
        csv << std::setprecision(9);
        for (const auto& sample : samples_) {
            csv << sample.sequence << ',' << sample.profile.capture_ms << ','
                << sample.profile.queue_ms << ','
                << sample.profile.detector.preprocess_ms << ','
                << sample.profile.detector.inference_ms << ','
                << sample.profile.detector.h2d_ms << ','
                << sample.profile.detector.d3d11_to_cuda_ms << ','
                << sample.profile.detector.d3d11_to_directml_ms << ','
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
                << bool_name(sample.aim_has_target) << ','
                << bool_name(sample.aim_has_command) << ','
                << sample.aim_target.track_id << ','
                << track_state_name(sample.aim_target.state) << ','
                << bool_name(sample.aim_target.predicted) << ','
                << bool_name(sample.aim_target.lead_active) << ','
                << bool_name(sample.aim_base_point_inside_box) << ','
                << bool_name(sample.aim_prediction_point_outside_box) << ','
                << bool_name(sample.aim_command_toward_target) << ','
                << sample.aim_control_center_x << ','
                << sample.aim_control_center_y << ','
                << sample.aim_acquisition_range_radius << ','
                << sample.aim_active_range_radius << ','
                << bool_name(sample.aim_range_locked) << ','
                << bool_name(sample.aim_range_allows_control) << ','
                << sample.aim_target.x1 << ','
                << sample.aim_target.y1 << ','
                << sample.aim_target.x2 << ','
                << sample.aim_target.y2 << ','
                << sample.aim_target.base_aim_x << ','
                << sample.aim_target.base_aim_y << ','
                << sample.aim_target.aim_x << ','
                << sample.aim_target.aim_y << ','
                << sample.aim_target.velocity_x << ','
                << sample.aim_target.velocity_y << ','
                << sample.aim_target.lead_x << ','
                << sample.aim_target.lead_y << ','
                << sample.aim_target.observation_age_ms << ','
                << sample.aim_command.dx_counts << ','
                << sample.aim_command.dy_counts << ','
                << sample.person_detection_count << ','
                << sample.head_detection_count << ','
                << sample.max_person_confidence << ','
                << sample.max_head_confidence << ",\"";
            for (std::size_t index = 0;
                 index < sample.detection_count_by_class.size(); ++index) {
                if (index != 0) csv << ';';
                csv << sample.detection_count_by_class[index];
            }
            csv << "\",\"";
            for (std::size_t index = 0;
                 index < sample.max_confidence_by_class.size(); ++index) {
                if (index != 0) csv << ';';
                csv << sample.max_confidence_by_class[index];
            }
            csv << "\"," << bool_name(
                    sample.profile.detector.explicit_device_copy) << ','
                << bool_name(sample.profile.detector.gpu_preprocess) << ','
                << bool_name(
                    sample.profile.detector.d3d11_cuda_interop) << ','
                << bool_name(
                    sample.profile.detector.d3d11_directml_interop) << ','
                << sample.profile.detector.input_upload_bytes << ','
                << sample.profile.detector.input_device_copy_bytes << ','
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
                << bool_name(debug_sample_succeeded(sample)) << ','
                << bool_name(sample.service.valid) << ','
                << bool_name(sample.capture_stages.ndi_valid) << ','
                << bool_name(
                    sample.capture_stages.runtime_handoff_valid) << ','
                << sample.capture_stages.receive_call_ms << ','
                << sample.capture_stages.metadata_ms << ','
                << sample.capture_stages.geometry_ms << ','
                << sample.capture_stages.pool_acquire_ms << ','
                << sample.capture_stages.color_convert_ms << ','
                << bool_name(
                    sample.capture_stages.performance_query_sampled) << ','
                << sample.capture_stages.performance_query_ms << ','
                << bool_name(
                    sample.capture_stages.queue_depth_sampled) << ','
                << sample.capture_stages.queue_query_ms << ','
                << sample.capture_stages.queued_video_frames << ','
                << sample.capture_stages.queued_audio_frames << ','
                << sample.capture_stages.queued_metadata_frames << ','
                << sample.capture_stages.pool_publish_ms << ','
                << sample.capture_stages.runtime_capture_grab_ms << ','
                << sample.capture_stages.runtime_queue_publish_ms << ','
                << bool_name(sample.service.preview_attempted) << ','
                << bool_name(sample.service.preview_published) << ','
                << sample.service.preview_ms << ','
                << sample.service.snapshot_ms << ','
                << sample.service.snapshot_lock_wait_ms << ','
                << sample.service.debug_ring_ms << ','
                << sample.service.profile_window_ms << ','
                << sample.service.service_tail_ms << ','
                << sample.service.pipeline_service_ms << ','
                << sample.service.pipeline_complete_ms << ','
                << sample.source_dropped_frames << ','
                << sample.transport_dropped_frames << ','
                << sample.transport_invalid_packets << ','
                << sample.runtime_overwritten_frames << '\n';
        }

        std::ostringstream json;
        json << std::setprecision(9)
             << "{\n  \"schema\": 8,\n"
             << "  \"session_id\": \"" << json_escape(config_.session_id)
             << "\",\n  \"model_path\": \""
             << json_escape(config_.model_path) << "\",\n"
             << "  \"provider\": \"" << json_escape(config_.provider)
             << "\",\n  \"capture_backend\": \""
             << json_escape(config_.capture_backend)
             << "\",\n  \"mouse_backend\": \""
             << json_escape(config_.mouse_backend) << "\",\n"
             << "  \"performance_probes_enabled\": "
             << bool_name(config_.performance_probes_enabled) << ",\n"
             << "  \"sample_count\": " << summary_.sample_count << ",\n"
             << "  \"successful_samples\": "
             << summary_.successful_samples << ",\n"
             << "  \"failed_samples\": " << summary_.failed_samples << ",\n"
             << "  \"report_samples_dropped\": "
             << summary_.report_samples_dropped << ",\n"
             << "  \"runtime_samples_dropped\": "
             << summary_.runtime_samples_dropped << ",\n";
        append_json_coverage(json, summary_.coverage);
        append_json_queue_depth(json, summary_.ndi_video_queue_depth);
        append_json_snapshot(json, final_snapshot);
        json << "  \"timing\": {\n";
        append_json_timing(json, "capture", summary_.capture, false);
        append_json_timing(json, "queue", summary_.queue, false);
        append_json_timing(json, "preprocess", summary_.preprocess, false);
        append_json_timing(json, "inference", summary_.inference, false);
        append_json_timing(json, "h2d", summary_.h2d, false);
        append_json_timing(
            json, "d3d11_to_cuda", summary_.d3d11_to_cuda, false);
        append_json_timing(
            json, "d3d11_to_directml",
            summary_.d3d11_to_directml, false);
        append_json_timing(
            json, "gpu_preprocess", summary_.gpu_preprocess, false);
        append_json_timing(json, "execution", summary_.execution, false);
        append_json_timing(json, "d2h", summary_.d2h, false);
        append_json_timing(json, "postprocess", summary_.postprocess, false);
        append_json_timing(json, "aim", summary_.aim, false);
        append_json_timing(json, "mouse", summary_.mouse, false);
        append_json_timing(json, "total", summary_.total, false);
        append_json_timing(
            json, "ndi_receive_call", summary_.ndi_receive_call, false);
        append_json_timing(
            json, "ndi_metadata", summary_.ndi_metadata, false);
        append_json_timing(
            json, "ndi_geometry", summary_.ndi_geometry, false);
        append_json_timing(
            json, "ndi_pool_acquire", summary_.ndi_pool_acquire, false);
        append_json_timing(
            json, "ndi_color_convert", summary_.ndi_color_convert, false);
        append_json_timing(
            json, "ndi_performance_query",
            summary_.ndi_performance_query, false);
        append_json_timing(
            json, "ndi_queue_query", summary_.ndi_queue_query, false);
        append_json_timing(
            json, "ndi_pool_publish", summary_.ndi_pool_publish, false);
        append_json_timing(
            json, "runtime_capture_grab",
            summary_.runtime_capture_grab, false);
        append_json_timing(
            json, "runtime_queue_publish",
            summary_.runtime_queue_publish, false);
        append_json_timing(
            json, "runtime_handoff", summary_.runtime_handoff, false);
        append_json_timing(json, "preview", summary_.preview, false);
        append_json_timing(json, "snapshot", summary_.snapshot, false);
        append_json_timing(
            json, "snapshot_lock_wait", summary_.snapshot_lock_wait, false);
        append_json_timing(
            json, "debug_ring", summary_.debug_ring, false);
        append_json_timing(
            json, "profile_window", summary_.profile_window, false);
        append_json_timing(
            json, "service_tail", summary_.service_tail, false);
        append_json_timing(
            json, "pipeline_service", summary_.pipeline_service, false);
        append_json_timing(
            json, "pipeline_complete", summary_.pipeline_complete, true);
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
                 << ", \"aim_has_target\": "
                 << bool_name(sample.aim_has_target)
                 << ", \"aim_has_command\": "
                 << bool_name(sample.aim_has_command)
                 << ", \"aim_track_id\": " << sample.aim_target.track_id
                 << ", \"aim_track_state\": \""
                 << track_state_name(sample.aim_target.state)
                 << "\", \"aim_track_predicted\": "
                 << bool_name(sample.aim_target.predicted)
                 << ", \"aim_lead_active\": "
                 << bool_name(sample.aim_target.lead_active)
                 << ", \"aim_base_point_inside_box\": "
                 << bool_name(sample.aim_base_point_inside_box)
                 << ", \"aim_prediction_point_outside_box\": "
                 << bool_name(sample.aim_prediction_point_outside_box)
                 << ", \"aim_command_toward_target\": "
                 << bool_name(sample.aim_command_toward_target)
                 << ", \"aim_control_center_x\": "
                 << sample.aim_control_center_x
                 << ", \"aim_control_center_y\": "
                 << sample.aim_control_center_y
                 << ", \"aim_acquisition_range_radius\": "
                 << sample.aim_acquisition_range_radius
                 << ", \"aim_active_range_radius\": "
                 << sample.aim_active_range_radius
                 << ", \"aim_range_locked\": "
                 << bool_name(sample.aim_range_locked)
                 << ", \"aim_range_allows_control\": "
                 << bool_name(sample.aim_range_allows_control)
                 << ", \"aim_box\": [" << sample.aim_target.x1 << ", "
                 << sample.aim_target.y1 << ", " << sample.aim_target.x2
                 << ", " << sample.aim_target.y2 << ']'
                 << ", \"aim_base_point\": ["
                 << sample.aim_target.base_aim_x << ", "
                 << sample.aim_target.base_aim_y << ']'
                 << ", \"aim_final_point\": ["
                 << sample.aim_target.aim_x << ", "
                 << sample.aim_target.aim_y << ']'
                 << ", \"aim_velocity\": ["
                 << sample.aim_target.velocity_x << ", "
                 << sample.aim_target.velocity_y << ']'
                 << ", \"aim_lead\": [" << sample.aim_target.lead_x
                 << ", " << sample.aim_target.lead_y << ']'
                 << ", \"aim_observation_age_ms\": "
                 << sample.aim_target.observation_age_ms
                 << ", \"aim_command\": ["
                 << sample.aim_command.dx_counts << ", "
                 << sample.aim_command.dy_counts << ']'
                 << ", \"person_detection_count\": "
                 << sample.person_detection_count
                 << ", \"head_detection_count\": "
                 << sample.head_detection_count
                 << ", \"max_person_confidence\": "
                 << sample.max_person_confidence
                 << ", \"max_head_confidence\": "
                 << sample.max_head_confidence
                 << ", \"detection_count_by_class\": [";
            for (std::size_t class_index = 0;
                 class_index < sample.detection_count_by_class.size();
                 ++class_index) {
                if (class_index != 0) json << ", ";
                json << sample.detection_count_by_class[class_index];
            }
            json << "], \"max_confidence_by_class\": [";
            for (std::size_t class_index = 0;
                 class_index < sample.max_confidence_by_class.size();
                 ++class_index) {
                if (class_index != 0) json << ", ";
                json << sample.max_confidence_by_class[class_index];
            }
            json << ']'
                 << ", \"explicit_device_copy\": "
                 << bool_name(
                     sample.profile.detector.explicit_device_copy)
                 << ", \"gpu_preprocess\": "
                 << bool_name(sample.profile.detector.gpu_preprocess)
                 << ", \"d3d11_cuda_interop\": "
                 << bool_name(
                     sample.profile.detector.d3d11_cuda_interop)
                 << ", \"d3d11_directml_interop\": "
                 << bool_name(
                     sample.profile.detector.d3d11_directml_interop)
                 << ", \"input_upload_bytes\": "
                 << sample.profile.detector.input_upload_bytes
                 << ", \"input_device_copy_bytes\": "
                 << sample.profile.detector.input_device_copy_bytes
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
                 << ", \"total_ms\": " << sample.profile.total_ms
                 << ", \"performance_probes\": "
                 << bool_name(sample.service.valid)
                 << ", \"ndi_probe_valid\": "
                 << bool_name(sample.capture_stages.ndi_valid)
                 << ", \"runtime_handoff_valid\": "
                 << bool_name(
                     sample.capture_stages.runtime_handoff_valid)
                 << ", \"ndi_receive_call_ms\": "
                 << sample.capture_stages.receive_call_ms
                 << ", \"ndi_metadata_ms\": "
                 << sample.capture_stages.metadata_ms
                 << ", \"ndi_geometry_ms\": "
                 << sample.capture_stages.geometry_ms
                 << ", \"ndi_pool_acquire_ms\": "
                 << sample.capture_stages.pool_acquire_ms
                 << ", \"ndi_color_convert_ms\": "
                 << sample.capture_stages.color_convert_ms
                 << ", \"ndi_performance_query_sampled\": "
                 << bool_name(
                     sample.capture_stages.performance_query_sampled)
                 << ", \"ndi_performance_query_ms\": "
                 << sample.capture_stages.performance_query_ms
                 << ", \"ndi_queue_depth_sampled\": "
                 << bool_name(sample.capture_stages.queue_depth_sampled)
                 << ", \"ndi_queue_query_ms\": "
                 << sample.capture_stages.queue_query_ms
                 << ", \"ndi_queued_video_frames\": "
                 << sample.capture_stages.queued_video_frames
                 << ", \"ndi_queued_audio_frames\": "
                 << sample.capture_stages.queued_audio_frames
                 << ", \"ndi_queued_metadata_frames\": "
                 << sample.capture_stages.queued_metadata_frames
                 << ", \"ndi_pool_publish_ms\": "
                 << sample.capture_stages.pool_publish_ms
                 << ", \"runtime_capture_grab_ms\": "
                 << sample.capture_stages.runtime_capture_grab_ms
                 << ", \"runtime_queue_publish_ms\": "
                 << sample.capture_stages.runtime_queue_publish_ms
                 << ", \"preview_attempted\": "
                 << bool_name(sample.service.preview_attempted)
                 << ", \"preview_published\": "
                 << bool_name(sample.service.preview_published)
                 << ", \"preview_ms\": " << sample.service.preview_ms
                 << ", \"snapshot_ms\": " << sample.service.snapshot_ms
                 << ", \"snapshot_lock_wait_ms\": "
                 << sample.service.snapshot_lock_wait_ms
                 << ", \"debug_ring_ms\": "
                 << sample.service.debug_ring_ms
                 << ", \"profile_window_ms\": "
                 << sample.service.profile_window_ms
                 << ", \"service_tail_ms\": "
                 << sample.service.service_tail_ms
                 << ", \"pipeline_service_ms\": "
                 << sample.service.pipeline_service_ms
                 << ", \"pipeline_complete_ms\": "
                 << sample.service.pipeline_complete_ms
                 << ", \"source_dropped_frames\": "
                 << sample.source_dropped_frames
                 << ", \"transport_dropped_frames\": "
                 << sample.transport_dropped_frames
                 << ", \"transport_invalid_packets\": "
                 << sample.transport_invalid_packets
                 << ", \"runtime_overwritten_frames\": "
                 << sample.runtime_overwritten_frames << "}"
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
