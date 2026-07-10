#ifndef CIRCLE_FOV_H
#define CIRCLE_FOV_H

// 圆形视野（FOV）计算工具
// 用于将检测区域限制在屏幕中心的圆形范围内，实现圆形瞄准区域限制

#include <algorithm>

#include <opencv2/core.hpp>

// 将圆形 FOV 半径百分比限制在 1-100 范围内
inline int clampCircleFovRadiusPercent(int radiusPercent)
{
    return std::clamp(radiusPercent, 1, 100);
}

// 根据图像尺寸和半径百分比计算圆形 FOV 的实际像素半径
inline float getCircleFovRadiusPixels(const cv::Size& size, int radiusPercent)
{
    const float maxRadius = static_cast<float>(std::min(size.width, size.height)) * 0.5f;
    return maxRadius * (static_cast<float>(clampCircleFovRadiusPercent(radiusPercent)) / 100.0f);
}

// 获取图像中心点坐标
inline cv::Point2f getCircleFovCenter(const cv::Size& size)
{
    return cv::Point2f(static_cast<float>(size.width) * 0.5f, static_cast<float>(size.height) * 0.5f);
}

// 判断点是否在圆形 FOV 范围内
inline bool pointInsideCircleFov(const cv::Point2f& point, const cv::Size& size, int radiusPercent)
{
    if (size.width <= 0 || size.height <= 0)
        return true;

    const cv::Point2f center = getCircleFovCenter(size);
    const float radius = getCircleFovRadiusPixels(size, radiusPercent);
    const float dx = point.x - center.x;
    const float dy = point.y - center.y;
    return dx * dx + dy * dy <= radius * radius;
}

#endif // CIRCLE_FOV_H
