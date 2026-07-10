#ifndef POSTPROCESS_H
#define POSTPROCESS_H

// 目标检测后处理模块
// 包含 Detection 结构体和 NMS（非极大值抑制）函数声明

#include <chrono>
#include <cstdint>
#include <vector>
#include <opencv2/opencv.hpp>

// 检测结果结构体
struct Detection
{
    cv::Rect box;        // 目标边界框
    float confidence;    // 置信度分数
    int classId;         // 类别 ID
};

// 非极大值抑制（NMS），去除重叠的检测框
void NMS(
    std::vector<Detection>& detections,
    float nmsThreshold,
    std::chrono::duration<double, std::milli>* nmsTime = nullptr
);

#ifdef USE_CUDA
// TensorRT 后处理：将模型原始输出解析为 Detection 列表
std::vector<Detection> postProcessYolo(
    const float* output,
    const std::vector<int64_t>& shape,
    int numClasses,
    float confThreshold,
    float nmsThreshold,
    int maxDetections = 100,
    std::chrono::duration<double, std::milli>* nmsTime = nullptr
);
#endif

#ifndef USE_CUDA
// DirectML 后处理：将 ONNX Runtime 输出解析为 Detection 列表
std::vector<Detection> postProcessYoloDML(
    const float* output,
    const std::vector<int64_t>& shape,
    int numClasses,
    float confThreshold,
    float nmsThreshold,
    int maxDetections = 100,
    std::chrono::duration<double, std::milli>* nmsTime = nullptr
);
#endif
#endif // POSTPROCESS_H
