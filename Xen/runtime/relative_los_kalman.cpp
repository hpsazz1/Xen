#include "relative_los_kalman.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr double kMinimumDtSeconds = 1e-4;
constexpr double kMaximumDtSeconds = 0.5;
constexpr double kInitialAngleVariance = 4.0;
constexpr double kInitialRateVariance = 400.0;
constexpr double kMeasurementStdDegrees = 0.15;
}

void RelativeLosKalman::reset()
{
    horizontal_ = {};
    vertical_ = {};
    estimate_ = {};
}

void RelativeLosKalman::update(double angleX, double angleY, float confidence,
    TimePoint observationTime, TimePoint controlTime)
{
    update(angleX, angleY, confidence, observationTime, controlTime, Settings{});
}

void RelativeLosKalman::update(double angleX, double angleY, float confidence,
    TimePoint observationTime, TimePoint controlTime, const Settings& settings)
{
    if (observationTime.time_since_epoch().count() == 0 ||
        controlTime.time_since_epoch().count() == 0 ||
        !std::isfinite(angleX) || !std::isfinite(angleY))
    {
        estimate_ = {};
        return;
    }

    const float boundedConfidence = std::isfinite(confidence)
        ? std::clamp(confidence, 0.0f, 1.0f) : 0.0f;
    const double variance = measurementVariance(boundedConfidence);
    AxisState* states[] = { &horizontal_, &vertical_ };
    const double measurements[] = { angleX, angleY };
    for (int index = 0; index < 2; ++index)
    {
        AxisState& state = *states[index];
        if (!state.initialized)
        {
            state.initialized = true;
            state.time = observationTime;
            state.angle = measurements[index];
            state.rate = 0.0;
            state.p00 = kInitialAngleVariance;
            state.p01 = 0.0;
            state.p11 = kInitialRateVariance;
        }
        else
        {
            predict(state, boundedDtSeconds(state.time, observationTime),
                accelerationStd(state, settings));
            state.time = observationTime;
        }
        correct(state, measurements[index], variance);
    }

    // 持久状态必须停留在最新观测时刻。控制时刻通常晚于观测，而下一帧观测又可能
    // 早于上一次控制时刻；若把持久状态推进到controlTime，下一帧就会用过去测量校正
    // 未来状态并再次外推同一段延迟。这里只复制后验状态用于控制投影，避免时间倒流。
    AxisState projectedHorizontal = horizontal_;
    AxisState projectedVertical = vertical_;
    predict(projectedHorizontal,
        boundedDtSeconds(projectedHorizontal.time, controlTime),
        accelerationStd(horizontal_, settings));
    predict(projectedVertical,
        boundedDtSeconds(projectedVertical.time, controlTime),
        accelerationStd(vertical_, settings));
    projectedHorizontal.time = controlTime;
    projectedVertical.time = controlTime;
    estimate_.x = toEstimate(projectedHorizontal);
    estimate_.y = toEstimate(projectedVertical);
    estimate_.measurementConfidence = boundedConfidence;
    const double normalizedNis = std::max(
        horizontal_.diagnostic.nis, vertical_.diagnostic.nis);
    estimate_.feedforwardConfidence = boundedConfidence * boundedConfidence *
        std::exp(-0.5 * std::min(normalizedNis, 20.0));
}

double RelativeLosKalman::boundedDtSeconds(TimePoint start, TimePoint end)
{
    if (start.time_since_epoch().count() == 0 || end.time_since_epoch().count() == 0)
        return 0.0;
    const double seconds = std::chrono::duration<double>(end - start).count();
    if (!std::isfinite(seconds) || seconds <= 0.0)
        return 0.0;
    return std::clamp(seconds, kMinimumDtSeconds, kMaximumDtSeconds);
}

double RelativeLosKalman::measurementVariance(float confidence)
{
    const double confidenceScale = std::clamp(static_cast<double>(confidence), 0.05, 1.0);
    const double standardDeviation = kMeasurementStdDegrees / confidenceScale;
    return standardDeviation * standardDeviation;
}

double RelativeLosKalman::accelerationStd(const AxisState& state,
    const Settings& settings)
{
    const Settings defaults{};
    const double stationary = std::isfinite(
        settings.accelerationStdDegreesPerSecond2)
        ? std::clamp(settings.accelerationStdDegreesPerSecond2, 1.0, 2000.0)
        : defaults.accelerationStdDegreesPerSecond2;
    const double moving = std::isfinite(
        settings.movingAccelerationStdDegreesPerSecond2)
        ? std::clamp(settings.movingAccelerationStdDegreesPerSecond2, 1.0, 2000.0)
        : defaults.movingAccelerationStdDegreesPerSecond2;
    const double threshold = std::isfinite(
        settings.movingRateThresholdDegreesPerSecond)
        ? std::clamp(settings.movingRateThresholdDegreesPerSecond, 0.1, 1000.0)
        : defaults.movingRateThresholdDegreesPerSecond;
    // 速度比值作为连续混合权重，避免在阈值附近反复切换过程噪声。
    const double movingWeight = std::clamp(std::abs(state.rate) / threshold,
        0.0, 1.0);
    return stationary + (moving - stationary) * movingWeight;
}

void RelativeLosKalman::predict(AxisState& state, double dt,
    double accelerationStdDegreesPerSecond2)
{
    if (!state.initialized || dt <= 0.0)
        return;
    const double dt2 = dt * dt;
    const double dt3 = dt2 * dt;
    const double dt4 = dt2 * dt2;
    const double q = accelerationStdDegreesPerSecond2 *
        accelerationStdDegreesPerSecond2;
    state.angle += state.rate * dt;
    state.p00 += 2.0 * dt * state.p01 + dt2 * state.p11 + q * dt4 * 0.25;
    state.p01 += dt * state.p11 + q * dt3 * 0.5;
    state.p11 += q * dt2;
}

void RelativeLosKalman::correct(AxisState& state, double measurement, double variance)
{
    const double innovation = measurement - state.angle;
    const double innovationVariance = std::max(state.p00 + variance, 1e-9);
    const double gainAngle = state.p00 / innovationVariance;
    const double gainRate = state.p01 / innovationVariance;
    state.angle += gainAngle * innovation;
    state.rate += gainRate * innovation;
    const double oldP00 = state.p00;
    const double oldP01 = state.p01;
    state.p00 = std::max((1.0 - gainAngle) * oldP00, 1e-12);
    state.p01 = (1.0 - gainAngle) * oldP01;
    state.p11 = std::max(state.p11 - gainRate * oldP01, 1e-12);
    state.diagnostic.innovationDegrees = innovation;
    state.diagnostic.innovationVariance = innovationVariance;
    state.diagnostic.nis = innovation * innovation / innovationVariance;
}

RelativeLosKalmanAxisEstimate RelativeLosKalman::toEstimate(const AxisState& state)
{
    RelativeLosKalmanAxisEstimate result = state.diagnostic;
    result.valid = state.initialized;
    result.angleDegrees = state.angle;
    result.rateDegreesPerSecond = state.rate;
    result.angleVariance = std::max(state.p00, 0.0);
    return result;
}
