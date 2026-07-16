#include "maneuver_los_estimator.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr double kMinimumDtSeconds = 1e-4;
constexpr double kMaximumDtSeconds = 0.5;
constexpr double kMeasurementStdDegrees = 0.15;
}

void ManeuverLosEstimator::reset()
{
    constantVelocity_.reset();
    horizontalAcceleration_ = {};
    verticalAcceleration_ = {};
    constantAccelerationEstimate_ = {};
    selected_ = {};
    diagnostics_ = {};
    lastObservationTime_ = {};
    previousManeuverModelActive_ = false;
}

void ManeuverLosEstimator::update(double angleX, double angleY, float confidence,
    TimePoint observationTime, TimePoint controlTime, const Settings& settings)
{
    constantVelocity_.update(angleX, angleY, confidence,
        observationTime, controlTime, settings.constantVelocitySettings);
    const bool needsAccelerationModel =
        settings.mode != ManeuverLosEstimatorMode::ConstantVelocity;
    if (needsAccelerationModel)
    {
        updateConstantAcceleration(angleX, angleY, confidence,
            observationTime, controlTime, settings.jerkStdDegreesPerSecond3);
    }
    else
    {
        constantAccelerationEstimate_ = {};
    }

    const auto& velocity = constantVelocity_.estimate();
    const auto& acceleration = constantAccelerationEstimate_;
    const double elapsed = boundedDtSeconds(lastObservationTime_, observationTime);
    lastObservationTime_ = observationTime;

    bool active = settings.mode == ManeuverLosEstimatorMode::ConstantAcceleration;
    if (settings.mode == ManeuverLosEstimatorMode::ManeuverGatedConstantAcceleration)
    {
        const double threshold = std::isfinite(
            settings.maneuverRateThresholdDegreesPerSecond)
            ? std::clamp(settings.maneuverRateThresholdDegreesPerSecond, 0.1, 1000.0)
            : 12.0;
        const double hold = std::isfinite(settings.maneuverHoldSeconds)
            ? std::clamp(settings.maneuverHoldSeconds, 0.0, 1.0) : 0.120;
        const double rateMagnitude = std::hypot(
            velocity.x.rateDegreesPerSecond,
            velocity.y.rateDegreesPerSecond);
        if (velocity.x.valid && velocity.y.valid && rateMagnitude >= threshold)
            diagnostics_.maneuverHoldRemainingSeconds = hold;
        else
            diagnostics_.maneuverHoldRemainingSeconds = std::max(
                0.0, diagnostics_.maneuverHoldRemainingSeconds - elapsed);
        active = diagnostics_.maneuverHoldRemainingSeconds > 0.0;
    }
    else
    {
        diagnostics_.maneuverHoldRemainingSeconds = active
            ? std::clamp(settings.maneuverHoldSeconds, 0.0, 1.0) : 0.0;
    }

    diagnostics_.selectionChanged = active != previousManeuverModelActive_;
    if (diagnostics_.selectionChanged)
        ++diagnostics_.selectionCount;
    previousManeuverModelActive_ = active;
    diagnostics_.maneuverModelActive = active;
    diagnostics_.modelAngleDeltaDegrees = needsAccelerationModel
        ? std::hypot(
            acceleration.x.angleDegrees - velocity.x.angleDegrees,
            acceleration.y.angleDegrees - velocity.y.angleDegrees)
        : 0.0;
    diagnostics_.modelRateDeltaDegreesPerSecond = needsAccelerationModel
        ? std::hypot(
            acceleration.x.rateDegreesPerSecond - velocity.x.rateDegreesPerSecond,
            acceleration.y.rateDegreesPerSecond - velocity.y.rateDegreesPerSecond)
        : 0.0;
    selected_ = active ? acceleration : velocity;
}

double ManeuverLosEstimator::boundedDtSeconds(TimePoint start, TimePoint end)
{
    if (start.time_since_epoch().count() == 0 || end.time_since_epoch().count() == 0)
        return 0.0;
    const double seconds = std::chrono::duration<double>(end - start).count();
    if (!std::isfinite(seconds) || seconds <= 0.0)
        return 0.0;
    return std::clamp(seconds, kMinimumDtSeconds, kMaximumDtSeconds);
}

void ManeuverLosEstimator::predictAcceleration(
    AccelerationAxis& axis, double dt, double jerkStdDegreesPerSecond3)
{
    if (!axis.initialized || dt <= 0.0)
        return;
    const double jerkStd = std::isfinite(jerkStdDegreesPerSecond3)
        ? std::clamp(jerkStdDegreesPerSecond3, 1.0, 100000.0) : 8000.0;
    const double dt2 = dt * dt;
    const double transition[3][3]{
        { 1.0, dt, 0.5 * dt2 },
        { 0.0, 1.0, dt },
        { 0.0, 0.0, 1.0 }
    };
    const double noiseGain[3]{ dt2 * dt / 6.0, dt2 * 0.5, dt };
    double predictedState[3]{};
    double intermediate[3][3]{};
    double predictedCovariance[3][3]{};
    for (int row = 0; row < 3; ++row)
    {
        for (int column = 0; column < 3; ++column)
        {
            predictedState[row] += transition[row][column] * axis.state[column];
            for (int inner = 0; inner < 3; ++inner)
                intermediate[row][column] +=
                    transition[row][inner] * axis.covariance[inner][column];
        }
    }
    const double variance = jerkStd * jerkStd;
    for (int row = 0; row < 3; ++row)
    for (int column = 0; column < 3; ++column)
    {
        for (int inner = 0; inner < 3; ++inner)
            predictedCovariance[row][column] +=
                intermediate[row][inner] * transition[column][inner];
        predictedCovariance[row][column] +=
            variance * noiseGain[row] * noiseGain[column];
    }
    for (int index = 0; index < 3; ++index)
        axis.state[index] = predictedState[index];
    for (int row = 0; row < 3; ++row)
    for (int column = 0; column < 3; ++column)
        axis.covariance[row][column] = predictedCovariance[row][column];
}

void ManeuverLosEstimator::correctAcceleration(
    AccelerationAxis& axis, double measurement, double measurementVariance)
{
    const double innovation = measurement - axis.state[0];
    const double innovationVariance = std::max(
        axis.covariance[0][0] + measurementVariance, 1e-9);
    double gain[3]{};
    double oldCovariance[3][3]{};
    for (int row = 0; row < 3; ++row)
    for (int column = 0; column < 3; ++column)
        oldCovariance[row][column] = axis.covariance[row][column];
    for (int index = 0; index < 3; ++index)
    {
        gain[index] = oldCovariance[index][0] / innovationVariance;
        axis.state[index] += gain[index] * innovation;
    }
    for (int row = 0; row < 3; ++row)
    for (int column = 0; column < 3; ++column)
        axis.covariance[row][column] =
            oldCovariance[row][column] - gain[row] * oldCovariance[0][column];
    for (int index = 0; index < 3; ++index)
        axis.covariance[index][index] = std::max(
            axis.covariance[index][index], 1e-12);
    axis.diagnostic.innovationDegrees = innovation;
    axis.diagnostic.innovationVariance = innovationVariance;
    axis.diagnostic.nis = innovation * innovation / innovationVariance;
}

RelativeLosKalmanAxisEstimate ManeuverLosEstimator::toEstimate(
    const AccelerationAxis& axis)
{
    RelativeLosKalmanAxisEstimate result = axis.diagnostic;
    result.valid = axis.initialized;
    result.angleDegrees = axis.state[0];
    result.rateDegreesPerSecond = axis.state[1];
    result.angleVariance = std::max(axis.covariance[0][0], 0.0);
    return result;
}

void ManeuverLosEstimator::updateConstantAcceleration(
    double angleX, double angleY, float confidence,
    TimePoint observationTime, TimePoint controlTime,
    double jerkStdDegreesPerSecond3)
{
    if (observationTime.time_since_epoch().count() == 0 ||
        controlTime.time_since_epoch().count() == 0 ||
        !std::isfinite(angleX) || !std::isfinite(angleY))
    {
        constantAccelerationEstimate_ = {};
        return;
    }
    const float boundedConfidence = std::isfinite(confidence)
        ? std::clamp(confidence, 0.0f, 1.0f) : 0.0f;
    const double confidenceScale = std::clamp(
        static_cast<double>(boundedConfidence), 0.05, 1.0);
    const double measurementStd = kMeasurementStdDegrees / confidenceScale;
    const double measurementVariance = measurementStd * measurementStd;
    AccelerationAxis* axes[]{ &horizontalAcceleration_, &verticalAcceleration_ };
    const double measurements[]{ angleX, angleY };
    for (int index = 0; index < 2; ++index)
    {
        AccelerationAxis& axis = *axes[index];
        if (!axis.initialized)
        {
            axis.initialized = true;
            axis.time = observationTime;
            axis.state[0] = measurements[index];
            axis.covariance[0][0] = 4.0;
            axis.covariance[1][1] = 400.0;
            axis.covariance[2][2] = 40000.0;
        }
        else
        {
            predictAcceleration(axis,
                boundedDtSeconds(axis.time, observationTime),
                jerkStdDegreesPerSecond3);
            axis.time = observationTime;
        }
        correctAcceleration(axis, measurements[index], measurementVariance);
    }
    AccelerationAxis projectedHorizontal = horizontalAcceleration_;
    AccelerationAxis projectedVertical = verticalAcceleration_;
    predictAcceleration(projectedHorizontal,
        boundedDtSeconds(projectedHorizontal.time, controlTime),
        jerkStdDegreesPerSecond3);
    predictAcceleration(projectedVertical,
        boundedDtSeconds(projectedVertical.time, controlTime),
        jerkStdDegreesPerSecond3);
    constantAccelerationEstimate_.x = toEstimate(projectedHorizontal);
    constantAccelerationEstimate_.y = toEstimate(projectedVertical);
    constantAccelerationEstimate_.measurementConfidence = boundedConfidence;
    const double normalizedNis = std::max(
        horizontalAcceleration_.diagnostic.nis,
        verticalAcceleration_.diagnostic.nis);
    constantAccelerationEstimate_.feedforwardConfidence =
        boundedConfidence * boundedConfidence *
        std::exp(-0.5 * std::min(normalizedNis, 20.0));
}
