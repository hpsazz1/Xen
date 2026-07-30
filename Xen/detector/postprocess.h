#ifndef DETECTOR_POSTPROCESS_H
#define DETECTOR_POSTPROCESS_H

#include <vector>
#include "detector/preprocess.h"
#include "detector/detector.h"

namespace detector::detail {

// ── YOLOv8/v11 格式 ──
void decode_yolov8(const float* data, int anchors, int channels,
                   std::vector<Detection>& dets);

// ── YOLOv10 格式（NMS-free） ──
void decode_yolov10(const float* data, int num_dets,
                    std::vector<Detection>& dets);

// ── YOLOv5 格式 ──
void decode_yolov5(const float* data, int anchors, int channels,
                   std::vector<Detection>& dets);

// ── NMS ──
void nms(std::vector<Detection>& dets, float nms_thresh, int top_k);

// ── 坐标还原（归一化 → 原始像素） ──
void scale_detections(std::vector<Detection>& dets,
                      const LetterBoxInfo& info);

} // namespace detector::detail

#endif // DETECTOR_POSTPROCESS_H
