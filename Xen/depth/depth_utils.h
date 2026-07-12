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

    /**
     * @brief 从灰度深度图生成二值遮罩（直方图阈值 + 椭圆膨胀）
     *
     * 根据 nearPercent 参数计算深度直方图的累积阈值，
     * 与阈值比较生成二值遮罩，并做椭圆膨胀以扩大遮罩范围。
     * 由 capture.cpp 和 game_overlay_loop.cpp 共享使用。
     *
     * @param depthGray   灰度深度图 (CV_8UC1)
     * @param nearPercent 近端百分比 [1,100]（越小越只保留近处物体）
     * @param expandPixels 膨胀半径（0=不膨胀）
     * @param invert      是否反转遮罩
     * @return 二值遮罩 (CV_8UC1)，与 depthGray 同尺寸
     */
    cv::Mat generateDepthMaskFallback(
        const cv::Mat& depthGray,
        int nearPercent,
        int expandPixels,
        bool invert);
}

#endif
