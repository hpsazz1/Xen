#include "detector/video_visibility_internal.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

using detector::detail::FrameVisibility;
using detector::detail::VideoVisibilityAnnotation;
using detector::detail::VideoVisibilityExpectation;
using detector::detail::VideoVisibilityMetrics;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << message << '\n';
}

bool near(double left, double right) {
    return std::fabs(left - right) < 1e-9;
}

VideoVisibilityExpectation expectation() {
    VideoVisibilityExpectation expected;
    expected.video_file = "scene.mp4";
    expected.video_sha256 =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    expected.source_width = 2560;
    expected.source_height = 1440;
    expected.frame_count = 5;
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
  "frame_count": 5,
  "input_mode": "center",
  "roi_x": 1120,
  "roi_y": 560,
  "roi_width": 320,
  "roi_height": 320,
  "policy": "target_frame_visibility_v1",
  "intervals": [
    {"start_frame": 0, "end_frame": 1, "state": "not_visible"},
    {"start_frame": 2, "end_frame": 3, "state": "visible"},
    {"start_frame": 4, "end_frame": 4, "state": "ignore"}
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
            ("xen-video-visibility-tests-" + std::to_string(suffix));
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

void test_valid_annotation_and_metrics() {
    VideoVisibilityAnnotation annotation;
    std::string error;
    expect(detector::detail::parse_video_visibility_annotation(
               valid_json(), expectation(), annotation, error),
           "合法三态区间标注必须通过：" + error);
    expect(annotation.policy == detector::detail::kVideoVisibilityPolicy,
           "标注策略必须保留到评价结果");
    expect(annotation.frames.size() == 5 &&
               annotation.frames[0] == FrameVisibility::NOT_VISIBLE &&
               annotation.frames[1] == FrameVisibility::NOT_VISIBLE &&
               annotation.frames[2] == FrameVisibility::VISIBLE &&
               annotation.frames[3] == FrameVisibility::VISIBLE &&
               annotation.frames[4] == FrameVisibility::IGNORED,
           "区间必须无歧义展开为逐帧三态真值");

    VideoVisibilityMetrics metrics;
    const bool detections[] = {false, true, true, false, true};
    for (std::size_t index = 0; index < annotation.frames.size(); ++index) {
        detector::detail::record_video_visibility(
            annotation.frames[index], detections[index], metrics);
    }
    expect(metrics.annotations_present && metrics.annotated_frames == 5,
           "启用标注时必须登记全部评价帧");
    expect(metrics.visible_frames == 2 &&
               metrics.visible_detected_frames == 1 &&
               metrics.visible_missed_frames == 1 &&
               metrics.longest_visible_miss_sequence == 1,
           "可见帧命中、漏检和最长连续漏检统计错误");
    expect(metrics.not_visible_frames == 2 &&
               metrics.not_visible_detected_frames == 1 &&
               metrics.ignored_frames == 1 &&
               metrics.ignored_detected_frames == 1,
           "不可见和忽略帧诊断统计错误");
    expect(detector::detail::video_visibility_recall_available(metrics) &&
               near(detector::detail::video_visibility_recall(metrics), 0.5) &&
               near(detector::detail::video_visibility_evaluable_rate(metrics),
                    0.8),
           "可见帧 Recall 或可评价覆盖率错误");

    VideoVisibilityMetrics sequence;
    detector::detail::record_video_visibility(
        FrameVisibility::VISIBLE, false, sequence);
    detector::detail::record_video_visibility(
        FrameVisibility::VISIBLE, false, sequence);
    detector::detail::record_video_visibility(
        FrameVisibility::IGNORED, false, sequence);
    detector::detail::record_video_visibility(
        FrameVisibility::VISIBLE, false, sequence);
    expect(sequence.longest_visible_miss_sequence == 2,
           "ignore/not_visible 必须中断连续可见漏检序列");

    const VideoVisibilityMetrics empty;
    expect(!detector::detail::video_visibility_recall_available(empty) &&
               near(detector::detail::video_visibility_recall(empty), 0.0) &&
               near(detector::detail::video_visibility_evaluable_rate(empty),
                    0.0),
           "无标注模式必须显式保持 Recall 不可用");
}

void test_annotation_contract_rejections() {
    const VideoVisibilityExpectation expected = expectation();
    VideoVisibilityAnnotation annotation;
    std::string error;

    expect(!detector::detail::parse_video_visibility_annotation(
               replace_once(valid_json(), "\"schema_version\": 1",
                            "\"schema_version\": 2"),
               expected, annotation, error),
           "未知 schema 必须拒绝");
    expect(!detector::detail::parse_video_visibility_annotation(
               replace_once(valid_json(), "\"schema_version\": 1",
                            "\"schema_version\": 0"),
               expected, annotation, error) && !error.empty(),
           "schema 0 也必须给出明确拒绝原因");
    expect(!detector::detail::parse_video_visibility_annotation(
               replace_once(valid_json(), "\"roi_x\": 1120",
                            "\"roi_x\": 1119"),
               expected, annotation, error),
           "ROI 偏移一像素也必须拒绝");
    expect(!detector::detail::parse_video_visibility_annotation(
               replace_once(valid_json(), "0123456789ABCDEF",
                            "1123456789ABCDEF"),
               expected, annotation, error),
           "视频 SHA-256 不一致必须拒绝");
    expect(!detector::detail::parse_video_visibility_annotation(
               replace_once(valid_json(), "target_frame_visibility_v1",
                            "unversioned_policy"),
               expected, annotation, error),
           "未知可见性策略必须拒绝");
    expect(!detector::detail::parse_video_visibility_annotation(
               replace_once(valid_json(),
                            "{\"start_frame\": 2, \"end_frame\": 3",
                            "{\"start_frame\": 3, \"end_frame\": 3"),
               expected, annotation, error),
           "标注区间存在缺口必须拒绝");
    expect(!detector::detail::parse_video_visibility_annotation(
               replace_once(valid_json(),
                            "{\"start_frame\": 2, \"end_frame\": 3",
                            "{\"start_frame\": 1, \"end_frame\": 3"),
               expected, annotation, error),
           "标注区间重叠必须拒绝");
    expect(!detector::detail::parse_video_visibility_annotation(
               replace_once(valid_json(), "\"end_frame\": 4",
                            "\"end_frame\": 3"),
               expected, annotation, error),
           "标注区间未覆盖尾帧必须拒绝");
    expect(!detector::detail::parse_video_visibility_annotation(
               replace_once(valid_json(), "\"state\": \"ignore\"",
                            "\"state\": \"unknown\""),
               expected, annotation, error),
           "未知三态值必须拒绝");
    expect(!detector::detail::parse_video_visibility_annotation(
               replace_once(valid_json(), "\"schema_version\": 1,",
                            "\"schema_version\": 1, \"typo\": 1,"),
               expected, annotation, error),
           "未知根字段必须拒绝，避免拼写错误静默生效");

    VideoVisibilityExpectation invalid_geometry = expected;
    invalid_geometry.roi_x = invalid_geometry.source_width;
    expect(!detector::detail::parse_video_visibility_annotation(
               valid_json(), invalid_geometry, annotation, error),
           "超出源画面的 ROI 评价契约必须拒绝");
}

void test_hash_file_and_annotation_set() {
    TemporaryDirectory temporary;
    expect(temporary.valid(), "必须能创建可见性测试临时目录");
    if (!temporary.valid()) return;

    const auto content_path = temporary.path() / "abc.bin";
    write_file(content_path, "abc");
    std::string sha256;
    std::string error;
    expect(detector::detail::compute_file_sha256(
               content_path, sha256, error),
           "文件 SHA-256 计算必须成功：" + error);
    expect(sha256 ==
               "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD",
           "文件 SHA-256 必须与标准测试向量一致");

    const auto load_path = temporary.path() / "valid.json";
    write_file(load_path, valid_json());
    VideoVisibilityAnnotation loaded;
    expect(detector::detail::load_video_visibility_annotation(
               load_path, expectation(), loaded, error) &&
               loaded.frames.size() == 5,
           "必须能从 UTF-8 JSON 文件加载完整标注：" + error);

    const std::vector<std::filesystem::path> videos{
        temporary.path() / "a.mp4", temporary.path() / "b.avi"};
    const auto first = detector::detail::video_visibility_annotation_path(
        temporary.path(), videos[0]);
    const auto second = detector::detail::video_visibility_annotation_path(
        temporary.path(), videos[1]);
    expect(first.filename() == L"a.mp4.visibility.json" &&
               second.filename() == L"b.avi.visibility.json",
           "标注文件名必须保留视频扩展名");
    write_file(first, "{}");
    write_file(second, "{}");
    expect(detector::detail::validate_video_visibility_annotation_set(
               temporary.path(), videos, error),
           "标注集合一一对应时必须通过：" + error);

    const auto extra = temporary.path() / "stale.mp4.visibility.json";
    write_file(extra, "{}");
    expect(!detector::detail::validate_video_visibility_annotation_set(
               temporary.path(), videos, error),
           "多余旧标注必须拒绝");
    std::error_code filesystem_error;
    std::filesystem::remove(extra, filesystem_error);
    std::filesystem::remove(second, filesystem_error);
    expect(!detector::detail::validate_video_visibility_annotation_set(
               temporary.path(), videos, error),
           "缺失任一场景标注必须拒绝");
}

} // namespace

int main() {
    test_valid_annotation_and_metrics();
    test_annotation_contract_rejections();
    test_hash_file_and_annotation_set();

    if (failures != 0) {
        std::cerr << "视频可见性测试失败数：" << failures << '\n';
        return 1;
    }
    std::cout << "视频可见性标注与 Recall 测试全部通过。\n";
    return 0;
}
