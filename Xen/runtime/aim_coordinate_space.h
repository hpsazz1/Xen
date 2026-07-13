#pragma once

#include <algorithm>
#include <cmath>

namespace AimCoordinateSpace
{
/**
 * @brief 解析用于 FOV 角度换算的源图像尺寸
 *
 * 检测输入通常只是从完整画面中心裁出的区域，例如从 2560×1440 原图裁出 320×320。
 * 裁剪不会改变单个像素对应的视角，因此必须优先使用捕获后端报告的原始源尺寸；
 * 仅在源尺寸尚未发布或捕获后端无法报告时，才回退到检测尺寸。
 *
 * @param sourcePixels 捕获后端报告的原始宽度或高度，单位：像素
 * @param detectionPixels 检测输入宽度或高度，单位：像素
 * @return FOV 实际覆盖的像素数量，最小为 1
 */
inline double resolveFovPixelSpan(int sourcePixels, double detectionPixels)
{
    if (sourcePixels > 0)
        return static_cast<double>(sourcePixels);
    return std::max(1.0, detectionPixels);
}

/**
 * @brief 将源图像中的一个像素误差转换为鼠标设备计数
 *
 * @param fovDegrees 当前轴的完整视场角，单位：度
 * @param fovPixelSpan 当前轴完整 FOV 覆盖的源像素数量
 * @param sensitivityDenominator sensitivity × yaw/pitch × FOV 缩放
 * @return 每源像素所需的设备 counts；分母无效时返回 1 作为安全回退
 */
inline double countsPerSourcePixel(
    double fovDegrees,
    double fovPixelSpan,
    double sensitivityDenominator)
{
    if (!std::isfinite(sensitivityDenominator) ||
        std::abs(sensitivityDenominator) <= 1e-9)
    {
        return 1.0;
    }

    const double safePixelSpan = std::max(1.0, fovPixelSpan);
    return (fovDegrees / safePixelSpan) / sensitivityDenominator;
}
}
