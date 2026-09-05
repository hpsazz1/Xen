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

/// 与 letterbox() 契约相同，但复用调用方持有的 resize 缓冲区和输出张量。
/// Detector 热路径使用此入口，避免每帧重复分配大块内存。
bool letterbox_reuse(const cv::Mat& src, cv::Mat& dst,
                     cv::Mat& resize_buffer,
                     int target_w, int target_h,
                     LetterBoxInfo& info) noexcept;

/// 只生成模型尺寸的 uint8 BGR LetterBox 图像，不执行通道重排和归一化。
/// TensorRT CUDA Graph 的 GPU 前处理路径用它保持 OpenCV resize 结果与 CPU 路径一致，
/// 随后把 uint8 图像上传到固定设备缓冲并由 CUDA kernel 生成 NCHW float 张量。
/// resize_buffer 与 padding_buffer 是前处理独立持有的可复用写缓冲，不得借用输入存储。
/// dst 只返回 src 或缓冲区的浅引用读视图；调用方应在下一次调用前完成读取。
bool letterbox_bgr_reuse(const cv::Mat& src, cv::Mat& dst,
                         cv::Mat& resize_buffer, cv::Mat& padding_buffer,
                         int target_w, int target_h,
                         LetterBoxInfo& info) noexcept;

} // namespace detector::detail

#endif // DETECTOR_PREPROCESS_H
