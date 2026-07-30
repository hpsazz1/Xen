#include "detector/postprocess.h"
#include "detector/preprocess.h"

#include <cmath>
#include <cstddef>
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

bool near(float actual, float expected, float tolerance = 1e-4f) {
    return std::fabs(actual - expected) <= tolerance;
}

void test_output_format_resolution() {
    OutputFormat resolved = OutputFormat::AUTO;
    expect(detector::detail::resolve_output_format(
               {1, 84, 8400}, OutputFormat::AUTO, resolved) &&
           resolved == OutputFormat::CHANNEL_FIRST,
           "[B,4+C,A] 应识别为 CHANNEL_FIRST");

    expect(detector::detail::resolve_output_format(
               {1, 25200, 85}, OutputFormat::AUTO, resolved) &&
           resolved == OutputFormat::ANCHOR_FIRST_OBJECTNESS,
           "[B,A,5+C] 应识别为 ANCHOR_FIRST_OBJECTNESS");

    expect(detector::detail::resolve_output_format(
               {1, 300, 6}, OutputFormat::AUTO, resolved) &&
           resolved == OutputFormat::END_TO_END,
           "[B,N,6] 应识别为 END_TO_END");

    expect(!detector::detail::resolve_output_format(
               {1, 4, 4}, OutputFormat::AUTO, resolved),
           "未知布局必须失败关闭");
}

void test_channel_first_decode() {
    constexpr int64_t anchors = 8;
    constexpr int64_t features = 6; // 4 坐标 + 2 类
    std::vector<float> tensor(
        static_cast<size_t>(anchors * features), 0.0f);

    // anchor 0: cx=20, cy=30, w=10, h=8，类别 1 置信度 0.9。
    tensor[0 * anchors + 0] = 20.0f;
    tensor[1 * anchors + 0] = 30.0f;
    tensor[2 * anchors + 0] = 10.0f;
    tensor[3 * anchors + 0] = 8.0f;
    tensor[4 * anchors + 0] = 0.1f;
    tensor[5 * anchors + 0] = 0.9f;

    // anchor 1 低于阈值，应在解码阶段过滤。
    tensor[0 * anchors + 1] = 50.0f;
    tensor[1 * anchors + 1] = 50.0f;
    tensor[2 * anchors + 1] = 10.0f;
    tensor[3 * anchors + 1] = 10.0f;
    tensor[4 * anchors + 1] = 0.2f;
    tensor[5 * anchors + 1] = 0.3f;

    std::vector<Detection> detections;
    expect(detector::detail::decode_output(
               tensor.data(), {1, features, anchors},
               OutputFormat::CHANNEL_FIRST, 0.5f, detections),
           "CHANNEL_FIRST 解码应成功");
    expect(detections.size() == 1,
           "CHANNEL_FIRST 应只保留一个高置信度框");
    if (detections.size() == 1) {
        expect(near(detections[0].x1, 15.0f) &&
               near(detections[0].y1, 26.0f) &&
               near(detections[0].x2, 25.0f) &&
               near(detections[0].y2, 34.0f),
               "CHANNEL_FIRST 的 cxcywh 转 xyxy 错误");
        expect(detections[0].class_id == 1 &&
               near(detections[0].confidence, 0.9f),
               "CHANNEL_FIRST 的类别或置信度错误");
    }
}

void test_anchor_first_objectness_decode() {
    constexpr int64_t anchors = 8;
    constexpr int64_t features = 7; // 4 坐标 + obj + 2 类
    std::vector<float> tensor(
        static_cast<size_t>(anchors * features), 0.0f);
    float* row = tensor.data();
    row[0] = 40.0f;
    row[1] = 50.0f;
    row[2] = 20.0f;
    row[3] = 10.0f;
    row[4] = 0.8f;
    row[5] = 0.25f;
    row[6] = 0.75f;

    std::vector<Detection> detections;
    expect(detector::detail::decode_output(
               tensor.data(), {1, anchors, features},
               OutputFormat::ANCHOR_FIRST_OBJECTNESS,
               0.5f, detections),
           "ANCHOR_FIRST_OBJECTNESS 解码应成功");
    expect(detections.size() == 1,
           "ANCHOR_FIRST_OBJECTNESS 应保留一个框");
    if (detections.size() == 1) {
        expect(near(detections[0].confidence, 0.6f),
               "置信度必须为 objectness × 类别概率");
        expect(detections[0].class_id == 1,
               "应选择概率最高的类别");
    }
}

void test_end_to_end_decode() {
    const std::vector<float> tensor = {
        10.0f, 20.0f, 30.0f, 40.0f, 0.95f, 2.0f,
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
    };
    std::vector<Detection> detections;
    expect(detector::detail::decode_output(
               tensor.data(), {1, 2, 6}, OutputFormat::END_TO_END,
               0.25f, detections),
           "END_TO_END 解码应成功");
    expect(detections.size() == 1 && detections[0].class_id == 2,
           "END_TO_END 应过滤补零行并保留 class");
}

void test_scale_and_nms() {
    detector::detail::LetterBoxInfo info;
    info.scale = 0.5f;
    info.pad_x = 0.0f;
    info.pad_y = 25.0f;
    info.orig_w = 200;
    info.orig_h = 100;

    std::vector<Detection> detections = {
        {10.0f, 30.0f, 60.0f, 55.0f, 0.9f, 0},
        {11.0f, 31.0f, 59.0f, 54.0f, 0.8f, 0},
        {11.0f, 31.0f, 59.0f, 54.0f, 0.7f, 1},
    };
    detector::detail::scale_detections(detections, info);
    expect(detections.size() == 3, "有效框不应在坐标还原时丢失");
    if (!detections.empty()) {
        expect(near(detections[0].x1, 20.0f) &&
               near(detections[0].y1, 10.0f) &&
               near(detections[0].x2, 120.0f) &&
               near(detections[0].y2, 60.0f),
               "LetterBox 坐标还原公式错误");
    }

    detector::detail::nms(detections, 0.5f, 10);
    expect(detections.size() == 2,
           "NMS 应抑制同类别重叠框，但保留不同类别框");
}

void test_preprocess_contract() {
    cv::Mat bgr(1, 1, CV_8UC3, cv::Scalar(10, 20, 30));
    cv::Mat blob;
    detector::detail::LetterBoxInfo info;
    expect(detector::detail::letterbox(bgr, blob, 1, 1, info),
           "合法 BGR 图像前处理应成功");
    if (!blob.empty()) {
        const float* values = blob.ptr<float>();
        expect(near(values[0], 30.0f / 255.0f) &&
               near(values[1], 20.0f / 255.0f) &&
               near(values[2], 10.0f / 255.0f),
               "前处理必须执行 BGR→RGB 和 CHW 排列");
    }

    cv::Mat invalid;
    expect(!detector::detail::letterbox(invalid, blob, 640, 640, info),
           "空图必须安全失败");
}

void test_tensorrt_cache_defaults() {
    const DetectorConfig config;
    expect(config.enable_trt_engine_cache,
           "TensorRT Engine Cache 默认应启用");
    expect(config.enable_trt_timing_cache,
           "TensorRT Timing Cache 默认应启用");
    expect(!config.trt_cache_path.empty(),
           "启用 TensorRT 缓存时默认目录不能为空");
}

} // namespace

int main() {
    test_output_format_resolution();
    test_channel_first_decode();
    test_anchor_first_objectness_decode();
    test_end_to_end_decode();
    test_scale_and_nms();
    test_preprocess_contract();
    test_tensorrt_cache_defaults();

    if (failures != 0) {
        std::cerr << "Detector 测试失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "Detector 纯算法测试全部通过。\n";
    return 0;
}
