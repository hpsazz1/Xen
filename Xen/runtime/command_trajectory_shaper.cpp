#include "command_trajectory_shaper.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace
{
double clampFinite(double value, double minimum, double maximum, double fallback)
{
    return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
}

bool limitVector(double& x, double& y, double limit)
{
    const double magnitude = std::hypot(x, y);
    if (magnitude <= limit || magnitude <= 0.0)
        return false;
    const double scale = limit / magnitude;
    x *= scale;
    y *= scale;
    return true;
}

std::string lowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}
}

TrajectoryShaperMode parseTrajectoryShaperMode(const std::string& value)
{
    return lowerAscii(value) == "trapezoid"
        ? TrajectoryShaperMode::Trapezoid
        : TrajectoryShaperMode::Off;
}

void CommandTrajectoryShaper::configure(const Settings& settings)
{
    settings_ = settings;
    settings_.maxVelocityCountsPerSecond = clampFinite(
        settings.maxVelocityCountsPerSecond, 1.0, 100000.0, 1440.0);
    settings_.maxAccelerationCountsPerSecond2 = clampFinite(
        settings.maxAccelerationCountsPerSecond2, 1.0, 10000000.0, 60000.0);
    settings_.maxJerkCountsPerSecond3 = clampFinite(
        settings.maxJerkCountsPerSecond3, 1.0, 1000000000.0, 4000000.0);
    reset();
}

CommandTrajectoryShaper::Result CommandTrajectoryShaper::update(
    const TrajectoryRequest& request,
    double outputDurationSeconds,
    FrameTiming::Clock::time_point tickTime)
{
    if (!request.valid)
        return emergencyReset(tickTime);

    const double dt = clampFinite(outputDurationSeconds, 1.0 / 1000.0, 0.100, 1.0 / 240.0);
    const double requestDt = clampFinite(
        request.requestDurationSeconds, 1.0 / 1000.0, 0.100, dt);
    double targetVelocityX = request.requestedCountsX / requestDt;
    double targetVelocityY = request.requestedCountsY / requestDt;
    const bool targetVelocityLimited = limitVector(
        targetVelocityX, targetVelocityY, settings_.maxVelocityCountsPerSecond);
    state_.targetVelocityCountsPerSecX = targetVelocityX;
    state_.targetVelocityCountsPerSecY = targetVelocityY;

    if (settings_.mode == TrajectoryShaperMode::Off)
    {
        state_.targetVelocityCountsPerSecX = request.requestedCountsX / requestDt;
        state_.targetVelocityCountsPerSecY = request.requestedCountsY / requestDt;
        const double previousVelocityX = state_.velocityCountsPerSecX;
        const double previousVelocityY = state_.velocityCountsPerSecY;
        const double previousAccelerationX = state_.accelerationCountsPerSec2X;
        const double previousAccelerationY = state_.accelerationCountsPerSec2Y;
        state_.velocityCountsPerSecX = request.requestedCountsX / requestDt;
        state_.velocityCountsPerSecY = request.requestedCountsY / requestDt;
        state_.accelerationCountsPerSec2X =
            (state_.velocityCountsPerSecX - previousVelocityX) / dt;
        state_.accelerationCountsPerSec2Y =
            (state_.velocityCountsPerSecY - previousVelocityY) / dt;
        state_.jerkCountsPerSec3X =
            (state_.accelerationCountsPerSec2X - previousAccelerationX) / dt;
        state_.jerkCountsPerSec3Y =
            (state_.accelerationCountsPerSec2Y - previousAccelerationY) / dt;
        Result result = buildResult(
            request.requestedCountsX, request.requestedCountsY, tickTime);
        result.output.requestSequence = request.sequence;
        result.output.velocityLimited = false;
        return result;
    }

    const double previousVelocityX = state_.velocityCountsPerSecX;
    const double previousVelocityY = state_.velocityCountsPerSecY;
    const double previousAccelerationX = state_.accelerationCountsPerSec2X;
    const double previousAccelerationY = state_.accelerationCountsPerSec2Y;

    double desiredAccelerationX = (targetVelocityX - previousVelocityX) / dt;
    double desiredAccelerationY = (targetVelocityY - previousVelocityY) / dt;
    const bool accelerationLimited = limitVector(
        desiredAccelerationX, desiredAccelerationY,
        settings_.maxAccelerationCountsPerSecond2);

    double accelerationDeltaX = desiredAccelerationX - previousAccelerationX;
    double accelerationDeltaY = desiredAccelerationY - previousAccelerationY;
    const bool jerkLimited = limitVector(
        accelerationDeltaX, accelerationDeltaY,
        settings_.maxJerkCountsPerSecond3 * dt);
    double newAccelerationX = previousAccelerationX + accelerationDeltaX;
    double newAccelerationY = previousAccelerationY + accelerationDeltaY;
    bool finalAccelerationLimited = limitVector(
        newAccelerationX, newAccelerationY,
        settings_.maxAccelerationCountsPerSecond2);

    // 保证速度边界处始终存在“本周期把加速度降为零”的可行解。若允许当前加速度
    // 大于J×dt，速度已经贴近上限时，速度球、加速度球和jerk球可能没有交集，只能
    // 通过一次违反jerk的硬裁剪救场。把加速度再限制到J×dt后，零加速度始终位于
    // 当前jerk可达集合内，后续速度投影可以沿候选加速度到零的线段完成且不破坏jerk。
    finalAccelerationLimited = limitVector(
        newAccelerationX, newAccelerationY,
        std::min(settings_.maxAccelerationCountsPerSecond2,
                 settings_.maxJerkCountsPerSecond3 * dt)) || finalAccelerationLimited;

    double newVelocityX = previousVelocityX + newAccelerationX * dt;
    double newVelocityY = previousVelocityY + newAccelerationY * dt;
    bool finalVelocityLimited =
        std::hypot(newVelocityX, newVelocityY) > settings_.maxVelocityCountsPerSecond;
    if (finalVelocityLimited)
    {
        // previousVelocity已在速度球内，零加速度可行；在[0,1]上二分最大可行比例，
        // 使保存的加速度与实际速度差分保持一致，而不是只投影速度后留下伪加速度。
        double lower = 0.0;
        double upper = 1.0;
        for (int iteration = 0; iteration < 60; ++iteration)
        {
            const double scale = (lower + upper) * 0.5;
            const double candidateVelocityX =
                previousVelocityX + newAccelerationX * scale * dt;
            const double candidateVelocityY =
                previousVelocityY + newAccelerationY * scale * dt;
            if (std::hypot(candidateVelocityX, candidateVelocityY) <=
                settings_.maxVelocityCountsPerSecond)
                lower = scale;
            else
                upper = scale;
        }
        newAccelerationX *= lower;
        newAccelerationY *= lower;
        newVelocityX = previousVelocityX + newAccelerationX * dt;
        newVelocityY = previousVelocityY + newAccelerationY * dt;
    }
    state_.velocityCountsPerSecX = newVelocityX;
    state_.velocityCountsPerSecY = newVelocityY;
    state_.accelerationCountsPerSec2X = newAccelerationX;
    state_.accelerationCountsPerSec2Y = newAccelerationY;
    state_.jerkCountsPerSec3X = (newAccelerationX - previousAccelerationX) / dt;
    state_.jerkCountsPerSec3Y = (newAccelerationY - previousAccelerationY) / dt;

    // 梯形积分使用周期首尾速度的平均值，避免只取末速度造成系统性位移高估。
    const double shapedCountsX = (previousVelocityX + newVelocityX) * 0.5 * dt;
    const double shapedCountsY = (previousVelocityY + newVelocityY) * 0.5 * dt;
    Result result = buildResult(shapedCountsX, shapedCountsY, tickTime);
    result.output.requestSequence = request.sequence;
    result.output.velocityLimited = targetVelocityLimited || finalVelocityLimited;
    result.output.accelerationLimited = accelerationLimited || finalAccelerationLimited;
    result.output.jerkLimited = jerkLimited;
    return result;
}

CommandTrajectoryShaper::Result CommandTrajectoryShaper::emergencyReset(
    FrameTiming::Clock::time_point tickTime)
{
    reset();
    Result result = buildResult(0.0, 0.0, tickTime);
    result.output.emergencyReset = true;
    return result;
}

void CommandTrajectoryShaper::reset()
{
    state_ = {};
    quantizationRemainderX_ = 0.0;
    quantizationRemainderY_ = 0.0;
}

CommandTrajectoryShaper::Result CommandTrajectoryShaper::buildResult(
    double shapedCountsX,
    double shapedCountsY,
    FrameTiming::Clock::time_point tickTime)
{
    state_.positionCountsX += shapedCountsX;
    state_.positionCountsY += shapedCountsY;
    const double quantizedInputX = shapedCountsX + quantizationRemainderX_;
    const double quantizedInputY = shapedCountsY + quantizationRemainderY_;
    const int outputCountsX = static_cast<int>(std::round(quantizedInputX));
    const int outputCountsY = static_cast<int>(std::round(quantizedInputY));
    quantizationRemainderX_ = quantizedInputX - outputCountsX;
    quantizationRemainderY_ = quantizedInputY - outputCountsY;

    Result result;
    result.state = state_;
    result.output.mode = settings_.mode;
    result.output.outputProduced = true;
    result.output.shapedCountsX = shapedCountsX;
    result.output.shapedCountsY = shapedCountsY;
    result.output.quantizationRemainderX = quantizationRemainderX_;
    result.output.quantizationRemainderY = quantizationRemainderY_;
    result.output.outputCountsX = outputCountsX;
    result.output.outputCountsY = outputCountsY;
    result.output.commandSuppressed = true;
    result.output.scheduledTickTime = tickTime;
    result.output.outputTickTime = tickTime;
    return result;
}
