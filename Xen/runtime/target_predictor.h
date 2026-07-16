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
        bool oscillationSuppressed = false; // 高频往返模式仅关闭提前量，基础跟踪保持生效
        bool highSpeedSuppressed = false; // 超出已验证常速度范围时撤销不可信外推
        bool stationarySuppressed = false; // 连续低速达到停止确认时间后撤销提前量
        bool motionEvidenceSuppressed = false; // 建向、回归退化或反向确认期间是否撤销提前量
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

    // 高频往返模式不再追逐框内pivot；准星已在框内时目标坐标固定为准星中心，
    // 越界时只追到带少量内缩的最近边缘，避免在目标框左右两侧持续摆动。
    static double boxHoldCoordinate(
        double aimCenter, double boxStart, double boxLength, double inset = 2.0)
    {
        if (!std::isfinite(aimCenter) || !std::isfinite(boxStart) ||
            !std::isfinite(boxLength) || boxLength <= 0.0)
        {
            return aimCenter;
        }
        const double safeInset = std::clamp(inset, 0.0, boxLength * 0.5);
        return std::clamp(
            aimCenter, boxStart + safeInset, boxStart + boxLength - safeInset);
    }

    // 自运动伪迹通常不会只持续一个观测帧。门控命中后继续撤销四个后续观测，
    // 覆盖约100 FPS下40 ms的控制响应尾迹，同时不清空真实目标的稳健速度与方向状态。
    void applySelfMotionSuppression(Result& result, bool artifactDetected)
    {
        constexpr int kHoldFrames = 4;
        // static实测的伪预测只持续1~2帧；真实left/right一旦连续预测满5帧，
        // 屏幕收敛与自身视角同向是正常闭环跟随，不再允许自运动门控反复打断。
        // 持续运动豁免只能阻止“新保持”启动。若早期伪迹已经启动保持，内部回归仍可能
        // 在保持期间累计出持续运动锁存；此时提前返回会让保持中途穿透，并把未消费计数
        // 遗留到成熟预测阶段再次触发。已有保持必须连续执行到尾迹计数归零。
        const bool holdAlreadyActive = selfMotionSuppressionFramesRemaining_ > 0;
        if (!holdAlreadyActive && artifactDetected && sustainedMotionConfirmed_)
            return;

        if (artifactDetected)
            selfMotionSuppressionFramesRemaining_ = kHoldFrames;

        if (!artifactDetected && selfMotionSuppressionFramesRemaining_ <= 0)
            return;

        result.x -= result.offsetX;
        result.y -= result.offsetY;
        result.offsetX = 0.0;
        result.offsetY = 0.0;
        result.selfMotionSuppressed = true;
        resetPredictionDistanceSmoothing();

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
                  const Settings& settings,
                  double maxObservationGapSeconds = 0.10,
                  bool preserveMatureDirectionDuringCatchUp = false)
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
        const double maximumGap = std::clamp(
            maxObservationGapSeconds, 0.10, 0.35);
        if (!std::isfinite(dt) || dt <= 0.0 || dt > maximumGap)
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
            x, y, observationTime, dt, span, settings,
            maximumGap > 0.10, preserveMatureDirectionDuringCatchUp);
        updateDirection(
            sampleVelocityX, sampleVelocityY, reliableMotion, span, observationTime,
            preserveMatureDirectionDuringCatchUp);
        const bool oscillationSuppressed =
            observationTime < oscillationSuppressedUntil_;
        // 稳定高速直线仍符合常速度模型，不能仅因速度快而误杀；速度与加速度同时越界
        // 表示常速度假设可能失效。已持续预测且当前原始位移与稳健方向同向时，首帧即可
        // 确认是真实加速；单帧反向跳点、停止和刚重建方向仍保留连续三帧保护。
        const double fittedSpeed = std::hypot(velocityX_, velocityY_);
        const double sampleSpeed = std::hypot(sampleVelocityX, sampleVelocityY);
        const double sampleFitAlignment = fittedSpeed > 1e-9 && sampleSpeed > 1e-9
            ? (sampleVelocityX * velocityX_ + sampleVelocityY * velocityY_) /
                (sampleSpeed * fittedSpeed)
            : -1.0;
        // r28单向复测中，left/right在约462~474 px/s稳定速度下因回归加速度抖动
        // 反复命中旧400 px/s门槛；jump速度P95为1908~2103 px/s。速度门槛提高到
        // 检测跨度2倍/秒，隔离普通单向跟随，同时仍覆盖真正的快速横跳。
        const bool highSpeedTransient = !preserveMatureDirectionDuringCatchUp &&
            directionLocked_ &&
            fittedSpeed > span * 2.0 &&
            std::hypot(accelerationX_, accelerationY_) > span * 5.0;
        if (highSpeedTransient)
            ++highSpeedTransientSamples_;
        else
            highSpeedTransientSamples_ = 0;
        const bool confidentlyAccelerating = highSpeedTransient && reliableMotion &&
            predictionEstablished_ && continuousPredictionFrames_ > 0 &&
            sampleFitAlignment > 0.50;
        // 可信首帧命中后直接进入确认态；否则下一帧会因预测已撤销而失去
        // continuousPredictionFrames_，在三帧确认完成前重新释放一次提前量。
        if (confidentlyAccelerating)
            highSpeedTransientSamples_ = std::max(highSpeedTransientSamples_, 3);
        const bool highSpeedSuppressed = highSpeedTransientSamples_ >= 3;
        if (directionLocked_ && predictionEstablished_ &&
            !suppressPrediction_ && !oscillationSuppressed && !highSpeedSuppressed)
            ++continuousPredictionFrames_;
        else
            continuousPredictionFrames_ = 0;
        if (continuousPredictionFrames_ >= 3)
            sustainedMotionConfirmed_ = true;

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

        // 方向锁定只代表回归窗口初步同向；继续收到四次可靠方向后才释放提前量。
        // static实测的补偿残差通常仅维持1~3帧，会在输出前被停止或自运动门控撤销。
        if (!directionLocked_ || !predictionEstablished_ ||
            suppressPrediction_ || oscillationSuppressed || highSpeedSuppressed)
        {
            // 停止、反向和安全门控必须立即撤销提前量；不能让平滑状态在重新放行时
            // 带回上一段运动的距离，否则视觉上会形成一次方向正确但幅值陈旧的脉冲。
            resetPredictionDistanceSmoothing();
            Result result{
                x, y, velocityX_, velocityY_, accelerationX_, accelerationY_,
                leadSeconds, 0.0, 0.0, directionLocked_, true
            };
            result.oscillationSuppressed = oscillationSuppressed;
            result.highSpeedSuppressed = highSpeedSuppressed;
            result.stationarySuppressed = stationarySuppressed_;
            result.motionEvidenceSuppressed =
                !directionLocked_ || !predictionEstablished_ || suppressPrediction_;
            return result;
        }

        const double speedAlongDirection = std::max(
            0.0, velocityX_ * directionX_ + velocityY_ * directionY_);
        const double strength = std::clamp(settings.predictionStrength, 0.0, 4.0);
        // 方向刚建立或刚完成反转时，完整提前量会把短暂可靠窗口放大成左右脉冲。
        // 用七个有效预测帧渐进释放：约100 FPS下70 ms即可恢复全额持续提前，
        // 高频反转和跳跃的2~3帧短段只获得部分提前，不改变方向确认与失效规则。
        constexpr int kFullLeadReleaseFrames = 7;
        const double leadReleaseScale = std::min(
            1.0, continuousPredictionFrames_ /
                static_cast<double>(kFullLeadReleaseFrames));
        // r21将jump上限降至64 px后仍明显远离目标框；反事实回放显示24 px兼顾
        // 追赶收益与框内稳定。限制为检测跨度7.5%，普通left/right约20 px提前基本不受影响。
        const double maxPredictionDistance = std::max(12.0, span * 0.075);
        const double desiredPredictionDistance = std::clamp(
            speedAlongDirection * leadSeconds * strength * leadReleaseScale,
            0.0, maxPredictionDistance);
        if (desiredPredictionDistance <= 1e-9)
            resetPredictionDistanceSmoothing();

        // r41水平实测中预测方向没有翻转，但回归速度抖动使提前距离单帧变化P95达到
        // 3.8~6.5 px，约两成帧在24 px上限附近伸缩。只对已通过全部门控的距离做
        // 30 ms一阶低通：约三个100 FPS观测即可跟上真实速度变化，同时不平滑方向，
        // 也不延迟上方停止、反向、自运动、高速和高频往返的立即撤销。
        constexpr double kPredictionDistanceSmoothingSeconds = 0.030;
        double predictionDistance = 0.0;
        if (desiredPredictionDistance > 1e-9)
        {
            if (!predictionDistanceSmoothingInitialized_)
            {
                smoothedPredictionDistance_ = desiredPredictionDistance;
                predictionDistanceSmoothingInitialized_ = true;
            }
            else
            {
                const double alpha = 1.0 - std::exp(
                    -dt / kPredictionDistanceSmoothingSeconds);
                smoothedPredictionDistance_ +=
                    alpha * (desiredPredictionDistance - smoothedPredictionDistance_);
            }
            smoothedPredictionDistance_ = std::clamp(
                smoothedPredictionDistance_, 0.0, maxPredictionDistance);
            predictionDistance = smoothedPredictionDistance_;
        }
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
        stationarySince_ = {};
        stationarySuppressed_ = false;
        unreliableSamples_ = 0;
        reliableDirectionSamples_ = 0;
        selfMotionSuppressionFramesRemaining_ = 0;
        selfMotionRearmPending_ = false;
        continuousPredictionFrames_ = 0;
        sustainedMotionConfirmed_ = false;
        highSpeedTransientSamples_ = 0;
        sparseResumeFramesRemaining_ = 0;
        resetPredictionDistanceSmoothing();
        directionReversalTimes_.clear();
        oscillationSuppressedUntil_ = {};
        hasCommittedDirection_ = false;
        committedDirectionSince_ = {};
        committedDirectionX_ = committedDirectionY_ = 0.0;
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
        stationarySince_ = {};
        stationarySuppressed_ = false;
        unreliableSamples_ = 0;
        reliableDirectionSamples_ = 0;
        selfMotionRearmPending_ = true;
        continuousPredictionFrames_ = 0;
        sustainedMotionConfirmed_ = false;
        highSpeedTransientSamples_ = 0;
        sparseResumeFramesRemaining_ = 0;
        resetPredictionDistanceSmoothing();
        observations_.clear();
        if (initialized_)
            observations_.push_back({ previousX_, previousY_, previousObservationTime_ });
    }

    bool updateMotion(double x, double y, TimePoint observationTime,
                      double dt, double span, const Settings& settings,
                      bool allowSparseResume,
                      bool preserveMatureDirectionDuringCatchUp)
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
            // 短暂松开后窗口内暂时只有“松开前末帧+恢复首帧”。若端点速度仍与
            // 已成熟方向一致，保留最后可靠速度最多两个退化帧；后续四帧窗口会
            // 很快接管。方向不一致、停止或普通稀疏输入仍走原有冷启动清零。
            const double resumeVelocityX = (x - previousX_) / dt;
            const double resumeVelocityY = (y - previousY_) / dt;
            const double resumeSpeed = std::hypot(resumeVelocityX, resumeVelocityY);
            const double resumeAlignment = resumeSpeed > 1e-9
                ? (resumeVelocityX * directionX_ + resumeVelocityY * directionY_) /
                    resumeSpeed
                : -1.0;
            const double activationSpeed = std::max(50.0, span * 0.20);
            const bool startsSparseResume = allowSparseResume &&
                directionLocked_ && predictionEstablished_ && hasVelocity_ &&
                resumeSpeed >= activationSpeed * 0.50 && resumeAlignment >= 0.50;
            if (startsSparseResume)
            {
                // 跨暂停恢复后的回归窗口需要三次新观测才能重新达到四样本。调用方只会在
                // 首帧放宽最大间隔，因此由预测器内部锁存后续一帧，避免首帧恢复、次帧
                // 清零、第三帧再恢复的固定脉冲。锁存仅容忍量化静止或非反向退化；明确
                // 反向仍立即走冷启动，不能沿用松开前的旧方向。
                sparseResumeFramesRemaining_ = 2;
            }
            const bool clearlyReversed = resumeSpeed >= activationSpeed * 0.50 &&
                resumeAlignment < 0.0;
            if (sparseResumeFramesRemaining_ > 0 &&
                directionLocked_ && predictionEstablished_ && hasVelocity_ &&
                !clearlyReversed)
            {
                --sparseResumeFramesRemaining_;
                return false;
            }
            sparseResumeFramesRemaining_ = 0;
            velocityX_ = velocityY_ = 0.0;
            accelerationX_ = accelerationY_ = 0.0;
            hasVelocity_ = false;
            return false;
        }

        // 对窗口内全部坐标做最小二乘直线拟合，避免单帧差分把框抖动放大成数百px/s。
        // 四样本回归已经接管速度估计，跨暂停锁存不再参与后续普通观测。
        sparseResumeFramesRemaining_ = 0;
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

        // 有界追赶保护期内，补偿回归若突然与成熟方向相反，不得用它覆盖最后可信
        // 速度。调用方已经确认这是同目标、同方向短恢复；保护期外仍按普通反向处理。
        const double fittedSpeedForAlignment =
            std::hypot(fittedVelocityX, fittedVelocityY);
        const double fittedDirectionAlignment = fittedSpeedForAlignment > 1e-9
            ? (fittedVelocityX * directionX_ + fittedVelocityY * directionY_) /
                fittedSpeedForAlignment
            : 1.0;
        if (reliableMotion && preserveMatureDirectionDuringCatchUp &&
            sustainedMotionConfirmed_ && directionLocked_ && predictionEstablished_ &&
            fittedDirectionAlignment < 0.0)
        {
            return false;
        }

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
                         bool reliableMotion, double span, TimePoint observationTime,
                         bool preserveMatureDirectionDuringCatchUp)
    {
        const double sampleSpeed = std::hypot(sampleVelocityX, sampleVelocityY);
        const double activationSpeed = std::max(50.0, span * 0.20);

        if (sampleSpeed < activationSpeed * 0.50)
        {
            unreliableSamples_ = 0;
            pendingDirectionSamples_ = 0;
            if (stationarySince_.time_since_epoch().count() == 0)
                stationarySince_ = observationTime;
            const double stationarySeconds = std::chrono::duration<double>(
                observationTime - stationarySince_).count();
            // r42实机持续左移时，检测量化与视角补偿会产生约20 ms的平台；稳健回归仍有
            // 170~275 px/s同向速度，旧两帧门控却硬撤销提前量并触发重新释放。成熟运动改用
            // 真实时间确认：短平台保持连续，连续低速30 ms仍立即撤销，50 ms后释放方向锁。
            constexpr double kStationarySuppressionSeconds = 0.030;
            constexpr double kStationaryUnlockSeconds = 0.050;
            if (stationarySeconds >= kStationarySuppressionSeconds)
            {
                suppressPrediction_ = true;
                stationarySuppressed_ = true;
                sustainedMotionConfirmed_ = false;
            }
            if (stationarySeconds >= kStationaryUnlockSeconds)
            {
                directionLocked_ = false;
                predictionEstablished_ = false;
                reliableDirectionSamples_ = 0;
            }
            return;
        }
        stationarySince_ = {};
        stationarySuppressed_ = false;

        if (!reliableMotion)
        {
            if (preserveMatureDirectionDuringCatchUp &&
                sustainedMotionConfirmed_ && directionLocked_ && predictionEstablished_)
            {
                unreliableSamples_ = 0;
                pendingDirectionSamples_ = 0;
                suppressPrediction_ = false;
                return;
            }
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
                sustainedMotionConfirmed_ = false;
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
                lockPendingDirection(observationTime, span);
                suppressPrediction_ = false;
            }
            return;
        }

        const double alignment =
            sampleDirectionX * directionX_ + sampleDirectionY * directionY_;
        if (alignment < -0.25)
        {
            // 同方向短恢复的快速追赶会在相机响应尾迹中制造单帧数千px/s的补偿反向，
            // 而成熟回归速度仍保持原方向。仅在调用方给出的有界保护期内保留已经
            // 持续确认的方向；静止分支仍在上方生效，保护期结束后真实反向照常首帧
            // 撤销并走三次可靠回归确认。
            if (preserveMatureDirectionDuringCatchUp &&
                sustainedMotionConfirmed_ && predictionEstablished_)
            {
                pendingDirectionSamples_ = 0;
                suppressPrediction_ = false;
                return;
            }
            // 反向必须由稳健回归窗口确认；单帧坐标差分不再直接关闭预测。
            suppressPrediction_ = true;
            predictionEstablished_ = false;
            reliableDirectionSamples_ = 0;
            accumulatePendingDirection(sampleDirectionX, sampleDirectionY);
            // 已锁定方向的反转比初次建向更敏感；连续三次稳健回归同向后才换边，
            // 避免 left/right 的短时闭环回弹在 30~70 ms 内反复重建预测侧。
            if (pendingDirectionSamples_ >= 3)
            {
                lockPendingDirection(observationTime, span);
                suppressPrediction_ = false;
            }
            return;
        }

        pendingDirectionSamples_ = 0;
        suppressPrediction_ = alignment < 0.50;
        if (!suppressPrediction_)
        {
            ++reliableDirectionSamples_;
            if (reliableDirectionSamples_ >= 4)
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

    void lockPendingDirection(TimePoint observationTime, double span)
    {
        constexpr double kMinimumCommittedDirectionSeconds = 0.080;
        const double committedAlignment = hasCommittedDirection_
            ? pendingDirectionX_ * committedDirectionX_ +
                pendingDirectionY_ * committedDirectionY_
            : 1.0;
        const double committedDirectionSeconds = hasCommittedDirection_
            ? std::chrono::duration<double>(
                observationTime - committedDirectionSince_).count()
            : 0.0;
        if (hasCommittedDirection_ && committedAlignment < -0.25 &&
            committedDirectionSeconds >= kMinimumCommittedDirectionSeconds &&
            std::hypot(velocityX_, velocityY_) <= span)
        {
            // reverse实测常先因低速或窗口退化解锁，再从相反方向重新建立。
            // r29 right出现43~49 ms短反向及恢复，连续记为两次换向并误触抑制；
            // 新方向至少保持80 ms才允许再次计数，真实reverse每段持续时间远高于该值。
            recordDirectionReversal(observationTime);
        }
        if (!hasCommittedDirection_ || committedAlignment < 0.50)
        {
            committedDirectionSince_ = observationTime;
            if (hasCommittedDirection_)
                sustainedMotionConfirmed_ = false;
        }
        directionX_ = pendingDirectionX_;
        directionY_ = pendingDirectionY_;
        normalize(directionX_, directionY_);
        committedDirectionX_ = directionX_;
        committedDirectionY_ = directionY_;
        hasCommittedDirection_ = true;
        directionLocked_ = true;
        predictionEstablished_ = false;
        reliableDirectionSamples_ = 0;
        pendingDirectionSamples_ = 0;
        selfMotionRearmPending_ = false;
    }

    void recordDirectionReversal(TimePoint observationTime)
    {
        constexpr double kOscillationWindowSeconds = 1.5;
        directionReversalTimes_.push_back(observationTime);
        while (!directionReversalTimes_.empty() &&
               std::chrono::duration<double>(
                   observationTime - directionReversalTimes_.front()).count() >
                   kOscillationWindowSeconds)
        {
            directionReversalTimes_.pop_front();
        }
        // r21实测reverse有38组三次换向不超过1.5秒，jump最短为1.847秒。
        // 命中后每次新反转都延长保持；停止高频往返1.5秒后自动恢复普通预测。
        if (directionReversalTimes_.size() >= 3)
        {
            oscillationSuppressedUntil_ = observationTime +
                std::chrono::duration_cast<TimePoint::duration>(
                    std::chrono::duration<double>(kOscillationWindowSeconds));
        }
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

    void resetPredictionDistanceSmoothing()
    {
        smoothedPredictionDistance_ = 0.0;
        predictionDistanceSmoothingInitialized_ = false;
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
    TimePoint stationarySince_{}; // 当前连续低速证据起点；使用真实时间避免帧率改变停止语义
    bool stationarySuppressed_ = false; // 是否已达到30 ms停止确认并撤销提前量
    int unreliableSamples_ = 0;
    int reliableDirectionSamples_ = 0;
    int selfMotionSuppressionFramesRemaining_ = 0; // 自运动伪迹命中后的剩余抑制观测帧数
    bool selfMotionRearmPending_ = false; // 保持结束后是否仍需更强净位移证据
    int continuousPredictionFrames_ = 0; // 当前方向连续有效预测帧数，用于识别真实持续运动
    bool sustainedMotionConfirmed_ = false; // 持续运动锁存；短暂回归退化不得重新开放自运动门控
    std::deque<TimePoint> directionReversalTimes_; // 最近可靠换向时间，用于识别高频小幅往返
    TimePoint oscillationSuppressedUntil_{}; // 高频往返停止后的自动恢复时刻
    bool hasCommittedDirection_ = false; // 是否存在跨解锁保留的最后可靠方向
    TimePoint committedDirectionSince_{}; // 当前可靠方向的建立时刻，用于过滤短回摆及其恢复
    double committedDirectionX_ = 0.0;
    double committedDirectionY_ = 0.0;
    int highSpeedTransientSamples_ = 0; // 不确定高速瞬态仍需三帧确认，可信同向加速可首帧门控
    double smoothedPredictionDistance_ = 0.0; // 已放行预测提前距离的一阶平滑状态，px
    int sparseResumeFramesRemaining_ = 0; // 短暂停恢复后等待四样本回归接管的剩余退化帧数
    bool predictionDistanceSmoothingInitialized_ = false;
    std::deque<Observation> observations_;
    TimePoint previousObservationTime_{};
};
