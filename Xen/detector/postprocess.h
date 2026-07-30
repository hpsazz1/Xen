#ifndef DETECTOR_POSTPROCESS_H
#define DETECTOR_POSTPROCESS_H

#include <cstdint>
#include <vector>

#include "detector/detector.h"
#include "detector/preprocess.h"

namespace detector::detail {

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

/// 同类别 NMS。top_k 表示最终最多保留的检测数量。
void nms(std::vector<Detection>& dets,
         float nms_threshold,
         int top_k) noexcept;

/// 将模型输入像素坐标还原到原始图像，裁剪越界坐标并删除退化框。
void scale_detections(std::vector<Detection>& dets,
                      const LetterBoxInfo& info) noexcept;

} // namespace detector::detail

#endif // DETECTOR_POSTPROCESS_H
