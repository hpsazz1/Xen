#pragma once
// ============================================================
// detection_filters.h — DML / TRT 共享检测滤波器
// 提取自 dml_detector.cpp(320-449) 与 trt_detector.cpp(81-194)
// ============================================================
#include <vector>
#include "postProcess.h"

/**
 * 判断检测框与深度掩码是否有重叠区域
 * @return 中心点非零或 ROI 有非零像素 → true
 */
bool intersectsDepthMask(const cv::Rect& box, const cv::Mat& mask);

/**
 * 根据深度掩码抑制与目标区域相交的检测结果
 * 仅在 USE_CUDA 时生效；DML 编译下为空操作
 */
void filterDetectionsByDepthMask(std::vector<Detection>& detections);

/**
 * 根据圆形 FOV 剔除中心点位于有效区域外的检测框
 */
void filterDetectionsByCircleFov(std::vector<Detection>& detections);
