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
        double verticalCatchUpErrorDegrees = 0.0;
        double maxCountsPerSecond = 1440.0;
        double feedforwardGain = 0.0;
        double reversalFeedforwardBoost = 0.0;
        double reversalFeedforwardSeconds = 0.0;
        double integralTimeSeconds = 0.0;
        double integralZoneDegrees = 0.0;
        double integralOutputRatio = 0.25;
        double leadHorizonSeconds = 0.0;
        double leadStrength = 0.0;
        double settleErrorDegrees = 0.080;
        double settleRateDegreesPerSecond = 1.200;
        double reverseConfirmationSeconds = 0.080;
        // 仅扩大低速反向确认的误差带，不改变固定为1.5倍的静止锁存退出边界。
        double reverseConfirmationErrorMultiplier = 1.5;
        // 仅供离线候选验证：低速反向误差越过退出边界时，先确认持续性再解除静止锁存。
        bool confirmLowSpeedReverseSettleRelease = false;
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
        // 可选的静止进入判定误差。只影响未锁存时的连续安静样本，不参与P/I/前馈或释放。
        double settleEntryErrorDegreesX = 0.0;
        double settleEntryErrorDegreesY = 0.0;
        bool settleEntryErrorValid = false;
        // guard阻止安静计数时，可选暂停新输出，等待已发送命令兑现；正常运动态不受影响。
        bool holdOutputWhileSettleEntryBlocked = false;
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
        bool settleEntryCommandHeld = false;
        bool lowSpeedReverseSuppressed = false;
        bool verticalCatchUpActive = false;
        bool reversalFeedforwardActive = false;
        bool reversalDetected = false;
        double reverseConfirmationSeconds = 0.0;
        double feedbackCountsX = 0.0;
        double feedbackCountsY = 0.0;
        double effectiveResponseSecondsY = 0.0;
        double trackingFeedforwardCountsX = 0.0;
        double trackingFeedforwardCountsY = 0.0;
        double effectiveFeedforwardGainX = 0.0;
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
    double pendingSettleReleaseX_ = 0.0;
    double pendingSettleReleaseY_ = 0.0;
    double pendingSettleReleaseSeconds_ = 0.0;
    int acceptedMovingRateSignX_ = 0;
    double reversalFeedforwardRemainingSeconds_ = 0.0;
    double integralErrorDegreeSecondsX_ = 0.0;
    double integralErrorDegreeSecondsY_ = 0.0;
};

#endif
