#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>

// 基于连续真实检测观测的运动学预测器。
// 输入坐标必须先补偿程序自身视角运动；模块不使用不可靠的目标框测距或武器弹速假设。
class TargetPredictor
{
public:
    struct Settings
    {
        bool enabled = true;
        double additionalLeadSeconds = 0.050; // 除检测观测年龄外的基础前瞻时间
        double velocityTimeConstantSeconds = 0.015; // 目标自身速度低通时间常数
        double predictionStrength = 1.0; // 运动学提前总强度；0关闭位移但保留诊断
    };

    struct Result
    {
        double x = 0.0;
        double y = 0.0;
        double velocityX = 0.0; // 已补偿自身视角运动的目标速度，px/sec
        double velocityY = 0.0;
        double accelerationX = 0.0; // 平滑目标加速度，px/sec^2
        double accelerationY = 0.0;
        double leadSeconds = 0.0;
        double offsetX = 0.0;
        double offsetY = 0.0;
        bool directionLocked = false;
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
            return baseResult(x, y);
        }

        if (controlTime.time_since_epoch().count() == 0)
            controlTime = std::chrono::steady_clock::now();
        if (observationTime.time_since_epoch().count() == 0)
            observationTime = controlTime;

        if (!initialized_)
        {
            initialize(x, y, observationTime);
            return baseResult(x, y);
        }

        const double dt = std::chrono::duration<double>(
            observationTime - previousObservationTime_).count();
        if (!std::isfinite(dt) || dt <= 0.0 || dt > 0.10)
        {
            reset();
            initialize(x, y, observationTime);
            return baseResult(x, y);
        }

        const double span = std::max(1.0, detectionSpan);
        double sampleVelocityX = (x - previousX_) / dt;
        double sampleVelocityY = (y - previousY_) / dt;
        clampVector(sampleVelocityX, sampleVelocityY, span * 6.0);

        updateMotion(sampleVelocityX, sampleVelocityY, dt, span, settings);
        updateDirection(sampleVelocityX, sampleVelocityY, span);

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

        if (!directionLocked_ || suppressPrediction_)
        {
            return {
                x, y, velocityX_, velocityY_, accelerationX_, accelerationY_,
                leadSeconds, 0.0, 0.0, directionLocked_, true
            };
        }

        const double speedAlongDirection = std::max(
            0.0, velocityX_ * directionX_ + velocityY_ * directionY_);
        const double accelerationAlongDirection =
            accelerationX_ * directionX_ + accelerationY_ * directionY_;
        const double kinematicDistance = std::max(
            0.0,
            speedAlongDirection * leadSeconds +
            0.5 * accelerationAlongDirection * leadSeconds * leadSeconds);
        const double strength = std::clamp(settings.predictionStrength, 0.0, 4.0);
        const double maxPredictionDistance = std::max(12.0, span * 0.35);
        const double predictionDistance = std::clamp(
            kinematicDistance * strength, 0.0, maxPredictionDistance);
        const double offsetX = directionX_ * predictionDistance;
        const double offsetY = directionY_ * predictionDistance;
        return {
            x + offsetX, y + offsetY,
            velocityX_, velocityY_, accelerationX_, accelerationY_,
            leadSeconds, offsetX, offsetY, true, true
        };
    }

    void reset()
    {
        initialized_ = false;
        hasVelocity_ = false;
        hasAcceleration_ = false;
        directionLocked_ = false;
        suppressPrediction_ = false;
        previousX_ = previousY_ = 0.0;
        velocityX_ = velocityY_ = 0.0;
        accelerationX_ = accelerationY_ = 0.0;
        directionX_ = directionY_ = 0.0;
        pendingDirectionX_ = pendingDirectionY_ = 0.0;
        pendingDirectionSamples_ = 0;
        stationarySamples_ = 0;
        previousObservationTime_ = {};
    }

private:
    static Result baseResult(double x, double y)
    {
        return { x, y, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, false, false };
    }

    void initialize(double x, double y, std::chrono::steady_clock::time_point observationTime)
    {
        initialized_ = true;
        previousX_ = x;
        previousY_ = y;
        previousObservationTime_ = observationTime;
    }

    void updateMotion(double sampleVelocityX, double sampleVelocityY,
                      double dt, double span, const Settings& settings)
    {
        if (!hasVelocity_)
        {
            velocityX_ = sampleVelocityX;
            velocityY_ = sampleVelocityY;
            hasVelocity_ = true;
            return;
        }

        const double previousVelocityX = velocityX_;
        const double previousVelocityY = velocityY_;
        const double velocityTau = std::clamp(
            settings.velocityTimeConstantSeconds, 0.005, 0.250);
        const double velocityAlpha = 1.0 - std::exp(-dt / velocityTau);
        velocityX_ += (sampleVelocityX - velocityX_) * velocityAlpha;
        velocityY_ += (sampleVelocityY - velocityY_) * velocityAlpha;

        double sampleAccelerationX = (velocityX_ - previousVelocityX) / dt;
        double sampleAccelerationY = (velocityY_ - previousVelocityY) / dt;
        clampVector(sampleAccelerationX, sampleAccelerationY, span * 30.0);
        if (!hasAcceleration_)
        {
            accelerationX_ = sampleAccelerationX;
            accelerationY_ = sampleAccelerationY;
            hasAcceleration_ = true;
            return;
        }

        const double accelerationTau = std::clamp(velocityTau * 0.60, 0.015, 0.100);
        const double accelerationAlpha = 1.0 - std::exp(-dt / accelerationTau);
        accelerationX_ += (sampleAccelerationX - accelerationX_) * accelerationAlpha;
        accelerationY_ += (sampleAccelerationY - accelerationY_) * accelerationAlpha;
    }

    void updateDirection(double sampleVelocityX, double sampleVelocityY, double span)
    {
        const double sampleSpeed = std::hypot(sampleVelocityX, sampleVelocityY);
        const double activationSpeed = std::max(40.0, span * 0.15);
        if (sampleSpeed < activationSpeed * 0.50)
        {
            ++stationarySamples_;
            suppressPrediction_ = true;
            pendingDirectionSamples_ = 0;
            if (stationarySamples_ >= 2)
            {
                directionLocked_ = false;
                velocityX_ = velocityY_ = 0.0;
                accelerationX_ = accelerationY_ = 0.0;
            }
            return;
        }
        stationarySamples_ = 0;

        const double sampleDirectionX = sampleVelocityX / sampleSpeed;
        const double sampleDirectionY = sampleVelocityY / sampleSpeed;
        if (!directionLocked_)
        {
            accumulatePendingDirection(sampleDirectionX, sampleDirectionY);
            suppressPrediction_ = true;
            if (pendingDirectionSamples_ >= 3)
            {
                lockPendingDirection();
                suppressPrediction_ = false;
            }
            return;
        }

        const double alignment =
            sampleDirectionX * directionX_ + sampleDirectionY * directionY_;
        if (alignment < -0.25)
        {
            suppressPrediction_ = true;
            accumulatePendingDirection(sampleDirectionX, sampleDirectionY);
            if (pendingDirectionSamples_ >= 2)
            {
                lockPendingDirection();
                velocityX_ = sampleVelocityX;
                velocityY_ = sampleVelocityY;
                accelerationX_ = accelerationY_ = 0.0;
                suppressPrediction_ = false;
            }
            return;
        }

        pendingDirectionSamples_ = 0;
        suppressPrediction_ = alignment < 0.20;
        if (!suppressPrediction_)
        {
            directionX_ = directionX_ * 0.80 + sampleDirectionX * 0.20;
            directionY_ = directionY_ * 0.80 + sampleDirectionY * 0.20;
            normalize(directionX_, directionY_);
        }
    }

    void accumulatePendingDirection(double x, double y)
    {
        if (pendingDirectionSamples_ == 0 ||
            x * pendingDirectionX_ + y * pendingDirectionY_ < 0.70)
        {
            pendingDirectionX_ = x;
            pendingDirectionY_ = y;
            pendingDirectionSamples_ = 1;
            return;
        }
        pendingDirectionX_ += x;
        pendingDirectionY_ += y;
        normalize(pendingDirectionX_, pendingDirectionY_);
        ++pendingDirectionSamples_;
    }

    void lockPendingDirection()
    {
        directionX_ = pendingDirectionX_;
        directionY_ = pendingDirectionY_;
        normalize(directionX_, directionY_);
        directionLocked_ = true;
        pendingDirectionSamples_ = 0;
    }

    static void clampVector(double& x, double& y, double maximumLength)
    {
        const double length = std::hypot(x, y);
        if (length <= maximumLength || length <= 1e-9)
            return;
        const double scale = maximumLength / length;
        x *= scale;
        y *= scale;
    }

    static void normalize(double& x, double& y)
    {
        const double length = std::hypot(x, y);
        if (length <= 1e-9)
        {
            x = y = 0.0;
            return;
        }
        x /= length;
        y /= length;
    }

    bool initialized_ = false;
    bool hasVelocity_ = false;
    bool hasAcceleration_ = false;
    bool directionLocked_ = false;
    bool suppressPrediction_ = false;
    double previousX_ = 0.0;
    double previousY_ = 0.0;
    double velocityX_ = 0.0;
    double velocityY_ = 0.0;
    double accelerationX_ = 0.0;
    double accelerationY_ = 0.0;
    double directionX_ = 0.0;
    double directionY_ = 0.0;
    double pendingDirectionX_ = 0.0;
    double pendingDirectionY_ = 0.0;
    int pendingDirectionSamples_ = 0;
    int stationarySamples_ = 0;
    std::chrono::steady_clock::time_point previousObservationTime_{};
};
