#include "debug/debug.h"

#include "log/log.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << message << '\n';
}

RuntimePipelineSample make_sample(
        std::uint64_t sequence,
        double total_ms,
        bool success) {
    RuntimePipelineSample sample;
    sample.sequence = sequence;
    sample.geometry.encoded_width = 320;
    sample.geometry.encoded_height = 320;
    sample.geometry.source_width = 2560;
    sample.geometry.source_height = 1440;
    sample.geometry.roi_x = 1120.0;
    sample.geometry.roi_y = 560.0;
    sample.geometry.roi_width = 320;
    sample.geometry.roi_height = 320;
    sample.geometry.source_pixels_per_pixel_x = 1.0;
    sample.geometry.source_pixels_per_pixel_y = 1.0;
    sample.profile.capture_ms = 0.2;
    sample.profile.queue_ms = 0.3;
    sample.profile.detector.preprocess_ms = 0.4;
    sample.profile.detector.inference_ms = 0.5;
    sample.profile.detector.h2d_ms = 0.1;
    sample.profile.detector.d3d11_to_cuda_ms = 0.02;
    sample.profile.detector.d3d11_to_directml_ms = 0.03;
    sample.profile.detector.gpu_preprocess_ms = 0.05;
    sample.profile.detector.explicit_device_copy = true;
    sample.profile.detector.gpu_preprocess = true;
    sample.profile.detector.d3d11_cuda_interop = true;
    sample.profile.detector.input_upload_bytes = 0;
    sample.profile.detector.input_device_copy_bytes = 409600;
    sample.profile.detector.execution_ms = 0.2;
    sample.profile.detector.d2h_ms = 0.1;
    sample.profile.detector.postprocess_ms = 0.3;
    sample.profile.aim.total_ms = 0.6;
    sample.profile.mouse_ms = 0.7;
    sample.profile.total_ms = total_ms;
    sample.capture_stages.ndi_valid = true;
    sample.capture_stages.runtime_handoff_valid = true;
    sample.capture_stages.receive_call_ms = 0.11;
    sample.capture_stages.metadata_ms = 0.01;
    sample.capture_stages.geometry_ms = 0.02;
    sample.capture_stages.pool_acquire_ms = 0.01;
    sample.capture_stages.color_convert_ms = 0.03;
    sample.capture_stages.performance_query_sampled = true;
    sample.capture_stages.performance_query_ms = 0.004;
    sample.capture_stages.queue_depth_sampled = true;
    sample.capture_stages.queue_query_ms = 0.005;
    sample.capture_stages.queued_video_frames = 2;
    sample.capture_stages.pool_publish_ms = 0.01;
    sample.capture_stages.runtime_capture_grab_ms = 0.02;
    sample.capture_stages.runtime_queue_publish_ms = 0.01;
    sample.service.valid = true;
    sample.service.preview_attempted = true;
    sample.service.preview_ms = 0.01;
    sample.service.snapshot_ms = 0.10;
    sample.service.snapshot_lock_wait_ms = 0.01;
    sample.service.debug_ring_ms = 0.02;
    sample.service.profile_window_ms = 0.03;
    sample.service.service_tail_ms = 0.20;
    sample.service.pipeline_complete_ms = total_ms + 0.20;
    sample.service.pipeline_service_ms =
        sample.service.pipeline_complete_ms - sample.profile.queue_ms;
    sample.source_dropped_frames = sequence;
    sample.runtime_overwritten_frames = sequence - 1;
    sample.detection_status = success
        ? DetectionStatus::SUCCESS : DetectionStatus::INFERENCE_FAILED;
    sample.aim_status = success ? AimStatus::SUCCESS : AimStatus::NOT_RUN;
    sample.mouse_status = success ? MouseStatus::READY : MouseStatus::CLOSED;
    sample.mouse_sent = success;
    if (success) {
        sample.aim_control_center_x = 160.0f;
        sample.aim_control_center_y = 160.0f;
        sample.aim_acquisition_range_radius = 144.0f;
        sample.aim_active_range_radius = 72.0f;
        sample.aim_has_target = true;
        sample.aim_has_command = true;
        sample.aim_range_locked = true;
        sample.aim_range_allows_control = true;
        sample.aim_base_point_inside_box = true;
        sample.aim_prediction_point_outside_box = true;
        sample.aim_command_toward_target = true;
        sample.aim_target.track_id = 7;
        sample.aim_target.state = TrackState::CONFIRMED;
        sample.aim_target.x1 = 150.0f;
        sample.aim_target.y1 = 100.0f;
        sample.aim_target.x2 = 230.0f;
        sample.aim_target.y2 = 220.0f;
        sample.aim_target.base_aim_x = 200.0f;
        sample.aim_target.base_aim_y = 140.0f;
        sample.aim_target.aim_x = 240.0f;
        sample.aim_target.aim_y = 140.0f;
        sample.aim_target.velocity_x = 250.0f;
        sample.aim_target.lead_x = 40.0f;
        sample.aim_target.observation_age_ms = 20.0f;
        sample.aim_target.confidence = 0.9f;
        sample.aim_target.lead_active = true;
        sample.aim_command.sequence = sequence;
        sample.aim_command.dx_counts = 20;
        sample.aim_command.dy_counts = -10;
    }
    sample.person_detection_count = 1;
    sample.head_detection_count = 2;
    sample.max_person_confidence = 0.864f;
    sample.max_head_confidence = 0.871f;
    sample.detection_count_by_class[0] = 1;
    sample.detection_count_by_class[1] = 2;
    sample.max_confidence_by_class[0] = 0.864f;
    sample.max_confidence_by_class[1] = 0.871f;
    return sample;
}

void test_report_summary_and_atomic_files() {
    const auto root = std::filesystem::temp_directory_path() /
                      "xen_debug_report_test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    DebugReport report;
    DebugReportConfig config;
    config.csv_path = (root / "nested" / "runtime.csv").string();
    config.json_path = (root / "nested" / "runtime.json").string();
    config.session_id = "test-session";
    config.model_path = "models/test.onnx";
    config.provider = "CPUExecutionProvider";
    config.capture_backend = "UDP_MJPEG";
    config.mouse_backend = "kmbox_net";
    config.max_samples = 3;
    config.performance_probes_enabled = true;
    std::string error;
    expect(report.start(config, error), "Debug 报告合法配置应成功启动");
    if (!report.active()) return;

    const std::vector<RuntimePipelineSample> samples{
        make_sample(1, 1.0, true),
        make_sample(2, 100.0, false),
        make_sample(3, 3.0, true),
        make_sample(4, 5.0, true)};
    report.ingest(samples);
    RuntimeSnapshot final_snapshot;
    final_snapshot.debug_samples_dropped = 7;
    final_snapshot.provider = "CPUExecutionProvider";
    final_snapshot.active_model_path = "models/test.onnx";
    final_snapshot.detector_generation = 3;
    final_snapshot.captured_frames = 5;
    final_snapshot.processed_frames = 4;
    final_snapshot.failed_frames = 1;
    final_snapshot.source_dropped_frames = 2;
    final_snapshot.duplication_recoveries = 6;
    final_snapshot.transport_dropped_frames = 3;
    final_snapshot.transport_invalid_packets = 4;
    final_snapshot.overwritten_frames = 1;
    final_snapshot.mouse_commands = 0;
    final_snapshot.preview_enabled = true;
    final_snapshot.preview_sampled_frames = 123;
    final_snapshot.preview_dropped_frames = 4;
    final_snapshot.encoded_width = 320;
    final_snapshot.encoded_height = 320;
    final_snapshot.source_width = 2560;
    final_snapshot.source_height = 1440;
    final_snapshot.capture_roi_x = 1120.0;
    final_snapshot.capture_roi_y = 560.0;
    final_snapshot.capture_roi_width = 320;
    final_snapshot.capture_roi_height = 320;
    final_snapshot.source_pixels_per_pixel_x = 1.0;
    final_snapshot.source_pixels_per_pixel_y = 1.0;
    DebugCoverageSummary coverage;
    coverage.available = true;
    coverage.warmup_start_overwritten_frames = 1;
    coverage.warmup_end_overwritten_frames = 2;
    coverage.formal_end_overwritten_frames = 4;
    coverage.startup = {1, 1, 1, 1, 1, 0, true};
    coverage.warmup = {1, 2, 2, 1, 1, 0, true};
    coverage.formal = {3, 3, 5, 2, 1, 1, true};
    expect(report.finalize(final_snapshot, error, &coverage),
           "Debug 报告应原子发布 CSV 和 JSON: " + error);
    const auto& summary = report.summary();
    expect(summary.sample_count == 3 && summary.successful_samples == 2 &&
               summary.failed_samples == 1 &&
               summary.report_samples_dropped == 1 &&
               summary.runtime_samples_dropped == 7,
           "报告必须区分保留样本、失败样本和两级丢弃统计");
    expect(summary.total.sample_count == 2 &&
               summary.total.mean_ms == 4.0 &&
               summary.total.p50_ms == 4.0 &&
               summary.total.max_ms == 5.0 &&
               summary.total.p95_ms > 4.8 &&
               summary.total.p95_ms < 5.0,
           "失败样本不得进入成功耗时的均值和分位数");
    expect(summary.pipeline_complete.sample_count == 2 &&
               summary.ndi_receive_call.sample_count == 2 &&
               summary.ndi_video_queue_depth.sample_count == 2 &&
               summary.ndi_video_queue_depth.mean_frames == 2.0 &&
               summary.coverage.available &&
               summary.coverage.formal.sample_count == 3,
           "探针分段、NDI 队列深度和覆盖阶段必须独立汇总");

    std::ifstream csv(root / "nested" / "runtime.csv");
    std::ifstream json(root / "nested" / "runtime.json");
    const std::string csv_text(
        (std::istreambuf_iterator<char>(csv)),
        std::istreambuf_iterator<char>());
    const std::string json_text(
        (std::istreambuf_iterator<char>(json)),
        std::istreambuf_iterator<char>());
    expect(csv_text.find("Xen Runtime Debug Report v8") != std::string::npos &&
               csv_text.find("sequence,capture_ms") != std::string::npos &&
               csv_text.find("d3d11_to_cuda_ms") != std::string::npos &&
               csv_text.find("d3d11_to_directml_ms") != std::string::npos &&
               csv_text.find("gpu_preprocess_ms") != std::string::npos &&
               csv_text.find("INFERENCE_FAILED") != std::string::npos &&
               csv_text.find("# final_source_width,2560") !=
                   std::string::npos &&
               csv_text.find("# final_roi_x,1120") != std::string::npos &&
               csv_text.find("# final_duplication_recoveries,6") !=
                   std::string::npos &&
               csv_text.find("# final_preview_enabled,true") !=
                   std::string::npos &&
                csv_text.find("person_detection_count,head_detection_count") !=
                    std::string::npos &&
                csv_text.find("aim_base_x,aim_base_y,aim_final_x") !=
                    std::string::npos &&
               csv_text.find("ndi_receive_call_ms") != std::string::npos &&
               csv_text.find("pipeline_complete_ms") != std::string::npos &&
               csv_text.find("# coverage_phase,formal,3") !=
                   std::string::npos &&
               csv_text.find(",1,2,") != std::string::npos &&
               csv_text.find("\"1;2;0;0;") != std::string::npos,
           "CSV 必须包含 schema、分类置信度、失败状态、预览状态和最终几何");
    expect(json_text.find("\"schema\": 8") != std::string::npos &&
               json_text.find("\"timing\"") != std::string::npos &&
               json_text.find("\"explicit_device_copy\": true") !=
                   std::string::npos &&
               json_text.find("\"gpu_preprocess\"") != std::string::npos &&
               json_text.find("\"d3d11_cuda_interop\": true") !=
                   std::string::npos &&
               json_text.find("\"d3d11_directml_interop\": false") !=
                   std::string::npos &&
               json_text.find("\"input_device_copy_bytes\": 409600") !=
                   std::string::npos &&
               json_text.find("\"runtime_samples_dropped\": 7") !=
                   std::string::npos &&
               json_text.find("\"final_snapshot\"") != std::string::npos &&
               json_text.find("\"source_width\": 2560") !=
                   std::string::npos &&
               json_text.find("\"duplication_recoveries\": 6") !=
                   std::string::npos &&
               json_text.find("\"failed_frames\": 1") !=
                   std::string::npos &&
               json_text.find("\"preview_enabled\": true") !=
                   std::string::npos &&
               json_text.find("\"preview_sampled_frames\": 123") !=
                   std::string::npos &&
                json_text.find("\"person_detection_count\": 1") !=
                    std::string::npos &&
                json_text.find("\"aim_track_id\": 7") !=
                    std::string::npos &&
                 json_text.find("\"aim_lead_active\": true") !=
                     std::string::npos &&
                 json_text.find(
                     "\"aim_base_point_inside_box\": true") !=
                     std::string::npos &&
                 json_text.find(
                     "\"aim_prediction_point_outside_box\": true") !=
                     std::string::npos &&
                json_text.find("\"aim_observation_age_ms\": 20") !=
                    std::string::npos &&
                json_text.find("\"aim_active_range_radius\": 72") !=
                    std::string::npos &&
               json_text.find("\"max_head_confidence\":") !=
                   std::string::npos &&
               json_text.find("\"detection_count_by_class\": [1, 2, 0") !=
                   std::string::npos &&
               json_text.find("\"performance_probes_enabled\": true") !=
                   std::string::npos &&
               json_text.find("\"ndi_video_queue_depth\"") !=
                   std::string::npos &&
               json_text.find("\"coverage\"") != std::string::npos &&
               json_text.find(
                   "\"trailing_runtime_overwritten_frames\": 1") !=
                   std::string::npos &&
               json_text.find("\"pipeline_complete_ms\"") !=
                   std::string::npos,
           "JSON 必须包含分类置信度、分段统计、Runtime 丢弃数和最终快照");
    bool has_temp = false;
    if (std::filesystem::exists(root / "nested")) {
        for (const auto& entry : std::filesystem::directory_iterator(
                 root / "nested")) {
            if (entry.path().string().find(".tmp.") != std::string::npos) {
                has_temp = true;
            }
        }
    }
    expect(!has_temp, "成功发布后不得遗留临时报告文件");
    std::filesystem::remove_all(root, ignored);
}

void test_report_rejects_invalid_capacity() {
    DebugReport report;
    DebugReportConfig config;
    config.max_samples = 0;
    std::string error;
    expect(!report.start(config, error) && !error.empty() &&
               !report.active(),
           "零容量 Debug 报告必须拒绝启动");
}

void test_disabled_probes_are_not_reported_as_zero_cost_samples() {
    const auto root = std::filesystem::temp_directory_path() /
                      "xen_debug_report_probe_off_test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    DebugReport report;
    DebugReportConfig config;
    config.csv_path = (root / "runtime.csv").string();
    config.json_path = (root / "runtime.json").string();
    config.session_id = "probe-off";
    config.max_samples = 1;
    config.performance_probes_enabled = false;
    std::string error;
    expect(report.start(config, error),
           "关闭性能探针时报告仍应正常启动: " + error);
    if (!report.active()) return;

    RuntimePipelineSample sample = make_sample(1, 1.0, true);
    sample.capture_stages = {};
    sample.service = {};
    report.ingest(std::span<const RuntimePipelineSample>(&sample, 1));
    RuntimeSnapshot final_snapshot;
    expect(report.finalize(final_snapshot, error),
           "关闭性能探针时报告仍应正常发布: " + error);
    const auto& summary = report.summary();
    expect(summary.total.sample_count == 1 &&
               summary.pipeline_complete.sample_count == 0 &&
               summary.runtime_handoff.sample_count == 0 &&
               summary.ndi_receive_call.sample_count == 0 &&
               summary.ndi_video_queue_depth.sample_count == 0,
           "关闭探针的默认零值不得进入任何新增分段汇总");

    std::ifstream json(root / "runtime.json");
    const std::string json_text(
        (std::istreambuf_iterator<char>(json)),
        std::istreambuf_iterator<char>());
    expect(json_text.find("\"performance_probes_enabled\": false") !=
               std::string::npos &&
               json_text.find(
                   "\"pipeline_complete\": {\"sample_count\": 0") !=
                   std::string::npos,
           "关闭探针的 JSON 必须显式区分未测量与零耗时");
    std::filesystem::remove_all(root, ignored);
}

void test_shared_success_semantics() {
    RuntimePipelineSample disabled_mouse = make_sample(1, 1.0, true);
    disabled_mouse.mouse_status = MouseStatus::DISABLED;
    disabled_mouse.mouse_sent = false;
    expect(debug_sample_succeeded(disabled_mouse),
           "未发送命令时禁用 Mouse 是合法成功样本");
    disabled_mouse.mouse_sent = true;
    expect(!debug_sample_succeeded(disabled_mouse),
           "实际发送命令时 Mouse 必须处于 READY");
    RuntimePipelineSample failed = make_sample(2, 1.0, false);
    expect(!debug_sample_succeeded(failed),
           "Detector 或 Aim 失败不得进入成功耗时分位数");
}

} // namespace

int main() {
    LogConfig log_config;
    log_config.enable_console = false;
    log_config.enable_file = false;
    log_config.enable_ringbuf = false;
    Log::init(log_config);
    test_report_summary_and_atomic_files();
    test_report_rejects_invalid_capacity();
    test_disabled_probes_are_not_reported_as_zero_cost_samples();
    test_shared_success_semantics();
    Log::shutdown();
    if (failures != 0) {
        std::cerr << "Debug 测试失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "Debug 测试全部通过。\n";
    return 0;
}
