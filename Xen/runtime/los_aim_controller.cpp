#include "los_aim_controller.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr double kMinimumDegreesPerCount = 1e-9;

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

    // 基础 P 只纠正控制时刻的角误差；相对视线角速度不允许混入误差 D 项。
    output.feedbackCountsX =
        input.errorDegreesX * responseFraction / input.degreesPerCountX;
    output.feedbackCountsY =
        input.errorDegreesY * responseFraction / input.degreesPerCountY;

    // 前馈表示本周期为维持目标角速度所需的相机位移。NIS/检测置信度生成的
    // confidence 只缩放前馈和提前参考，低可信时 P 反馈仍可把准星拉回目标。
    const double feedforwardGain =
        clampFinite(settings.feedforwardGain, 0.0, 2.0, 0.0);
    output.trackingFeedforwardCountsX =
        input.relativeLosRateDegreesPerSecondX * dt * feedforwardGain * confidence /
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
        output.leadReferenceDegreesY * responseFraction / input.degreesPerCountY;

    const double integralTime = settings.integralTimeSeconds > 0.0
        ? clampFinite(settings.integralTimeSeconds, 0.050, 2.0, 0.0) : 0.0;
    const double integralZone = std::max(0.0, settings.integralZoneDegrees);
    double candidateIntegralX = integralErrorDegreeSecondsX_;
    double candidateIntegralY = integralErrorDegreeSecondsY_;
    if (integralTime <= 0.0 || integralZone <= 0.0)
    {
        reset();
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

    output.valid = true;
    return output;
}

void LosAimController::reset()
{
    integralErrorDegreeSecondsX_ = 0.0;
    integralErrorDegreeSecondsY_ = 0.0;
}
