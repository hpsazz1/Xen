#ifndef DETECTOR_PREPROCESS_H
#define DETECTOR_PREPROCESS_H

#include <opencv2/core.hpp>

namespace detector::detail {

/// LetterBox 填充参数，用于后处理坐标还原
struct LetterBoxInfo {
    float scale_x;
    float scale_y;
    float pad_x;
    float pad_y;
    int   orig_w;
    int   orig_h;
};

/// 同步 LetterBox + BGR→RGB + HWC→CHW + 归一化 [0,1]
/// dst 为 1×(3×H×W) 的 float 矩阵，符合 ONNX 输入要求
void letterbox(const cv::Mat& src, cv::Mat& dst,
               int target_w, int target_h,
               LetterBoxInfo& info);

} // namespace detector::detail

#endif // DETECTOR_PREPROCESS_H
