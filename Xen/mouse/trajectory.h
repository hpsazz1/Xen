#pragma once
// ============================================================
//  Trajectory — 仿人鼠标轨迹模拟
//    从 MouseThread 中提取，独立为两个纯算法函数：
//    - windMouseMove:  黄金比例双正弦振荡 + 随机漫步噪声
//    - bezierMove:     三次贝塞尔弧线 + Perlin 噪声扰动
//    所有状态通过 WindState 结构体传入传出，无内部全局状态。
// ============================================================
#include <algorithm>
#include <cmath>
#include <random>
#include "../third_party/motion_control/filters.h"  // perlinNoise

namespace Trajectory {

// ----- WindMouse 持久状态 -----
struct WindState
{
    double carryX = 0.0;
    double carryY = 0.0;
    double velX = 0.0;
    double velY = 0.0;
    double noiseX = 0.0;
    double noiseY = 0.0;
    double fracX = 0.0;
    double fracY = 0.0;
    double patternX = 0.0;
    double patternY = 0.0;
    double patternPhaseA = 0.0;
    double patternPhaseB = 0.0;
    double patternRateA = 0.08;
    double patternRateB = 0.11;
    std::mt19937 rng{ std::random_device{}() };
};

// ----- Bezier 弧线轨迹状态 -----
struct BezierState
{
    double fracX = 0.0;
    double fracY = 0.0;
};

/**
 * windMouseMove — WindMouse 轨迹模拟算法
 *
 * 将目标位移 dx/dy 分解为多个子步，叠加双正弦振荡模式
 * （黄金比例频率偏移）和指数平滑随机噪声，模拟人类光标移动的
 * 自然曲线轨迹。
 *
 * @param dx, dy         目标相对位移（像素空间整数）
 * @param windG           移速系数（越大越直越猛）
 * @param windW           摆动幅度（越小越直）
 * @param windM           单步上限
 * @param windD           微调距离阈值
 * @param state           持久状态（跨帧保持速度/噪声/相位）
 * @param emit            回调: emit(stepX, stepY) 发出子步位移
 */
template <typename EmitFn>
void windMouseMove(int dx, int dy,
                   double windG, double windW, double windM, double windD,
                   WindState& state, EmitFn&& emit)
{
    if (dx == 0 && dy == 0) return;

    state.carryX += static_cast<double>(dx);
    state.carryY += static_cast<double>(dy);

    const double baseG = std::clamp(windG, 0.05, 50.0);
    const double baseW = std::clamp(windW, 0.0, 80.0);
    const double baseM = std::max(1.0, windM);
    const double baseD = std::max(1.0, windD);

    static thread_local std::uniform_real_distribution<double> noiseDist(-1.0, 1.0);
    static thread_local std::uniform_real_distribution<double> clipDist(0.55, 1.0);
    constexpr double twoPi = 6.28318530717958647692;

    const double carryMag = std::hypot(state.carryX, state.carryY);
    const int maxSubsteps = std::clamp(static_cast<int>(carryMag * 0.24) + 1, 1, 5);

    for (int i = 0; i < maxSubsteps; ++i)
    {
        const double dist = std::hypot(state.carryX, state.carryY);
        const double velMag = std::hypot(state.velX, state.velY);

        if (dist < 0.20 && velMag < 0.12)
        {
            int flushX = static_cast<int>(std::round(state.fracX));
            int flushY = static_cast<int>(std::round(state.fracY));
            if (flushX != 0 || flushY != 0)
            {
                state.fracX -= static_cast<double>(flushX);
                state.fracY -= static_cast<double>(flushY);
                state.carryX -= static_cast<double>(flushX);
                state.carryY -= static_cast<double>(flushY);
                emit(flushX, flushY);
            }
            break;
        }

        const double normDist = std::clamp(dist / baseD, 0.0, 1.0);
        const double zeroFactor = (dist < 3.0) ? 0.0 : std::min(dist / 50.0, 1.0);
        const double pullGain = baseG * (0.25 + 0.75 * normDist);
        const double noiseAmp = baseW * (0.15 + 0.85 * normDist) * zeroFactor;

        double pullX = 0.0, pullY = 0.0;
        if (dist > 1e-8)
        {
            pullX = state.carryX / dist * pullGain;
            pullY = state.carryY / dist * pullGain;
        }

        state.patternRateA = std::clamp(state.patternRateA + noiseDist(state.rng) * 0.004, 0.025, 0.280);
        state.patternRateB = std::clamp(state.patternRateB + noiseDist(state.rng) * 0.004, 0.025, 0.280);

        const double stepTempo = 0.20 + 0.95 * normDist;
        state.patternPhaseA += state.patternRateA * stepTempo;
        state.patternPhaseB += state.patternRateB * stepTempo;
        if (state.patternPhaseA > twoPi) state.patternPhaseA = std::fmod(state.patternPhaseA, twoPi);
        if (state.patternPhaseB > twoPi) state.patternPhaseB = std::fmod(state.patternPhaseB, twoPi);

        const double oscAX = std::sin(state.patternPhaseA);
        const double oscBX = std::sin(state.patternPhaseB + 1.61803398875);
        const double oscAY = std::cos(state.patternPhaseA * 0.79 + 0.35);
        const double oscBY = std::cos(state.patternPhaseB * 1.17 - 0.48);

        const double patternAmp = baseW * (0.05 + 0.55 * normDist);
        const double patternTargetX = (oscAX + 0.58 * oscBX) * patternAmp;
        const double patternTargetY = (oscAY + 0.58 * oscBY) * patternAmp;
        const double patternBlend = 0.12 + 0.20 * normDist;
        state.patternX = state.patternX * (1.0 - patternBlend) + patternTargetX * patternBlend;
        state.patternY = state.patternY * (1.0 - patternBlend) + patternTargetY * patternBlend;

        state.noiseX = state.noiseX * 0.72 + noiseDist(state.rng) * noiseAmp * 0.28;
        state.noiseY = state.noiseY * 0.72 + noiseDist(state.rng) * noiseAmp * 0.28;

        const double windForceX = state.noiseX + state.patternX * 0.42;
        const double windForceY = state.noiseY + state.patternY * 0.42;

        const double drag = 0.82 + (1.0 - normDist) * 0.10;
        state.velX = state.velX * drag + pullX + windForceX;
        state.velY = state.velY * drag + pullY + windForceY;

        const double vCap = std::max(0.65, baseM * (0.30 + 0.70 * normDist));
        const double newVelMag = std::hypot(state.velX, state.velY);
        if (newVelMag > vCap)
        {
            const double clip = vCap * clipDist(state.rng);
            state.velX = (state.velX / newVelMag) * clip;
            state.velY = (state.velY / newVelMag) * clip;
        }

        state.fracX += state.velX;
        state.fracY += state.velY;

        int stepX = static_cast<int>(std::round(state.fracX));
        int stepY = static_cast<int>(std::round(state.fracY));
        if (stepX == 0 && stepY == 0) continue;

        state.fracX -= static_cast<double>(stepX);
        state.fracY -= static_cast<double>(stepY);
        state.carryX -= static_cast<double>(stepX);
        state.carryY -= static_cast<double>(stepY);
        emit(stepX, stepY);
    }

    const double carryCap = 500.0;
    const double finalCarryMag = std::hypot(state.carryX, state.carryY);
    if (finalCarryMag > carryCap)
    {
        const double s = carryCap / finalCarryMag;
        state.carryX *= s;
        state.carryY *= s;
    }
}

/**
 * bezierMove — 三次贝塞尔弧线轨迹
 *
 * 使用 Perlin 噪声扰动贝塞尔控制点，产生自然的弧线移动路径。
 * 子步间累加亚像素分数，跨整数阈值时发送。
 */
template <typename EmitFn>
void bezierMove(int dx, int dy, float bezierStrength,
                BezierState& state, int frameCounter, EmitFn&& emit)
{
    if (dx == 0 && dy == 0) return;

    const double dist = std::hypot(dx, dy);
    int steps = std::max(3, static_cast<int>(std::ceil(dist / 6.0)));
    steps = std::min(steps, 14);

    double side1 = filters::perlinNoise(frameCounter, 1.7);
    double side2 = filters::perlinNoise(frameCounter, 2.3);

    double nx = dist > 1e-8 ? -dy / dist : 0.0;
    double ny = dist > 1e-8 ?  dx / dist : 0.0;
    double offset = dist * static_cast<double>(bezierStrength) * 0.65;

    double cp1x = dx * 0.33 + nx * offset * side1;
    double cp1y = dy * 0.33 + ny * offset * side1;
    double cp2x = dx * 0.67 + nx * offset * side2;
    double cp2y = dy * 0.67 + ny * offset * side2;

    for (int i = 1; i <= steps; ++i)
    {
        double t = static_cast<double>(i) / steps;
        double t2 = t * t, t3 = t2 * t;
        double mt = 1.0 - t, mt2 = mt * mt;
        double bx = 3.0 * mt2 * t * cp1x + 3.0 * mt * t2 * cp2x + t3 * dx;
        double by = 3.0 * mt2 * t * cp1y + 3.0 * mt * t2 * cp2y + t3 * dy;

        double pt = static_cast<double>(i - 1) / steps;
        double pt2 = pt * pt, pt3 = pt2 * pt;
        double pmt = 1.0 - pt, pmt2 = pmt * pmt;
        double pbx = 3.0 * pmt2 * pt * cp1x + 3.0 * pmt * pt2 * cp2x + pt3 * dx;
        double pby = 3.0 * pmt2 * pt * cp1y + 3.0 * pmt * pt2 * cp2y + pt3 * dy;

        state.fracX += (bx - pbx);
        state.fracY += (by - pby);
        int stepX = static_cast<int>(std::round(state.fracX));
        int stepY = static_cast<int>(std::round(state.fracY));
        if (stepX != 0 || stepY != 0)
        {
            state.fracX -= static_cast<double>(stepX);
            state.fracY -= static_cast<double>(stepY);
            emit(stepX, stepY);
        }
    }
}

} // namespace Trajectory
