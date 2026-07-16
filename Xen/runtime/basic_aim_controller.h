#pragma once

#include <algorithm>
#include <cmath>

// 写入流水线 CSV 的控制器行为修订号。改变稳定、积分或限速语义时必须递增，
// 使现场数据能够确认实际运行的控制器，而不是只依据文件目录或口头版本判断。
inline constexpr int kBasicAimControllerRevision = 45;

// 帧率无关的一阶基础控制器。
// 输入是检测空间像素误差，输出是当前帧应发送的设备 counts。
class BasicAimController
{
public:
    struct Settings
    {
        double responseSeconds = 0.080;
        double maxCountsPerSecond = 1440.0; // 独立控制器单元测试的保守默认值；现场配置按场景注入
        double integralTimeSeconds = 0.0;   // 独立控制器默认关闭积分；现场移动配置显式启用
        double settleRadiusPixels = 5.0;
        double releaseRadiusPixels = 8.0;
        bool preserveMovingIntegralX = false; // X轴确认持续移动时保留越心积分
        bool preserveMovingIntegralY = false; // Y轴确认持续移动时保留越心积分
        bool allowMovingInsideSettleX = true; // X轴误差快速变化时允许独立释放稳定锁存
        bool allowMovingInsideSettleY = true; // Y轴误差快速变化时允许独立释放稳定锁存
    };

    struct Output
    {
        double countsX = 0.0;
        double countsY = 0.0;
        double requestedPixelX = 0.0;
        double requestedPixelY = 0.0;
        double integralCountsX = 0.0;
        double integralCountsY = 0.0;
        double errorDistance = 0.0;
        double frameCountLimit = 0.0;
        double errorMotion = 0.0;
        double errorMotionX = 0.0;
        double errorMotionY = 0.0;
        double settleMotionThreshold = 0.0;
        double effectiveResponseSecondsX = 0.0;
        double effectiveResponseSecondsY = 0.0;
        bool settled = false;
        bool settledX = false;
        bool settledY = false;
        bool speedLimited = false; // 本次输出是否被最大设备速率截断
        bool movingInsideSettle = false;
        bool movingInsideSettleX = false;
        bool movingInsideSettleY = false;
        bool horizontalCatchUp = false; // 水平大误差是否使用短时快速比例响应
        bool verticalCatchUp = false; // 垂直大误差是否使用短时快速比例响应
    };

    Output update(double errorX, double errorY, double dt,
                  double countsPerPixelX, double countsPerPixelY,
                  const Settings& settings)
    {
        Output out;
        out.errorDistance = std::hypot(errorX, errorY);
        dt = std::clamp(dt, 1.0 / 500.0, 0.05);

        const double response = std::clamp(settings.responseSeconds, 0.010, 0.500);
        const double integralTime = settings.integralTimeSeconds > 0.0
            ? std::clamp(settings.integralTimeSeconds, 0.050, 1.0) : 0.0;

        const double settleRadius = std::max(0.0, settings.settleRadiusPixels);
        const double releaseRadius = std::max(settleRadius, settings.releaseRadiusPixels);
        // r26在32 px处直接从40 ms切到20 ms，实测该档68.3%的观测撞到速度上限，
        // 既没有形成可控的额外追赶，还会在阈值附近产生增益跳变。16~32 px改为从半响应
        // 连续缩短到八分之三响应，32 px以上保持该下限；积分、二维速度预算和稳定区不变。
        const double catchUpThreshold = std::max(16.0, releaseRadius * 2.0);
        out.horizontalCatchUp = std::abs(errorX) >= catchUpThreshold;
        out.verticalCatchUp = std::abs(errorY) >= catchUpThreshold;
        const auto catchUpResponse = [response, catchUpThreshold](double error)
        {
            const double catchUpProgress = std::clamp(
                (std::abs(error) - catchUpThreshold) / catchUpThreshold, 0.0, 1.0);
            return response * (0.5 - catchUpProgress * 0.125);
        };
        out.effectiveResponseSecondsX = out.horizontalCatchUp
            ? catchUpResponse(errorX) : response;
        out.effectiveResponseSecondsY = out.verticalCatchUp
            ? catchUpResponse(errorY) : response;
        const double errorMotionX = hasPreviousError_
            ? std::abs(errorX - previousErrorX_) : 0.0;
        const double errorMotionY = hasPreviousError_
            ? std::abs(errorY - previousErrorY_) : 0.0;
        const double errorMotion = std::hypot(errorMotionX, errorMotionY);
        // PI 静止锁存只应用于误差基本不变的目标。启用积分后，如果相邻有效观测的误差
        // 变化达到稳定半径的四分之一（320 检测区域默认 1.25 px），说明目标仍在移动；
        // 即使尚未越过 8 px 释放半径也要立即恢复控制，避免高频换向时连续停发。
        const double settleMotionThreshold = std::max(1.0, settleRadius * 0.25);
        const bool movingInsideSettleX = settings.allowMovingInsideSettleX &&
            integralTime > 0.0 && hasPreviousError_ &&
            errorMotionX >= settleMotionThreshold;
        const bool movingInsideSettleY = settings.allowMovingInsideSettleY &&
            integralTime > 0.0 && hasPreviousError_ &&
            errorMotionY >= settleMotionThreshold;
        out.errorMotion = errorMotion;
        out.errorMotionX = errorMotionX;
        out.errorMotionY = errorMotionY;
        out.settleMotionThreshold = settleMotionThreshold;
        out.movingInsideSettleX = movingInsideSettleX;
        out.movingInsideSettleY = movingInsideSettleY;
        out.movingInsideSettle = movingInsideSettleX || movingInsideSettleY;

        // 匀速跟踪时 PI 可能在中心附近产生亚像素级越心，这只是积分自然回调，不能等同于
        // 目标反转。只有误差已位于旧积分反方向且扩大到稳定半径，才撤销该轴旧积分；
        // 这样小幅越心仍保留维持速度，静止过冲或真实反转又能在 5 px 边界内完成解卷绕。
        if (integralTime > 0.0)
        {
            if (!settings.preserveMovingIntegralX &&
                errorX * integralCountErrorX_ < 0.0 && std::abs(errorX) >= settleRadius)
                integralCountErrorX_ = 0.0;
            if (!settings.preserveMovingIntegralY &&
                errorY * integralCountErrorY_ < 0.0 && std::abs(errorY) >= settleRadius)
                integralCountErrorY_ = 0.0;
        }

        const double retainedIntegralCountsX = integralTime > 0.0
            ? integralCountErrorX_ * dt / (response * integralTime) : 0.0;
        const double retainedIntegralCountsY = integralTime > 0.0
            ? integralCountErrorY_ * dt / (response * integralTime) : 0.0;
        // 设备最终发送整数 counts；至少半个 count 的保留积分代表当前帧仍有可执行的
        // 持续运动需求。此时不能把“进入中心”误判成静止并清空 PI 前馈能力。
        const bool hasActionableIntegralX = settings.preserveMovingIntegralX &&
            std::abs(retainedIntegralCountsX) >= 0.5;
        const bool hasActionableIntegralY = settings.preserveMovingIntegralY &&
            std::abs(retainedIntegralCountsY) >= 0.5;

        // 二维共用稳定状态会让水平移动目标的Y轴持续追逐约1~2 px检测噪声。
        // 每轴沿用原分量半径独立停发，避免改变既有单轴静止精度；可靠垂直或
        // 斜向运动通过对应轴运动标志立即释放。两轴同时锁存时稳定区是矩形，
        // 这是分轴控制的明确语义，不再用另一轴误差阻止当前轴停发。
        const auto updateAxisSettle = [settleRadius, releaseRadius]
            (double error, bool actionableIntegral, bool movingInsideSettle,
             bool& settled, double& integralState, double& previousOutput,
             double& pendingReverse, int& pendingReverseFrames)
        {
            const auto clearAxisState = [&]()
            {
                integralState = 0.0;
                previousOutput = 0.0;
                pendingReverse = 0.0;
                pendingReverseFrames = 0;
            };
            if (settled)
            {
                if (std::abs(error) <= releaseRadius &&
                    !actionableIntegral && !movingInsideSettle)
                {
                    clearAxisState();
                    return true;
                }
                settled = false;
            }
            if (std::abs(error) <= settleRadius &&
                !actionableIntegral && !movingInsideSettle)
            {
                settled = true;
                clearAxisState();
                return true;
            }
            return false;
        };
        out.settledX = updateAxisSettle(
            errorX, hasActionableIntegralX, movingInsideSettleX,
            settledX_, integralCountErrorX_, previousOutputX_,
            pendingReverseX_, pendingReverseFramesX_);
        out.settledY = updateAxisSettle(
            errorY, hasActionableIntegralY, movingInsideSettleY,
            settledY_, integralCountErrorY_, previousOutputY_,
            pendingReverseY_, pendingReverseFramesY_);
        out.settled = out.settledX && out.settledY;

        const double responseFractionX =
            1.0 - std::exp(-dt / out.effectiveResponseSecondsX);
        const double responseFractionY =
            1.0 - std::exp(-dt / out.effectiveResponseSecondsY);
        const double proportionalCountsX = out.settledX
            ? 0.0 : errorX * responseFractionX * countsPerPixelX;
        const double proportionalCountsY = out.settledY
            ? 0.0 : errorY * responseFractionY * countsPerPixelY;

        double candidateIntegralX = integralCountErrorX_;
        double candidateIntegralY = integralCountErrorY_;
        if (integralTime > 0.0)
        {
            // 越心时渐进泄放旧侧积分，避免硬清零的稳态偏差和原量换侧的反向脉冲。
            constexpr double kMovingIntegralCrossingBleed = 0.50;
            if (settings.preserveMovingIntegralX)
            {
                if (errorX * candidateIntegralX < 0.0 &&
                    std::abs(errorX) >= settleRadius)
                    candidateIntegralX *= kMovingIntegralCrossingBleed;
            }
            if (settings.preserveMovingIntegralY)
            {
                if (errorY * candidateIntegralY < 0.0 &&
                    std::abs(errorY) >= settleRadius)
                    candidateIntegralY *= kMovingIntegralCrossingBleed;
            }
            // 匀速目标对纯比例控制形成固定滞后。积分项累计 counts 误差并提供持续速度，
            // 使系统能够消除斜坡输入的稳态误差；误差换向时清零，避免反转后旧积分拖拽。
            if (out.settledX)
                candidateIntegralX = 0.0;
            else
                candidateIntegralX += errorX * countsPerPixelX * dt;
            if (out.settledY)
                candidateIntegralY = 0.0;
            else
                candidateIntegralY += errorY * countsPerPixelY * dt;

            // 积分速度最多占设备总预算的一半；远距离比例项限速时不提交新积分，
            // 防止静止大步进和设备饱和期间产生 wind-up。
            const double maxIntegralState =
                std::max(1.0, settings.maxCountsPerSecond) * 0.5 * response * integralTime;
            const double candidateMagnitude = std::hypot(candidateIntegralX, candidateIntegralY);
            if (candidateMagnitude > maxIntegralState && candidateMagnitude > 0.0)
            {
                const double scale = maxIntegralState / candidateMagnitude;
                candidateIntegralX *= scale;
                candidateIntegralY *= scale;
            }
            out.integralCountsX = out.settledX
                ? 0.0 : candidateIntegralX * dt / (response * integralTime);
            out.integralCountsY = out.settledY
                ? 0.0 : candidateIntegralY * dt / (response * integralTime);
        }
        else
        {
            clearIntegralState();
        }

        out.countsX = proportionalCountsX + out.integralCountsX;
        out.countsY = proportionalCountsY + out.integralCountsY;
        // 中心附近的 ±1/±2 counts 反向脉冲属于量化抖动，不应让设备在
        // 两侧来回扫动；大于该阈值的真实 reverse/jump 修正保持原样。
        constexpr double kSmallReverseCounts = 4.0;
        constexpr int kReverseConfirmationFrames = 2;
        const auto suppressUnconfirmedReverse = [releaseRadius, kSmallReverseCounts,
                                                  kReverseConfirmationFrames]
            (double& output, double error, double previousOutput,
             double& candidate, int& candidateFrames)
        {
            if (std::abs(error) > releaseRadius || previousOutput == 0.0 ||
                output * previousOutput >= 0.0 ||
                std::abs(output) > kSmallReverseCounts)
            {
                candidate = 0.0;
                candidateFrames = 0;
                return;
            }
            if (candidate * output < 0.0 || candidate == 0.0)
                candidateFrames = 1;
            else
                ++candidateFrames;
            candidate = output;
            if (candidateFrames < kReverseConfirmationFrames)
                output = 0.0;
            else
            {
                candidate = 0.0;
                candidateFrames = 0;
            }
        };
        suppressUnconfirmedReverse(
            out.countsX, errorX, previousOutputX_, pendingReverseX_, pendingReverseFramesX_);
        suppressUnconfirmedReverse(
            out.countsY, errorY, previousOutputY_, pendingReverseY_, pendingReverseFramesY_);
        out.requestedPixelX = std::abs(countsPerPixelX) > 1e-12
            ? out.countsX / countsPerPixelX : 0.0;
        out.requestedPixelY = std::abs(countsPerPixelY) > 1e-12
            ? out.countsY / countsPerPixelY : 0.0;

        out.frameCountLimit = std::max(1.0, settings.maxCountsPerSecond) * dt;
        const double magnitude = std::hypot(out.countsX, out.countsY);
        if (magnitude > out.frameCountLimit && magnitude > 0.0)
        {
            out.speedLimited = true;
            const double scale = out.frameCountLimit / magnitude;
            out.countsX *= scale;
            out.countsY *= scale;
            out.integralCountsX *= scale;
            out.integralCountsY *= scale;
            out.requestedPixelX *= scale;
            out.requestedPixelY *= scale;
        }
        else if (integralTime > 0.0)
        {
            integralCountErrorX_ = candidateIntegralX;
            integralCountErrorY_ = candidateIntegralY;
        }
        rememberError(errorX, errorY);
        if (out.countsX != 0.0)
            previousOutputX_ = out.countsX;
        if (out.countsY != 0.0)
            previousOutputY_ = out.countsY;
        return out;
    }

    void reset()
    {
        settledX_ = false;
        settledY_ = false;
        clearIntegralState();
        previousErrorX_ = 0.0;
        previousErrorY_ = 0.0;
        hasPreviousError_ = false;
        previousOutputX_ = 0.0;
        previousOutputY_ = 0.0;
        pendingReverseX_ = 0.0;
        pendingReverseY_ = 0.0;
        pendingReverseFramesX_ = 0;
        pendingReverseFramesY_ = 0;
    }
    bool settled() const { return settledX_ && settledY_; }

private:
    void clearIntegralState()
    {
        integralCountErrorX_ = 0.0;
        integralCountErrorY_ = 0.0;
    }

    void rememberError(double errorX, double errorY)
    {
        previousErrorX_ = errorX;
        previousErrorY_ = errorY;
        hasPreviousError_ = true;
    }

    bool settledX_ = false;
    bool settledY_ = false;
    double integralCountErrorX_ = 0.0;
    double integralCountErrorY_ = 0.0;
    double previousErrorX_ = 0.0;
    double previousErrorY_ = 0.0;
    bool hasPreviousError_ = false;
    double previousOutputX_ = 0.0;
    double previousOutputY_ = 0.0;
    double pendingReverseX_ = 0.0;
    double pendingReverseY_ = 0.0;
    int pendingReverseFramesX_ = 0;
    int pendingReverseFramesY_ = 0;
};
