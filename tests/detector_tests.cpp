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

    expect(!detector::detail::resolve_output_format(
               {1, 300, 6}, OutputFormat::AUTO, resolved),
           "无 metadata 的 [B,N,6] 存在歧义，AUTO 必须失败关闭");

    expect(detector::detail::resolve_output_format(
               {1, 300, 6}, OutputFormat::END_TO_END, resolved) &&
           resolved == OutputFormat::END_TO_END,
           "显式 END_TO_END 应接受 [B,N,6]");

    expect(detector::detail::resolve_output_format(
               {1, 64, 6}, OutputFormat::ANCHOR_FIRST_OBJECTNESS,
               resolved) &&
           resolved == OutputFormat::ANCHOR_FIRST_OBJECTNESS,
           "显式 objectness 不应按候选数量猜测并拒绝小输出");

    expect(detector::detail::resolve_output_format(
               {1, 84, 32}, OutputFormat::CHANNEL_FIRST, resolved) &&
           resolved == OutputFormat::CHANNEL_FIRST,
           "显式 CHANNEL_FIRST 不应要求 anchors 大于 features");

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
    std::vector<Detection> output;
    std::vector<unsigned char> suppressed;
    expect(detector::detail::finalize_detections(
               detections, OutputFormat::CHANNEL_FIRST, 0.5f, 10,
               info, output, suppressed),
           "合法候选框后处理必须成功");
    expect(output.size() == 2,
           "NMS 应抑制同类别重叠框，但保留不同类别框");
    if (!output.empty()) {
        expect(near(output[0].x1, 20.0f) &&
               near(output[0].y1, 10.0f) &&
               near(output[0].x2, 120.0f) &&
               near(output[0].y2, 60.0f),
               "LetterBox 坐标还原公式错误");
    }

    detector::detail::LetterBoxInfo edge_info;
    edge_info.scale = 1.0f;
    edge_info.orig_w = 100;
    edge_info.orig_h = 100;
    std::vector<Detection> edge_candidates = {
        {-100.0f, 0.0f, 10.0f, 10.0f, 0.9f, 0},
        {0.0f, 0.0f, 10.0f, 10.0f, 0.8f, 0},
    };
    expect(detector::detail::finalize_detections(
               edge_candidates, OutputFormat::CHANNEL_FIRST, 0.5f, 10,
               edge_info, output, suppressed),
           "越界候选框后处理必须成功");
    expect(output.size() == 2,
           "越界框必须先 NMS 再裁剪，不能因提前裁剪改变 IoU");

    edge_info.scale = 0.0f;
    expect(!detector::detail::finalize_detections(
               edge_candidates, OutputFormat::CHANNEL_FIRST, 0.5f, 10,
               edge_info, output, suppressed),
           "非法 LetterBox 参数不得伪装成成功空检测");
}

void test_detection_status_names() {
    expect(std::string(DetectionStatusName(DetectionStatus::SUCCESS)) ==
               "SUCCESS",
           "DetectionStatus 应提供稳定可读名称");
    expect(std::string(DetectionStatusName(
               DetectionStatus::POSTPROCESS_FAILED)) ==
               "POSTPROCESS_FAILED",
           "后处理失败状态名称错误");
    expect(InferenceProfile{}.status == DetectionStatus::NOT_RUN,
           "默认 profile 状态应为 NOT_RUN");
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

    // 两个像素验证一次循环同时完成 RGB、归一化和 CHW，不能只验证单像素
    // 而漏掉通道平面步长错误。
    cv::Mat two_pixels(1, 2, CV_8UC3);
    two_pixels.at<cv::Vec3b>(0, 0) = cv::Vec3b(10, 20, 30);
    two_pixels.at<cv::Vec3b>(0, 1) = cv::Vec3b(40, 50, 60);
    expect(detector::detail::letterbox(two_pixels, blob, 2, 1, info),
           "两像素前处理应成功");
    if (blob.total() == 6) {
        const float* values = blob.ptr<float>();
        expect(near(values[0], 30.0f / 255.0f) &&
               near(values[1], 60.0f / 255.0f) &&
               near(values[2], 20.0f / 255.0f) &&
               near(values[3], 50.0f / 255.0f) &&
               near(values[4], 10.0f / 255.0f) &&
               near(values[5], 40.0f / 255.0f),
               "融合前处理的 RGB/CHW 平面布局错误");
    }

    cv::Mat resize_buffer;
    cv::Mat reusable_blob;
    cv::Mat larger(2, 4, CV_8UC3, cv::Scalar(1, 2, 3));
    expect(detector::detail::letterbox_reuse(
               larger, reusable_blob, resize_buffer, 2, 2, info),
           "可复用前处理首次调用应成功");
    const unsigned char* first_blob_address = reusable_blob.data;
    const unsigned char* first_resize_address = resize_buffer.data;
    expect(detector::detail::letterbox_reuse(
               larger, reusable_blob, resize_buffer, 2, 2, info),
           "可复用前处理第二次调用应成功");
    expect(reusable_blob.data == first_blob_address &&
           resize_buffer.data == first_resize_address,
           "固定输入尺寸不应重复分配前处理缓冲区");

    cv::Mat prepared_bgr;
    expect(detector::detail::letterbox_bgr_reuse(
               larger, prepared_bgr, resize_buffer, 2, 2, info),
           "CUDA 路径的 uint8 BGR LetterBox 应成功");
    expect(prepared_bgr.type() == CV_8UC3 &&
               prepared_bgr.cols == 2 && prepared_bgr.rows == 2 &&
               prepared_bgr.at<cv::Vec3b>(0, 0) == cv::Vec3b(1, 2, 3),
           "CUDA 路径必须保留 OpenCV resize 后的 BGR uint8 像素");

    cv::Mat tall(2, 1, CV_8UC3, cv::Scalar(10, 20, 30));
    expect(detector::detail::letterbox_bgr_reuse(
               tall, prepared_bgr, resize_buffer, 4, 4, info),
           "带填充的 uint8 BGR LetterBox 应成功");
    expect(info.scale == 2.0f && info.pad_x == 1.0f &&
               info.pad_y == 0.0f &&
               prepared_bgr.at<cv::Vec3b>(0, 0) ==
                   cv::Vec3b(114, 114, 114) &&
               prepared_bgr.at<cv::Vec3b>(0, 1) ==
                   cv::Vec3b(10, 20, 30),
           "uint8 BGR LetterBox 的几何和 114 填充值必须与 CPU 路径一致");
}

void test_tensorrt_cache_defaults() {
    const DetectorConfig config;
    expect(config.enable_trt_engine_cache,
           "TensorRT Engine Cache 默认应启用");
    expect(config.enable_trt_timing_cache,
           "TensorRT Timing Cache 默认应启用");
    expect(config.enable_trt_cuda_graph,
           "固定 shape 实时推理默认应启用 TensorRT CUDA Graph");
    expect(config.enable_gpu_preprocess,
           "TensorRT CUDA Graph 默认应启用 CUDA 通道重排与归一化");
    expect(!config.enable_output_fingerprint,
           "原始输出指纹默认必须关闭，避免污染正式推理性能");
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
    test_detection_status_names();

    if (failures != 0) {
        std::cerr << "Detector 测试失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "Detector 纯算法测试全部通过。\n";
    return 0;
}
