#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>

// 基于连续真实检测观测的最小前瞻预测器。
// 调用方负责目标身份和丢失生命周期；一旦目标丢失必须调用 reset()，本模块不产生滑行帧。
class TargetPredictor
{
public:
    struct Settings
    {
        bool enabled = true;
        double additionalLeadSeconds = 0.020; // 除检测观测年龄外的固定前瞻时间
        double velocityTimeConstantSeconds = 0.035; // 速度低通时间常数，按真实观测间隔计算
    };

    struct Result
    {
        double x = 0.0;
        double y = 0.0;
        double velocityX = 0.0; // px/sec
        double velocityY = 0.0; // px/sec
        double leadSeconds = 0.0;
        bool applied = false;
    };

    Result update(double x, double y,
                  std::chrono::steady_clock::time_point observationTime,
                  std::chrono::steady_clock::time_point controlTime,
                  double detectionSpan,
                  const Settings& settings)
    {
        if (!settings.enabled || !std::isfinite(x) || !std::isfinite(y))
        {
            reset();
            return { x, y, 0.0, 0.0, 0.0, false };
        }

        if (controlTime.time_since_epoch().count() == 0)
            controlTime = std::chrono::steady_clock::now();
        if (observationTime.time_since_epoch().count() == 0)
            observationTime = controlTime;

        if (!initialized_)
        {
            initialized_ = true;
            previousX_ = x;
            previousY_ = y;
            previousObservationTime_ = observationTime;
            return { x, y, 0.0, 0.0, 0.0, false };
        }

        const double dt = std::chrono::duration<double>(
            observationTime - previousObservationTime_).count();
        if (!std::isfinite(dt) || dt <= 0.0 || dt > 0.10)
        {
            reset();
            initialized_ = true;
            previousX_ = x;
            previousY_ = y;
            previousObservationTime_ = observationTime;
            return { x, y, 0.0, 0.0, 0.0, false };
        }

        double sampleVelocityX = (x - previousX_) / dt;
        double sampleVelocityY = (y - previousY_) / dt;
        const double span = std::max(1.0, detectionSpan);
        const double maxVelocity = span * 6.0;
        const double sampleSpeed = std::hypot(sampleVelocityX, sampleVelocityY);
        if (sampleSpeed > maxVelocity)
        {
            const double scale = maxVelocity / sampleSpeed;
            sampleVelocityX *= scale;
            sampleVelocityY *= scale;
        }

        if (!hasVelocity_)
        {
            velocityX_ = sampleVelocityX;
            velocityY_ = sampleVelocityY;
            hasVelocity_ = true;
        }
        else
        {
            const double tau = std::clamp(
                settings.velocityTimeConstantSeconds, 0.005, 0.250);
            const double alpha = 1.0 - std::exp(-dt / tau);
            velocityX_ += (sampleVelocityX - velocityX_) * alpha;
            velocityY_ += (sampleVelocityY - velocityY_) * alpha;
        }

        previousX_ = x;
        previousY_ = y;
        previousObservationTime_ = observationTime;

        double observationAge = std::chrono::duration<double>(
            controlTime - observationTime).count();
        if (!std::isfinite(observationAge) || observationAge < 0.0)
            observationAge = 0.0;
        observationAge = std::min(observationAge, 0.100);
        const double leadSeconds = std::clamp(
            observationAge + settings.additionalLeadSeconds, 0.0, 0.120);

        double predictionX = velocityX_ * leadSeconds;
        double predictionY = velocityY_ * leadSeconds;
        const double maxPredictionDistance = std::max(8.0, span * 0.10);
        const double predictionDistance = std::hypot(predictionX, predictionY);
        if (predictionDistance > maxPredictionDistance)
        {
            const double scale = maxPredictionDistance / predictionDistance;
            predictionX *= scale;
            predictionY *= scale;
        }

        return {
            x + predictionX,
            y + predictionY,
            velocityX_, velocityY_, leadSeconds, true
        };
    }

    void reset()
    {
        initialized_ = false;
        hasVelocity_ = false;
        previousX_ = previousY_ = 0.0;
        velocityX_ = velocityY_ = 0.0;
        previousObservationTime_ = {};
    }

private:
    bool initialized_ = false;
    bool hasVelocity_ = false;
    double previousX_ = 0.0;
    double previousY_ = 0.0;
    double velocityX_ = 0.0;
    double velocityY_ = 0.0;
    std::chrono::steady_clock::time_point previousObservationTime_{};
};
