#pragma once

#include <algorithm>
#include <cmath>

// 帧率无关的一阶基础控制器。
// 输入是检测空间像素误差，输出是当前帧应发送的设备 counts。
class BasicAimController
{
public:
    struct Settings
    {
        double responseSeconds = 0.080;
        double maxCountsPerSecond = 1440.0; // 四链路九宫格复测值；仅放宽远距限速，单帧预算仍按真实 dt 换算
        double integralTimeSeconds = 0.0;   // 0 表示关闭；现场移动目标复测通过后再推广默认值
        double settleRadiusPixels = 5.0;
        double releaseRadiusPixels = 8.0;
    };

    struct Output
    {
        double countsX = 0.0;
        double countsY = 0.0;
        double requestedPixelX = 0.0;
        double requestedPixelY = 0.0;
        double integralCountsX = 0.0;
        double integralCountsY = 0.0;
        double errorDistance = 0.0;
        double frameCountLimit = 0.0;
        bool settled = false;
        bool speedLimited = false; // 本次输出是否被最大设备速率截断
    };

    Output update(double errorX, double errorY, double dt,
                  double countsPerPixelX, double countsPerPixelY,
                  const Settings& settings)
    {
        Output out;
        out.errorDistance = std::hypot(errorX, errorY);
        dt = std::clamp(dt, 1.0 / 500.0, 0.05);

        const double settleRadius = std::max(0.0, settings.settleRadiusPixels);
        const double releaseRadius = std::max(settleRadius, settings.releaseRadiusPixels);
        if (settled_)
        {
            if (out.errorDistance <= releaseRadius)
            {
                clearIntegralState();
                out.settled = true;
                return out;
            }
            settled_ = false;
        }
        if (out.errorDistance <= settleRadius)
        {
            settled_ = true;
            clearIntegralState();
            out.settled = true;
            return out;
        }

        const double response = std::clamp(settings.responseSeconds, 0.010, 0.500);
        const double responseFraction = 1.0 - std::exp(-dt / response);
        const double proportionalCountsX = errorX * responseFraction * countsPerPixelX;
        const double proportionalCountsY = errorY * responseFraction * countsPerPixelY;

        const double integralTime = settings.integralTimeSeconds > 0.0
            ? std::clamp(settings.integralTimeSeconds, 0.050, 1.0) : 0.0;
        double candidateIntegralX = integralCountErrorX_;
        double candidateIntegralY = integralCountErrorY_;
        if (integralTime > 0.0)
        {
            // 匀速目标对纯比例控制形成固定滞后。积分项累计 counts 误差并提供持续速度，
            // 使系统能够消除斜坡输入的稳态误差；误差换向时清零，避免反转后旧积分拖拽。
            if (hasPreviousError_ && errorX * previousErrorX_ < 0.0)
            {
                integralCountErrorX_ = 0.0;
                candidateIntegralX = 0.0;
            }
            if (hasPreviousError_ && errorY * previousErrorY_ < 0.0)
            {
                integralCountErrorY_ = 0.0;
                candidateIntegralY = 0.0;
            }
            candidateIntegralX += errorX * countsPerPixelX * dt;
            candidateIntegralY += errorY * countsPerPixelY * dt;

            // 积分速度最多占设备总预算的一半；远距离比例项限速时不提交新积分，
            // 防止静止大步进和设备饱和期间产生 wind-up。
            const double maxIntegralState =
                std::max(1.0, settings.maxCountsPerSecond) * 0.5 * response * integralTime;
            const double candidateMagnitude = std::hypot(candidateIntegralX, candidateIntegralY);
            if (candidateMagnitude > maxIntegralState && candidateMagnitude > 0.0)
            {
                const double scale = maxIntegralState / candidateMagnitude;
                candidateIntegralX *= scale;
                candidateIntegralY *= scale;
            }
            out.integralCountsX = candidateIntegralX * dt / (response * integralTime);
            out.integralCountsY = candidateIntegralY * dt / (response * integralTime);
        }
        else
        {
            clearIntegralState();
        }

        out.countsX = proportionalCountsX + out.integralCountsX;
        out.countsY = proportionalCountsY + out.integralCountsY;
        out.requestedPixelX = std::abs(countsPerPixelX) > 1e-12
            ? out.countsX / countsPerPixelX : 0.0;
        out.requestedPixelY = std::abs(countsPerPixelY) > 1e-12
            ? out.countsY / countsPerPixelY : 0.0;

        out.frameCountLimit = std::max(1.0, settings.maxCountsPerSecond) * dt;
        const double magnitude = std::hypot(out.countsX, out.countsY);
        if (magnitude > out.frameCountLimit && magnitude > 0.0)
        {
            out.speedLimited = true;
            const double scale = out.frameCountLimit / magnitude;
            out.countsX *= scale;
            out.countsY *= scale;
            out.integralCountsX *= scale;
            out.integralCountsY *= scale;
            out.requestedPixelX *= scale;
            out.requestedPixelY *= scale;
        }
        else if (integralTime > 0.0)
        {
            integralCountErrorX_ = candidateIntegralX;
            integralCountErrorY_ = candidateIntegralY;
        }
        if (integralTime > 0.0)
        {
            previousErrorX_ = errorX;
            previousErrorY_ = errorY;
            hasPreviousError_ = true;
        }
        return out;
    }

    void reset()
    {
        settled_ = false;
        clearIntegralState();
    }
    bool settled() const { return settled_; }

private:
    void clearIntegralState()
    {
        integralCountErrorX_ = 0.0;
        integralCountErrorY_ = 0.0;
        previousErrorX_ = 0.0;
        previousErrorY_ = 0.0;
        hasPreviousError_ = false;
    }

    bool settled_ = false;
    double integralCountErrorX_ = 0.0;
    double integralCountErrorY_ = 0.0;
    double previousErrorX_ = 0.0;
    double previousErrorY_ = 0.0;
    bool hasPreviousError_ = false;
};
