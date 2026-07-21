#pragma once

#include <algorithm>
#include <cmath>

// 数值写入流水线 CSV，顺序属于诊断契约，不得重排。
enum class ManualControlState : int
{
    Auto = 0,
    Blend = 1,
    ManualOverride = 2,
    Recover = 3,
};

inline const char* manualControlStateName(ManualControlState state)
{
    switch (state)
    {
    case ManualControlState::Blend: return "blend";
    case ManualControlState::ManualOverride: return "manual_override";
    case ManualControlState::Recover: return "recover";
    default: return "auto";
    }
}

class ManualControlArbiter
{
public:
    struct Settings
    {
        double enterSpeedDegreesPerSecond = 0.60;
        double fullSpeedDegreesPerSecond = 3.00;
        double exitSpeedDegreesPerSecond = 0.25;
        double overrideSpeedDegreesPerSecond = 3.00;
        double conflictAlignment = -0.25;
        double sameDirectionWeight = 0.50;
        double crossDirectionWeight = 0.20;
        double enterHoldSeconds = 0.015;
        double exitHoldSeconds = 0.080;
        double recoverySeconds = 0.150;
    };

    struct Output
    {
        ManualControlState state = ManualControlState::Auto;
        double manualVelocityXDegreesPerSecond = 0.0;
        double manualVelocityYDegreesPerSecond = 0.0;
        double manualSpeedDegreesPerSecond = 0.0;
        double alignment = 0.0;
        double autoWeight = 1.0;
        bool manualActive = false;
        bool enteredManual = false;
        bool exitedManual = false;
    };

    Output update(
        double manualDeltaXDegrees,
        double manualDeltaYDegrees,
        double sampleSeconds,
        double autoDeltaXDegrees,
        double autoDeltaYDegrees,
        const Settings& settings = Settings{})
    {
        const double dt = std::clamp(sampleSeconds, 0.001, 0.100);
        const double rawVelocityX = manualDeltaXDegrees / dt;
        const double rawVelocityY = manualDeltaYDegrees / dt;
        // 20 ms 一阶低通抑制单包尖峰，但不引入跨状态的移动平均窗口。
        const double alpha = 1.0 - std::exp(-dt / 0.020);
        filteredVelocityX_ += (rawVelocityX - filteredVelocityX_) * alpha;
        filteredVelocityY_ += (rawVelocityY - filteredVelocityY_) * alpha;

        Output out;
        out.manualVelocityXDegreesPerSecond = filteredVelocityX_;
        out.manualVelocityYDegreesPerSecond = filteredVelocityY_;
        out.manualSpeedDegreesPerSecond =
            std::hypot(filteredVelocityX_, filteredVelocityY_);

        const double autoMagnitude = std::hypot(autoDeltaXDegrees, autoDeltaYDegrees);
        if (out.manualSpeedDegreesPerSecond > 1e-9 && autoMagnitude > 1e-9)
        {
            out.alignment = std::clamp(
                (filteredVelocityX_ * autoDeltaXDegrees +
                    filteredVelocityY_ * autoDeltaYDegrees) /
                (out.manualSpeedDegreesPerSecond * autoMagnitude),
                -1.0, 1.0);
        }

        const double enterSpeed = std::max(0.01, settings.enterSpeedDegreesPerSecond);
        const double exitSpeed = std::clamp(
            settings.exitSpeedDegreesPerSecond, 0.001, enterSpeed);
        const double overrideSpeed = std::max(
            enterSpeed, settings.overrideSpeedDegreesPerSecond);
        const bool moving = out.manualSpeedDegreesPerSecond >= enterSpeed;
        const bool quiet = out.manualSpeedDegreesPerSecond <= exitSpeed;
        const bool conflict = moving && out.alignment <= settings.conflictAlignment;
        const bool strongManual = out.manualSpeedDegreesPerSecond >= overrideSpeed;

        if (moving)
        {
            quietSeconds_ = 0.0;
            activeSeconds_ += dt;
        }
        else
        {
            if (quiet)
                quietSeconds_ += dt;
            else
                quietSeconds_ = 0.0;
            if (state_ == ManualControlState::Auto)
                activeSeconds_ = 0.0;
        }

        const bool shouldEnter = moving &&
            (activeSeconds_ >= std::max(0.0, settings.enterHoldSeconds) ||
                strongManual || conflict);
        switch (state_)
        {
        case ManualControlState::Auto:
            if (shouldEnter)
            {
                state_ = (strongManual || conflict)
                    ? ManualControlState::ManualOverride
                    : ManualControlState::Blend;
                out.enteredManual = true;
                activeSeconds_ = 0.0;
            }
            break;
        case ManualControlState::Blend:
        case ManualControlState::ManualOverride:
            if (strongManual || conflict)
                state_ = ManualControlState::ManualOverride;
            else if (moving)
                state_ = ManualControlState::Blend;
            else if (quiet && quietSeconds_ >= std::max(0.0, settings.exitHoldSeconds))
            {
                state_ = ManualControlState::Recover;
                recoveryElapsed_ = 0.0;
            }
            break;
        case ManualControlState::Recover:
            if (shouldEnter)
            {
                state_ = (strongManual || conflict)
                    ? ManualControlState::ManualOverride
                    : ManualControlState::Blend;
                activeSeconds_ = 0.0;
            }
            else
            {
                recoveryElapsed_ += dt;
                if (recoveryElapsed_ >= std::max(0.001, settings.recoverySeconds))
                {
                    state_ = ManualControlState::Auto;
                    out.exitedManual = true;
                    activeSeconds_ = 0.0;
                }
            }
            break;
        }

        const double fullSpeed = std::max(
            enterSpeed + 0.01, settings.fullSpeedDegreesPerSecond);
        const double intensity = std::clamp(
            (out.manualSpeedDegreesPerSecond - enterSpeed) /
                (fullSpeed - enterSpeed),
            0.0, 1.0);
        if (state_ == ManualControlState::ManualOverride || conflict)
        {
            out.autoWeight = 0.0;
        }
        else if (state_ == ManualControlState::Blend)
        {
            const double retainedWeight = out.alignment > 0.5
                ? settings.sameDirectionWeight
                : settings.crossDirectionWeight;
            out.autoWeight = 1.0 - intensity *
                (1.0 - std::clamp(retainedWeight, 0.0, 1.0));
        }
        else if (state_ == ManualControlState::Recover)
        {
            const double progress = std::clamp(
                recoveryElapsed_ / std::max(0.001, settings.recoverySeconds),
                0.0, 1.0);
            out.autoWeight = progress * progress * (3.0 - 2.0 * progress);
        }

        out.state = state_;
        out.manualActive = state_ != ManualControlState::Auto;
        return out;
    }

    void reset()
    {
        state_ = ManualControlState::Auto;
        filteredVelocityX_ = 0.0;
        filteredVelocityY_ = 0.0;
        quietSeconds_ = 0.0;
        activeSeconds_ = 0.0;
        recoveryElapsed_ = 0.0;
    }

    bool manualActive() const { return state_ != ManualControlState::Auto; }

private:
    ManualControlState state_ = ManualControlState::Auto;
    double filteredVelocityX_ = 0.0;
    double filteredVelocityY_ = 0.0;
    double quietSeconds_ = 0.0;
    double activeSeconds_ = 0.0;
    double recoveryElapsed_ = 0.0;
};
