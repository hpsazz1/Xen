#pragma once

#include <algorithm>
#include <cmath>

/**
 * @brief 当前瞄准状态使用的视场角与游戏灵敏度缩放。
 *
 * baseFovDegrees是游戏灵敏度标定时的水平FOV，scopeFovDegrees是按下开镜键后的
 * 实际水平FOV。垂直FOV按水平比例同步变化，以匹配项目现有线性像素角度模型。
 */
struct EffectiveFovState
{
    bool scalingEnabled = false;
    bool zoomed = false;
    double horizontalDegrees = 1.0;
    double verticalDegrees = 1.0;
    double sensitivityScale = 1.0;
};

inline double projectionFovScale(double currentFovDegrees, double baseFovDegrees)
{
    if (!std::isfinite(currentFovDegrees) || !std::isfinite(baseFovDegrees) ||
        currentFovDegrees <= 1.0 || currentFovDegrees > 179.0 ||
        baseFovDegrees <= 1.0 || baseFovDegrees > 179.0)
    {
        return 1.0;
    }
    constexpr double kDegreesToRadians = 3.14159265358979323846 / 180.0;
    return std::tan(currentFovDegrees * kDegreesToRadians * 0.5) /
        std::tan(baseFovDegrees * kDegreesToRadians * 0.5);
}

inline EffectiveFovState resolveEffectiveFov(
    double configuredHorizontalDegrees,
    double configuredVerticalDegrees,
    bool scalingEnabled,
    double baseFovDegrees,
    double scopeFovDegrees,
    bool zoomed)
{
    EffectiveFovState result;
    const double safeHorizontal = std::clamp(configuredHorizontalDegrees, 1.0, 179.0);
    const double safeVertical = std::clamp(configuredVerticalDegrees, 1.0, 179.0);
    const bool validScaling = scalingEnabled && std::isfinite(baseFovDegrees) &&
        std::isfinite(scopeFovDegrees) && baseFovDegrees > 1.0 &&
        scopeFovDegrees > 1.0 && scopeFovDegrees <= 179.0;

    result.scalingEnabled = validScaling;
    result.zoomed = validScaling && zoomed;
    result.horizontalDegrees = result.zoomed ? scopeFovDegrees : safeHorizontal;
    if (result.zoomed)
    {
        constexpr double kDegreesToRadians = 3.14159265358979323846 / 180.0;
        constexpr double kRadiansToDegrees = 180.0 / 3.14159265358979323846;
        const double baseProjectionAspect =
            std::tan(safeVertical * kDegreesToRadians * 0.5) /
            std::tan(safeHorizontal * kDegreesToRadians * 0.5);
        result.verticalDegrees = std::clamp(
            2.0 * std::atan(
                std::tan(result.horizontalDegrees * kDegreesToRadians * 0.5) *
                baseProjectionAspect) * kRadiansToDegrees,
            1.0, 179.0);
    }
    else
    {
        result.verticalDegrees = safeVertical;
    }
    result.sensitivityScale = validScaling
        ? projectionFovScale(result.horizontalDegrees, baseFovDegrees)
        : 1.0;
    return result;
}
