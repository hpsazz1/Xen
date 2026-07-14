#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>

// 基于连续真实检测观测的稳健常速度预测器。
// 输入坐标必须先补偿程序自身已经发送的视角运动；模块不使用目标框测距或武器弹速假设。
class TargetPredictor
{
public:
    struct Settings
    {
        bool enabled = true;
        double additionalLeadSeconds = 0.050; // 除检测观测年龄外的基础前瞻时间
        double velocityTimeConstantSeconds = 0.050; // 兼容旧配置名，实际表示稳健速度回归窗口
        double predictionStrength = 1.0; // 常速度提前总强度；0关闭位移但保留诊断
    };

    struct Result
    {
        double x = 0.0;
        double y = 0.0;
        double velocityX = 0.0; // 已补偿自身视角运动并通过稳健回归的目标速度，px/sec
        double velocityY = 0.0;
        double accelerationX = 0.0; // 仅供诊断，不参与控制输出，px/sec^2
        double accelerationY = 0.0;
        double leadSeconds = 0.0;
        double offsetX = 0.0;
        double offsetY = 0.0;
        bool directionLocked = false;
        bool applied = false;
        bool selfMotionSuppressed = false;
    };

    // 静止目标被程序拉向中心时，补偿残差可能表现为“预测速度继续向误差外侧”。
    // 只有自身视角与误差同向、屏幕目标明确向中心收敛且预测速度反而向外时才判为伪迹。
    static bool isSelfMotionArtifact(
        double errorX, double errorY,
        double screenDeltaX, double screenDeltaY,
        double viewDeltaX, double viewDeltaY,
        double velocityX, double velocityY)
    {
        const double errorLength = std::hypot(errorX, errorY);
        const double screenLength = std::hypot(screenDeltaX, screenDeltaY);
        const double viewLength = std::hypot(viewDeltaX, viewDeltaY);
        const double velocityLength = std::hypot(velocityX, velocityY);
        if (errorLength < 4.0 || screenLength < 0.5 || viewLength < 1.0 ||
            velocityLength < 64.0)
        {
            return false;
        }

        const double viewErrorAlignment =
            (viewDeltaX * errorX + viewDeltaY * errorY) / (viewLength * errorLength);
        const double screenErrorAlignment =
            (screenDeltaX * errorX + screenDeltaY * errorY) / (screenLength * errorLength);
        const double velocityErrorAlignment =
            (velocityX * errorX + velocityY * errorY) / (velocityLength * errorLength);
        const double screenViewAlignment =
            (screenDeltaX * viewDeltaX + screenDeltaY * viewDeltaY) /
            (screenLength * viewLength);
        return viewErrorAlignment > 0.50 &&
               screenErrorAlignment < -0.35 &&
               velocityErrorAlignment > 0.70 &&
               screenViewAlignment < -0.35;
    }

    // 自运动伪迹通常不会只持续一个观测帧。门控命中后继续撤销四个后续观测，
    // 覆盖约100 FPS下40 ms的控制响应尾迹，同时不清空真实目标的稳健速度与方向状态。
    void applySelfMotionSuppression(Result& result, bool artifactDetected)
    {
        constexpr int kHoldFrames = 4;
        if (artifactDetected)
            selfMotionSuppressionFramesRemaining_ = kHoldFrames;

        if (!artifactDetected && selfMotionSuppressionFramesRemaining_ <= 0)
            return;

        result.x -= result.offsetX;
        result.y -= result.offsetY;
        result.offsetX = 0.0;
        result.offsetY = 0.0;
        result.selfMotionSuppressed = true;

        if (!artifactDetected)
        {
            --selfMotionSuppressionFramesRemaining_;
            // 保持期内的坐标变化主要来自控制器过冲和检测框回弹，不能作为真实反向运动证据。
            // 尾帧结束时只丢弃运动学证据，保留当前观测基点，让后续新观测重新确认方向。
            if (selfMotionSuppressionFramesRemaining_ == 0)
                discardMotionEvidence();
        }
    }

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

        const bool reliableMotion = updateMotion(
            x, y, observationTime, dt, span, settings);
        updateDirection(sampleVelocityX, sampleVelocityY, reliableMotion, span);

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
        const double strength = std::clamp(settings.predictionStrength, 0.0, 4.0);
        const double maxPredictionDistance = std::max(12.0, span * 0.35);
        const double predictionDistance = std::clamp(
            speedAlongDirection * leadSeconds * strength,
            0.0, maxPredictionDistance);
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
        directionLocked_ = false;
        suppressPrediction_ = false;
        predictionEstablished_ = false;
        previousX_ = previousY_ = 0.0;
        velocityX_ = velocityY_ = 0.0;
        accelerationX_ = accelerationY_ = 0.0;
        directionX_ = directionY_ = 0.0;
        pendingDirectionX_ = pendingDirectionY_ = 0.0;
        pendingDirectionSamples_ = 0;
        stationarySamples_ = 0;
        unreliableSamples_ = 0;
        reliableDirectionSamples_ = 0;
        selfMotionSuppressionFramesRemaining_ = 0;
        selfMotionRearmPending_ = false;
        observations_.clear();
        previousObservationTime_ = {};
    }

private:
    using TimePoint = std::chrono::steady_clock::time_point;

    struct Observation
    {
        double x = 0.0;
        double y = 0.0;
        TimePoint time{};
    };

    static Result baseResult(double x, double y)
    {
        return { x, y, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, false, false };
    }

    void initialize(double x, double y, TimePoint observationTime)
    {
        initialized_ = true;
        previousX_ = x;
        previousY_ = y;
        previousObservationTime_ = observationTime;
        observations_.push_back({ x, y, observationTime });
    }

    void discardMotionEvidence()
    {
        hasVelocity_ = false;
        directionLocked_ = false;
        suppressPrediction_ = false;
        predictionEstablished_ = false;
        velocityX_ = velocityY_ = 0.0;
        accelerationX_ = accelerationY_ = 0.0;
        directionX_ = directionY_ = 0.0;
        pendingDirectionX_ = pendingDirectionY_ = 0.0;
        pendingDirectionSamples_ = 0;
        stationarySamples_ = 0;
        unreliableSamples_ = 0;
        reliableDirectionSamples_ = 0;
        selfMotionRearmPending_ = true;
        observations_.clear();
        if (initialized_)
            observations_.push_back({ previousX_, previousY_, previousObservationTime_ });
    }

    bool updateMotion(double x, double y, TimePoint observationTime,
                      double dt, double span, const Settings& settings)
    {
        observations_.push_back({ x, y, observationTime });

        // 低于40 ms的窗口在约100 FPS实测中通常只有两三帧，无法区分检测抖动与真实移动。
        const double windowSeconds = std::clamp(
            settings.velocityTimeConstantSeconds, 0.040, 0.120);
        while (observations_.size() > 2 &&
               std::chrono::duration<double>(
                   observationTime - observations_[1].time).count() >= windowSeconds)
        {
            observations_.pop_front();
        }
        while (observations_.size() > 16)
            observations_.pop_front();

        const double duration = std::chrono::duration<double>(
            observations_.back().time - observations_.front().time).count();
        if (observations_.size() < 4 || duration < 0.025)
        {
            velocityX_ = velocityY_ = 0.0;
            accelerationX_ = accelerationY_ = 0.0;
            hasVelocity_ = false;
            return false;
        }

        // 对窗口内全部坐标做最小二乘直线拟合，避免单帧差分把框抖动放大成数百px/s。
        double meanTime = 0.0;
        double meanX = 0.0;
        double meanY = 0.0;
        for (const auto& observation : observations_)
        {
            meanTime += std::chrono::duration<double>(
                observation.time - observations_.front().time).count();
            meanX += observation.x;
            meanY += observation.y;
        }
        const double sampleCount = static_cast<double>(observations_.size());
        meanTime /= sampleCount;
        meanX /= sampleCount;
        meanY /= sampleCount;

        double timeVariance = 0.0;
        double covarianceX = 0.0;
        double covarianceY = 0.0;
        double pathLength = 0.0;
        for (size_t index = 0; index < observations_.size(); ++index)
        {
            const double relativeTime = std::chrono::duration<double>(
                observations_[index].time - observations_.front().time).count();
            const double centeredTime = relativeTime - meanTime;
            timeVariance += centeredTime * centeredTime;
            covarianceX += centeredTime * (observations_[index].x - meanX);
            covarianceY += centeredTime * (observations_[index].y - meanY);
            if (index > 0)
            {
                pathLength += std::hypot(
                    observations_[index].x - observations_[index - 1].x,
                    observations_[index].y - observations_[index - 1].y);
            }
        }
        if (timeVariance <= 1e-9)
            return false;

        double fittedVelocityX = covarianceX / timeVariance;
        double fittedVelocityY = covarianceY / timeVariance;
        clampVector(fittedVelocityX, fittedVelocityY, span * 6.0);

        // 明显单轴运动时吸附到主轴，避免横移把检测框高度抖动转换成垂直准星摆动。
        if (std::abs(fittedVelocityY) < std::abs(fittedVelocityX) * 0.35)
            fittedVelocityY = 0.0;
        else if (std::abs(fittedVelocityX) < std::abs(fittedVelocityY) * 0.35)
            fittedVelocityX = 0.0;

        const double netDisplacement = std::hypot(
            observations_.back().x - observations_.front().x,
            observations_.back().y - observations_.front().y);
        const double linearity = pathLength > 1e-9 ? netDisplacement / pathLength : 0.0;
        const double normalActivationDisplacement = std::max(3.0, span * 0.0125);
        // 自运动保持后的约4~6 px越心回弹在实测中仍可能形成稳定回归；
        // 重新武装阶段要求至少2.5%检测跨度的净位移，普通首次预测门槛保持不变。
        const double activationDisplacement = selfMotionRearmPending_
            ? std::max(normalActivationDisplacement, span * 0.025)
            : normalActivationDisplacement;
        const double activationSpeed = std::max(50.0, span * 0.20);
        const bool reliableMotion =
            netDisplacement >= activationDisplacement &&
            std::hypot(fittedVelocityX, fittedVelocityY) >= activationSpeed &&
            linearity >= 0.65;

        // 不可靠窗口只更新噪声诊断，不覆盖最后一次可靠控制速度，避免提前量单帧归零或翻向。
        if (hasVelocity_)
        {
            accelerationX_ = (fittedVelocityX - velocityX_) / dt;
            accelerationY_ = (fittedVelocityY - velocityY_) / dt;
            clampVector(accelerationX_, accelerationY_, span * 30.0);
        }
        else
        {
            accelerationX_ = accelerationY_ = 0.0;
        }
        if (reliableMotion)
        {
            velocityX_ = fittedVelocityX;
            velocityY_ = fittedVelocityY;
            hasVelocity_ = true;
        }
        return reliableMotion;
    }

    void updateDirection(double sampleVelocityX, double sampleVelocityY,
                         bool reliableMotion, double span)
    {
        const double sampleSpeed = std::hypot(sampleVelocityX, sampleVelocityY);
        const double activationSpeed = std::max(50.0, span * 0.20);

        if (sampleSpeed < activationSpeed * 0.50)
        {
            ++stationarySamples_;
            unreliableSamples_ = 0;
            pendingDirectionSamples_ = 0;
            // 一帧低速常由检测框量化造成；连续两帧才撤销，第三帧才释放方向锁定。
            if (!predictionEstablished_ || stationarySamples_ >= 2)
                suppressPrediction_ = true;
            if (stationarySamples_ >= 3)
            {
                directionLocked_ = false;
                predictionEstablished_ = false;
                reliableDirectionSamples_ = 0;
            }
            return;
        }
        stationarySamples_ = 0;

        if (!reliableMotion)
        {
            ++unreliableSamples_;
            pendingDirectionSamples_ = 0;
            // 已锁定后容忍最多两帧窗口退化，使用最后可靠速度维持连续提前。
            if (!directionLocked_ || !predictionEstablished_ || unreliableSamples_ >= 3)
                suppressPrediction_ = true;
            if (unreliableSamples_ >= 5)
            {
                directionLocked_ = false;
                predictionEstablished_ = false;
                reliableDirectionSamples_ = 0;
            }
            return;
        }
        unreliableSamples_ = 0;

        const double fittedSpeed = std::hypot(velocityX_, velocityY_);
        const double sampleDirectionX = velocityX_ / fittedSpeed;
        const double sampleDirectionY = velocityY_ / fittedSpeed;
        if (!directionLocked_)
        {
            accumulatePendingDirection(sampleDirectionX, sampleDirectionY);
            suppressPrediction_ = true;
            // 回归窗口本身已包含至少四帧，再确认两次即可形成至少五帧的连续运动证据。
            if (pendingDirectionSamples_ >= 2)
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
            // 反向必须由稳健回归窗口确认；单帧坐标差分不再直接关闭预测。
            suppressPrediction_ = true;
            predictionEstablished_ = false;
            reliableDirectionSamples_ = 0;
            accumulatePendingDirection(sampleDirectionX, sampleDirectionY);
            if (pendingDirectionSamples_ >= 2)
            {
                lockPendingDirection();
                suppressPrediction_ = false;
            }
            return;
        }

        pendingDirectionSamples_ = 0;
        suppressPrediction_ = alignment < 0.50;
        if (!suppressPrediction_)
        {
            ++reliableDirectionSamples_;
            if (reliableDirectionSamples_ >= 3)
                predictionEstablished_ = true;
            directionX_ = directionX_ * 0.85 + sampleDirectionX * 0.15;
            directionY_ = directionY_ * 0.85 + sampleDirectionY * 0.15;
            normalize(directionX_, directionY_);
        }
    }

    void accumulatePendingDirection(double x, double y)
    {
        if (pendingDirectionSamples_ == 0 ||
            x * pendingDirectionX_ + y * pendingDirectionY_ < 0.80)
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
        predictionEstablished_ = false;
        reliableDirectionSamples_ = 0;
        pendingDirectionSamples_ = 0;
        selfMotionRearmPending_ = false;
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
    bool directionLocked_ = false;
    bool suppressPrediction_ = false;
    bool predictionEstablished_ = false;
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
    int unreliableSamples_ = 0;
    int reliableDirectionSamples_ = 0;
    int selfMotionSuppressionFramesRemaining_ = 0; // 自运动伪迹命中后的剩余抑制观测帧数
    bool selfMotionRearmPending_ = false; // 保持结束后是否仍需更强净位移证据
    std::deque<Observation> observations_;
    TimePoint previousObservationTime_{};
};
