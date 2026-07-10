#pragma once
#ifdef USE_CUDA

// 深度估计工具函数
// 包含色图类型枚举和图像缩放辅助函数

#include <opencv2/opencv.hpp>
#include <tuple>

namespace depth_anything
{
    // OpenCV 色图类型枚举
    enum ColormapTypes
    {
        COLORMAP_AUTUMN = 0,
        COLORMAP_BONE = 1,
        COLORMAP_JET = 2,
        COLORMAP_WINTER = 3,
        COLORMAP_RAINBOW = 4,
        COLORMAP_OCEAN = 5,
        COLORMAP_SUMMER = 6,
        COLORMAP_SPRING = 7,
        COLORMAP_COOL = 8,
        COLORMAP_HSV = 9,
        COLORMAP_PINK = 10,
        COLORMAP_HOT = 11,
        COLORMAP_PARULA = 12,
        COLORMAP_MAGMA = 13,
        COLORMAP_INFERNO = 14,
        COLORMAP_PLASMA = 15,
        COLORMAP_VIRIDIS = 16,
        COLORMAP_CIVIDIS = 17,
        COLORMAP_TWILIGHT = 18,
        COLORMAP_TWILIGHT_SHIFTED = 19,
        COLORMAP_TURBO = 20,
        COLORMAP_DEEPGREEN = 21
    };

    // 保持宽高比的深度图缩放，返回缩放后图像、新宽度、新高度
    std::tuple<cv::Mat, int, int> resize_depth(const cv::Mat& img, int w, int h);
}

#endif
