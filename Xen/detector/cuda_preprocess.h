#pragma once
#ifdef USE_CUDA
// CUDA 图像预处理核函数声明
// 将 HWC 格式（高 x 宽 x 通道）的 BGR 图像转换为 CHW 格式（通道 x 高 x 宽）的归一化浮点张量
#include <cstddef>
#include <cuda_runtime.h>

// 启动 CUDA 核函数，执行 HWC 到 CHW 的转换和归一化
// srcHwc: 输入 HWC 格式的 BGR 图像（浮点）
// srcStepBytes: 输入图像每行字节数
// dstChw: 输出 CHW 格式的归一化张量
// width, height: 图像宽高
// stream: CUDA 流
void launch_hwc_to_chw_norm(
    const float* srcHwc,
    size_t srcStepBytes,
    float* dstChw,
    int width,
    int height,
    cudaStream_t stream
);
#endif
