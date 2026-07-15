#ifndef RELATIVE_LOS_KALMAN_H
#define RELATIVE_LOS_KALMAN_H

#include <chrono>

struct RelativeLosKalmanAxisEstimate
{
    bool valid = false;
    double angleDegrees = 0.0;
    double rateDegreesPerSecond = 0.0;
    double angleVariance = 0.0;
    double innovationDegrees = 0.0;
    double innovationVariance = 0.0;
    double nis = 0.0;
};

struct RelativeLosKalmanEstimate
{
    RelativeLosKalmanAxisEstimate x{};
    RelativeLosKalmanAxisEstimate y{};
    double measurementConfidence = 0.0;
    double feedforwardConfidence = 0.0;
};

// 二维独立常速度角度Kalman。每轴状态为[LOS角度(度),相对视线角速度(度/秒)]。
class RelativeLosKalman
{
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    struct Settings
    {
        // 静止或低速目标使用较低过程噪声抑制检测抖动；速度升高后平滑过渡到运动过程噪声，
        // 兼顾“目标静止/角色静止”和“目标运动/角色静止”两类主场景。
        double accelerationStdDegreesPerSecond2 = 90.0;
        double movingAccelerationStdDegreesPerSecond2 = 360.0;
        double movingRateThresholdDegreesPerSecond = 8.0;
    };

    void reset();
    void update(double angleX, double angleY, float confidence,
        TimePoint observationTime, TimePoint controlTime);
    void update(double angleX, double angleY, float confidence,
        TimePoint observationTime, TimePoint controlTime,
        const Settings& settings);
    const RelativeLosKalmanEstimate& estimate() const { return estimate_; }

private:
    struct AxisState
    {
        bool initialized = false;
        TimePoint time{};
        double angle = 0.0;
        double rate = 0.0;
        double p00 = 1.0;
        double p01 = 0.0;
        double p11 = 1.0;
        RelativeLosKalmanAxisEstimate diagnostic{};
    };

    static double boundedDtSeconds(TimePoint start, TimePoint end);
    static double measurementVariance(float confidence);
    static double accelerationStd(const AxisState& state,
        const Settings& settings);
    static void predict(AxisState& state, double dt,
        double accelerationStdDegreesPerSecond2);
    static void correct(AxisState& state, double measurement, double variance);
    static RelativeLosKalmanAxisEstimate toEstimate(const AxisState& state);

    AxisState horizontal_{};
    AxisState vertical_{};
    RelativeLosKalmanEstimate estimate_{};
};

#endif
