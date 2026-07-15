#ifndef LOS_AIM_CONTROLLER_H
#define LOS_AIM_CONTROLLER_H

// P0-4A 角度控制器只负责把“纠偏、速度维持、经验提前、积分”组合成单周期
// counts 请求。设备发送、整数余量和速度/加速度/jerk 整形属于 P0-4B，不在此处实现。
class LosAimController
{
public:
    struct Settings
    {
        double responseSeconds = 0.080;
        double maxCountsPerSecond = 1440.0;
        double feedforwardGain = 0.0;
        double integralTimeSeconds = 0.0;
        double integralZoneDegrees = 0.0;
        double integralOutputRatio = 0.25;
        double leadHorizonSeconds = 0.0;
        double leadStrength = 0.0;
        double settleErrorDegrees = 0.080;
        double settleRateDegreesPerSecond = 1.200;
        double reverseConfirmationSeconds = 0.080;
    };

    struct Input
    {
        double errorDegreesX = 0.0;
        double errorDegreesY = 0.0;
        double relativeLosRateDegreesPerSecondX = 0.0;
        double relativeLosRateDegreesPerSecondY = 0.0;
        double feedforwardConfidence = 0.0;
        double degreesPerCountX = 0.0;
        double degreesPerCountY = 0.0;
        double dtSeconds = 0.0;
        bool valid = false;
    };

    struct Output
    {
        bool valid = false;
        bool speedLimited = false;
        bool integralFrozen = false;
        bool settled = false;
        bool settleReleased = false;
        int settleConfirmationSamples = 0;
        bool lowSpeedReverseSuppressed = false;
        double reverseConfirmationSeconds = 0.0;
        double feedbackCountsX = 0.0;
        double feedbackCountsY = 0.0;
        double trackingFeedforwardCountsX = 0.0;
        double trackingFeedforwardCountsY = 0.0;
        double leadReferenceDegreesX = 0.0;
        double leadReferenceDegreesY = 0.0;
        double leadCountsX = 0.0;
        double leadCountsY = 0.0;
        double integralCountsX = 0.0;
        double integralCountsY = 0.0;
        double unlimitedCountsX = 0.0;
        double unlimitedCountsY = 0.0;
        double limitedCountsX = 0.0;
        double limitedCountsY = 0.0;
        double frameCountLimit = 0.0;
    };

    Output update(const Input& input, const Settings& settings);
    void reset();

private:
    bool settled_ = false;
    int quietSamples_ = 0;
    bool hasAcceptedSettleBandDirection_ = false;
    double acceptedSettleBandDirectionX_ = 0.0;
    double acceptedSettleBandDirectionY_ = 0.0;
    double pendingReverseX_ = 0.0;
    double pendingReverseY_ = 0.0;
    double pendingReverseSeconds_ = 0.0;
    double integralErrorDegreeSecondsX_ = 0.0;
    double integralErrorDegreeSecondsY_ = 0.0;
};

#endif
