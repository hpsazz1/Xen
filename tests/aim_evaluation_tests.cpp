#include "aim/aim_evaluation_internal.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

using aim::detail::AimEvaluationConfig;
using aim::detail::AimEvaluationFrame;
using aim::detail::AimEvaluationMetrics;
using aim::detail::AimControlContinuityMetrics;
using aim::detail::AimGroundTruthAnnotation;
using aim::detail::AimGroundTruthExpectation;
using aim::detail::AimGroundTruthState;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << message << '\n';
}

bool near(double left, double right) {
    return std::fabs(left - right) < 1e-9;
}

AimGroundTruthExpectation expectation() {
    AimGroundTruthExpectation expected;
    expected.video_file = "scene.mp4";
    expected.video_sha256 =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    expected.source_width = 2560;
    expected.source_height = 1440;
    expected.frame_count = 8;
    expected.input_mode = "center";
    expected.roi_x = 1120;
    expected.roi_y = 560;
    expected.roi_width = 320;
    expected.roi_height = 320;
    return expected;
}

std::string valid_json() {
    return R"json({
  "schema_version": 1,
  "video_file": "scene.mp4",
  "video_sha256": "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF",
  "source_width": 2560,
  "source_height": 1440,
  "frame_count": 8,
  "input_mode": "center",
  "roi_x": 1120,
  "roi_y": 560,
  "roi_width": 320,
  "roi_height": 320,
  "policy": "aim_ground_truth_v1",
  "frames": [
    {"frame_index": 0, "state": "visible", "targets": [
      {"track_id": 1, "x1": 1180, "y1": 600, "x2": 1220, "y2": 680}
    ]},
    {"frame_index": 1, "state": "visible", "targets": [
      {"track_id": 1, "x1": 1182, "y1": 600, "x2": 1222, "y2": 680}
    ]},
    {"frame_index": 2, "state": "visible", "targets": [
      {"track_id": 1, "x1": 1184, "y1": 600, "x2": 1224, "y2": 680}
    ]},
    {"frame_index": 3, "state": "visible", "targets": [
      {"track_id": 1, "x1": 1186, "y1": 600, "x2": 1226, "y2": 680}
    ]},
    {"frame_index": 4, "state": "visible", "targets": [
      {"track_id": 1, "x1": 1188, "y1": 600, "x2": 1228, "y2": 680}
    ]},
    {"frame_index": 5, "state": "visible", "targets": [
      {"track_id": 1, "x1": 1190, "y1": 600, "x2": 1230, "y2": 680},
      {"track_id": 2, "x1": 1320, "y1": 600, "x2": 1360, "y2": 680}
    ]},
    {"frame_index": 6, "state": "not_visible", "targets": []},
    {"frame_index": 7, "state": "ignore", "targets": []}
  ]
})json";
}

std::string replace_once(std::string input,
                         const std::string& from,
                         const std::string& to) {
    const std::size_t position = input.find(from);
    if (position != std::string::npos) input.replace(position, from.size(), to);
    return input;
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto suffix = std::chrono::steady_clock::now()
                                .time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("xen-aim-evaluation-tests-" + std::to_string(suffix));
        std::error_code error;
        std::filesystem::create_directories(path_, error);
        valid_ = !error;
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept { return path_; }
    bool valid() const noexcept { return valid_; }

private:
    std::filesystem::path path_;
    bool valid_ = false;
};

void write_file(const std::filesystem::path& path,
                const std::string& content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
}

AimEvaluationFrame output_frame(std::size_t index,
                                std::uint64_t track_id,
                                float x1, float y1,
                                float x2, float y2) {
    AimEvaluationFrame frame;
    frame.frame_index = index;
    frame.source_width = 2560;
    frame.source_height = 1440;
    frame.source_roi_x = 1120.0f;
    frame.source_roi_y = 560.0f;
    frame.roi_width = 320;
    frame.roi_height = 320;
    frame.aim_status = AimStatus::SUCCESS;
    frame.has_target = track_id != 0;
    if (frame.has_target) {
        frame.target.track_id = track_id;
        frame.target.state = TrackState::CONFIRMED;
        frame.target.x1 = x1;
        frame.target.y1 = y1;
        frame.target.x2 = x2;
        frame.target.y2 = y2;
    }
    return frame;
}

AimEvaluationFrame command_frame(std::size_t index,
                                 std::uint64_t track_id,
                                 int dx_counts, int dy_counts,
                                 bool predicted = false) {
    AimEvaluationFrame frame = output_frame(
        index, track_id, 60.0f, 40.0f, 100.0f, 120.0f);
    frame.target.predicted = predicted;
    frame.target.state = predicted ? TrackState::LOST : TrackState::CONFIRMED;
    frame.has_command = true;
    frame.command.sequence = static_cast<std::uint64_t>(index + 1U);
    frame.command.captured_at = std::chrono::steady_clock::time_point{
        std::chrono::milliseconds(index + 1U)};
    frame.command.dx_counts = dx_counts;
    frame.command.dy_counts = dy_counts;
    return frame;
}

void test_annotation_contract() {
    AimGroundTruthAnnotation annotation;
    std::string error;
    expect(aim::detail::parse_aim_ground_truth_annotation(
               valid_json(), expectation(), annotation, error),
           "合法逐帧 Aim 真值必须通过：" + error);
    expect(annotation.policy == aim::detail::kAimGroundTruthPolicy &&
               annotation.source_width == 2560 &&
               annotation.source_height == 1440 &&
               annotation.roi_x == 1120 && annotation.roi_y == 560 &&
               annotation.roi_width == 320 && annotation.roi_height == 320 &&
               annotation.frames.size() == 8,
           "解析后必须保留主机 FOV 与 ROI 几何");
    expect(annotation.frames[0].state == AimGroundTruthState::VISIBLE &&
               annotation.frames[0].targets.size() == 1 &&
               annotation.frames[5].targets.size() == 2 &&
               annotation.frames[6].state == AimGroundTruthState::NOT_VISIBLE &&
               annotation.frames[7].state == AimGroundTruthState::IGNORED,
           "逐帧状态、目标框和人工身份必须无歧义展开");
}

void test_annotation_rejections() {
    const AimGroundTruthExpectation expected = expectation();
    AimGroundTruthAnnotation annotation;
    std::string error;

    expect(!aim::detail::parse_aim_ground_truth_annotation(
               replace_once(valid_json(), "\"schema_version\": 1",
                            "\"schema_version\": 2"),
               expected, annotation, error),
           "未知 schema 必须拒绝");
    expect(!aim::detail::parse_aim_ground_truth_annotation(
               replace_once(valid_json(), "0123456789ABCDEF",
                            "1123456789ABCDEF"),
               expected, annotation, error),
           "视频 SHA-256 不一致必须拒绝");
    expect(!aim::detail::parse_aim_ground_truth_annotation(
               replace_once(valid_json(), "\"roi_x\": 1120",
                            "\"roi_x\": 1119"),
               expected, annotation, error),
           "主机 ROI 偏移一像素也必须拒绝");
    expect(!aim::detail::parse_aim_ground_truth_annotation(
               replace_once(valid_json(),
                            "\"frame_index\": 1, \"state\": \"visible\"",
                            "\"frame_index\": 2, \"state\": \"visible\""),
               expected, annotation, error),
           "逐帧序号缺口必须拒绝");
    expect(!aim::detail::parse_aim_ground_truth_annotation(
               replace_once(valid_json(),
                            "{\"frame_index\": 0, \"state\": \"visible\", \"targets\": [\n"
                            "      {\"track_id\": 1, \"x1\": 1180, \"y1\": 600, \"x2\": 1220, \"y2\": 680}\n"
                            "    ]}",
                            "{\"frame_index\": 0, \"state\": \"visible\", \"targets\": []}"),
               expected, annotation, error),
           "visible 帧没有目标必须拒绝");
    expect(!aim::detail::parse_aim_ground_truth_annotation(
               replace_once(valid_json(),
                            "{\"track_id\": 2, \"x1\": 1320",
                            "{\"track_id\": 1, \"x1\": 1320"),
               expected, annotation, error),
           "同一帧重复人工 track_id 必须拒绝");
    expect(!aim::detail::parse_aim_ground_truth_annotation(
               replace_once(valid_json(), "\"x1\": 1320, \"y1\": 600",
                            "\"x1\": 1500, \"y1\": 600"),
               expected, annotation, error),
           "中心位于主机 ROI 外的 visible 目标必须拒绝");
    expect(!aim::detail::parse_aim_ground_truth_annotation(
               replace_once(valid_json(), "\"policy\": \"aim_ground_truth_v1\"",
                            "\"policy\": \"aim_ground_truth_v1\", \"typo\": 1"),
               expected, annotation, error),
           "未知根字段必须拒绝，避免拼写错误静默生效");
}

void test_tracking_metrics() {
    AimGroundTruthAnnotation annotation;
    std::string error;
    expect(aim::detail::parse_aim_ground_truth_annotation(
               valid_json(), expectation(), annotation, error),
           "评价前真值必须可解析：" + error);
    if (annotation.frames.empty()) return;

    std::vector<AimEvaluationFrame> frames;
    frames.push_back(output_frame(0, 10, 60, 40, 100, 120));
    frames.push_back(output_frame(1, 10, 62, 40, 102, 120));
    frames.push_back(output_frame(2, 11, 64, 40, 104, 120));
    frames.push_back(output_frame(3, 0, 0, 0, 0, 0));
    frames.push_back(output_frame(4, 11, 68, 40, 108, 120));
    frames.push_back(output_frame(5, 20, 200, 40, 240, 120));
    frames.push_back(output_frame(6, 0, 0, 0, 0, 0));
    AimEvaluationFrame failed = output_frame(7, 0, 0, 0, 0, 0);
    failed.aim_status = AimStatus::TRACKING_FAILED;
    frames.push_back(failed);

    AimEvaluationMetrics metrics;
    for (const auto& frame : frames) {
        expect(aim::detail::record_aim_evaluation(
                   annotation, AimEvaluationConfig{}, frame, metrics, error),
               "合法 Aim 评价帧必须成功：" + error);
    }
    expect(aim::detail::finalize_aim_evaluation(
               annotation, metrics, error),
           "完整逐帧评价必须可收口：" + error);
    expect(metrics.complete && metrics.annotated_frames == 8 &&
               metrics.visible_frames == 6 &&
               metrics.matched_visible_frames == 5 &&
               metrics.missed_visible_frames == 1 &&
               metrics.not_visible_frames == 1 && metrics.ignored_frames == 1,
           "ROI 可见、命中、漏失和忽略统计错误");
    expect(metrics.id_switches == 1 && metrics.track_fragments == 4 &&
               metrics.track_fragmentation_events == 2 &&
               metrics.unnecessary_switches == 1,
           "ID Switch、轨迹片段或无必要切换统计错误");
    expect(metrics.invalid_aim_frames == 1 &&
               aim::detail::aim_roi_recall_available(metrics) &&
               near(aim::detail::aim_roi_recall(metrics), 5.0 / 6.0),
           "Aim 失败帧隔离或 ROI Recall 计算错误");
}

void test_source_to_scaled_roi_coordinates() {
    AimGroundTruthAnnotation annotation;
    std::string error;
    expect(aim::detail::parse_aim_ground_truth_annotation(
               valid_json(), expectation(), annotation, error),
           "缩放坐标测试真值必须可解析：" + error);
    if (annotation.frames.empty()) return;

    AimEvaluationFrame frame = output_frame(0, 100, 15, 10, 25, 30);
    frame.roi_width = 80;
    frame.roi_height = 80;
    frame.source_pixels_per_roi_pixel_x = 4.0f;
    frame.source_pixels_per_roi_pixel_y = 4.0f;
    AimEvaluationMetrics metrics;
    expect(aim::detail::record_aim_evaluation(
               annotation, AimEvaluationConfig{}, frame, metrics, error) &&
               metrics.matched_visible_frames == 1,
           "320×320 主机 ROI 缩放为 80×80 后必须按主机比例匹配，不能使用辅机分辨率：" +
               error);

    AimEvaluationFrame wrong = output_frame(0, 100, 15, 10, 25, 30);
    wrong.roi_width = 80;
    wrong.roi_height = 80;
    AimEvaluationMetrics wrong_metrics;
    expect(!aim::detail::record_aim_evaluation(
               annotation, AimEvaluationConfig{}, wrong, wrong_metrics, error),
           "编码 ROI 尺寸与主机像素比例不一致必须拒绝");

    AimEvaluationFrame wrong_origin = output_frame(0, 100, 60, 40, 100, 120);
    wrong_origin.source_roi_x = 1119.0f;
    AimEvaluationMetrics wrong_origin_metrics;
    expect(!aim::detail::record_aim_evaluation(
               annotation, AimEvaluationConfig{}, wrong_origin,
               wrong_origin_metrics, error),
           "主机 ROI 原点漂移一像素必须拒绝");
}

void test_control_continuity_metrics() {
    std::string error;

    std::vector<AimEvaluationFrame> frames;
    frames.push_back(command_frame(0, 10, 3, 4));
    frames.push_back(command_frame(1, 10, 6, 8));
    frames.push_back(command_frame(2, 10, -6, -8));
    AimEvaluationFrame quantized_zero =
        output_frame(3, 10, 60, 40, 100, 120);
    quantized_zero.command.sequence = 4;
    quantized_zero.command.captured_at =
        std::chrono::steady_clock::time_point{std::chrono::milliseconds(4)};
    frames.push_back(quantized_zero);
    frames.push_back(command_frame(4, 10, 50, 0));
    frames.push_back(command_frame(5, 20, 0, 50));
    frames.push_back(command_frame(6, 20, 0, 40, true));
    frames.push_back(output_frame(7, 0, 0, 0, 0, 0));

    AimControlContinuityMetrics control;
    for (const auto& frame : frames) {
        expect(aim::detail::record_aim_control_continuity(
                   AimEvaluationConfig{}, frame, control, error),
               "合法控制命令序列必须成功记录：" + error);
    }
    expect(aim::detail::finalize_aim_control_continuity(
               frames.size(), control, error),
           "控制连续性评价必须可完整收口：" + error);

    expect(control.complete && control.evaluated_frames == 8 &&
               control.invalid_aim_frames == 0 &&
               control.command_frames == 6 &&
               control.observed_command_frames == 5 &&
               control.predicted_command_frames == 1 &&
               control.target_without_command_frames == 1 &&
               control.no_target_frames == 1 &&
               control.predicted_target_frames == 1,
           "命令、死区/量化空命令、无目标和预测帧计数错误");
    expect(control.continuity_segments == 4 &&
               control.target_switches == 1 &&
               control.target_state_changes == 1 &&
               control.prediction_state_changes == 1 &&
               control.direction_reversals == 1 &&
               control.limit_boundary_frames == 2,
           "连续段边界、切换、方向反转或限幅计数错误");
    expect(control.abs_dx_counts.samples == 6 &&
               near(control.abs_dx_counts.mean, 65.0 / 6.0) &&
               near(control.abs_dx_counts.maximum, 50.0) &&
               control.abs_dy_counts.samples == 6 &&
               near(control.abs_dy_counts.mean, 110.0 / 6.0) &&
               near(control.magnitude_counts.mean, 27.5) &&
               near(control.magnitude_counts.maximum, 50.0),
           "绝对轴向或向量命令分布错误");
    expect(control.delta_counts.samples == 2 &&
               near(control.delta_counts.mean, 12.5) &&
               near(control.delta_counts.p50, 12.5) &&
               near(control.delta_counts.p95, 19.25) &&
               near(control.delta_counts.p99, 19.85) &&
               near(control.delta_counts.maximum, 20.0) &&
               control.acceleration_counts.samples == 1 &&
               near(control.acceleration_counts.mean, 25.0),
           "一阶/二阶 counts 变化分位数错误，或跨语义边界错误串联");
}

void test_control_contract_rejections() {
    std::string error;

    AimControlContinuityMetrics metrics;
    AimEvaluationFrame command_without_target = command_frame(0, 10, 1, 0);
    command_without_target.has_target = false;
    expect(!aim::detail::record_aim_control_continuity(
               AimEvaluationConfig{}, command_without_target, metrics, error),
           "无目标帧不得携带控制命令");

    AimEvaluationFrame zero_command = command_frame(0, 10, 0, 0);
    AimControlContinuityMetrics zero_metrics;
    expect(!aim::detail::record_aim_control_continuity(
               AimEvaluationConfig{}, zero_command, zero_metrics, error),
           "has_command=true 时零向量必须拒绝");

    AimEvaluationFrame stale_command = output_frame(0, 10, 60, 40, 100, 120);
    stale_command.command.sequence = 1;
    AimControlContinuityMetrics stale_metrics;
    expect(!aim::detail::record_aim_control_continuity(
               AimEvaluationConfig{}, stale_command, stale_metrics, error),
           "has_command=false 时残留命令快照必须拒绝");

    AimEvaluationFrame stale_timestamp =
        output_frame(0, 10, 60, 40, 100, 120);
    stale_timestamp.command.captured_at =
        std::chrono::steady_clock::time_point{std::chrono::milliseconds(1)};
    AimControlContinuityMetrics stale_timestamp_metrics;
    expect(!aim::detail::record_aim_control_continuity(
               AimEvaluationConfig{}, stale_timestamp,
               stale_timestamp_metrics, error),
           "has_command=false 时仅残留命令时间戳也必须拒绝");

    AimEvaluationFrame invalid_target = output_frame(0, 10, 60, 40, 100, 120);
    invalid_target.target.confidence =
        std::numeric_limits<float>::quiet_NaN();
    AimControlContinuityMetrics invalid_metrics;
    expect(!aim::detail::record_aim_control_continuity(
               AimEvaluationConfig{}, invalid_target, invalid_metrics, error),
           "目标快照中的非有限数必须拒绝");

    AimEvaluationFrame inconsistent_prediction =
        output_frame(0, 10, 60, 40, 100, 120);
    inconsistent_prediction.target.predicted = true;
    AimControlContinuityMetrics prediction_metrics;
    expect(!aim::detail::record_aim_control_continuity(
               AimEvaluationConfig{}, inconsistent_prediction,
               prediction_metrics, error),
           "预测标志与轨迹状态不一致时必须拒绝");

    AimEvaluationFrame stale_target = output_frame(0, 10, 60, 40, 100, 120);
    stale_target.has_target = false;
    AimControlContinuityMetrics stale_target_metrics;
    expect(!aim::detail::record_aim_control_continuity(
               AimEvaluationConfig{}, stale_target,
               stale_target_metrics, error),
           "has_target=false 时残留目标快照必须拒绝");

    AimEvaluationConfig invalid_config;
    invalid_config.max_counts_per_frame =
        std::numeric_limits<float>::quiet_NaN();
    AimControlContinuityMetrics config_metrics;
    expect(!aim::detail::record_aim_control_continuity(
               invalid_config, output_frame(0, 0, 0, 0, 0, 0),
               config_metrics, error),
           "非有限限幅配置必须在评价入口拒绝");

    AimControlContinuityMetrics failed_metrics;
    AimEvaluationFrame failed = output_frame(0, 0, 0, 0, 0, 0);
    failed.aim_status = AimStatus::NOT_RUN;
    expect(aim::detail::record_aim_control_continuity(
               AimEvaluationConfig{}, failed, failed_metrics, error) &&
               aim::detail::record_aim_control_continuity(
                   AimEvaluationConfig{}, output_frame(1, 0, 0, 0, 0, 0),
                   failed_metrics, error) &&
               aim::detail::finalize_aim_control_continuity(
                   2, failed_metrics, error) &&
               failed_metrics.invalid_aim_frames == 1 &&
               failed_metrics.no_target_frames == 1,
           "Aim 失败帧必须单独计数并从成功状态守恒中排除：" + error);

    AimControlContinuityMetrics incomplete_metrics;
    expect(aim::detail::record_aim_control_continuity(
               AimEvaluationConfig{}, output_frame(0, 0, 0, 0, 0, 0),
               incomplete_metrics, error) &&
               !aim::detail::finalize_aim_control_continuity(
                   2, incomplete_metrics, error),
           "未覆盖全部视频帧的控制连续性不得发布");
}

void test_sequence_and_annotation_set() {
    AimGroundTruthAnnotation annotation;
    std::string error;
    expect(aim::detail::parse_aim_ground_truth_annotation(
               valid_json(), expectation(), annotation, error),
           "序列测试真值必须可解析：" + error);

    AimEvaluationMetrics metrics;
    const AimEvaluationFrame second = output_frame(1, 10, 62, 40, 102, 120);
    expect(!aim::detail::record_aim_evaluation(
               annotation, AimEvaluationConfig{}, second, metrics, error),
           "Aim 评价必须从第 0 帧连续开始");
    expect(!aim::detail::finalize_aim_evaluation(annotation, metrics, error),
           "未覆盖全部帧不得发布完整指标");

    TemporaryDirectory temporary;
    expect(temporary.valid(), "必须能创建 Aim 评价测试临时目录");
    if (!temporary.valid()) return;
    const auto annotation_path = temporary.path() / "scene.mp4.aim.json";
    write_file(annotation_path, valid_json());
    AimGroundTruthAnnotation loaded;
    expect(aim::detail::load_aim_ground_truth_annotation(
               annotation_path, expectation(), loaded, error) &&
               loaded.frames.size() == 8,
           "必须能从 UTF-8 JSON 文件加载 Aim 真值：" + error);

    const std::vector<std::filesystem::path> videos{
        temporary.path() / "scene.mp4"};
    expect(aim::detail::aim_ground_truth_annotation_path(
               temporary.path(), videos[0]).filename() ==
               L"scene.mp4.aim.json",
           "Aim 真值文件名必须保留视频扩展名");
    expect(aim::detail::validate_aim_ground_truth_annotation_set(
               temporary.path(), videos, error),
           "Aim 真值集合一一对应时必须通过：" + error);
    write_file(temporary.path() / "stale.avi.aim.json", "{}");
    expect(!aim::detail::validate_aim_ground_truth_annotation_set(
               temporary.path(), videos, error),
           "Aim 真值目录中的多余场景必须拒绝");
}

} // namespace

int main() {
    test_annotation_contract();
    test_annotation_rejections();
    test_tracking_metrics();
    test_source_to_scaled_roi_coordinates();
    test_control_continuity_metrics();
    test_control_contract_rejections();
    test_sequence_and_annotation_set();

    if (failures != 0) {
        std::cerr << "Aim 真实视频评价测试失败数：" << failures << '\n';
        return 1;
    }
    std::cout << "Aim 真实视频标注与追踪评价测试全部通过。\n";
    return 0;
}
