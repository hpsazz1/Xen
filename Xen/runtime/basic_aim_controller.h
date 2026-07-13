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
        double maxCountsPerSecond = 960.0; // 物理设备速率；单帧预算使用捕获窗实际 FPS 对应的 dt
        double settleRadiusPixels = 5.0;
        double releaseRadiusPixels = 8.0;
    };

    struct Output
    {
        double countsX = 0.0;
        double countsY = 0.0;
        double requestedPixelX = 0.0;
        double requestedPixelY = 0.0;
        double errorDistance = 0.0;
        double frameCountLimit = 0.0;
        bool settled = false;
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
                out.settled = true;
                return out;
            }
            settled_ = false;
        }
        if (out.errorDistance <= settleRadius)
        {
            settled_ = true;
            out.settled = true;
            return out;
        }

        const double response = std::clamp(settings.responseSeconds, 0.010, 0.500);
        const double responseFraction = 1.0 - std::exp(-dt / response);
        out.requestedPixelX = errorX * responseFraction;
        out.requestedPixelY = errorY * responseFraction;
        out.countsX = out.requestedPixelX * countsPerPixelX;
        out.countsY = out.requestedPixelY * countsPerPixelY;

        out.frameCountLimit = std::max(1.0, settings.maxCountsPerSecond) * dt;
        const double magnitude = std::hypot(out.countsX, out.countsY);
        if (magnitude > out.frameCountLimit && magnitude > 0.0)
        {
            const double scale = out.frameCountLimit / magnitude;
            out.countsX *= scale;
            out.countsY *= scale;
            out.requestedPixelX *= scale;
            out.requestedPixelY *= scale;
        }
        return out;
    }

    void reset() { settled_ = false; }
    bool settled() const { return settled_; }

private:
    bool settled_ = false;
};
