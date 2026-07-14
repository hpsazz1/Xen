#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

// 基于连续真实检测观测的目标运动预测器。
// 输入坐标必须先补偿自瞄自身视角运动；调用方负责目标身份和丢失生命周期。
class TargetPredictor
{
public:
    struct Bounds
    {
        double x = 0.0;
        double y = 0.0;
        double width = 0.0;
        double height = 0.0;
    };

    struct Settings
    {
        bool enabled = true;
        double additionalLeadSeconds = 0.020; // 除检测观测年龄外的固定前瞻时间
        double velocityTimeConstantSeconds = 0.035; // 目标自身速度低通时间常数
        double outsideBoxScale = 0.50; // 越过目标框边缘后继续前置的目标投影身位数
    };

    struct Result
    {
        double x = 0.0;
        double y = 0.0;
        double velocityX = 0.0; // 已补偿自身视角运动的目标速度，px/sec
        double velocityY = 0.0;
        double leadSeconds = 0.0;
        double offsetX = 0.0;
        double offsetY = 0.0;
        bool directionLocked = false;
        bool outsideApplied = false;
        bool applied = false;
    };

    Result update(double x, double y,
                  const Bounds& bounds,
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

        updateVelocity(sampleVelocityX, sampleVelocityY, dt, settings);
        updateDirection(sampleVelocityX, sampleVelocityY, bounds);

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
                x, y, velocityX_, velocityY_, leadSeconds,
                0.0, 0.0, directionLocked_, false, true
            };
        }

        const double speedAlongDirection = std::max(
            0.0, velocityX_ * directionX_ + velocityY_ * directionY_);
        double predictionDistance = speedAlongDirection * leadSeconds;
        bool outsideApplied = false;
        if (settings.outsideBoxScale > 0.0 && validBounds(bounds))
        {
            const double edgeDistance = distanceToBoxEdge(x, y, bounds, directionX_, directionY_);
            const double projectedSpan =
                std::abs(directionX_) * bounds.width +
                std::abs(directionY_) * bounds.height;
            const double outsideDistance = std::clamp(
                settings.outsideBoxScale, 0.0, 2.0) * projectedSpan;
            predictionDistance = std::max(
                predictionDistance, edgeDistance + outsideDistance);
            outsideApplied = true;
        }

        const double maxPredictionDistance = outsideApplied
            ? std::max(16.0, span * 0.45)
            : std::max(8.0, span * 0.10);
        predictionDistance = std::clamp(predictionDistance, 0.0, maxPredictionDistance);
        const double offsetX = directionX_ * predictionDistance;
        const double offsetY = directionY_ * predictionDistance;
        return {
            x + offsetX, y + offsetY,
            velocityX_, velocityY_, leadSeconds,
            offsetX, offsetY, true, outsideApplied, true
        };
    }

    void reset()
    {
        initialized_ = false;
        hasVelocity_ = false;
        directionLocked_ = false;
        suppressPrediction_ = false;
        previousX_ = previousY_ = 0.0;
        velocityX_ = velocityY_ = 0.0;
        directionX_ = directionY_ = 0.0;
        pendingDirectionX_ = pendingDirectionY_ = 0.0;
        pendingDirectionSamples_ = 0;
        stationarySamples_ = 0;
        previousObservationTime_ = {};
    }

private:
    static Result baseResult(double x, double y)
    {
        return { x, y, 0.0, 0.0, 0.0, 0.0, 0.0, false, false, false };
    }

    void initialize(double x, double y, std::chrono::steady_clock::time_point observationTime)
    {
        initialized_ = true;
        previousX_ = x;
        previousY_ = y;
        previousObservationTime_ = observationTime;
    }

    void updateVelocity(double sampleVelocityX, double sampleVelocityY,
                        double dt, const Settings& settings)
    {
        if (!hasVelocity_)
        {
            velocityX_ = sampleVelocityX;
            velocityY_ = sampleVelocityY;
            hasVelocity_ = true;
            return;
        }

        const double tau = std::clamp(
            settings.velocityTimeConstantSeconds, 0.005, 0.250);
        const double alpha = 1.0 - std::exp(-dt / tau);
        velocityX_ += (sampleVelocityX - velocityX_) * alpha;
        velocityY_ += (sampleVelocityY - velocityY_) * alpha;
    }

    void updateDirection(double sampleVelocityX, double sampleVelocityY, const Bounds& bounds)
    {
        const double sampleSpeed = std::hypot(sampleVelocityX, sampleVelocityY);
        const double targetMinorSpan = validBounds(bounds)
            ? std::max(1.0, std::min(bounds.width, bounds.height)) : 24.0;
        const double activationSpeed = std::max(40.0, targetMinorSpan * 1.5);

        if (sampleSpeed < activationSpeed * 0.50)
        {
            ++stationarySamples_;
            suppressPrediction_ = true;
            pendingDirectionSamples_ = 0;
            if (stationarySamples_ >= 2)
            {
                directionLocked_ = false;
                velocityX_ = velocityY_ = 0.0;
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

    static bool validBounds(const Bounds& bounds)
    {
        return std::isfinite(bounds.x) && std::isfinite(bounds.y) &&
            std::isfinite(bounds.width) && std::isfinite(bounds.height) &&
            bounds.width > 1.0 && bounds.height > 1.0;
    }

    static double distanceToBoxEdge(
        double x, double y, const Bounds& bounds, double directionX, double directionY)
    {
        double distance = std::numeric_limits<double>::infinity();
        if (directionX > 1e-6)
            distance = std::min(distance, (bounds.x + bounds.width - x) / directionX);
        else if (directionX < -1e-6)
            distance = std::min(distance, (bounds.x - x) / directionX);
        if (directionY > 1e-6)
            distance = std::min(distance, (bounds.y + bounds.height - y) / directionY);
        else if (directionY < -1e-6)
            distance = std::min(distance, (bounds.y - y) / directionY);
        return std::isfinite(distance) ? std::max(0.0, distance) : 0.0;
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
    bool directionLocked_ = false;
    bool suppressPrediction_ = false;
    double previousX_ = 0.0;
    double previousY_ = 0.0;
    double velocityX_ = 0.0;
    double velocityY_ = 0.0;
    double directionX_ = 0.0;
    double directionY_ = 0.0;
    double pendingDirectionX_ = 0.0;
    double pendingDirectionY_ = 0.0;
    int pendingDirectionSamples_ = 0;
    int stationarySamples_ = 0;
    std::chrono::steady_clock::time_point previousObservationTime_{};
};
