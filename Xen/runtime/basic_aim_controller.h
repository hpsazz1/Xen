#pragma once

#include <algorithm>
#include <cmath>

// 写入流水线 CSV 的控制器行为修订号。改变稳定、积分或限速语义时必须递增，
// 使现场数据能够确认实际运行的控制器，而不是只依据文件目录或口头版本判断。
inline constexpr int kBasicAimControllerRevision = 30;

// 帧率无关的一阶基础控制器。
// 输入是检测空间像素误差，输出是当前帧应发送的设备 counts。
class BasicAimController
{
public:
    struct Settings
    {
        double responseSeconds = 0.080;
        double maxCountsPerSecond = 1440.0; // 四链路九宫格复测值；仅放宽远距限速，单帧预算仍按真实 dt 换算
        double integralTimeSeconds = 0.0;   // 0 表示关闭；现场移动目标复测通过后再推广默认值
        double settleRadiusPixels = 5.0;
        double releaseRadiusPixels = 8.0;
        bool preserveMovingIntegral = false; // 预测已确认持续移动时，越心误差不代表目标反向
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
        double settleMotionThreshold = 0.0;
        double effectiveResponseSecondsX = 0.0;
        double effectiveResponseSecondsY = 0.0;
        bool settled = false;
        bool speedLimited = false; // 本次输出是否被最大设备速率截断
        bool movingInsideSettle = false;
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
        const double errorMotion = hasPreviousError_
            ? std::hypot(errorX - previousErrorX_, errorY - previousErrorY_) : 0.0;
        // PI 静止锁存只应用于误差基本不变的目标。启用积分后，如果相邻有效观测的误差
        // 变化达到稳定半径的四分之一（320 检测区域默认 1.25 px），说明目标仍在移动；
        // 即使尚未越过 8 px 释放半径也要立即恢复控制，避免高频换向时连续停发。
        const double settleMotionThreshold = std::max(1.0, settleRadius * 0.25);
        const bool movingInsideSettle = integralTime > 0.0 && hasPreviousError_ &&
            errorMotion >= settleMotionThreshold;
        out.errorMotion = errorMotion;
        out.settleMotionThreshold = settleMotionThreshold;
        out.movingInsideSettle = movingInsideSettle;

        // 匀速跟踪时 PI 可能在中心附近产生亚像素级越心，这只是积分自然回调，不能等同于
        // 目标反转。只有误差已位于旧积分反方向且扩大到稳定半径，才撤销该轴旧积分；
        // 这样小幅越心仍保留维持速度，静止过冲或真实反转又能在 5 px 边界内完成解卷绕。
        if (integralTime > 0.0)
        {
            if (!settings.preserveMovingIntegral &&
                errorX * integralCountErrorX_ < 0.0 && std::abs(errorX) >= settleRadius)
                integralCountErrorX_ = 0.0;
            if (!settings.preserveMovingIntegral &&
                errorY * integralCountErrorY_ < 0.0 && std::abs(errorY) >= settleRadius)
                integralCountErrorY_ = 0.0;
        }

        const double retainedIntegralCountsX = integralTime > 0.0
            ? integralCountErrorX_ * dt / (response * integralTime) : 0.0;
        const double retainedIntegralCountsY = integralTime > 0.0
            ? integralCountErrorY_ * dt / (response * integralTime) : 0.0;
        // 设备最终发送整数 counts；至少半个 count 的保留积分代表当前帧仍有可执行的
        // 持续运动需求。此时不能把“进入中心”误判成静止并清空 PI 前馈能力。
        const bool hasActionableIntegral =
            std::hypot(retainedIntegralCountsX, retainedIntegralCountsY) >= 0.5;

        if (settled_)
        {
            if (out.errorDistance <= releaseRadius && !hasActionableIntegral && !movingInsideSettle)
            {
                clearIntegralState();
                rememberError(errorX, errorY);
                out.settled = true;
                return out;
            }
            settled_ = false;
        }
        if (out.errorDistance <= settleRadius && !hasActionableIntegral && !movingInsideSettle)
        {
            settled_ = true;
            clearIntegralState();
            rememberError(errorX, errorY);
            out.settled = true;
            return out;
        }

        const double responseFractionX =
            1.0 - std::exp(-dt / out.effectiveResponseSecondsX);
        const double responseFractionY =
            1.0 - std::exp(-dt / out.effectiveResponseSecondsY);
        const double proportionalCountsX = errorX * responseFractionX * countsPerPixelX;
        const double proportionalCountsY = errorY * responseFractionY * countsPerPixelY;

        double candidateIntegralX = integralCountErrorX_;
        double candidateIntegralY = integralCountErrorY_;
        if (integralTime > 0.0)
        {
            // 匀速目标对纯比例控制形成固定滞后。积分项累计 counts 误差并提供持续速度，
            // 使系统能够消除斜坡输入的稳态误差；误差换向时清零，避免反转后旧积分拖拽。
            candidateIntegralX += errorX * countsPerPixelX * dt;
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
            out.integralCountsX = candidateIntegralX * dt / (response * integralTime);
            out.integralCountsY = candidateIntegralY * dt / (response * integralTime);
        }
        else
        {
            clearIntegralState();
        }

        out.countsX = proportionalCountsX + out.integralCountsX;
        out.countsY = proportionalCountsY + out.integralCountsY;
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
        return out;
    }

    void reset()
    {
        settled_ = false;
        clearIntegralState();
        previousErrorX_ = 0.0;
        previousErrorY_ = 0.0;
        hasPreviousError_ = false;
    }
    bool settled() const { return settled_; }

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

    bool settled_ = false;
    double integralCountErrorX_ = 0.0;
    double integralCountErrorY_ = 0.0;
    double previousErrorX_ = 0.0;
    double previousErrorY_ = 0.0;
    bool hasPreviousError_ = false;
};
