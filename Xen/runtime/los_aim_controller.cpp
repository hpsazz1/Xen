#include "los_aim_controller.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr double kMinimumDegreesPerCount = 1e-9;
constexpr double kVerticalCatchUpMinimumErrorRatio = 0.15;
constexpr double kVerticalCatchUpMinimumHorizontalRateDps = 5.0;

double clampFinite(double value, double minimum, double maximum, double fallback)
{
    return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
}
}

LosAimController::Output LosAimController::update(
    const Input& input, const Settings& settings)
{
    Output output;
    if (!input.valid || !std::isfinite(input.degreesPerCountX) ||
        !std::isfinite(input.degreesPerCountY) ||
        std::abs(input.degreesPerCountX) < kMinimumDegreesPerCount ||
        std::abs(input.degreesPerCountY) < kMinimumDegreesPerCount)
    {
        reset();
        return output;
    }

    const double dt = clampFinite(input.dtSeconds, 1.0 / 500.0, 0.050, 1.0 / 120.0);
    const double response = clampFinite(settings.responseSeconds, 0.010, 0.500, 0.080);
    const double maxCountsPerSecond =
        clampFinite(settings.maxCountsPerSecond, 1.0, 100000.0, 1440.0);
    const double confidence =
        clampFinite(input.feedforwardConfidence, 0.0, 1.0, 0.0);
    const double responseFraction = 1.0 - std::exp(-dt / response);
    const double verticalCatchUpError = clampFinite(
        settings.verticalCatchUpErrorDegrees, 0.0, 20.0, 0.0);
    double effectiveResponseY = response;
    if (verticalCatchUpError > 0.0 &&
        std::abs(input.errorDegreesY) >= verticalCatchUpError &&
        std::abs(input.errorDegreesY) >=
            std::abs(input.errorDegreesX) * kVerticalCatchUpMinimumErrorRatio &&
        std::abs(input.relativeLosRateDegreesPerSecondX) >=
            kVerticalCatchUpMinimumHorizontalRateDps)
    {
        output.verticalCatchUpActive = true;
        // 角度域大误差追赶沿用r30已经验证过的连续响应曲线：从阈值处的0.5倍
        // 平滑缩短到两倍阈值处的0.375倍，避免硬切增益，同时让二维限速下的Y轴
        // 获得与其角误差相称的预算。阈值为0时完全保持原控制语义。
        const double progress = std::clamp(
            (std::abs(input.errorDegreesY) - verticalCatchUpError) /
                verticalCatchUpError,
            0.0, 1.0);
        effectiveResponseY = response * (0.5 - progress * 0.125);
    }
    const double responseFractionY = 1.0 - std::exp(-dt / effectiveResponseY);
    output.effectiveResponseSecondsY = effectiveResponseY;

    // 静止锁存同时要求二维角误差和相对LOS速率安静，避免把正在穿越准星的
    // 真实目标仅因瞬时误差很小而停住。退出阈值固定为进入阈值的1.5倍，
    // 形成Schmitt回差；连续两个观测才进入，但越过退出阈值时当帧恢复。
    const double settleError = clampFinite(
        settings.settleErrorDegrees, 0.0, 1.0, 0.0);
    const double settleRate = clampFinite(
        settings.settleRateDegreesPerSecond, 0.0, 20.0, 0.0);
    const bool settleEnabled = settleError > 0.0 && settleRate > 0.0;
    const double errorMagnitude = std::hypot(input.errorDegreesX, input.errorDegreesY);
    const double rateMagnitude = std::hypot(
        input.relativeLosRateDegreesPerSecondX,
        input.relativeLosRateDegreesPerSecondY);
    if (!settleEnabled)
    {
        settled_ = false;
        quietSamples_ = 0;
    }
    else if (settled_)
    {
        if (errorMagnitude > settleError * 1.5 || rateMagnitude > settleRate * 1.5)
        {
            settled_ = false;
            quietSamples_ = 0;
            output.settleReleased = true;
        }
    }
    else if (errorMagnitude <= settleError && rateMagnitude <= settleRate)
    {
        quietSamples_ = std::min(quietSamples_ + 1, 2);
        settled_ = quietSamples_ >= 2;
    }
    else
    {
        quietSamples_ = 0;
    }
    output.settled = settled_;
    output.settleConfirmationSamples = quietSamples_;
    if (settled_)
    {
        // 静止期间所有控制分量和积分历史都归零；以后启用前馈或积分时也不能
        // 通过隐藏分量继续形成亚count反向脉冲。
        integralErrorDegreeSecondsX_ = 0.0;
        integralErrorDegreeSecondsY_ = 0.0;
        pendingReverseX_ = 0.0;
        pendingReverseY_ = 0.0;
        pendingReverseSeconds_ = 0.0;
        acceptedMovingRateSignX_ = 0;
        reversalFeedforwardRemainingSeconds_ = 0.0;
        output.valid = true;
        output.frameCountLimit = maxCountsPerSecond * dt;
        return output;
    }

    // 基础 P 只纠正控制时刻的角误差；相对视线角速度不允许混入误差 D 项。
    output.feedbackCountsX =
        input.errorDegreesX * responseFraction / input.degreesPerCountX;
    output.feedbackCountsY =
        input.errorDegreesY * responseFractionY / input.degreesPerCountY;

    // 前馈表示本周期为维持目标角速度所需的相机位移。NIS/检测置信度生成的
    // confidence 只缩放前馈和提前参考，低可信时 P 反馈仍可把准星拉回目标。
    const double feedforwardGain =
        clampFinite(settings.feedforwardGain, 0.0, 2.0, 0.0);
    const double reversalFeedforwardBoost =
        clampFinite(settings.reversalFeedforwardBoost, 0.0, 2.0, 0.0);
    const double reversalFeedforwardSeconds =
        clampFinite(settings.reversalFeedforwardSeconds, 0.0, 0.500, 0.0);
    double effectiveFeedforwardGainX = feedforwardGain;
    if (reversalFeedforwardBoost > 0.0 && reversalFeedforwardSeconds > 0.0 &&
        settleRate > 0.0)
    {
        // 仅把超过静止速率边界的符号变化视为真实换向；低速估计抖动不会刷新方向。
        // 事件窗口按真实时间递减，使 60/94/144 FPS 下的额外前馈持续时间一致。
        const double movingRateThreshold = settleRate;
        const double rateX = input.relativeLosRateDegreesPerSecondX;
        const int movingRateSignX = std::abs(rateX) >= movingRateThreshold
            ? ((rateX > 0.0) - (rateX < 0.0)) : 0;
        if (movingRateSignX != 0)
        {
            if (acceptedMovingRateSignX_ != 0 &&
                movingRateSignX != acceptedMovingRateSignX_)
            {
                reversalFeedforwardRemainingSeconds_ = reversalFeedforwardSeconds;
                output.reversalDetected = true;
            }
            acceptedMovingRateSignX_ = movingRateSignX;
        }
        if (reversalFeedforwardRemainingSeconds_ > 0.0)
        {
            output.reversalFeedforwardActive = true;
            effectiveFeedforwardGainX = std::min(
                2.0, feedforwardGain + reversalFeedforwardBoost);
            reversalFeedforwardRemainingSeconds_ = std::max(
                0.0, reversalFeedforwardRemainingSeconds_ - dt);
        }
    }
    else
    {
        acceptedMovingRateSignX_ = 0;
        reversalFeedforwardRemainingSeconds_ = 0.0;
    }
    output.effectiveFeedforwardGainX = effectiveFeedforwardGainX;
    output.trackingFeedforwardCountsX =
        input.relativeLosRateDegreesPerSecondX * dt * effectiveFeedforwardGainX * confidence /
        input.degreesPerCountX;
    output.trackingFeedforwardCountsY =
        input.relativeLosRateDegreesPerSecondY * dt * feedforwardGain * confidence /
        input.degreesPerCountY;

    const double leadHorizon =
        clampFinite(settings.leadHorizonSeconds, 0.0, 0.250, 0.0);
    const double leadStrength = clampFinite(settings.leadStrength, 0.0, 4.0, 0.0);
    output.leadReferenceDegreesX =
        input.relativeLosRateDegreesPerSecondX * leadHorizon * leadStrength * confidence;
    output.leadReferenceDegreesY =
        input.relativeLosRateDegreesPerSecondY * leadHorizon * leadStrength * confidence;
    output.leadCountsX =
        output.leadReferenceDegreesX * responseFraction / input.degreesPerCountX;
    output.leadCountsY =
        output.leadReferenceDegreesY * responseFractionY / input.degreesPerCountY;

    const double integralTime = settings.integralTimeSeconds > 0.0
        ? clampFinite(settings.integralTimeSeconds, 0.050, 2.0, 0.0) : 0.0;
    const double integralZone = std::max(0.0, settings.integralZoneDegrees);
    double candidateIntegralX = integralErrorDegreeSecondsX_;
    double candidateIntegralY = integralErrorDegreeSecondsY_;
    if (integralTime <= 0.0 || integralZone <= 0.0)
    {
        // 积分关闭只清积分历史，不能连带清除独立的静止回差状态。
        integralErrorDegreeSecondsX_ = 0.0;
        integralErrorDegreeSecondsY_ = 0.0;
    }
    else
    {
        // I-zone 外不保留历史积分；误差跨过零点时也立即解卷绕，避免反转后沿旧方向拖拽。
        if (std::abs(input.errorDegreesX) > integralZone ||
            input.errorDegreesX * candidateIntegralX < 0.0)
            candidateIntegralX = 0.0;
        else
            candidateIntegralX += input.errorDegreesX * dt;
        if (std::abs(input.errorDegreesY) > integralZone ||
            input.errorDegreesY * candidateIntegralY < 0.0)
            candidateIntegralY = 0.0;
        else
            candidateIntegralY += input.errorDegreesY * dt;

        const double integralRatio =
            clampFinite(settings.integralOutputRatio, 0.0, 0.5, 0.25);
        const double integralCountsLimit = maxCountsPerSecond * dt * integralRatio;
        output.integralCountsX = candidateIntegralX * dt /
            (response * integralTime * input.degreesPerCountX);
        output.integralCountsY = candidateIntegralY * dt /
            (response * integralTime * input.degreesPerCountY);
        const double integralMagnitude =
            std::hypot(output.integralCountsX, output.integralCountsY);
        if (integralMagnitude > integralCountsLimit && integralMagnitude > 0.0)
        {
            const double scale = integralCountsLimit / integralMagnitude;
            output.integralCountsX *= scale;
            output.integralCountsY *= scale;
            // 将状态同步到已限幅的积分输出，防止内部保存不可执行的隐性 wind-up。
            candidateIntegralX = output.integralCountsX * response * integralTime *
                input.degreesPerCountX / dt;
            candidateIntegralY = output.integralCountsY * response * integralTime *
                input.degreesPerCountY / dt;
        }
    }

    output.unlimitedCountsX = output.feedbackCountsX +
        output.trackingFeedforwardCountsX + output.leadCountsX + output.integralCountsX;
    output.unlimitedCountsY = output.feedbackCountsY +
        output.trackingFeedforwardCountsY + output.leadCountsY + output.integralCountsY;
    output.limitedCountsX = output.unlimitedCountsX;
    output.limitedCountsY = output.unlimitedCountsY;
    output.frameCountLimit = maxCountsPerSecond * dt;
    const double requestedMagnitude =
        std::hypot(output.unlimitedCountsX, output.unlimitedCountsY);
    if (requestedMagnitude > output.frameCountLimit && requestedMagnitude > 0.0)
    {
        output.speedLimited = true;
        output.integralFrozen = integralTime > 0.0;
        const double scale = output.frameCountLimit / requestedMagnitude;
        output.limitedCountsX *= scale;
        output.limitedCountsY *= scale;
        // 总输出饱和时不提交本周期积分候选，但保留此前可执行状态。
    }
    else if (integralTime > 0.0)
    {
        integralErrorDegreeSecondsX_ = candidateIntegralX;
        integralErrorDegreeSecondsY_ = candidateIntegralY;
    }

    // 静止释放后的低速反向脉冲必须在同一二维方向持续一段真实时间，防止
    // 检测噪声或量化余量让两个孤立修正被设备执行成正反抖动。真实相对LOS
    // 速率一旦达到静止判定阈值就立即放行，避免给真实reverse/jump增加固定等待；
    // 确认窗只处理静止误差回差区与速率阈值以内、最容易由观测噪声触发的低速反向请求；
    // 已接受方向也只在该区域更新，区外的大幅运动不能覆盖下一次近中心判断的参照方向。
    const double reverseConfirmation = clampFinite(
        settings.reverseConfirmationSeconds, 0.0, 0.250, 0.0);
    const double limitedMagnitude = std::hypot(
        output.limitedCountsX, output.limitedCountsY);
    if (reverseConfirmation > 0.0 && settleError > 0.0 && settleRate > 0.0 &&
        errorMagnitude <= settleError * 1.5 && limitedMagnitude > 1e-12)
    {
        const double directionDot =
            output.limitedCountsX * acceptedSettleBandDirectionX_ +
            output.limitedCountsY * acceptedSettleBandDirectionY_;
        const bool lowSpeedReverse = hasAcceptedSettleBandDirection_ &&
            directionDot < 0.0 && rateMagnitude < settleRate;
        if (lowSpeedReverse)
        {
            const bool samePendingDirection = pendingReverseSeconds_ > 0.0 &&
                output.limitedCountsX * pendingReverseX_ +
                    output.limitedCountsY * pendingReverseY_ > 0.0;
            if (!samePendingDirection)
                pendingReverseSeconds_ = 0.0;
            pendingReverseX_ = output.limitedCountsX;
            pendingReverseY_ = output.limitedCountsY;
            pendingReverseSeconds_ += dt;
            output.reverseConfirmationSeconds = pendingReverseSeconds_;
            if (pendingReverseSeconds_ + 1e-12 < reverseConfirmation)
            {
                output.lowSpeedReverseSuppressed = true;
                output.limitedCountsX = 0.0;
                output.limitedCountsY = 0.0;
            }
        }
        else
        {
            pendingReverseSeconds_ = 0.0;
        }
        if (!output.lowSpeedReverseSuppressed)
        {
            hasAcceptedSettleBandDirection_ = true;
            acceptedSettleBandDirectionX_ = output.limitedCountsX;
            acceptedSettleBandDirectionY_ = output.limitedCountsY;
            pendingReverseX_ = 0.0;
            pendingReverseY_ = 0.0;
            pendingReverseSeconds_ = 0.0;
        }
    }

    output.valid = true;
    return output;
}

void LosAimController::reset()
{
    settled_ = false;
    quietSamples_ = 0;
    hasAcceptedSettleBandDirection_ = false;
    acceptedSettleBandDirectionX_ = 0.0;
    acceptedSettleBandDirectionY_ = 0.0;
    pendingReverseX_ = 0.0;
    pendingReverseY_ = 0.0;
    pendingReverseSeconds_ = 0.0;
    acceptedMovingRateSignX_ = 0;
    reversalFeedforwardRemainingSeconds_ = 0.0;
    integralErrorDegreeSecondsX_ = 0.0;
    integralErrorDegreeSecondsY_ = 0.0;
}
