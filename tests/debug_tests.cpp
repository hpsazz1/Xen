#include "debug/debug.h"

#include "log/log.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <windows.h>

namespace {

int failures = 0;

struct Win32HandleCloser {
    void operator()(void* handle) const noexcept {
        if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
};

using ScopedWin32Handle = std::unique_ptr<void, Win32HandleCloser>;

constexpr std::string_view kDebugPairTestRootPrefix =
    "xen-debug-pair-test-";
constexpr std::string_view kDebugPairTestOwnerFile =
    ".xen-debug-pair-test-owner";

struct OwnedDebugPairTestRoot {
    std::filesystem::path temporary_base;
    std::filesystem::path path;
    std::filesystem::path owner_path;
    std::string guid;
    std::string owner_value;
};

std::string make_test_run_guid() {
    std::string guid = "00000000-0000-4000-8000-000000000000";
    constexpr char hexadecimal[] = "0123456789abcdef";
    std::random_device random;
    for (char& character : guid) {
        if (character == '0') {
            character = hexadecimal[random() & 0x0fU];
        }
    }
    guid[14] = '4';
    guid[19] = hexadecimal[8U + (random() & 0x03U)];
    return guid;
}

bool query_non_reparse_path(const std::filesystem::path& path,
                            std::string& error,
                            DWORD* attributes = nullptr) {
    const DWORD value = GetFileAttributesW(path.c_str());
    if (value == INVALID_FILE_ATTRIBUTES) {
        error = "无法读取临时测试路径属性，Win32Error=" +
            std::to_string(GetLastError()) + ", path=" + path.string();
        return false;
    }
    if ((value & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        error = "临时测试路径包含 reparse point，拒绝清理: " +
            path.string();
        return false;
    }
    if (attributes) *attributes = value;
    return true;
}

bool validate_existing_path_chain(const std::filesystem::path& path,
                                  std::string& error) {
    auto current = std::filesystem::absolute(path).lexically_normal();
    for (;;) {
        if (!query_non_reparse_path(current, error)) return false;
        const auto parent = current.parent_path();
        if (parent.empty() || parent == current) return true;
        current = parent;
    }
}

bool owner_sentinel_matches(const OwnedDebugPairTestRoot& root,
                            std::string& error) {
    DWORD attributes = 0;
    if (!query_non_reparse_path(root.owner_path, error, &attributes) ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
        if (error.empty()) error = "临时测试 owner sentinel 不是文件";
        return false;
    }
    std::ifstream input(root.owner_path, std::ios::binary);
    const std::string actual(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    if (!input.is_open() || actual != root.owner_value) {
        error = "临时测试 owner sentinel 不属于本轮";
        return false;
    }
    return true;
}

bool create_owned_debug_pair_test_root(
        OwnedDebugPairTestRoot& root,
        std::string& error) {
    root = {};
    root.temporary_base = std::filesystem::absolute(
        std::filesystem::temp_directory_path()).lexically_normal();
    if (root.temporary_base.filename().empty()) {
        root.temporary_base = root.temporary_base.parent_path();
    }
    if (!validate_existing_path_chain(root.temporary_base, error)) {
        return false;
    }
    root.guid = make_test_run_guid();
    root.path = root.temporary_base /
        (std::string(kDebugPairTestRootPrefix) + root.guid);
    std::error_code create_error;
    if (!std::filesystem::create_directory(root.path, create_error)) {
        error = create_error
            ? "创建本轮 GUID 临时测试根失败: " + create_error.message()
            : "本轮 GUID 临时测试根已存在，拒绝复用";
        return false;
    }
    root.owner_path = root.path / std::string(kDebugPairTestOwnerFile);
    root.owner_value = "xen-debug-pair-test-owner:" + root.guid;
    std::ofstream owner(root.owner_path, std::ios::binary);
    owner << root.owner_value;
    owner.close();
    if (!owner) {
        error = "写入本轮临时测试 owner sentinel 失败";
        return false;
    }
    return owner_sentinel_matches(root, error);
}

bool cleanup_owned_debug_pair_test_root(
        const OwnedDebugPairTestRoot& root,
        std::string& error) {
    const auto normalized_root =
        std::filesystem::absolute(root.path).lexically_normal();
    const auto expected_leaf =
        std::string(kDebugPairTestRootPrefix) + root.guid;
    if (normalized_root.parent_path() != root.temporary_base ||
        normalized_root.filename() != expected_leaf ||
        root.owner_path != normalized_root /
            std::string(kDebugPairTestOwnerFile)) {
        error = "临时测试根不是本轮 temp base 的 GUID direct child";
        return false;
    }
    DWORD root_attributes = 0;
    if (!validate_existing_path_chain(normalized_root, error) ||
        !query_non_reparse_path(
            normalized_root, error, &root_attributes) ||
        (root_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        !owner_sentinel_matches(root, error)) {
        if (error.empty()) error = "本轮临时测试根不是目录";
        return false;
    }

    try {
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(normalized_root)) {
            if (!query_non_reparse_path(entry.path(), error)) return false;
        }
    } catch (const std::filesystem::filesystem_error& exception) {
        error = "枚举本轮临时测试根失败: " +
            std::string(exception.what());
        return false;
    }

    std::error_code remove_error;
    std::filesystem::remove_all(normalized_root, remove_error);
    std::error_code exists_error;
    const bool still_exists =
        std::filesystem::exists(normalized_root, exists_error);
    if (remove_error || exists_error || still_exists) {
        error = "清理本轮临时测试根失败: remove=" +
            remove_error.message() + ", exists=" +
            exists_error.message();
        return false;
    }
    return true;
}

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
    sample.profile.source_time_basis = SourceTimeBasis::NDI_SDK_SUBMISSION;
    sample.profile.source_clock_status = SourceClockStatus::VALID;
    sample.profile.source_timing_valid = true;
    sample.profile.source_to_capture_ms = 2.4;
    sample.profile.source_to_control_ms = 3.5;
    sample.profile.source_clock_uncertainty_ms = 0.2;
    sample.profile.source_clock_round_trip_ms = 0.3;
    sample.profile.source_clock_rate = 1.000001;
    sample.profile.source_clock_mapping_age_ms = 25.0;
    sample.profile.source_clock_sample_count = 8;
    sample.profile.source_clock_session_id = 99;
    sample.profile.control_timing_valid = true;
    sample.profile.mouse_backend_completion_timing_valid = success;
    sample.profile.mouse_protocol_ack_timing_valid = success;
    sample.profile.mouse_physical_effect_timing_valid = false;
    sample.profile.capture_to_control_ms = 1.1;
    sample.profile.control_to_mouse_backend_completion_ms = 0.8;
    sample.profile.capture_to_mouse_backend_completion_ms = 1.9;
    sample.profile.source_to_mouse_backend_completion_ms = 4.3;
    sample.profile.control_to_mouse_protocol_ack_ms = 0.7;
    sample.profile.capture_to_mouse_protocol_ack_ms = 1.8;
    sample.profile.source_to_mouse_protocol_ack_ms = 4.2;
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
    sample.aim_lock_active = success;
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
        sample.aim_target.matched_observation_valid = true;
        sample.aim_target.matched_observation_x1 = 148.0f;
        sample.aim_target.matched_observation_y1 = 98.0f;
        sample.aim_target.matched_observation_x2 = 232.0f;
        sample.aim_target.matched_observation_y2 = 222.0f;
        sample.aim_target.matched_observation_head_only = false;
        sample.aim_target.matched_observation_aim_from_head = true;
        sample.aim_target.base_aim_x = 200.0f;
        sample.aim_target.base_aim_y = 140.0f;
        sample.aim_target.delay_compensated_aim_x = 210.0f;
        sample.aim_target.delay_compensated_aim_y = 140.0f;
        sample.aim_target.aim_x = 240.0f;
        sample.aim_target.aim_y = 140.0f;
        sample.aim_target.velocity_x = 250.0f;
        sample.aim_target.lead_x = 40.0f;
        sample.aim_target.delay_compensation_x = 10.0f;
        sample.aim_target.delay_compensation_ms_x = 8.0f;
        sample.aim_target.delay_compensation_ms_y = 8.0f;
        sample.aim_target.delay_compensation_ms = 8.0f;
        sample.aim_target.observation_age_ms = 20.0f;
        sample.aim_target.confidence = 0.9f;
        sample.aim_target.lead_active = true;
        sample.aim_control.evaluated = true;
        sample.aim_control.controller_dt_ms = 4.167f;
        sample.aim_control.proportional_x_counts = 2.5f;
        sample.aim_control.feedforward_x_counts = 0.75f;
        sample.aim_control.desired_before_reverse_x_counts = -3.25f;
        sample.aim_control.desired_x_counts = 0.0f;
        sample.aim_control.pending_absolute_x_counts = 6.0f;
        sample.aim_control.modelled_response_x_counts = 2.25f;
        sample.aim_control.observer_phase_command_x_counts = 3.0f;
        sample.aim_control.observer_consistency_weight_x = 0.81f;
        sample.aim_control.pending_positive_x = true;
        sample.aim_control.reverse_output_direction_x = 1.0f;
        sample.aim_control.reverse_candidate_x = true;
        sample.aim_control.reverse_previous_direction_pending_x = true;
        sample.aim_control.reverse_evidence_ratio_seconds_x = 0.0002f;
        sample.aim_control.reverse_position_peak_error_x = 18.0f;
        sample.aim_control.reverse_translation_seconds_x = 0.016f;
        sample.aim_control.reverse_translation_raw_left_x_roi_pixels = -1.5f;
        sample.aim_control.reverse_translation_raw_right_x_roi_pixels = -0.5f;
        sample.aim_control.reverse_translation_raw_common_x_roi_pixels = -0.5f;
        sample.aim_control.reverse_translation_control_evidence_x = -0.65f;
        sample.aim_control.reverse_translation_gap_seconds_x = 0.008f;
        sample.aim_control.reverse_translation_fresh_evidence_x = false;
        sample.aim_control.reverse_translation_reset_reason_x =
            AimReverseTranslationResetReason::WEAK_BUDGET_EXHAUSTED;
        sample.aim_control.reverse_required_evidence_ratio_seconds_x =
            0.00042f;
        sample.aim_control.reverse_probe_direction_x = -1.0f;
        sample.aim_control.reverse_probe_age_ms_x = 8.334f;
        sample.aim_control.reverse_gate_blocked_x = true;
        sample.aim_control.reverse_translation_ready_x = true;
        sample.aim_control.reverse_position_improvement_reset_x = true;
        sample.aim_control.reverse_probe_active_x = true;
        sample.aim_control.reverse_probe_limited_x = true;
        sample.aim_command.sequence = sequence;
        sample.aim_command.dx_counts = 20;
        sample.aim_command.dy_counts = -10;
        sample.aim_landmark.status = aim_landmark::Status::VALID;
        sample.aim_landmark.semantic_kind =
            aim_landmark::SemanticKind::HEAD_BOX_CENTER;
        sample.aim_landmark.sequence = sequence;
        sample.aim_landmark.track_id = 7;
        sample.aim_landmark.candidate_count = 1;
        sample.aim_landmark.valid = true;
        sample.aim_landmark.fresh = true;
        sample.aim_landmark.x = 190.0f;
        sample.aim_landmark.y = 112.0f;
        sample.aim_landmark.x1 = 180.0f;
        sample.aim_landmark.y1 = 102.0f;
        sample.aim_landmark.x2 = 200.0f;
        sample.aim_landmark.y2 = 122.0f;
        sample.aim_landmark.confidence = 0.871f;
        sample.aim_landmark.class_id = 1;
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
    final_snapshot.output_allowed_by_config = false;
    final_snapshot.output_armed = false;
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
               summary.source_to_capture.sample_count == 2 &&
               summary.source_to_control.p50_ms == 3.5 &&
               summary.source_clock_uncertainty.p50_ms == 0.2 &&
               summary.capture_to_control.sample_count == 2 &&
               summary.control_to_mouse_backend_completion.sample_count == 2 &&
               summary.capture_to_mouse_backend_completion.p50_ms == 1.9 &&
               summary.control_to_mouse_protocol_ack.sample_count == 2 &&
               summary.control_to_mouse_physical_effect.sample_count == 0 &&
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
    const std::size_t sample_header_start =
        csv_text.find("sequence,capture_ms");
    const std::size_t sample_header_end =
        csv_text.find('\n', sample_header_start);
    const std::size_t sample_row_end =
        csv_text.find('\n', sample_header_end + 1);
    const std::string sample_header = csv_text.substr(
        sample_header_start, sample_header_end - sample_header_start);
    const std::string sample_row = csv_text.substr(
        sample_header_end + 1, sample_row_end - sample_header_end - 1);
    expect(std::count(sample_header.begin(), sample_header.end(), ',') ==
               std::count(sample_row.begin(), sample_row.end(), ','),
           "schema 17 CSV 样本头与数据行必须保持完全相同的列数");
    expect(csv_text.find("Xen Runtime Debug Report v17") !=
                   std::string::npos &&
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
                 csv_text.find("aim_base_x,aim_base_y,aim_delay_compensated_x") !=
                     std::string::npos &&
                 csv_text.find("aim_matched_observation_valid") !=
                     std::string::npos &&
                 csv_text.find("aim_matched_observation_x1") !=
                     std::string::npos &&
                 csv_text.find("aim_matched_observation_aim_from_head") !=
                     std::string::npos &&
                 csv_text.find(
                     "aim_landmark_status,aim_landmark_semantic_kind") !=
                     std::string::npos &&
                 csv_text.find(
                     "VALID,HEAD_BOX_CENTER,4,7,1,true,true,false,false") !=
                     std::string::npos &&
                csv_text.find("mouse_sent,aim_lock_active") !=
                    std::string::npos &&
                csv_text.find("aim_control_evaluated,aim_controller_dt_ms") !=
                    std::string::npos &&
                csv_text.find("aim_reverse_gate_blocked_x") !=
                    std::string::npos &&
                csv_text.find("aim_modelled_response_x_counts") !=
                    std::string::npos &&
                csv_text.find("aim_reverse_probe_age_ms_x") !=
                    std::string::npos &&
                csv_text.find("aim_reverse_translation_ready_x") !=
                    std::string::npos &&
                csv_text.find(
                    "aim_reverse_translation_raw_left_x_roi_pixels") !=
                    std::string::npos &&
                csv_text.find(
                    "aim_reverse_translation_reset_reason_x") !=
                    std::string::npos &&
                csv_text.find("WEAK_BUDGET_EXHAUSTED") !=
                    std::string::npos &&
                csv_text.find("aim_reverse_position_peak_error_x") !=
                    std::string::npos &&
                csv_text.find(
                    "aim_reverse_position_improvement_reset_x") !=
                    std::string::npos &&
               csv_text.find("ndi_receive_call_ms") != std::string::npos &&
               csv_text.find("pipeline_complete_ms") != std::string::npos &&
               csv_text.find("capture_to_mouse_backend_completion_ms") !=
                   std::string::npos &&
               csv_text.find("mouse_physical_effect_timing_valid") !=
                   std::string::npos &&
               csv_text.find("source_clock_uncertainty_ms") !=
                   std::string::npos &&
               csv_text.find("NDI_SDK_SUBMISSION,VALID,true") !=
                   std::string::npos &&
               csv_text.find("# coverage_phase,formal,3") !=
                   std::string::npos &&
               csv_text.find(",1,2,") != std::string::npos &&
               csv_text.find("\"1;2;0;0;") != std::string::npos,
           "CSV 必须包含 schema、分类置信度、失败状态、预览状态和最终几何");
    expect(json_text.find("\"schema\": 17") != std::string::npos &&
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
                json_text.find("\"aim_controller_dt_ms\": 4.") !=
                    std::string::npos &&
                json_text.find("\"aim_lock_active\": true") !=
                    std::string::npos &&
                json_text.find("\"aim_reverse_candidate_x\": true") !=
                    std::string::npos &&
                json_text.find(
                    "\"aim_modelled_response_x_counts\": 2.25") !=
                    std::string::npos &&
                json_text.find(
                    "\"aim_observer_consistency_weight_x\": 0.81") !=
                    std::string::npos &&
                json_text.find("\"aim_reverse_gate_blocked_x\": true") !=
                    std::string::npos &&
                json_text.find("\"aim_reverse_probe_active_x\": true") !=
                    std::string::npos &&
                json_text.find("\"aim_reverse_probe_age_ms_x\": 8.") !=
                    std::string::npos &&
                json_text.find(
                    "\"aim_reverse_translation_ready_x\": true") !=
                    std::string::npos &&
                json_text.find(
                    "\"aim_reverse_translation_raw_left_x_roi_pixels\": -1.5") !=
                    std::string::npos &&
                json_text.find(
                    "\"aim_reverse_translation_gap_seconds_x\": 0.008") !=
                    std::string::npos &&
                json_text.find(
                    "\"aim_reverse_translation_fresh_evidence_x\": false") !=
                    std::string::npos &&
                json_text.find(
                    "\"aim_reverse_translation_reset_reason_x\": \"WEAK_BUDGET_EXHAUSTED\"") !=
                    std::string::npos &&
                json_text.find(
                    "\"aim_reverse_position_peak_error_x\": 18") !=
                    std::string::npos &&
                json_text.find(
                    "\"aim_reverse_position_improvement_reset_x\": true") !=
                    std::string::npos &&
               json_text.find("\"final_snapshot\"") != std::string::npos &&
               json_text.find("\"output_armed\": false") !=
                   std::string::npos &&
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
                 json_text.find("\"aim_matched_observation_valid\"") !=
                     std::string::npos &&
                 json_text.find("\"aim_matched_observation_box\"") !=
                     std::string::npos &&
                 json_text.find("\"aim_matched_observation_aim_from_head\"") !=
                     std::string::npos &&
                 json_text.find("\"aim_landmark_status\": \"VALID\"") !=
                     std::string::npos &&
                 json_text.find(
                     "\"aim_landmark_semantic_kind\": \"HEAD_BOX_CENTER\"") !=
                     std::string::npos &&
                 json_text.find("\"aim_landmark_control_eligible\": false") !=
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
               json_text.find("\"aim_delay_compensation_ms_x\": 8") !=
                    std::string::npos &&
               json_text.find("\"aim_delay_compensation_ms_y\": 8") !=
                    std::string::npos &&
               json_text.find(
                   "\"capture_to_mouse_backend_completion_ms\": 1.9") !=
                   std::string::npos &&
               json_text.find(
                   "\"mouse_protocol_ack_timing_valid\": true") !=
                   std::string::npos &&
               json_text.find(
                   "\"mouse_physical_effect_timing_valid\": false") !=
                   std::string::npos &&
               json_text.find("\"source_clock_status\": \"VALID\"") !=
                   std::string::npos &&
               json_text.find("\"source_clock_session_id\": 99") !=
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

void test_report_pair_publish_failure_preserves_previous_pair() {
    OwnedDebugPairTestRoot owned_root;
    std::string ownership_error;
    const bool root_created = create_owned_debug_pair_test_root(
        owned_root, ownership_error);
    expect(root_created,
           "必须创建带 owner sentinel 的本轮 GUID 临时测试根: " +
               ownership_error);
    if (!root_created) return;
    const auto& root = owned_root.path;
    const auto cleanup_root = [&]() {
        std::string cleanup_error;
        const bool cleaned = cleanup_owned_debug_pair_test_root(
            owned_root, cleanup_error);
        expect(cleaned,
               "只能清理本轮 owner sentinel 未变且无 reparse 的临时根: " +
                   cleanup_error);
    };

    const auto csv_path = root / "runtime.csv";
    const auto json_path = root / "runtime.json";
    DebugReportConfig config;
    config.csv_path = csv_path.string();
    config.json_path = json_path.string();
    config.session_id = "old-session";
    config.max_samples = 1;
    std::string error;
    {
        DebugReport previous_report;
        expect(previous_report.start(config, error),
               "旧 report pair 应通过 DebugReport 公有 seam 启动: " + error);
        if (!previous_report.active()) {
            cleanup_root();
            return;
        }
        const RuntimePipelineSample sample = make_sample(1, 1.0, true);
        previous_report.ingest(
            std::span<const RuntimePipelineSample>(&sample, 1));
        RuntimeSnapshot final_snapshot;
        expect(previous_report.finalize(final_snapshot, error),
               "旧 report pair 应通过 DebugReport 公有 seam 发布: " + error);
    }
    std::ifstream previous_csv_stream(csv_path, std::ios::binary);
    std::ifstream previous_json_stream(json_path, std::ios::binary);
    const std::string previous_csv(
        (std::istreambuf_iterator<char>(previous_csv_stream)),
        std::istreambuf_iterator<char>());
    const std::string previous_json(
        (std::istreambuf_iterator<char>(previous_json_stream)),
        std::istreambuf_iterator<char>());
    previous_csv_stream.close();
    previous_json_stream.close();
    expect(previous_csv.find("# session_id,\"old-session\"") !=
                   std::string::npos &&
               previous_json.find("\"session_id\": \"old-session\"") !=
                   std::string::npos,
           "公有 seam 必须预置可识别的完整旧 report pair");

    ScopedWin32Handle json_reader(CreateFileW(
        json_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    expect(json_reader.get() != INVALID_HANDLE_VALUE,
           "测试必须成功锁定旧 JSON 才能注入第二次发布失败");
    if (json_reader.get() == INVALID_HANDLE_VALUE) {
        cleanup_root();
        return;
    }

    DebugReport report;
    config.session_id = "new-session";
    expect(report.start(config, error),
           "Debug report pair 测试应成功启动: " + error);
    if (report.active()) {
        const RuntimePipelineSample sample = make_sample(1, 1.0, true);
        report.ingest(std::span<const RuntimePipelineSample>(&sample, 1));
        RuntimeSnapshot final_snapshot;
        const bool finalized = report.finalize(final_snapshot, error);
        expect(!finalized && !error.empty(),
               "锁定 JSON 必须让第二次报告发布显式失败");
    }
    json_reader.reset();

    std::ifstream csv(csv_path, std::ios::binary);
    std::ifstream json(json_path, std::ios::binary);
    const std::string csv_text(
        (std::istreambuf_iterator<char>(csv)),
        std::istreambuf_iterator<char>());
    const std::string json_text(
        (std::istreambuf_iterator<char>(json)),
        std::istreambuf_iterator<char>());
    expect(csv_text == previous_csv && json_text == previous_json,
           "第二次发布失败后消费者必须仍读到完整旧 report pair");
    csv.close();
    json.close();

    const auto directory_has_only_pair = [&]() {
        std::size_t target_file_count = 0;
        for (const auto& entry : std::filesystem::directory_iterator(root)) {
            if (entry.path() == owned_root.owner_path) continue;
            if (entry.path() != csv_path && entry.path() != json_path) {
                return false;
            }
            ++target_file_count;
        }
        return target_file_count == 2;
    };
    expect(directory_has_only_pair(),
           "report pair 发布失败后目录只能保留旧 CSV/JSON 目标");

    RuntimeSnapshot retry_snapshot;
    expect(report.finalize(retry_snapshot, error),
           "解除第二次发布故障后应提交新 report pair: " + error);
    std::ifstream committed_csv_stream(csv_path, std::ios::binary);
    std::ifstream committed_json_stream(json_path, std::ios::binary);
    const std::string committed_csv(
        (std::istreambuf_iterator<char>(committed_csv_stream)),
        std::istreambuf_iterator<char>());
    const std::string committed_json(
        (std::istreambuf_iterator<char>(committed_json_stream)),
        std::istreambuf_iterator<char>());
    expect(committed_csv.find("# session_id,\"new-session\"") !=
                   std::string::npos &&
               committed_json.find("\"session_id\": \"new-session\"") !=
                   std::string::npos,
           "故障解除后的提交必须让 CSV/JSON 同时属于新 session");
    committed_csv_stream.close();
    committed_json_stream.close();
    expect(directory_has_only_pair(),
           "已有 pair 成功提交后不得遗留临时或回滚文件");
    cleanup_root();
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

void test_aim_lock_active_marker_lifecycle() {
    const auto root = std::filesystem::temp_directory_path() /
                      "xen_debug_aim_lock_marker_test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    DebugReportConfig config;
    config.csv_path = (root / "runtime.csv").string();
    config.json_path = (root / "runtime.json").string();
    config.session_id = "aim-lock-session";
    config.max_samples = 4;
    const auto marker = std::filesystem::path(
        config.json_path + ".aim-lock-active");
    std::string error;

    {
        DebugReport report;
        expect(report.start(config, error),
               "Aim-lock marker 测试报告应正常启动: " + error);
        if (!report.active()) return;

        RuntimePipelineSample inactive = make_sample(1, 1.0, false);
        inactive.aim_lock_active = false;
        report.ingest(std::span<const RuntimePipelineSample>(&inactive, 1));
        expect(!std::filesystem::exists(marker),
               "未激活 aim-lock 时不得发布 marker");

        RuntimePipelineSample active = make_sample(2, 1.0, true);
        active.aim_lock_active = true;
        report.ingest(std::span<const RuntimePipelineSample>(&active, 1));
        expect(std::filesystem::exists(marker),
               "首次观察到 aim-lock active 时必须发布 marker");
        std::ifstream marker_stream(marker);
        const std::string marker_text(
            (std::istreambuf_iterator<char>(marker_stream)),
            std::istreambuf_iterator<char>());
        marker_stream.close();
        expect(marker_text.find("\"schema\": 2") != std::string::npos &&
                   marker_text.find(
                       "\"session_id\": \"aim-lock-session\"") !=
                       std::string::npos &&
                   marker_text.find(
                       "\"gate\": \"AIM_LOCK_ACTIVE\"") !=
                       std::string::npos &&
                   marker_text.find("\"activation_epoch\": 1") !=
                       std::string::npos &&
                   marker_text.find("\"sequence\": 2") !=
                       std::string::npos,
               "aim-lock marker 必须公开 session、activation epoch 和 sequence");

        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        active.sequence = 3;
        report.ingest(std::span<const RuntimePipelineSample>(&active, 1));
        expect(std::filesystem::exists(marker),
               "aim-lock 连续激活期间必须保留 marker");
        std::ifstream heartbeat_stream(marker);
        const std::string heartbeat_text(
            (std::istreambuf_iterator<char>(heartbeat_stream)),
            std::istreambuf_iterator<char>());
        heartbeat_stream.close();
        expect(heartbeat_text.find("\"activation_epoch\": 1") !=
                       std::string::npos &&
                   heartbeat_text.find("\"sequence\": 3") !=
                       std::string::npos,
               "连续 aim-lock 必须刷新同一 activation epoch 的 heartbeat");

        inactive.sequence = 4;
        report.ingest(std::span<const RuntimePipelineSample>(&inactive, 1));
        expect(!std::filesystem::exists(marker),
               "观察到 aim-lock inactive 时必须移除 marker");

        active.sequence = 5;
        report.ingest(std::span<const RuntimePipelineSample>(&active, 1));
        expect(std::filesystem::exists(marker),
               "aim-lock 再次激活时必须重新发布 marker");
        std::ifstream reactivated_stream(marker);
        const std::string reactivated_text(
            (std::istreambuf_iterator<char>(reactivated_stream)),
            std::istreambuf_iterator<char>());
        reactivated_stream.close();
        expect(reactivated_text.find("\"activation_epoch\": 2") !=
                       std::string::npos &&
                   reactivated_text.find("\"sequence\": 5") !=
                       std::string::npos,
               "false 到 true 必须进入新的 activation epoch");
        RuntimeSnapshot final_snapshot;
        expect(report.finalize(final_snapshot, error),
               "Aim-lock marker 测试报告应正常发布: " + error);
        expect(!std::filesystem::exists(marker),
               "DebugReport finalize 必须移除 aim-lock marker");
    }

    {
        DebugReport report;
        expect(report.start(config, error),
               "析构清理测试报告应正常启动: " + error);
        RuntimePipelineSample active = make_sample(6, 1.0, true);
        active.aim_lock_active = true;
        report.ingest(std::span<const RuntimePipelineSample>(&active, 1));
        expect(std::filesystem::exists(marker),
               "析构清理前必须存在 aim-lock marker");
    }
    expect(!std::filesystem::exists(marker),
           "DebugReport 析构必须移除 aim-lock marker");

    const auto blocked_parent = root / "blocked-parent";
    {
        std::ofstream blocker(blocked_parent);
        blocker << "not-a-directory";
    }
    DebugReportConfig blocked_config = config;
    blocked_config.csv_path =
        (blocked_parent / "runtime.csv").string();
    blocked_config.json_path =
        (blocked_parent / "runtime.json").string();
    const auto blocked_marker = std::filesystem::path(
        blocked_config.json_path + ".aim-lock-active");
    {
        DebugReport report;
        expect(report.start(blocked_config, error),
               "marker 路径不可写不应阻止 DebugReport 启动: " + error);
        RuntimePipelineSample active = make_sample(7, 1.0, true);
        active.aim_lock_active = true;
        report.ingest(std::span<const RuntimePipelineSample>(&active, 1));
        std::error_code marker_error;
        expect(report.active() &&
                   !std::filesystem::exists(blocked_marker, marker_error),
               "marker I/O 失败必须保持缺席且不得终止 DebugReport");
    }

    std::filesystem::remove_all(root, ignored);
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
    test_report_pair_publish_failure_preserves_previous_pair();
    test_report_rejects_invalid_capacity();
    test_aim_lock_active_marker_lifecycle();
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
