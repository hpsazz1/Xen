#include "runtime/cross_domain_replay.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <sstream>

#include "runtime/aim_coordinate_space.h"
#include "runtime/basic_aim_controller.h"
#include "runtime/basic_target_filter.h"
#include "runtime/command_trajectory_shaper.h"
#include "runtime/los_aim_controller.h"
#include "runtime/relative_los_kalman.h"
#include "runtime/target_predictor.h"

namespace CrossDomainReplay
{
namespace
{
using Clock = std::chrono::steady_clock;

double percentile(std::vector<double> values, double ratio)
{
    if (values.empty())
        return 0.0;
    std::sort(values.begin(), values.end());
    const size_t index = static_cast<size_t>(std::floor(
        std::clamp(ratio, 0.0, 1.0) * static_cast<double>(values.size() - 1)));
    return values[index];
}

double interpolate(const SourceTrajectory& source, double time, bool horizontal)
{
    if (source.points.empty())
        return horizontal ? source.centerX : source.centerY;
    const auto after = std::upper_bound(source.points.begin(), source.points.end(), time,
        [](double value, const VideoReplay::TrajectoryPoint& point) {
            return value < point.timeSeconds;
        });
    if (after == source.points.begin())
        return horizontal ? after->globalX : after->globalY;
    const auto& first = *std::prev(after);
    if (after == source.points.end())
        return horizontal ? first.globalX : first.globalY;
    const auto& second = *after;
    const double span = second.timeSeconds - first.timeSeconds;
    const double alpha = span > 0.0 ? std::clamp((time - first.timeSeconds) / span, 0.0, 1.0) : 0.0;
    const double a = horizontal ? first.globalX : first.globalY;
    const double b = horizontal ? second.globalX : second.globalY;
    return a + (b - a) * alpha;
}

const VideoReplay::TrajectoryPoint& nearestPoint(const SourceTrajectory& source, double time)
{
    const auto after = std::lower_bound(source.points.begin(), source.points.end(), time,
        [](const VideoReplay::TrajectoryPoint& point, double value) {
            return point.timeSeconds < value;
        });
    if (after == source.points.begin())
        return *after;
    if (after == source.points.end())
        return source.points.back();
    return time - std::prev(after)->timeSeconds <= after->timeSeconds - time
        ? *std::prev(after) : *after;
}

AimCoordinateSpace::LosAngles targetAngles(
    const SourceTrajectory& source, const Variant& variant, double sourceTime)
{
    // 分辨率只改变采样密度；同一归一化屏幕轨迹在不同FOV下重新投影为角度。
    const double normalizedX = (interpolate(source, sourceTime, true) - source.centerX) /
        std::max(1.0, static_cast<double>(source.sourceWidth));
    const double normalizedY = (interpolate(source, sourceTime, false) - source.centerY) /
        std::max(1.0, static_cast<double>(source.sourceHeight));
    auto angles = AimCoordinateSpace::pixelOffsetToLosAngles(
        normalizedX * variant.sourceWidth,
        normalizedY * variant.sourceHeight,
        variant.fovXDegrees, variant.fovYDegrees,
        variant.sourceWidth, variant.sourceHeight);
    angles.yawDegrees *= variant.relativeMotionScale;
    angles.pitchDownDegrees *= variant.relativeMotionScale;
    return angles;
}

struct AppliedCommand
{
    double effectiveTime = 0.0;
    double yaw = 0.0;
    double pitch = 0.0;
};

struct Camera
{
    std::deque<AppliedCommand> pending;
    double yaw = 0.0;
    double pitch = 0.0;

    void apply(double time)
    {
        while (!pending.empty() && pending.front().effectiveTime <= time + 1e-12)
        {
            yaw += pending.front().yaw;
            pitch += pending.front().pitch;
            pending.pop_front();
        }
    }

    void submit(double time, double delaySeconds, int x, int y,
                const ControllerSettings& settings)
    {
        pending.push_back({ time + delaySeconds,
            x * settings.degreesPerCountX,
            y * settings.degreesPerCountY });
    }

    void clearPending()
    {
        pending.clear();
    }
};

struct MetricCollector
{
    std::vector<double> errors;
    std::vector<double> verticalErrors;
    std::vector<double> margins;
    std::vector<double> lateErrors;
    size_t inside = 0;
    size_t movingSamples = 0;
    size_t interruptions = 0;
    int previousOutputX = 0;
    int previousOutputY = 0;
    bool hasPreviousOutput = false;
    int previousRateSign = 0;
    double nisSum = 0.0;
    double covarianceSum = 0.0;
    double confidenceSum = 0.0;
    size_t diagnosticSamples = 0;
    size_t settledSamples = 0;
    size_t reverseSuppressedSamples = 0;
    size_t verticalCatchUpSamples = 0;
    size_t trajectoryVelocityLimitedSamples = 0;
    size_t trajectoryAccelerationLimitedSamples = 0;
    size_t trajectoryJerkLimitedSamples = 0;
    Metrics result;

    void addError(double errorX, double errorY, double halfBoxX, double halfBoxY, bool late)
    {
        const double magnitude = std::hypot(errorX, errorY);
        errors.push_back(magnitude);
        verticalErrors.push_back(std::abs(errorY));
        const double margin = std::min(halfBoxX - std::abs(errorX), halfBoxY - std::abs(errorY));
        margins.push_back(margin);
        if (margin >= 0.0)
            ++inside;
        if (late)
            lateErrors.push_back(magnitude);
    }

    void addOutput(int x, int y, bool moving)
    {
        const bool nonZero = x != 0 || y != 0;
        // 二维方向翻转必须是相邻非零向量的点积为负；仅因主轴从X切到Y，
        // 或某一轴跨零但总体仍同向，不能误报为控制器反转。
        if (nonZero && hasPreviousOutput &&
            x * previousOutputX + y * previousOutputY < 0)
            ++result.outputDirectionFlips;
        if (nonZero)
        {
            previousOutputX = x;
            previousOutputY = y;
            hasPreviousOutput = true;
        }
        if (moving)
        {
            ++movingSamples;
            if (x == 0 && y == 0)
                ++interruptions;
        }
        result.sentCounts += std::hypot(static_cast<double>(x), static_cast<double>(y));
    }

    void addEstimate(double rateX, double rateY, double truthRateX, double truthRateY,
                     double nis, double covariance, double confidence)
    {
        const double truth = std::abs(truthRateX) >= std::abs(truthRateY) ? truthRateX : truthRateY;
        const double estimate = std::abs(truthRateX) >= std::abs(truthRateY) ? rateX : rateY;
        const int truthSign = (truth > 0.05) - (truth < -0.05);
        const int rateSign = (estimate > 0.05) - (estimate < -0.05);
        if (truthSign != 0 && rateSign != 0 && truthSign != rateSign)
            ++result.estimatedDirectionErrors;
        if (rateSign != 0 && previousRateSign != 0 && rateSign != previousRateSign)
            ++result.estimatedRateSignFlips;
        if (rateSign != 0)
            previousRateSign = rateSign;
        nisSum += nis;
        covarianceSum += covariance;
        confidenceSum += confidence;
        ++diagnosticSamples;
    }

    void addSettle(bool settled, bool released)
    {
        settledSamples += settled ? 1U : 0U;
        result.settleReleases += released ? 1U : 0U;
    }

    void addReverseSuppression(bool suppressed)
    {
        reverseSuppressedSamples += suppressed ? 1U : 0U;
    }

    void addVerticalCatchUp(bool active)
    {
        verticalCatchUpSamples += active ? 1U : 0U;
    }

    void addTrajectoryOutput(const TrajectoryOutput& output)
    {
        ++result.trajectoryOutputs;
        trajectoryVelocityLimitedSamples += output.velocityLimited ? 1U : 0U;
        trajectoryAccelerationLimitedSamples += output.accelerationLimited ? 1U : 0U;
        trajectoryJerkLimitedSamples += output.jerkLimited ? 1U : 0U;
    }

    Metrics finish()
    {
        result.samples = errors.size();
        result.errorP50Degrees = percentile(errors, 0.50);
        result.errorP95Degrees = percentile(errors, 0.95);
        result.errorP99Degrees = percentile(errors, 0.99);
        result.verticalP95Degrees = percentile(verticalErrors, 0.95);
        result.insideBoxPercent = errors.empty() ? 0.0 : 100.0 * inside / errors.size();
        result.edgeMarginP05Degrees = percentile(margins, 0.05);
        result.interruptionPercent = movingSamples == 0 ? 0.0 : 100.0 * interruptions / movingSamples;
        result.lateHalfErrorP95Degrees = percentile(lateErrors, 0.95);
        if (diagnosticSamples > 0)
        {
            result.meanNis = nisSum / diagnosticSamples;
            result.meanCovariance = covarianceSum / diagnosticSamples;
            result.meanFeedforwardConfidence = confidenceSum / diagnosticSamples;
            result.settledPercent = 100.0 * settledSamples / diagnosticSamples;
            result.reverseSuppressedPercent =
                100.0 * reverseSuppressedSamples / diagnosticSamples;
            result.verticalCatchUpPercent =
                100.0 * verticalCatchUpSamples / diagnosticSamples;
        }
        if (result.trajectoryOutputs > 0)
        {
            result.trajectoryVelocityLimitedPercent =
                100.0 * trajectoryVelocityLimitedSamples / result.trajectoryOutputs;
            result.trajectoryAccelerationLimitedPercent =
                100.0 * trajectoryAccelerationLimitedSamples / result.trajectoryOutputs;
            result.trajectoryJerkLimitedPercent =
                100.0 * trajectoryJerkLimitedSamples / result.trajectoryOutputs;
        }
        return result;
    }
};

std::string canonicalScenario(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (value.find("static") != std::string::npos) return "static";
    if (value.find("reverse") != std::string::npos) return "reverse";
    if (value.find("jump") != std::string::npos) return "jump";
    if (value.find("left") != std::string::npos) return "left";
    if (value.find("right") != std::string::npos) return "right";
    return value;
}
}

std::vector<Variant> BuildRequiredVariants()
{
    std::vector<Variant> result;
    for (const double fov : { 90.0, 106.0, 120.0 })
    for (const auto resolution : { std::pair<int, int>{1920, 1080}, {2560, 1440} })
    for (const double fps : { 60.0, 94.0, 144.0 })
    for (const double delay : { 10.0, 20.0, 40.0 })
    for (const double speed : { 0.75, 1.0, 1.5 })
    {
        Variant variant;
        std::ostringstream name;
        name << "domain_fov" << static_cast<int>(fov) << '_' << resolution.first << 'x'
             << resolution.second << '_' << static_cast<int>(fps) << "fps_"
             << static_cast<int>(delay) << "ms_" << speed << 'x';
        variant.name = name.str();
        variant.sourceWidth = resolution.first;
        variant.sourceHeight = resolution.second;
        variant.fovXDegrees = fov;
        variant.fovYDegrees = fov * 74.0 / 106.0;
        variant.replayFps = fps;
        variant.observationDelayMs = delay;
        variant.speedScale = speed;
        result.push_back(variant);
    }
    for (const auto& role : {
        std::pair<const char*, double>{"role_same_target_faster", 0.5},
        {"role_same_equal", 0.05}, {"role_same_self_faster", -0.5},
        {"role_opposite", 1.5}, {"role_forward_left", 0.75},
        {"role_forward_center", 0.10}, {"role_forward_right", 0.75} })
    {
        Variant variant;
        variant.name = role.first;
        variant.relativeMotionScale = role.second;
        result.push_back(variant);
    }
    return result;
}

Comparison RunComparison(const SourceTrajectory& source, const Variant& variant,
                         const ControllerSettings& settings,
                         const std::filesystem::path& frameCsv)
{
    Comparison comparison;
    comparison.scenario = canonicalScenario(source.scenario);
    comparison.variant = variant;
    comparison.trajectoryMode = settings.trajectoryMode;
    // 回放参数可能来自命令行；非有限值回退到生产默认频率，有限值再限制到可验证范围。
    comparison.trajectoryOutputHz = std::isfinite(settings.trajectoryOutputHz)
        ? std::clamp(settings.trajectoryOutputHz, 30.0, 1000.0)
        : 240.0;
    if (source.points.empty())
    {
        comparison.reason = "empty trajectory";
        return comparison;
    }

    BasicTargetFilter legacyFilter;
    TargetPredictor legacyPredictor;
    BasicAimController legacyController;
    TargetPredictor::Settings predictorSettings;
    predictorSettings.enabled = true;
    predictorSettings.additionalLeadSeconds = settings.legacyPredictionLeadSeconds;
    predictorSettings.velocityTimeConstantSeconds = settings.legacyPredictionWindowSeconds;
    predictorSettings.predictionStrength = settings.legacyPredictionStrength;
    BasicAimController::Settings legacySettings;
    legacySettings.responseSeconds = settings.responseSeconds;
    legacySettings.maxCountsPerSecond = settings.maxCountsPerSecond;
    legacySettings.integralTimeSeconds = 0.0;
    legacySettings.settleRadiusPixels = 0.0;
    legacySettings.releaseRadiusPixels = 0.0;

    RelativeLosKalman kalman;
    LosAimController candidateController;
    LosAimController::Settings candidateSettings;
    candidateSettings.responseSeconds = settings.responseSeconds;
    candidateSettings.verticalCatchUpErrorDegrees =
        settings.verticalCatchUpErrorDegrees;
    candidateSettings.maxCountsPerSecond = settings.maxCountsPerSecond;
    candidateSettings.feedforwardGain = settings.feedforwardGain;
    candidateSettings.integralTimeSeconds = settings.integralTimeSeconds;
    candidateSettings.integralZoneDegrees = settings.integralZoneDegrees;
    candidateSettings.settleErrorDegrees = settings.settleErrorDegrees;
    candidateSettings.settleRateDegreesPerSecond = settings.settleRateDegreesPerSecond;
    candidateSettings.reverseConfirmationSeconds = settings.reverseConfirmationSeconds;
    CommandTrajectoryShaper shaper;
    CommandTrajectoryShaper::Settings shaperSettings;
    shaperSettings.mode = settings.trajectoryMode;
    shaperSettings.maxVelocityCountsPerSecond = settings.maxCountsPerSecond;
    shaperSettings.maxAccelerationCountsPerSecond2 =
        settings.trajectoryMaxAccelerationCountsPerSecond2;
    shaperSettings.maxJerkCountsPerSecond3 =
        settings.trajectoryMaxJerkCountsPerSecond3;
    shaper.configure(shaperSettings);

    Camera legacyCamera;
    Camera candidateCamera;
    MetricCollector legacyMetrics;
    MetricCollector candidateMetrics;
    std::ofstream frames;
    if (!frameCsv.empty())
    {
        std::filesystem::create_directories(frameCsv.parent_path());
        frames.open(frameCsv, std::ios::app);
        if (frames.tellp() == 0)
            frames << "Scenario,Variant,TimeSeconds,Detected,TruthYaw,TruthPitch,LegacyErrorX,LegacyErrorY,CandidateErrorX,CandidateErrorY,EstimateRateX,EstimateRateY,InnovationX,InnovationY,NisX,NisY,CovarianceX,CovarianceY,FeedforwardConfidence,Settled,SettleReleased,SettleConfirmationSamples,LowSpeedReverseSuppressed,VerticalCatchUpActive,ReverseConfirmationSeconds,EffectiveResponseSecondsY,FeedbackX,FeedbackY,FeedforwardX,FeedforwardY,LeadX,LeadY,IntegralX,IntegralY,RequestedX,RequestedY,ShapedX,ShapedY,SentX,SentY\n";
    }

    const double dt = 1.0 / std::clamp(variant.replayFps, 10.0, 1000.0);
    const double duration = source.points.back().timeSeconds /
        std::max(0.05, variant.speedScale);
    const double observationDelay = std::max(0.0, variant.observationDelayMs) / 1000.0;
    const double commandDelay = std::max(0.0, variant.commandToFrameDelayMs) / 1000.0;
    const auto epoch = Clock::time_point(std::chrono::seconds(1));
    AimCoordinateSpace::LosAngles previousTruth{};
    bool hasPreviousTruth = false;
    bool previousDetected = false;
    bool previousTruthMoving = false;
    uint64_t requestSequence = 0;
    const double trajectoryPeriod = 1.0 / comparison.trajectoryOutputHz;
    bool hasLatestTrajectoryRequest = false;
    double nextTrajectoryTickSeconds = 0.0;
    TrajectoryRequest latestTrajectoryRequest;

    // 离线对照显式遍历每个固定输出格点，而不是按观测帧调用一次后让调度器跳过中间tick。
    // 新请求只会从其到达后的首个格点生效；格点之间始终保持latest-only目标速度。
    auto serviceTrajectoryBefore = [&](double endTimeSeconds, bool includeEnd)
    {
        CommandTrajectoryShaper::Result lastResult;
        bool produced = false;
        if (settings.trajectoryMode != TrajectoryShaperMode::Trapezoid ||
            !hasLatestTrajectoryRequest)
            return std::pair{ produced, lastResult };
        const double epsilon = 1e-9;
        while (nextTrajectoryTickSeconds < endTimeSeconds - epsilon ||
               (includeEnd && nextTrajectoryTickSeconds <= endTimeSeconds + epsilon))
        {
            const auto tickPoint = epoch + std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(nextTrajectoryTickSeconds));
            lastResult = shaper.update(
                latestTrajectoryRequest, trajectoryPeriod, tickPoint);
            candidateCamera.submit(nextTrajectoryTickSeconds, commandDelay,
                lastResult.output.outputCountsX,
                lastResult.output.outputCountsY, settings);
            candidateMetrics.addOutput(
                lastResult.output.outputCountsX,
                lastResult.output.outputCountsY,
                previousTruthMoving);
            candidateMetrics.addTrajectoryOutput(lastResult.output);
            candidateMetrics.result.shapedCounts += std::hypot(
                lastResult.output.shapedCountsX,
                lastResult.output.shapedCountsY);
            produced = true;
            nextTrajectoryTickSeconds += trajectoryPeriod;
        }
        return std::pair{ produced, lastResult };
    };

    for (double observationTime = 0.0; observationTime <= duration + 1e-9; observationTime += dt)
    {
        const double controlTime = observationTime + observationDelay;
        legacyCamera.apply(observationTime);
        candidateCamera.apply(observationTime);
        const double sourceTime = std::min(source.points.back().timeSeconds,
            observationTime * variant.speedScale);
        const double controlSourceTime = std::min(source.points.back().timeSeconds,
            controlTime * variant.speedScale);
        const auto truthAtObservation = targetAngles(source, variant, sourceTime);
        const auto truthAtControl = targetAngles(source, variant, controlSourceTime);
        const auto& point = nearestPoint(source, sourceTime);
        const double cropWidth = source.detectionWidth * variant.sourceWidth /
            std::max(1, source.sourceWidth);
        const double cropHeight = source.detectionHeight * variant.sourceHeight /
            std::max(1, source.sourceHeight);
        const auto legacyRelativePixels = AimCoordinateSpace::losAnglesToPixelOffset(
            truthAtObservation.yawDegrees - legacyCamera.yaw,
            truthAtObservation.pitchDownDegrees - legacyCamera.pitch,
            variant.fovXDegrees, variant.fovYDegrees,
            variant.sourceWidth, variant.sourceHeight);
        const auto candidateRelativePixels = AimCoordinateSpace::losAnglesToPixelOffset(
            truthAtObservation.yawDegrees - candidateCamera.yaw,
            truthAtObservation.pitchDownDegrees - candidateCamera.pitch,
            variant.fovXDegrees, variant.fovYDegrees,
            variant.sourceWidth, variant.sourceHeight);
        const bool bothInsideDetectionCrop =
            std::abs(legacyRelativePixels.x) < cropWidth * 0.5 &&
            std::abs(legacyRelativePixels.y) < cropHeight * 0.5 &&
            std::abs(candidateRelativePixels.x) < cropWidth * 0.5 &&
            std::abs(candidateRelativePixels.y) < cropHeight * 0.5;
        const bool detected = point.detected && bothInsideDetectionCrop &&
            std::abs(point.timeSeconds - sourceTime) <= std::max(0.05, dt * variant.speedScale * 1.5);
        if (!detected)
        {
            if (previousDetected)
            {
                legacyFilter.reset();
                legacyPredictor.reset();
                legacyController.reset();
                kalman.reset();
                candidateController.reset();
                shaper.emergencyReset(epoch + std::chrono::duration_cast<Clock::duration>(
                    std::chrono::duration<double>(controlTime)));
                hasLatestTrajectoryRequest = false;
                nextTrajectoryTickSeconds = 0.0;
                // 新执行层的紧急停止语义包含清除尚未生效的旧命令，避免丢失后继续转动。
                candidateCamera.clearPending();
            }
            legacyMetrics.addOutput(0, 0, false);
            candidateMetrics.addOutput(0, 0, false);
            previousDetected = false;
            continue;
        }
        previousDetected = true;
        serviceTrajectoryBefore(controlTime, false);
        legacyCamera.apply(controlTime);
        candidateCamera.apply(controlTime);

        const double sourcePixelX = AimCoordinateSpace::sourcePixelDeltaForAngleDegrees(
            truthAtObservation.yawDegrees, variant.fovXDegrees, variant.sourceWidth);
        const double sourcePixelY = AimCoordinateSpace::sourcePixelDeltaForAngleDegrees(
            truthAtObservation.pitchDownDegrees, variant.fovYDegrees, variant.sourceHeight);
        const auto observationPoint = epoch + std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(observationTime));
        const auto controlPoint = epoch + std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(controlTime));
        const auto filtered = legacyFilter.update(sourcePixelX, sourcePixelY, observationPoint, dt,
            static_cast<double>(variant.sourceWidth));
        const auto predicted = legacyPredictor.update(filtered.x, filtered.y, observationPoint,
            controlPoint, static_cast<double>(variant.sourceWidth), predictorSettings);
        const auto predictedAngles = AimCoordinateSpace::pixelOffsetToLosAngles(
            predicted.x, predicted.y, variant.fovXDegrees, variant.fovYDegrees,
            variant.sourceWidth, variant.sourceHeight);
        const auto legacyOutput = legacyController.update(
            AimCoordinateSpace::sourcePixelDeltaForAngleDegrees(
                predictedAngles.yawDegrees - legacyCamera.yaw, variant.fovXDegrees, variant.sourceWidth),
            AimCoordinateSpace::sourcePixelDeltaForAngleDegrees(
                predictedAngles.pitchDownDegrees - legacyCamera.pitch, variant.fovYDegrees, variant.sourceHeight),
            dt,
            settings.degreesPerCountX == 0.0 ? 1.0 :
                AimCoordinateSpace::countsPerSourcePixel(variant.fovXDegrees, variant.sourceWidth,
                    settings.degreesPerCountX),
            settings.degreesPerCountY == 0.0 ? 1.0 :
                AimCoordinateSpace::countsPerSourcePixel(variant.fovYDegrees, variant.sourceHeight,
                    settings.degreesPerCountY),
            legacySettings);
        const int legacyX = static_cast<int>(std::round(legacyOutput.countsX));
        const int legacyY = static_cast<int>(std::round(legacyOutput.countsY));
        legacyCamera.submit(controlTime, commandDelay, legacyX, legacyY, settings);

        kalman.update(truthAtObservation.yawDegrees, truthAtObservation.pitchDownDegrees,
            point.confidence, observationPoint, controlPoint);
        const auto& estimate = kalman.estimate();
        LosAimController::Input input;
        input.valid = estimate.x.valid && estimate.y.valid;
        input.errorDegreesX = estimate.x.angleDegrees - candidateCamera.yaw;
        input.errorDegreesY = estimate.y.angleDegrees - candidateCamera.pitch;
        input.relativeLosRateDegreesPerSecondX = estimate.x.rateDegreesPerSecond;
        input.relativeLosRateDegreesPerSecondY = estimate.y.rateDegreesPerSecond;
        input.feedforwardConfidence = estimate.feedforwardConfidence;
        input.degreesPerCountX = settings.degreesPerCountX;
        input.degreesPerCountY = settings.degreesPerCountY;
        input.dtSeconds = dt;
        const auto candidateOutput = candidateController.update(input, candidateSettings);
        TrajectoryRequest request;
        request.valid = candidateOutput.valid;
        request.sequence = ++requestSequence;
        request.requestDurationSeconds = dt;
        request.requestedCountsX = candidateOutput.limitedCountsX;
        request.requestedCountsY = candidateOutput.limitedCountsY;
        request.requestTime = controlPoint;
        CommandTrajectoryShaper::Result shaped;
        if (settings.trajectoryMode == TrajectoryShaperMode::Off)
        {
            shaped = shaper.update(request, dt, controlPoint);
            candidateCamera.submit(controlTime, commandDelay,
                shaped.output.outputCountsX, shaped.output.outputCountsY, settings);
            candidateMetrics.addTrajectoryOutput(shaped.output);
        }
        else
        {
            latestTrajectoryRequest = request;
            if (!hasLatestTrajectoryRequest)
            {
                hasLatestTrajectoryRequest = true;
                nextTrajectoryTickSeconds = controlTime;
            }
            const auto scheduled = serviceTrajectoryBefore(controlTime, true);
            if (scheduled.first)
                shaped = scheduled.second;
            else
            {
                shaped.state = shaper.state();
                shaped.output.mode = settings.trajectoryMode;
            }
        }

        const double legacyErrorX = truthAtControl.yawDegrees - legacyCamera.yaw;
        const double legacyErrorY = truthAtControl.pitchDownDegrees - legacyCamera.pitch;
        const double candidateErrorX = truthAtControl.yawDegrees - candidateCamera.yaw;
        const double candidateErrorY = truthAtControl.pitchDownDegrees - candidateCamera.pitch;
        const double halfBoxX = AimCoordinateSpace::angleDegreesForSourcePixelDelta(
            std::max(1.0, point.boxWidth * variant.sourceWidth / source.sourceWidth) * 0.5,
            variant.fovXDegrees, variant.sourceWidth);
        const double halfBoxY = AimCoordinateSpace::angleDegreesForSourcePixelDelta(
            std::max(1.0, point.boxHeight * variant.sourceHeight / source.sourceHeight) * 0.5,
            variant.fovYDegrees, variant.sourceHeight);
        const bool late = observationTime >= duration * 0.5;
        legacyMetrics.addError(legacyErrorX, legacyErrorY, halfBoxX, halfBoxY, late);
        candidateMetrics.addError(candidateErrorX, candidateErrorY, halfBoxX, halfBoxY, late);
        const double truthRateX = hasPreviousTruth ?
            (truthAtObservation.yawDegrees - previousTruth.yawDegrees) / dt : 0.0;
        const double truthRateY = hasPreviousTruth ?
            (truthAtObservation.pitchDownDegrees - previousTruth.pitchDownDegrees) / dt : 0.0;
        const bool moving = std::hypot(truthRateX, truthRateY) > 0.5;
        legacyMetrics.addOutput(legacyX, legacyY, moving);
        if (settings.trajectoryMode == TrajectoryShaperMode::Off)
            candidateMetrics.addOutput(
                shaped.output.outputCountsX, shaped.output.outputCountsY, moving);
        previousTruthMoving = moving;
        candidateMetrics.addEstimate(estimate.x.rateDegreesPerSecond, estimate.y.rateDegreesPerSecond,
            truthRateX, truthRateY, std::max(estimate.x.nis, estimate.y.nis),
            std::max(estimate.x.angleVariance, estimate.y.angleVariance),
            estimate.feedforwardConfidence);
        candidateMetrics.addSettle(
            candidateOutput.settled, candidateOutput.settleReleased);
        candidateMetrics.addReverseSuppression(
            candidateOutput.lowSpeedReverseSuppressed);
        candidateMetrics.addVerticalCatchUp(candidateOutput.verticalCatchUpActive);
        candidateMetrics.result.requestedCounts += std::hypot(
            candidateOutput.limitedCountsX, candidateOutput.limitedCountsY);
        candidateMetrics.result.feedforwardCounts += std::hypot(
            candidateOutput.trackingFeedforwardCountsX,
            candidateOutput.trackingFeedforwardCountsY);
        if (settings.trajectoryMode == TrajectoryShaperMode::Off)
            candidateMetrics.result.shapedCounts += std::hypot(
                shaped.output.shapedCountsX, shaped.output.shapedCountsY);
        legacyMetrics.result.requestedCounts += std::hypot(legacyOutput.countsX, legacyOutput.countsY);
        legacyMetrics.result.shapedCounts = legacyMetrics.result.requestedCounts;

        if (frames)
        {
            frames << comparison.scenario << ',' << variant.name << ',' << std::fixed
                   << std::setprecision(6) << controlTime << ",1," << truthAtControl.yawDegrees << ','
                   << truthAtControl.pitchDownDegrees << ',' << legacyErrorX << ',' << legacyErrorY << ','
                   << candidateErrorX << ',' << candidateErrorY << ',' << estimate.x.rateDegreesPerSecond << ','
                   << estimate.y.rateDegreesPerSecond << ',' << estimate.x.innovationDegrees << ','
                   << estimate.y.innovationDegrees << ',' << estimate.x.nis << ',' << estimate.y.nis << ','
                   << estimate.x.angleVariance << ',' << estimate.y.angleVariance << ','
                   << estimate.feedforwardConfidence << ',' << (candidateOutput.settled ? 1 : 0) << ','
                   << (candidateOutput.settleReleased ? 1 : 0) << ','
                   << candidateOutput.settleConfirmationSamples << ','
                   << (candidateOutput.lowSpeedReverseSuppressed ? 1 : 0) << ','
                   << (candidateOutput.verticalCatchUpActive ? 1 : 0) << ','
                   << candidateOutput.reverseConfirmationSeconds << ','
                   << candidateOutput.effectiveResponseSecondsY << ','
                   << candidateOutput.feedbackCountsX << ','
                   << candidateOutput.feedbackCountsY << ',' << candidateOutput.trackingFeedforwardCountsX << ','
                   << candidateOutput.trackingFeedforwardCountsY << ',' << candidateOutput.leadCountsX << ','
                   << candidateOutput.leadCountsY << ',' << candidateOutput.integralCountsX << ','
                   << candidateOutput.integralCountsY << ',' << candidateOutput.limitedCountsX << ','
                   << candidateOutput.limitedCountsY << ',' << shaped.output.shapedCountsX << ','
                   << shaped.output.shapedCountsY << ',' << shaped.output.outputCountsX << ','
                   << shaped.output.outputCountsY << '\n';
        }
        previousTruth = truthAtObservation;
        hasPreviousTruth = true;
    }

    comparison.legacy = legacyMetrics.finish();
    comparison.candidate = candidateMetrics.finish();
    comparison.passed = EvaluateGate(comparison.scenario, variant,
        comparison.legacy, comparison.candidate, comparison.reason);
    return comparison;
}

bool EvaluateGate(const std::string& scenario, const Variant& variant,
                  const Metrics& legacy, const Metrics& candidate,
                  std::string& reason)
{
    if (legacy.samples == 0 || candidate.samples == 0)
    {
        reason = "no valid samples";
        return false;
    }
    auto fail = [&reason](const char* value) { reason = value; return false; };
    if (scenario == "static")
    {
        if (candidate.errorP95Degrees > legacy.errorP95Degrees * 1.05 + 1e-6)
            return fail("static P95 regressed over 5%");
        if (candidate.outputDirectionFlips > legacy.outputDirectionFlips)
            return fail("static output direction flips increased");
    }
    else if (scenario == "left" || scenario == "right")
    {
        if (candidate.errorP95Degrees > legacy.errorP95Degrees + 1e-6)
            return fail("unidirectional P95 regressed");
        if (candidate.interruptionPercent > legacy.interruptionPercent + 5.0)
            return fail("control interruptions increased over 5 points");
    }
    else if (scenario == "reverse")
    {
        const bool accuracy = candidate.errorP95Degrees <= legacy.errorP95Degrees * 0.90;
        const bool inside = candidate.insideBoxPercent >= legacy.insideBoxPercent + 10.0;
        if (!accuracy && !inside)
            return fail("reverse neither improved P95 by 10% nor inside-box by 10 points");
        if (candidate.lateHalfErrorP95Degrees > legacy.lateHalfErrorP95Degrees * 1.05 + 1e-6)
            return fail("reverse late half slowed down");
    }
    else if (scenario == "jump")
    {
        if (candidate.verticalP95Degrees > legacy.verticalP95Degrees * 0.90 + 1e-6)
            return fail("jump vertical P95 did not improve 10%");
        if (candidate.nonZeroOutputsAfterLoss > 1)
            return fail("output persisted after target loss");
    }
    if (variant.name.find("role_same_equal") != std::string::npos &&
        candidate.feedforwardCounts > candidate.sentCounts * 0.05 + 1e-6)
        return fail("equal-speed relative motion did not naturally collapse");
    if (legacy.errorP95Degrees > 1e-6 &&
        candidate.errorP95Degrees > legacy.errorP95Degrees * 10.0)
        return fail("cross-domain error degraded by an order of magnitude");
    reason = "passed";
    return true;
}

void WriteSummary(const std::filesystem::path& path,
                  const std::vector<Comparison>& comparisons)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    output << "Scenario,Variant,TrajectoryMode,TrajectoryOutputHz,Samples,LegacyP50Deg,LegacyP95Deg,LegacyP99Deg,CandidateP50Deg,CandidateP95Deg,CandidateP99Deg,LegacyVerticalP95Deg,CandidateVerticalP95Deg,LegacyInsideBoxPercent,CandidateInsideBoxPercent,LegacyEdgeMarginP05Deg,CandidateEdgeMarginP05Deg,LegacyInterruptionPercent,CandidateInterruptionPercent,LegacyOutputFlips,CandidateOutputFlips,EstimateDirectionErrors,EstimateRateSignFlips,MeanNis,MeanCovariance,MeanFeedforwardConfidence,RequestedCounts,ShapedCounts,SentCounts,FeedforwardCounts,SettledPercent,SettleReleases,ReverseSuppressedPercent,VerticalCatchUpPercent,TrajectoryOutputs,TrajectoryVelocityLimitedPercent,TrajectoryAccelerationLimitedPercent,TrajectoryJerkLimitedPercent,Passed,Reason\n";
    output << std::fixed << std::setprecision(6);
    for (const auto& item : comparisons)
    {
        output << item.scenario << ',' << item.variant.name << ','
               << trajectoryShaperModeName(item.trajectoryMode) << ','
               << item.trajectoryOutputHz << ',' << item.candidate.samples << ','
               << item.legacy.errorP50Degrees << ',' << item.legacy.errorP95Degrees << ','
               << item.legacy.errorP99Degrees << ',' << item.candidate.errorP50Degrees << ','
               << item.candidate.errorP95Degrees << ',' << item.candidate.errorP99Degrees << ','
               << item.legacy.verticalP95Degrees << ',' << item.candidate.verticalP95Degrees << ','
               << item.legacy.insideBoxPercent << ',' << item.candidate.insideBoxPercent << ','
               << item.legacy.edgeMarginP05Degrees << ',' << item.candidate.edgeMarginP05Degrees << ','
               << item.legacy.interruptionPercent << ',' << item.candidate.interruptionPercent << ','
               << item.legacy.outputDirectionFlips << ',' << item.candidate.outputDirectionFlips << ','
               << item.candidate.estimatedDirectionErrors << ',' << item.candidate.estimatedRateSignFlips << ','
               << item.candidate.meanNis << ',' << item.candidate.meanCovariance << ','
               << item.candidate.meanFeedforwardConfidence << ',' << item.candidate.requestedCounts << ','
               << item.candidate.shapedCounts << ',' << item.candidate.sentCounts << ','
               << item.candidate.feedforwardCounts << ','
               << item.candidate.settledPercent << ',' << item.candidate.settleReleases << ','
               << item.candidate.reverseSuppressedPercent << ','
               << item.candidate.verticalCatchUpPercent << ','
               << item.candidate.trajectoryOutputs << ','
               << item.candidate.trajectoryVelocityLimitedPercent << ','
               << item.candidate.trajectoryAccelerationLimitedPercent << ','
               << item.candidate.trajectoryJerkLimitedPercent << ','
               << (item.passed ? 1 : 0) << ',' << item.reason << '\n';
    }
}
}
