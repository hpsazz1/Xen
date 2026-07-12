#pragma once
// ============================================================
//  speed_curve.h — 统一速度曲线计算
//
//  根据目标距离动态计算鼠标移动速度倍率，使用三段式曲线：
//    1. 吸附区 (distance < snapRadius):  最低速度 × 吸附增强因子
//    2. 近端区 (distance < nearRadius):  指数曲线平滑插值
//    3. 远端区 (distance >= nearRadius): 线性映射到 [min, max]
//
//  由 MouseThread::calculate_speed_multiplier() 和
//  draw_target_correction_demo_game_overlay() 共享使用。
// ============================================================
#include <cmath>
#include <algorithm>

/**
 * @brief 计算速度倍率（三段式曲线）
 *
 * @param distance           目标到屏幕中心的像素距离
 * @param maxDistance        最大瞄准距离（通常为 hypot(res, res) / 2）
 * @param snapRadius         吸附半径（在此范围内使用最低速度微调）
 * @param nearRadius         近端半径（指数曲线过渡段终点）
 * @param speedCurveExponent 速度曲线指数（越大越偏向远端高速）
 * @param snapBoostFactor    吸附增强因子（>1 可增强吸附区速度）
 * @param minSpeed           最小速度倍率
 * @param maxSpeed           最大速度倍率
 * @return 速度倍率 ∈ [minSpeed, maxSpeed]
 */
inline double computeSpeedMultiplier(
    double distance,
    double maxDistance,
    float snapRadius,
    float nearRadius,
    float speedCurveExponent,
    float snapBoostFactor,
    double minSpeed,
    double maxSpeed)
{
    // 边界保护：确保 max >= min，防止负输出
    maxSpeed = std::max(maxSpeed, minSpeed);

    // 吸附区域：目标很近时使用最低速度（精确微调）乘以加成系数
    if (distance < static_cast<double>(snapRadius))
        return minSpeed * static_cast<double>(snapBoostFactor);

    // 近端区域：指数曲线插值
    // 注意：若 snapRadius >= nearRadius，吸附区会覆盖整个近端区，
    // 导致 nearRadius 处出现速度跳变。用户应确保 snapRadius < nearRadius。
    if (nearRadius > 0.0f && distance < static_cast<double>(nearRadius))
    {
        double t = distance / static_cast<double>(nearRadius);
        double curve = 1.0 - std::pow(1.0 - t, static_cast<double>(speedCurveExponent));
        const double nearNorm = std::clamp(
            static_cast<double>(nearRadius) / std::max(maxDistance, 1.0), 0.0, 1.0);
        const double nearSpeed = minSpeed + (maxSpeed - minSpeed) * nearNorm;
        double speed = minSpeed + (nearSpeed - minSpeed) * curve;
        return std::clamp(speed, minSpeed, maxSpeed);
    }

    // 远端区域：线性映射到 [min, max]
    double norm = std::clamp(distance / std::max(maxDistance, 1.0), 0.0, 1.0);
    double speed = minSpeed + (maxSpeed - minSpeed) * norm;
    return std::clamp(speed, minSpeed, maxSpeed);
}
