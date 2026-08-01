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

void test_segmentation_decode_and_mask() {
    detector::detail::SegmentationContract contract;
    expect(detector::detail::resolve_segmentation_contract(
               {1, 7, 2}, {1, 2, 2, 2}, OutputFormat::AUTO,
               contract),
           "YOLOv8-seg 双输出契约应解析成功");
    expect(contract.class_count == 1 && contract.mask_channels == 2 &&
               contract.anchors == 2 &&
               contract.prototype_height == 2 &&
               contract.prototype_width == 2,
           "分割类别数、系数维度或原型尺寸推导错误");
    expect(!detector::detail::resolve_segmentation_contract(
               {1, 6, 2}, {1, 2, 2, 2},
               OutputFormat::ANCHOR_FIRST_OBJECTNESS, contract),
           "分割模型不得接受 objectness 布局");
    expect(!detector::detail::resolve_segmentation_contract(
               {1, 6, 2}, {1, 2, 2, 2}, OutputFormat::AUTO,
               contract),
           "没有类别平面的分割输出必须失败关闭");

    expect(detector::detail::resolve_segmentation_contract(
               {1, 7, 2}, {1, 2, 2, 2}, OutputFormat::AUTO,
               contract),
           "恢复合法分割契约应成功");
    constexpr int64_t anchors = 2;
    std::vector<float> prediction(14, 0.0f);
    // 两个同类重叠框；anchor 0 置信度更高且系数选择全正原型，NMS 必须
    // 保留它，同时用 anchor_index 读取正确系数。
    prediction[0 * anchors + 0] = 2.0f;
    prediction[1 * anchors + 0] = 2.0f;
    prediction[2 * anchors + 0] = 2.0f;
    prediction[3 * anchors + 0] = 2.0f;
    prediction[4 * anchors + 0] = 0.9f;
    prediction[5 * anchors + 0] = 1.0f;
    prediction[6 * anchors + 0] = 0.0f;

    prediction[0 * anchors + 1] = 2.1f;
    prediction[1 * anchors + 1] = 2.1f;
    prediction[2 * anchors + 1] = 2.0f;
    prediction[3 * anchors + 1] = 2.0f;
    prediction[4 * anchors + 1] = 0.8f;
    prediction[5 * anchors + 1] = -1.0f;
    prediction[6 * anchors + 1] = 0.0f;

    const std::vector<float> prototypes{
        1.0f, 1.0f, 1.0f, 1.0f,
        0.0f, 0.0f, 0.0f, 0.0f,
    };
    std::vector<detector::detail::SegmentationCandidate> candidates;
    expect(detector::detail::decode_segmentation_output(
               prediction.data(), {1, 7, 2}, contract, 0.25f,
               candidates),
           "分割 raw head 解码应成功");
    expect(candidates.size() == 2,
           "分割解码应保留两个阈值以上候选");

    detector::detail::LetterBoxInfo info;
    info.scale = 1.0f;
    info.orig_w = 4;
    info.orig_h = 4;
    info.target_w = 4;
    info.target_h = 4;
    SegmentationResult result;
    std::vector<detector::detail::SegmentationCandidate> selected;
    std::vector<unsigned char> suppressed;
    std::vector<float> mask_logits;
    std::vector<std::uint8_t> mask_input;
    expect(detector::detail::finalize_segmentations(
               candidates, 0.5f, 10, info, prediction.data(),
               {1, 7, 2}, prototypes.data(), {1, 2, 2, 2},
               contract, true, result, selected, suppressed,
               mask_logits, mask_input),
           "分割 NMS 与掩码后处理应成功");
    expect(result.detections.size() == 1 && result.masks.size() == 1,
           "NMS 后检测框与掩码必须一一对齐");
    if (result.masks.size() == 1) {
        const InstanceMask& mask = result.masks[0];
        expect(mask.x == 1 && mask.y == 1 &&
                   mask.width == 2 && mask.height == 2 &&
                   mask.row_stride == 2,
               "实例掩码 ROI 几何错误");
        const std::uint8_t* first_row = result.mask_row(0, 0);
        const std::uint8_t* second_row = result.mask_row(0, 1);
        expect(first_row && second_row &&
                   first_row[0] == 1 && first_row[1] == 1 &&
                   second_row[0] == 1 && second_row[1] == 1,
               "正原型应生成全一 ROI 掩码");
        expect(result.mask_row(0, 2) == nullptr &&
                   result.mask_row(1, 0) == nullptr,
               "掩码行访问必须拒绝越界索引");
    }

    const SegmentationResult copied = result;
    expect(copied.mask_pixels && result.mask_pixels &&
               copied.mask_pixels.get() == result.mask_pixels.get(),
           "复制分割结果必须共享像素缓冲而非复制整幅掩码");

    candidates.clear();
    expect(detector::detail::decode_segmentation_output(
               prediction.data(), {1, 7, 2}, contract, 0.25f,
               candidates),
           "仅框路径重新解码应成功");
    SegmentationResult boxes_only;
    expect(detector::detail::finalize_segmentations(
               candidates, 0.5f, 10, info, prediction.data(),
               {1, 7, 2}, nullptr, {1, 2, 2, 2}, contract,
               false, boxes_only, selected, suppressed, mask_logits,
               mask_input),
           "分割模型的仅框热路径应无需生成掩码");
    expect(boxes_only.detections.size() == 1 &&
               boxes_only.masks.empty() && !boxes_only.mask_pixels,
           "detect() 兼容路径不得分配主机掩码缓冲");
}

void test_pose_decode_and_keypoints() {
    detector::detail::PoseContract contract;
    expect(detector::detail::resolve_pose_contract(
               {1, 11, 2}, OutputFormat::AUTO, 2, 3, contract),
           "YOLOv8-pose 输出契约应解析成功");
    expect(contract.class_count == 1 &&
               contract.keypoint_count == 2 &&
               contract.keypoint_dimensions == 3 &&
               contract.anchors == 2,
           "姿态类别、关键点 shape 或 anchor 数推导错误");
    expect(!detector::detail::resolve_pose_contract(
               {1, 11, 2}, OutputFormat::END_TO_END, 2, 3,
               contract),
           "姿态 raw head 不得接受 end-to-end 布局");
    expect(!detector::detail::resolve_pose_contract(
               {1, 11, 2}, OutputFormat::AUTO, 2, 4, contract),
           "关键点维度不是 2/3 时必须失败关闭");

    expect(detector::detail::resolve_pose_contract(
               {1, 11, 2}, OutputFormat::AUTO, 2, 3, contract),
           "恢复合法姿态契约应成功");
    constexpr int64_t anchors = 2;
    std::vector<float> prediction(22, 0.0f);
    // 两个同类重叠框，NMS 应保留 anchor 0；关键点平面刻意不同，验证
    // NMS 后仍通过 anchor_index 读取正确实例。
    prediction[0 * anchors + 0] = 2.0f;
    prediction[1 * anchors + 0] = 2.0f;
    prediction[2 * anchors + 0] = 2.0f;
    prediction[3 * anchors + 0] = 2.0f;
    prediction[4 * anchors + 0] = 0.9f;
    prediction[5 * anchors + 0] = 1.0f;
    prediction[6 * anchors + 0] = 1.5f;
    prediction[7 * anchors + 0] = 0.8f;
    prediction[8 * anchors + 0] = 3.0f;
    prediction[9 * anchors + 0] = 2.5f;
    prediction[10 * anchors + 0] = 0.4f;

    prediction[0 * anchors + 1] = 2.1f;
    prediction[1 * anchors + 1] = 2.1f;
    prediction[2 * anchors + 1] = 2.0f;
    prediction[3 * anchors + 1] = 2.0f;
    prediction[4 * anchors + 1] = 0.8f;
    prediction[5 * anchors + 1] = 0.0f;
    prediction[6 * anchors + 1] = 0.0f;
    prediction[7 * anchors + 1] = 0.1f;
    prediction[8 * anchors + 1] = 0.0f;
    prediction[9 * anchors + 1] = 0.0f;
    prediction[10 * anchors + 1] = 0.1f;

    std::vector<detector::detail::PoseCandidate> candidates;
    expect(detector::detail::decode_pose_output(
               prediction.data(), {1, 11, 2}, contract, 0.25f,
               candidates),
           "姿态 raw head 解码应成功");
    expect(candidates.size() == 2,
           "姿态解码应保留两个阈值以上候选");

    detector::detail::LetterBoxInfo info;
    info.scale = 0.5f;
    info.pad_x = 0.0f;
    info.pad_y = 1.0f;
    info.orig_w = 8;
    info.orig_h = 4;
    info.target_w = 4;
    info.target_h = 4;
    PoseResult result;
    std::vector<detector::detail::PoseCandidate> selected;
    std::vector<unsigned char> suppressed;
    expect(detector::detail::finalize_poses(
               candidates, 0.5f, 10, info, prediction.data(),
               {1, 11, 2}, contract, true, result, selected,
               suppressed),
           "姿态 NMS 与关键点后处理应成功");
    expect(result.detections.size() == 1 &&
               result.keypoints_per_detection == 2 &&
               result.keypoint_dimensions == 3 &&
               result.keypoints.size() == 2,
           "姿态实例与关键点连续布局错误");
    const PoseKeypoint* first = result.keypoint(0, 0);
    const PoseKeypoint* second = result.keypoint(0, 1);
    expect(first && second && near(first->x, 2.0f) &&
               near(first->y, 1.0f) &&
               near(first->confidence, 0.8f) &&
               near(second->x, 6.0f) && near(second->y, 3.0f) &&
               near(second->confidence, 0.4f),
           "关键点坐标、置信度或 anchor 对应关系错误");
    expect(result.keypoint(0, 2) == nullptr &&
               result.keypoint(1, 0) == nullptr,
           "关键点访问必须拒绝越界索引");

    candidates.clear();
    expect(detector::detail::decode_pose_output(
               prediction.data(), {1, 11, 2}, contract, 0.25f,
               candidates),
           "姿态仅框路径重新解码应成功");
    PoseResult boxes_only;
    expect(detector::detail::finalize_poses(
               candidates, 0.5f, 10, info, nullptr, {1, 11, 2},
               contract, false, boxes_only, selected, suppressed),
           "姿态模型仅框热路径应无需读取关键点平面");
    expect(boxes_only.detections.size() == 1 &&
               boxes_only.keypoints.empty() &&
               boxes_only.keypoints_per_detection == 0,
           "detect() 兼容路径不得生成关键点结果");

    detector::detail::PoseContract two_dimensional_contract;
    expect(detector::detail::resolve_pose_contract(
               {1, 9, 1}, OutputFormat::AUTO, 2, 2,
               two_dimensional_contract),
           "二维关键点契约应受支持");
    const std::vector<float> two_dimensional_prediction{
        2.0f, 2.0f, 2.0f, 2.0f, 0.9f,
        1.0f, 1.5f, 3.0f, 2.5f,
    };
    std::vector<detector::detail::PoseCandidate>
        two_dimensional_candidates;
    expect(detector::detail::decode_pose_output(
               two_dimensional_prediction.data(), {1, 9, 1},
               two_dimensional_contract, 0.25f,
               two_dimensional_candidates),
           "二维关键点 raw head 解码应成功");
    PoseResult two_dimensional_result;
    expect(detector::detail::finalize_poses(
               two_dimensional_candidates, 0.5f, 10, info,
               two_dimensional_prediction.data(), {1, 9, 1},
               two_dimensional_contract, true,
               two_dimensional_result, selected, suppressed),
           "二维关键点后处理应成功");
    const PoseKeypoint* two_dimensional_keypoint =
        two_dimensional_result.keypoint(0, 0);
    expect(two_dimensional_result.keypoint_dimensions == 2 &&
               two_dimensional_keypoint &&
               near(two_dimensional_keypoint->confidence, 1.0f),
           "二维关键点必须补充 confidence=1.0");
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
    expect(std::string(DetectionStatusName(
               DetectionStatus::UNSUPPORTED_TASK)) ==
               "UNSUPPORTED_TASK",
           "不支持任务状态名称错误");
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
    test_segmentation_decode_and_mask();
    test_pose_decode_and_keypoints();
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
