#ifndef MANEUVER_LOS_ESTIMATOR_H
#define MANEUVER_LOS_ESTIMATOR_H

#include <cstddef>

#include "relative_los_kalman.h"

enum class ManeuverLosEstimatorMode
{
    ConstantVelocity,
    ConstantAcceleration,
    ManeuverGatedConstantAcceleration
};

inline const char* maneuverLosEstimatorModeName(ManeuverLosEstimatorMode mode)
{
    if (mode == ManeuverLosEstimatorMode::ConstantAcceleration)
        return "constant_acceleration";
    if (mode == ManeuverLosEstimatorMode::ManeuverGatedConstantAcceleration)
        return "maneuver_gated_ca";
    return "kalman";
}

// 同帧并行运行常速度与常加速度模型，并以物理LOS速率门控模型选择。
// 本类不持有控制器或设备引用，因此只能产生可审计的角度状态候选。
class ManeuverLosEstimator
{
public:
    using TimePoint = RelativeLosKalman::TimePoint;

    struct Settings
    {
        ManeuverLosEstimatorMode mode = ManeuverLosEstimatorMode::ConstantVelocity;
        double jerkStdDegreesPerSecond3 = 8000.0;
        double maneuverRateThresholdDegreesPerSecond = 12.0;
        double maneuverHoldSeconds = 0.120;
        RelativeLosKalman::Settings constantVelocitySettings{};
    };

    struct Diagnostics
    {
        bool maneuverModelActive = false;
        bool selectionChanged = false;
        size_t selectionCount = 0;
        double maneuverHoldRemainingSeconds = 0.0;
        double modelAngleDeltaDegrees = 0.0;
        double modelRateDeltaDegreesPerSecond = 0.0;
    };

    void reset();
    void update(double angleX, double angleY, float confidence,
        TimePoint observationTime, TimePoint controlTime,
        const Settings& settings);

    const RelativeLosKalmanEstimate& selectedEstimate() const { return selected_; }
    const RelativeLosKalmanEstimate& constantVelocityEstimate() const
    {
        return constantVelocity_.estimate();
    }
    const RelativeLosKalmanEstimate& constantAccelerationEstimate() const
    {
        return constantAccelerationEstimate_;
    }
    const Diagnostics& diagnostics() const { return diagnostics_; }

private:
    struct AccelerationAxis
    {
        bool initialized = false;
        TimePoint time{};
        double state[3]{};
        double covariance[3][3]{};
        RelativeLosKalmanAxisEstimate diagnostic{};
    };

    static double boundedDtSeconds(TimePoint start, TimePoint end);
    static void predictAcceleration(
        AccelerationAxis& axis, double dt, double jerkStdDegreesPerSecond3);
    static void correctAcceleration(
        AccelerationAxis& axis, double measurement, double measurementVariance);
    static RelativeLosKalmanAxisEstimate toEstimate(const AccelerationAxis& axis);
    void updateConstantAcceleration(double angleX, double angleY, float confidence,
        TimePoint observationTime, TimePoint controlTime, double jerkStdDegreesPerSecond3);

    RelativeLosKalman constantVelocity_{};
    AccelerationAxis horizontalAcceleration_{};
    AccelerationAxis verticalAcceleration_{};
    RelativeLosKalmanEstimate constantAccelerationEstimate_{};
    RelativeLosKalmanEstimate selected_{};
    Diagnostics diagnostics_{};
    TimePoint lastObservationTime_{};
    bool previousManeuverModelActive_ = false;
};

#endif
