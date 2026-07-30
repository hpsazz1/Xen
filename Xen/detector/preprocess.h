#ifndef DETECTOR_PREPROCESS_H
#define DETECTOR_PREPROCESS_H

#include <opencv2/core.hpp>

namespace detector::detail {

/// LetterBox 填充参数，用于后处理坐标还原
struct LetterBoxInfo {
    float scale = 1.0f; ///< 原图坐标乘此比例后进入模型输入坐标系
    float pad_x = 0.0f; ///< 模型输入坐标系中的左侧填充像素
    float pad_y = 0.0f; ///< 模型输入坐标系中的顶部填充像素
    int   orig_w = 0;
    int   orig_h = 0;
    int   target_w = 0;
    int   target_h = 0;
};

/// 同步 LetterBox + BGR→RGB + HWC→CHW + 归一化 [0,1]
/// dst 为 1×(3×H×W) 的 float 矩阵，符合 ONNX 输入要求
/// 仅接受非空 CV_8UC3 图像和正数目标尺寸，输入非法时返回 false。
bool letterbox(const cv::Mat& src, cv::Mat& dst,
               int target_w, int target_h,
               LetterBoxInfo& info) noexcept;

} // namespace detector::detail

#endif // DETECTOR_PREPROCESS_H
