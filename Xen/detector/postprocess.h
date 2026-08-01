#ifndef DETECTOR_POSTPROCESS_H
#define DETECTOR_POSTPROCESS_H

#include <cstdint>
#include <vector>

#include "detector/detector.h"
#include "detector/preprocess.h"

namespace detector::detail {

struct SegmentationContract {
    int64_t class_count = 0;
    int64_t mask_channels = 0;
    int64_t anchors = 0;
    int64_t prototype_height = 0;
    int64_t prototype_width = 0;
};

struct SegmentationCandidate {
    Detection detection;       ///< NMS 前保持模型输入坐标
    int64_t anchor_index = -1; ///< 用于在 NMS 后读取对应掩码系数
};

struct PoseContract {
    int64_t class_count = 0;
    int64_t keypoint_count = 0;
    int64_t keypoint_dimensions = 0;
    int64_t anchors = 0;
};

struct PoseCandidate {
    Detection detection;       ///< NMS 前保持模型输入坐标
    int64_t anchor_index = -1; ///< NMS 后读取同一 anchor 的关键点
};

struct ObbContract {
    int64_t class_count = 0;
    int64_t anchors = 0;
};

struct ObbCandidate {
    OrientedDetection detection; ///< NMS 前保持模型输入坐标
};

/// 根据运行时输出形状解析检测输出契约。
/// AUTO 只接受可明确区分的三维单 batch 张量；无法确定时返回 false。
bool resolve_output_format(const std::vector<int64_t>& shape,
                           OutputFormat requested,
                           OutputFormat& resolved) noexcept;

/// 将一种已知输出契约统一解码为模型输入坐标系中的 Detection。
/// 函数在解码阶段提前应用置信度阈值，减少后续排序和 NMS 的数据量。
bool decode_output(const float* data,
                   const std::vector<int64_t>& shape,
                   OutputFormat format,
                   float conf_threshold,
                   std::vector<Detection>& dets) noexcept;

/// 验证 YOLOv8 兼容分割双输出契约。当前只接受 channel-first raw head，
/// 类别数由 prediction features - 4 - prototype channels 严格推导，不猜 metadata。
bool resolve_segmentation_contract(
    const std::vector<int64_t>& prediction_shape,
    const std::vector<int64_t>& prototype_shape,
    OutputFormat requested,
    SegmentationContract& contract) noexcept;

/// 解码分割 raw head 的框、类别与置信度；掩码系数留在原输出中，通过
/// anchor_index 在 NMS 后按需读取，避免为所有候选复制 M 个 float。
bool decode_segmentation_output(
    const float* prediction_data,
    const std::vector<int64_t>& prediction_shape,
    const SegmentationContract& contract,
    float conf_threshold,
    std::vector<SegmentationCandidate>& candidates) noexcept;

/// 验证 YOLOv8 兼容 pose raw head。关键点数量和维度来自已验证的
/// kpt_shape metadata，类别数由 features - 4 - K*D 严格推导。
bool resolve_pose_contract(
    const std::vector<int64_t>& prediction_shape,
    OutputFormat requested,
    int64_t keypoint_count,
    int64_t keypoint_dimensions,
    PoseContract& contract) noexcept;

/// 解码 pose raw head 的框、类别和置信度；关键点在 NMS 后按 anchor
/// 读取，避免为所有阈值候选复制 K*D 个 float。
bool decode_pose_output(
    const float* prediction_data,
    const std::vector<int64_t>& prediction_shape,
    const PoseContract& contract,
    float conf_threshold,
    std::vector<PoseCandidate>& candidates) noexcept;

/// 验证 YOLOv8 兼容 OBB raw head：[1,4+C+1,A]，最后一个 feature
/// 是已解码弧度角。仅接受 channel-first、非 end-to-end 布局。
bool resolve_obb_contract(
    const std::vector<int64_t>& prediction_shape,
    OutputFormat requested,
    ObbContract& contract) noexcept;

/// 解码 OBB 中心、宽高、类别置信度与角度。
bool decode_obb_output(
    const float* prediction_data,
    const std::vector<int64_t>& prediction_shape,
    const ObbContract& contract,
    float conf_threshold,
    std::vector<ObbCandidate>& candidates) noexcept;

/// 与 Ultralytics rotated NMS 一致的 ProbIoU 相似度。
float probabilistic_iou(const OrientedDetection& left,
                        const OrientedDetection& right) noexcept;

/// 同类别 NMS。top_k 表示最终最多保留的检测数量。
void nms(std::vector<Detection>& dets,
         float nms_threshold,
         int top_k) noexcept;

/// 完成候选框排序/NMS、top_k 和坐标还原。raw 输出必须先在模型坐标系
/// 执行 NMS，再裁剪到原图边界，避免越界框被提前裁剪后改变 IoU。
/// candidates 与 suppressed 由 Detector 长期持有，用于复用大块临时内存。
bool finalize_detections(std::vector<Detection>& candidates,
                         OutputFormat format,
                         float nms_threshold,
                         int top_k,
                         const LetterBoxInfo& info,
                         std::vector<Detection>& output,
                         std::vector<unsigned char>& suppressed) noexcept;

/// 对分割候选执行同类别 NMS、坐标还原，并按需生成原图坐标系中的紧凑 ROI
/// 二值掩码。selected、mask_logits 与 mask_input 由 Detector 长期持有并跨帧
/// 复用；最终像素缓冲由返回结果独占一次分配，复制结果时通过 shared_ptr 共享。
bool finalize_segmentations(
    std::vector<SegmentationCandidate>& candidates,
    float nms_threshold,
    int top_k,
    const LetterBoxInfo& info,
    const float* prediction_data,
    const std::vector<int64_t>& prediction_shape,
    const float* prototype_data,
    const std::vector<int64_t>& prototype_shape,
    const SegmentationContract& contract,
    bool generate_masks,
    SegmentationResult& output,
    std::vector<SegmentationCandidate>& selected,
    std::vector<unsigned char>& suppressed,
    std::vector<float>& mask_logits,
    std::vector<std::uint8_t>& mask_input) noexcept;

/// 对 pose 候选执行同类别 NMS、框/关键点坐标还原。generate_keypoints=false
/// 时仅返回框，不读取关键点平面，供默认 Detector/Aim 热路径使用。
bool finalize_poses(
    std::vector<PoseCandidate>& candidates,
    float nms_threshold,
    int top_k,
    const LetterBoxInfo& info,
    const float* prediction_data,
    const std::vector<int64_t>& prediction_shape,
    const PoseContract& contract,
    bool generate_keypoints,
    PoseResult& output,
    std::vector<PoseCandidate>& selected,
    std::vector<unsigned char>& suppressed) noexcept;

/// 执行按类别 Fast-NMS/ProbIoU、LetterBox 坐标还原并生成四角轴对齐
/// 包围盒。generate_oriented=false 时不构造公有旋转框 vector。
bool finalize_obbs(
    std::vector<ObbCandidate>& candidates,
    float nms_threshold,
    int top_k,
    const LetterBoxInfo& info,
    bool generate_oriented,
    ObbResult& output,
    std::vector<ObbCandidate>& selected,
    std::vector<unsigned char>& suppressed) noexcept;

/// 将模型输入像素坐标还原到原始图像，裁剪越界坐标并删除退化框。
bool scale_detections(std::vector<Detection>& dets,
                      const LetterBoxInfo& info) noexcept;

} // namespace detector::detail

#endif // DETECTOR_POSTPROCESS_H
