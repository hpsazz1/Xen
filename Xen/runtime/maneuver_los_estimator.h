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

// r62 离线正负样本共同通过的命令响应速率裕量。只用于 DML shadow 的机动证据门控，
// 不修改 LOS 测量、Kalman 状态、12 度/秒物理门槛或正式设备输出。
inline constexpr double kManeuverResponseRateUncertaintyGain = 1.25;

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
        // 每帧由外部命令响应模型给出的逐轴速率不确定度上界。0保持历史行为；
        // 门控只扣除可解释部分，不修改LOS测量、Kalman状态或12°/s物理门槛。
        double maneuverRateUncertaintyXDegreesPerSecond = 0.0;
        double maneuverRateUncertaintyYDegreesPerSecond = 0.0;
        RelativeLosKalman::Settings constantVelocitySettings{};
    };

    struct Diagnostics
    {
        bool maneuverModelActive = false;
        bool selectionChanged = false;
        size_t selectionCount = 0;
        double maneuverHoldRemainingSeconds = 0.0;
        double maneuverRateEvidenceDegreesPerSecond = 0.0;
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
