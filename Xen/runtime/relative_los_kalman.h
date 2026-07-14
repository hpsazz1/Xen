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

    void reset();
    void update(double angleX, double angleY, float confidence,
        TimePoint observationTime, TimePoint controlTime);
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
    static void predict(AxisState& state, double dt);
    static void correct(AxisState& state, double measurement, double variance);
    static RelativeLosKalmanAxisEstimate toEstimate(const AxisState& state);

    AxisState horizontal_{};
    AxisState vertical_{};
    RelativeLosKalmanEstimate estimate_{};
};

#endif
