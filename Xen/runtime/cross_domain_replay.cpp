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
#include "runtime/applied_view_motion_model.h"
#include "runtime/basic_aim_controller.h"
#include "runtime/basic_target_filter.h"
#include "runtime/command_trajectory_shaper.h"
#include "runtime/los_aim_controller.h"
#include "runtime/maneuver_los_estimator.h"
#include "runtime/relative_los_kalman.h"
#include "runtime/target_predictor.h"

namespace CrossDomainReplay
{
const char* candidateEstimatorModeName(CandidateEstimatorMode mode)
{
    if (mode == CandidateEstimatorMode::ConstantAcceleration)
        return "constant_acceleration";
    if (mode == CandidateEstimatorMode::ManeuverGatedConstantAcceleration)
        return "maneuver_gated_ca";
    if (mode == CandidateEstimatorMode::OracleControlTime)
        return "oracle_control_time";
    return "kalman";
}

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

AimCoordinateSpace::LosAngles targetAnglesAtSourcePosition(
    const SourceTrajectory& source, const Variant& variant,
    double globalX, double globalY)
{
    const double normalizedX = (globalX - source.centerX) /
        std::max(1.0, static_cast<double>(source.sourceWidth));
    const double normalizedY = (globalY - source.centerY) /
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
    AppliedViewMotionModel responseModel;
    Clock::time_point epoch = Clock::time_point(std::chrono::seconds(1));
    bool finiteResponse = false;
    double yaw = 0.0;
    double pitch = 0.0;

    void configure(double centerMs, double responseMs)
    {
        finiteResponse = responseMs > 1e-9;
        if (finiteResponse)
            responseModel.configure(centerMs, responseMs);
    }

    void apply(double time)
    {
        if (finiteResponse)
        {
            const auto point = epoch + std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(time));
            const auto view = responseModel.at(point);
            yaw = view.first;
            pitch = view.second;
            return;
        }
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
        if (finiteResponse)
        {
            const auto point = epoch + std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(time));
            responseModel.addCommand(
                x, y, settings.degreesPerCountX, settings.degreesPerCountY, point);
            return;
        }
        pending.push_back({ time + delaySeconds,
            x * settings.degreesPerCountX,
            y * settings.degreesPerCountY });
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
    size_t reversalFeedforwardSamples = 0;
    size_t verticalCatchUpSamples = 0;
    size_t maneuverModelSamples = 0;
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

    void addReversalFeedforward(bool active)
    {
        reversalFeedforwardSamples += active ? 1U : 0U;
    }

    void addVerticalCatchUp(bool active)
    {
        verticalCatchUpSamples += active ? 1U : 0U;
    }

    void addManeuverModel(bool active)
    {
        maneuverModelSamples += active ? 1U : 0U;
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
            result.reversalFeedforwardPercent =
                100.0 * reversalFeedforwardSamples / diagnosticSamples;
            result.verticalCatchUpPercent =
                100.0 * verticalCatchUpSamples / diagnosticSamples;
            result.maneuverModelPercent =
                100.0 * maneuverModelSamples / diagnosticSamples;
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
    return result;
}

Comparison RunComparison(const SourceTrajectory& source, const Variant& variant,
                         const ControllerSettings& settings,
                         const std::filesystem::path& frameCsv,
                         const std::vector<unsigned char>* frozenDetectionTimeline,
                         std::vector<unsigned char>* recordedDetectionTimeline)
{
    Comparison comparison;
    comparison.scenario = canonicalScenario(source.scenario);
    comparison.variant = variant;
    comparison.kalmanAccelerationStdDegreesPerSecond2 =
        std::isfinite(settings.kalmanAccelerationStdDegreesPerSecond2)
            ? std::clamp(settings.kalmanAccelerationStdDegreesPerSecond2,
                1.0, 2000.0) : 90.0;
    comparison.kalmanMovingAccelerationStdDegreesPerSecond2 =
        std::isfinite(settings.kalmanMovingAccelerationStdDegreesPerSecond2)
            ? std::clamp(settings.kalmanMovingAccelerationStdDegreesPerSecond2,
                1.0, 2000.0) : 360.0;
    comparison.kalmanMovingRateThresholdDegreesPerSecond =
        std::isfinite(settings.kalmanMovingRateThresholdDegreesPerSecond)
            ? std::clamp(settings.kalmanMovingRateThresholdDegreesPerSecond,
                0.1, 1000.0) : 8.0;
    comparison.legacyResponseSeconds = std::isfinite(settings.responseSeconds)
        ? std::clamp(settings.responseSeconds, 0.010, 0.500) : 0.080;
    comparison.candidateResponseSeconds =
        std::isfinite(settings.candidateResponseSeconds) &&
            settings.candidateResponseSeconds > 0.0
        ? std::clamp(settings.candidateResponseSeconds, 0.010, 0.500)
        : comparison.legacyResponseSeconds;
    comparison.candidateEstimatorMode = settings.candidateEstimatorMode;
    comparison.candidateJerkStdDegreesPerSecond3 =
        std::isfinite(settings.candidateJerkStdDegreesPerSecond3)
        ? std::clamp(settings.candidateJerkStdDegreesPerSecond3, 1.0, 100000.0)
        : 8000.0;
    comparison.candidateManeuverRateThresholdDegreesPerSecond =
        std::isfinite(settings.candidateManeuverRateThresholdDegreesPerSecond)
        ? std::clamp(settings.candidateManeuverRateThresholdDegreesPerSecond,
            0.1, 1000.0) : 12.0;
    comparison.candidateManeuverHoldSeconds =
        std::isfinite(settings.candidateManeuverHoldSeconds)
        ? std::clamp(settings.candidateManeuverHoldSeconds, 0.0, 1.0) : 0.120;
    const double baselineMaxCountsPerSecond =
        std::isfinite(settings.maxCountsPerSecond)
        ? std::clamp(settings.maxCountsPerSecond, 1.0, 100000.0) : 1440.0;
    comparison.candidateMaxCountsPerSecond =
        std::isfinite(settings.candidateMaxCountsPerSecond) &&
            settings.candidateMaxCountsPerSecond > 0.0
        ? std::clamp(settings.candidateMaxCountsPerSecond, 1.0, 100000.0)
        : baselineMaxCountsPerSecond;
    comparison.candidateIntegralTimeSeconds =
        std::isfinite(settings.integralTimeSeconds) &&
            settings.integralTimeSeconds > 0.0
        ? std::clamp(settings.integralTimeSeconds, 0.050, 2.0)
        : 0.0;
    comparison.candidateIntegralZoneDegrees =
        std::isfinite(settings.integralZoneDegrees)
        ? std::clamp(settings.integralZoneDegrees, 0.0, 10.0)
        : 1.0;
    comparison.feedforwardGain = std::isfinite(settings.feedforwardGain)
        ? std::clamp(settings.feedforwardGain, 0.0, 2.0)
        : 0.0;
    comparison.leadHorizonSeconds = std::isfinite(settings.leadHorizonSeconds)
        ? std::clamp(settings.leadHorizonSeconds, 0.0, 0.250) : 0.0;
    comparison.leadStrength = std::isfinite(settings.leadStrength)
        ? std::clamp(settings.leadStrength, 0.0, 4.0) : 0.0;
    comparison.reversalFeedforwardBoost =
        std::isfinite(settings.reversalFeedforwardBoost)
            ? std::clamp(settings.reversalFeedforwardBoost, 0.0, 2.0) : 0.0;
    comparison.reversalFeedforwardSeconds =
        std::isfinite(settings.reversalFeedforwardSeconds)
            ? std::clamp(settings.reversalFeedforwardSeconds, 0.0, 0.500) : 0.0;
    comparison.reverseConfirmationErrorMultiplier =
        std::isfinite(settings.reverseConfirmationErrorMultiplier)
            ? std::clamp(settings.reverseConfirmationErrorMultiplier, 1.5, 2.0) : 1.5;
    comparison.confirmLowSpeedReverseSettleRelease =
        settings.confirmLowSpeedReverseSettleRelease;
    comparison.staticFixedTruth = settings.staticFixedTruth;
    comparison.candidateViewMotionCompensation =
        settings.candidateViewMotionCompensation;
    comparison.candidateCommittedEndpointGuard =
        settings.candidateCommittedEndpointGuard &&
        settings.trajectoryMode == TrajectoryShaperMode::Off;
    comparison.candidateCommandCommitHorizonSeconds =
        std::isfinite(settings.candidateCommandCommitHorizonSeconds)
        ? std::clamp(settings.candidateCommandCommitHorizonSeconds, 0.0, 0.250)
        : 0.0;
    comparison.candidateSettleEntryCommandGuard =
        settings.candidateSettleEntryCommandGuard &&
        comparison.candidateCommandCommitHorizonSeconds > 0.0;
    comparison.candidateSettleEntryCommandHold =
        comparison.candidateSettleEntryCommandGuard &&
        settings.candidateSettleEntryCommandHold;
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

    const bool useStaticFixedTruth = comparison.staticFixedTruth &&
        canonicalScenario(source.scenario) == "static";
    AimCoordinateSpace::LosAngles staticTruthAngles{};
    if (useStaticFixedTruth)
    {
        std::vector<double> detectedX;
        std::vector<double> detectedY;
        for (const auto& point : source.points)
        {
            if (!point.detected)
                continue;
            detectedX.push_back(point.globalX);
            detectedY.push_back(point.globalY);
        }
        if (detectedX.empty())
        {
            for (const auto& point : source.points)
            {
                detectedX.push_back(point.globalX);
                detectedY.push_back(point.globalY);
            }
        }
        staticTruthAngles = targetAnglesAtSourcePosition(
            source, variant, percentile(detectedX, 0.5), percentile(detectedY, 0.5));
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
    legacySettings.responseSeconds = comparison.legacyResponseSeconds;
    legacySettings.maxCountsPerSecond = settings.maxCountsPerSecond;
    legacySettings.integralTimeSeconds = 0.0;
    legacySettings.settleRadiusPixels = 0.0;
    legacySettings.releaseRadiusPixels = 0.0;

    ManeuverLosEstimator estimator;
    ManeuverLosEstimator::Settings estimatorSettings;
    estimatorSettings.constantVelocitySettings.accelerationStdDegreesPerSecond2 =
        comparison.kalmanAccelerationStdDegreesPerSecond2;
    estimatorSettings.constantVelocitySettings.movingAccelerationStdDegreesPerSecond2 =
        comparison.kalmanMovingAccelerationStdDegreesPerSecond2;
    estimatorSettings.constantVelocitySettings.movingRateThresholdDegreesPerSecond =
        comparison.kalmanMovingRateThresholdDegreesPerSecond;
    estimatorSettings.jerkStdDegreesPerSecond3 =
        comparison.candidateJerkStdDegreesPerSecond3;
    estimatorSettings.maneuverRateThresholdDegreesPerSecond =
        comparison.candidateManeuverRateThresholdDegreesPerSecond;
    estimatorSettings.maneuverHoldSeconds =
        comparison.candidateManeuverHoldSeconds;
    if (comparison.candidateEstimatorMode == CandidateEstimatorMode::ConstantAcceleration)
        estimatorSettings.mode = ManeuverLosEstimatorMode::ConstantAcceleration;
    else if (comparison.candidateEstimatorMode ==
        CandidateEstimatorMode::ManeuverGatedConstantAcceleration)
        estimatorSettings.mode =
            ManeuverLosEstimatorMode::ManeuverGatedConstantAcceleration;
    LosAimController candidateController;
    LosAimController::Settings candidateSettings;
    candidateSettings.responseSeconds = comparison.candidateResponseSeconds;
    candidateSettings.verticalCatchUpErrorDegrees =
        settings.verticalCatchUpErrorDegrees;
    candidateSettings.maxCountsPerSecond = comparison.candidateMaxCountsPerSecond;
    candidateSettings.feedforwardGain = comparison.feedforwardGain;
    candidateSettings.leadHorizonSeconds = comparison.leadHorizonSeconds;
    candidateSettings.leadStrength = comparison.leadStrength;
    candidateSettings.reversalFeedforwardBoost =
        comparison.reversalFeedforwardBoost;
    candidateSettings.reversalFeedforwardSeconds =
        comparison.reversalFeedforwardSeconds;
    candidateSettings.integralTimeSeconds = comparison.candidateIntegralTimeSeconds;
    candidateSettings.integralZoneDegrees = comparison.candidateIntegralZoneDegrees;
    candidateSettings.settleErrorDegrees = settings.settleErrorDegrees;
    candidateSettings.settleRateDegreesPerSecond = settings.settleRateDegreesPerSecond;
    candidateSettings.reverseConfirmationSeconds = settings.reverseConfirmationSeconds;
    candidateSettings.reverseConfirmationErrorMultiplier =
        comparison.reverseConfirmationErrorMultiplier;
    candidateSettings.confirmLowSpeedReverseSettleRelease =
        comparison.confirmLowSpeedReverseSettleRelease;
    CommandTrajectoryShaper shaper;
    CommandTrajectoryShaper::Settings shaperSettings;
    shaperSettings.mode = settings.trajectoryMode;
    shaperSettings.maxVelocityCountsPerSecond = comparison.candidateMaxCountsPerSecond;
    shaperSettings.maxAccelerationCountsPerSecond2 =
        settings.trajectoryMaxAccelerationCountsPerSecond2;
    shaperSettings.maxJerkCountsPerSecond3 =
        settings.trajectoryMaxJerkCountsPerSecond3;
    shaper.configure(shaperSettings);

    Camera legacyCamera;
    Camera candidateCamera;
    legacyCamera.configure(variant.commandToFrameDelayMs, variant.commandResponseMs);
    candidateCamera.configure(variant.commandToFrameDelayMs, variant.commandResponseMs);
    AppliedViewMotionModel candidateViewMotionModel;
    candidateViewMotionModel.configure(
        variant.commandToFrameDelayMs, variant.commandResponseMs);
    MetricCollector legacyMetrics;
    MetricCollector candidateMetrics;
    std::ofstream frames;
    if (!frameCsv.empty())
    {
        std::filesystem::create_directories(frameCsv.parent_path());
        std::error_code sizeError;
        const bool writeHeader = !std::filesystem::exists(frameCsv, sizeError) ||
            std::filesystem::file_size(frameCsv, sizeError) == 0;
        frames.open(frameCsv, std::ios::app);
        if (writeHeader)
            frames << "Scenario,Variant,TimeSeconds,Detected,MeasurementYaw,MeasurementPitch,MeasuredRelativeYaw,MeasuredRelativePitch,ViewAtObservationYaw,ViewAtObservationPitch,ViewAtControlYaw,ViewAtControlPitch,ObservationTruthYaw,ObservationTruthPitch,TruthYaw,TruthPitch,TruthRateX,TruthRateY,PhysicalCameraRateX,PhysicalCameraRateY,BaselineAngleX,BaselineAngleY,BaselineRateX,BaselineRateY,CaAngleX,CaAngleY,CaRateX,CaRateY,ModelAngleDeltaDeg,ModelRateDeltaDps,ManeuverRateEvidenceDps,ManeuverModelActive,EstimateAngleX,EstimateAngleY,InputErrorX,InputErrorY,SettleEntryErrorX,SettleEntryErrorY,EstimateTruthBiasX,EstimateTruthBiasY,LegacyErrorX,LegacyErrorY,CandidateErrorX,CandidateErrorY,EstimateRateX,EstimateRateY,InnovationX,InnovationY,NisX,NisY,CovarianceX,CovarianceY,FeedforwardConfidence,Settled,SettleReleased,SettleConfirmationSamples,SettleEntryCommandHeld,LowSpeedReverseSuppressed,VerticalCatchUpActive,ReversalDetected,ReversalFeedforwardActive,EffectiveFeedforwardGainX,ReverseConfirmationSeconds,EffectiveResponseSecondsY,FeedbackX,FeedbackY,FeedforwardX,FeedforwardY,LeadX,LeadY,IntegralX,IntegralY,UnlimitedX,UnlimitedY,FrameCountLimit,UnlimitedToFrameLimitRatio,LimitedToUnlimitedRatio,PreGuardRequestedX,PreGuardRequestedY,CommittedEndpointResidualX,CommittedEndpointResidualY,CommittedEndpointGuardActive,RequestedX,RequestedY,ShapedX,ShapedY,SentX,SentY\n";
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
    bool hasPreviousPhysicalView = false;
    double previousPhysicalViewYaw = 0.0;
    double previousPhysicalViewPitch = 0.0;
    double previousPhysicalControlTime = 0.0;
    uint64_t requestSequence = 0;
    const double trajectoryPeriod = 1.0 / comparison.trajectoryOutputHz;
    bool hasLatestTrajectoryRequest = false;
    double nextTrajectoryTickSeconds = 0.0;
    TrajectoryRequest latestTrajectoryRequest;
    size_t replayFrameIndex = 0;
    if (recordedDetectionTimeline)
        recordedDetectionTimeline->clear();

    auto submitCandidate = [&](double timeSeconds, Clock::time_point sendTime,
                               int countsX, int countsY)
    {
        candidateCamera.submit(
            timeSeconds, commandDelay, countsX, countsY, settings);
        if (comparison.candidateViewMotionCompensation ||
            comparison.candidateCommittedEndpointGuard ||
            comparison.candidateCommandCommitHorizonSeconds > 0.0)
        {
            candidateViewMotionModel.addCommand(
                countsX, countsY,
                settings.degreesPerCountX, settings.degreesPerCountY,
                sendTime);
        }
    };

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
            submitCandidate(nextTrajectoryTickSeconds, tickPoint,
                lastResult.output.outputCountsX,
                lastResult.output.outputCountsY);
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
        const auto observationPoint = epoch + std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(observationTime));
        const auto controlPoint = epoch + std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(controlTime));
        // controlTime可能晚于下一帧observationTime，可变Camera一旦前推便不能回退。
        // 正式时轴候选必须按时间点查询历史，否则会把上一帧的未来相机状态扣到当前观测。
        const auto modeledViewAtObservation =
            candidateViewMotionModel.at(observationPoint);
        const auto modeledViewAtControl = candidateViewMotionModel.at(controlPoint);
        const double committedEndpointHorizonSeconds =
            (std::max(0.0, variant.commandToFrameDelayMs) +
             std::max(0.0, variant.commandResponseMs) * 0.5) / 1000.0;
        const auto committedEndpointPoint = controlPoint +
            std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(committedEndpointHorizonSeconds));
        const auto modeledViewAtCommittedEndpoint =
            candidateViewMotionModel.at(committedEndpointPoint);
        const auto commitPoint = controlPoint + std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(
                comparison.candidateCommandCommitHorizonSeconds));
        const auto modeledViewAtCommit = candidateViewMotionModel.at(commitPoint);
        const double sourceTime = std::min(source.points.back().timeSeconds,
            observationTime * variant.speedScale);
        const double controlSourceTime = std::min(source.points.back().timeSeconds,
            controlTime * variant.speedScale);
        const auto measurementAtObservation = targetAngles(source, variant, sourceTime);
        const auto truthAtObservation = useStaticFixedTruth
            ? staticTruthAngles : measurementAtObservation;
        const auto truthAtControl = useStaticFixedTruth
            ? staticTruthAngles : targetAngles(source, variant, controlSourceTime);
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
        const double candidatePhysicalYawAtObservation =
            comparison.candidateViewMotionCompensation
            ? modeledViewAtObservation.first : candidateCamera.yaw;
        const double candidatePhysicalPitchAtObservation =
            comparison.candidateViewMotionCompensation
            ? modeledViewAtObservation.second : candidateCamera.pitch;
        const auto candidateRelativePixels = AimCoordinateSpace::losAnglesToPixelOffset(
            truthAtObservation.yawDegrees - candidatePhysicalYawAtObservation,
            truthAtObservation.pitchDownDegrees - candidatePhysicalPitchAtObservation,
            variant.fovXDegrees, variant.fovYDegrees,
            variant.sourceWidth, variant.sourceHeight);
        const bool bothInsideDetectionCrop =
            std::abs(legacyRelativePixels.x) < cropWidth * 0.5 &&
            std::abs(legacyRelativePixels.y) < cropHeight * 0.5 &&
            std::abs(candidateRelativePixels.x) < cropWidth * 0.5 &&
            std::abs(candidateRelativePixels.y) < cropHeight * 0.5;
        const bool naturalDetection = point.detected && bothInsideDetectionCrop &&
            std::abs(point.timeSeconds - sourceTime) <= std::max(0.05, dt * variant.speedScale * 1.5);
        // 反事实必须复用基线的有效帧与重置边界；否则更强/更弱的候选会因离开
        // 裁剪区而改变后续样本，归因结果混入不同评估队列，无法区分算法收益。
        const bool detected = frozenDetectionTimeline
            ? (replayFrameIndex < frozenDetectionTimeline->size() &&
                (*frozenDetectionTimeline)[replayFrameIndex] != 0)
            : naturalDetection;
        if (recordedDetectionTimeline)
            recordedDetectionTimeline->push_back(detected ? 1U : 0U);
        ++replayFrameIndex;
        if (!detected)
        {
            if (previousDetected)
            {
                legacyFilter.reset();
                legacyPredictor.reset();
                legacyController.reset();
                estimator.reset();
                candidateController.reset();
                shaper.emergencyReset(epoch + std::chrono::duration_cast<Clock::duration>(
                    std::chrono::duration<double>(controlTime)));
                hasLatestTrajectoryRequest = false;
                nextTrajectoryTickSeconds = 0.0;
                // 已成功提交给物理相机的命令无法撤销；正式视角时轴候选必须保留其迟到响应。
                // 历史基线继续保留原有清队列语义，确保默认关闭时逐位复现既有矩阵。
                // 成功提交到物理相机的命令不能因后续检测短失被撤销；candidate与legacy
                // 必须使用相同的迟到响应契约，否则恢复窗口比较会人为偏向candidate。
            }
            legacyMetrics.addOutput(0, 0, false);
            candidateMetrics.addOutput(0, 0, false);
            previousDetected = false;
            hasPreviousPhysicalView = false;
            continue;
        }
        previousDetected = true;
        serviceTrajectoryBefore(controlTime, false);
        legacyCamera.apply(controlTime);
        candidateCamera.apply(controlTime);

        const double sourcePixelX = AimCoordinateSpace::sourcePixelDeltaForAngleDegrees(
            measurementAtObservation.yawDegrees, variant.fovXDegrees, variant.sourceWidth);
        const double sourcePixelY = AimCoordinateSpace::sourcePixelDeltaForAngleDegrees(
            measurementAtObservation.pitchDownDegrees, variant.fovYDegrees, variant.sourceHeight);
        const double measuredRelativeYaw = measurementAtObservation.yawDegrees -
            candidatePhysicalYawAtObservation;
        const double measuredRelativePitch =
            measurementAtObservation.pitchDownDegrees - candidatePhysicalPitchAtObservation;
        const double stabilizedMeasurementYaw =
            comparison.candidateViewMotionCompensation
            ? measuredRelativeYaw + modeledViewAtObservation.first
            : measurementAtObservation.yawDegrees;
        const double stabilizedMeasurementPitch =
            comparison.candidateViewMotionCompensation
            ? measuredRelativePitch + modeledViewAtObservation.second
            : measurementAtObservation.pitchDownDegrees;
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

        estimator.update(
            stabilizedMeasurementYaw, stabilizedMeasurementPitch,
            point.confidence, observationPoint, controlPoint, estimatorSettings);
        const RelativeLosKalmanEstimate estimate = estimator.selectedEstimate();
        const auto& baselineEstimate = estimator.constantVelocityEstimate();
        const auto& caEstimate = estimator.constantAccelerationEstimate();
        const auto& estimatorDiagnostics = estimator.diagnostics();
        const bool useManeuverModel =
            estimatorDiagnostics.maneuverModelActive;
        const double previousControlTime = std::max(0.0, controlTime - dt);
        const double previousControlSourceTime = std::min(
            source.points.back().timeSeconds,
            previousControlTime * variant.speedScale);
        const auto truthBeforeControl = targetAngles(
            source, variant, previousControlSourceTime);
        const double oracleRateX = controlTime > 0.0
            ? (truthAtControl.yawDegrees - truthBeforeControl.yawDegrees) / dt
            : 0.0;
        const double oracleRateY = controlTime > 0.0
            ? (truthAtControl.pitchDownDegrees - truthBeforeControl.pitchDownDegrees) / dt
            : 0.0;
        const double physicalRateDt = controlTime - previousPhysicalControlTime;
        const double physicalCameraRateX = hasPreviousPhysicalView && physicalRateDt > 1e-9
            ? (candidateCamera.yaw - previousPhysicalViewYaw) / physicalRateDt : 0.0;
        const double physicalCameraRateY = hasPreviousPhysicalView && physicalRateDt > 1e-9
            ? (candidateCamera.pitch - previousPhysicalViewPitch) / physicalRateDt : 0.0;
        const bool useOracle = comparison.candidateEstimatorMode ==
            CandidateEstimatorMode::OracleControlTime;
        LosAimController::Input input;
        input.valid = useOracle || (estimate.x.valid && estimate.y.valid);
        const double commitHorizon =
            comparison.candidateCommandCommitHorizonSeconds;
        const double candidateViewAtEvaluationYaw = commitHorizon > 0.0
            ? modeledViewAtCommit.first
            : (comparison.candidateViewMotionCompensation
                ? modeledViewAtControl.first : candidateCamera.yaw);
        const double candidateViewAtEvaluationPitch = commitHorizon > 0.0
            ? modeledViewAtCommit.second
            : (comparison.candidateViewMotionCompensation
                ? modeledViewAtControl.second : candidateCamera.pitch);
        const double targetAtEvaluationYaw = useOracle
            ? truthAtControl.yawDegrees + oracleRateX * commitHorizon
            : estimate.x.angleDegrees + estimate.x.rateDegreesPerSecond * commitHorizon;
        const double targetAtEvaluationPitch = useOracle
            ? truthAtControl.pitchDownDegrees + oracleRateY * commitHorizon
            : estimate.y.angleDegrees + estimate.y.rateDegreesPerSecond * commitHorizon;
        const double committedErrorX =
            targetAtEvaluationYaw - candidateViewAtEvaluationYaw;
        const double committedErrorY =
            targetAtEvaluationPitch - candidateViewAtEvaluationPitch;
        if (comparison.candidateSettleEntryCommandGuard)
        {
            // 已承诺终点只阻止过早进入settled；反馈和全部运动态分量仍使用当前时刻误差。
            const double currentViewYaw = comparison.candidateViewMotionCompensation
                ? modeledViewAtControl.first : candidateCamera.yaw;
            const double currentViewPitch = comparison.candidateViewMotionCompensation
                ? modeledViewAtControl.second : candidateCamera.pitch;
            input.errorDegreesX = (useOracle ? truthAtControl.yawDegrees :
                estimate.x.angleDegrees) - currentViewYaw;
            input.errorDegreesY = (useOracle ? truthAtControl.pitchDownDegrees :
                estimate.y.angleDegrees) - currentViewPitch;
            // settle候选已经满足低速边界，此处冻结当前目标角，只投影已发送相机命令。
            // 若继续把阈值内估计速率外推60 ms，会人为加入最多0.072°噪声，接近整个静止带。
            input.settleEntryErrorDegreesX =
                (useOracle ? truthAtControl.yawDegrees : estimate.x.angleDegrees) -
                modeledViewAtCommit.first;
            input.settleEntryErrorDegreesY =
                (useOracle ? truthAtControl.pitchDownDegrees : estimate.y.angleDegrees) -
                modeledViewAtCommit.second;
            input.settleEntryErrorValid = true;
            input.holdOutputWhileSettleEntryBlocked =
                comparison.candidateSettleEntryCommandHold;
        }
        else
        {
            input.errorDegreesX = committedErrorX;
            input.errorDegreesY = committedErrorY;
        }
        input.relativeLosRateDegreesPerSecondX = useOracle
            ? oracleRateX : estimate.x.rateDegreesPerSecond;
        input.relativeLosRateDegreesPerSecondY = useOracle
            ? oracleRateY : estimate.y.rateDegreesPerSecond;
        input.feedforwardConfidence = useOracle ? 1.0 : estimate.feedforwardConfidence;
        input.degreesPerCountX = settings.degreesPerCountX;
        input.degreesPerCountY = settings.degreesPerCountY;
        input.dtSeconds = dt;
        const auto candidateOutput = candidateController.update(input, candidateSettings);
        const double unlimitedMagnitude = std::hypot(
            candidateOutput.unlimitedCountsX, candidateOutput.unlimitedCountsY);
        const double limitedMagnitude = std::hypot(
            candidateOutput.limitedCountsX, candidateOutput.limitedCountsY);
        const double unlimitedToFrameLimitRatio = candidateOutput.frameCountLimit > 1e-9
            ? unlimitedMagnitude / candidateOutput.frameCountLimit : 0.0;
        const double limitedToUnlimitedRatio = unlimitedMagnitude > 1e-9
            ? limitedMagnitude / unlimitedMagnitude : 1.0;
        TrajectoryRequest request;
        request.valid = candidateOutput.valid;
        request.sequence = ++requestSequence;
        request.requestDurationSeconds = dt;
        const double preGuardRequestedX = candidateOutput.limitedCountsX;
        const double preGuardRequestedY = candidateOutput.limitedCountsY;
        const double targetAtControlYaw = useOracle
            ? truthAtControl.yawDegrees : estimate.x.angleDegrees;
        const double targetAtControlPitch = useOracle
            ? truthAtControl.pitchDownDegrees : estimate.y.angleDegrees;
        // 只叠加尚未兑现的视角增量，不能把模型的累计绝对值再次加到物理相机位置上。
        const double committedEndpointYaw = candidateCamera.yaw +
            (modeledViewAtCommittedEndpoint.first - modeledViewAtControl.first);
        const double committedEndpointPitch = candidateCamera.pitch +
            (modeledViewAtCommittedEndpoint.second - modeledViewAtControl.second);
        const double committedEndpointResidualX = settings.degreesPerCountX != 0.0
            ? (targetAtControlYaw - committedEndpointYaw) / settings.degreesPerCountX
            : 0.0;
        const double committedEndpointResidualY = settings.degreesPerCountY != 0.0
            ? (targetAtControlPitch - committedEndpointPitch) / settings.degreesPerCountY
            : 0.0;
        const auto guardAxis = [](double requested, double residual)
        {
            if (requested * residual <= 0.0)
                return 0.0;
            const double allowed = std::max(0.0, std::abs(residual) - 0.5);
            return std::copysign(std::min(std::abs(requested), allowed), requested);
        };
        request.requestedCountsX = comparison.candidateCommittedEndpointGuard
            ? guardAxis(preGuardRequestedX, committedEndpointResidualX)
            : preGuardRequestedX;
        request.requestedCountsY = comparison.candidateCommittedEndpointGuard
            ? guardAxis(preGuardRequestedY, committedEndpointResidualY)
            : preGuardRequestedY;
        const bool committedEndpointGuardActive =
            comparison.candidateCommittedEndpointGuard &&
            (std::abs(request.requestedCountsX - preGuardRequestedX) > 1e-9 ||
             std::abs(request.requestedCountsY - preGuardRequestedY) > 1e-9);
        request.requestTime = controlPoint;
        CommandTrajectoryShaper::Result shaped;
        if (settings.trajectoryMode == TrajectoryShaperMode::Off)
        {
            shaped = shaper.update(request, dt, controlPoint);
            submitCandidate(controlTime, controlPoint,
                shaped.output.outputCountsX, shaped.output.outputCountsY);
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
        candidateMetrics.addEstimate(
            useOracle ? oracleRateX : estimate.x.rateDegreesPerSecond,
            useOracle ? oracleRateY : estimate.y.rateDegreesPerSecond,
            truthRateX, truthRateY,
            useOracle ? 0.0 : std::max(estimate.x.nis, estimate.y.nis),
            useOracle ? 0.0 : std::max(
                estimate.x.angleVariance, estimate.y.angleVariance),
            useOracle ? 1.0 : estimate.feedforwardConfidence);
        candidateMetrics.addSettle(
            candidateOutput.settled, candidateOutput.settleReleased);
        candidateMetrics.addReverseSuppression(
            candidateOutput.lowSpeedReverseSuppressed);
        candidateMetrics.addReversalFeedforward(
            candidateOutput.reversalFeedforwardActive);
        candidateMetrics.addVerticalCatchUp(candidateOutput.verticalCatchUpActive);
        candidateMetrics.addManeuverModel(useManeuverModel);
        candidateMetrics.result.requestedCounts += std::hypot(
            request.requestedCountsX, request.requestedCountsY);
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
                   << std::setprecision(6) << controlTime << ",1,"
                   << measurementAtObservation.yawDegrees << ','
                   << measurementAtObservation.pitchDownDegrees << ','
                   << measuredRelativeYaw << ',' << measuredRelativePitch << ','
                   << modeledViewAtObservation.first << ','
                   << modeledViewAtObservation.second << ','
                   << modeledViewAtControl.first << ',' << modeledViewAtControl.second << ','
                   << truthAtObservation.yawDegrees << ','
                   << truthAtObservation.pitchDownDegrees << ','
                   << truthAtControl.yawDegrees << ',' << truthAtControl.pitchDownDegrees << ','
                   << oracleRateX << ',' << oracleRateY << ','
                   << physicalCameraRateX << ',' << physicalCameraRateY << ','
                   << baselineEstimate.x.angleDegrees << ','
                   << baselineEstimate.y.angleDegrees << ','
                   << baselineEstimate.x.rateDegreesPerSecond << ','
                   << baselineEstimate.y.rateDegreesPerSecond << ','
                   << caEstimate.x.angleDegrees << ',' << caEstimate.y.angleDegrees << ','
                   << caEstimate.x.rateDegreesPerSecond << ','
                   << caEstimate.y.rateDegreesPerSecond << ','
                   << estimatorDiagnostics.modelAngleDeltaDegrees << ','
                   << estimatorDiagnostics.modelRateDeltaDegreesPerSecond << ','
                   << estimatorDiagnostics.maneuverRateEvidenceDegreesPerSecond << ','
                   << (useManeuverModel ? 1 : 0) << ','
                   << estimate.x.angleDegrees << ',' << estimate.y.angleDegrees << ','
                   << input.errorDegreesX << ',' << input.errorDegreesY << ','
                   << (input.settleEntryErrorValid
                        ? input.settleEntryErrorDegreesX : input.errorDegreesX) << ','
                   << (input.settleEntryErrorValid
                        ? input.settleEntryErrorDegreesY : input.errorDegreesY) << ','
                   << estimate.x.angleDegrees - truthAtControl.yawDegrees << ','
                   << estimate.y.angleDegrees - truthAtControl.pitchDownDegrees << ','
                   << legacyErrorX << ',' << legacyErrorY << ','
                   << candidateErrorX << ',' << candidateErrorY << ',' << estimate.x.rateDegreesPerSecond << ','
                   << estimate.y.rateDegreesPerSecond << ',' << estimate.x.innovationDegrees << ','
                   << estimate.y.innovationDegrees << ',' << estimate.x.nis << ',' << estimate.y.nis << ','
                   << estimate.x.angleVariance << ',' << estimate.y.angleVariance << ','
                   << estimate.feedforwardConfidence << ',' << (candidateOutput.settled ? 1 : 0) << ','
                   << (candidateOutput.settleReleased ? 1 : 0) << ','
                   << candidateOutput.settleConfirmationSamples << ','
                   << (candidateOutput.settleEntryCommandHeld ? 1 : 0) << ','
                   << (candidateOutput.lowSpeedReverseSuppressed ? 1 : 0) << ','
                   << (candidateOutput.verticalCatchUpActive ? 1 : 0) << ','
                   << (candidateOutput.reversalDetected ? 1 : 0) << ','
                   << (candidateOutput.reversalFeedforwardActive ? 1 : 0) << ','
                   << candidateOutput.effectiveFeedforwardGainX << ','
                   << candidateOutput.reverseConfirmationSeconds << ','
                   << candidateOutput.effectiveResponseSecondsY << ','
                   << candidateOutput.feedbackCountsX << ','
                   << candidateOutput.feedbackCountsY << ',' << candidateOutput.trackingFeedforwardCountsX << ','
                   << candidateOutput.trackingFeedforwardCountsY << ',' << candidateOutput.leadCountsX << ','
                   << candidateOutput.leadCountsY << ',' << candidateOutput.integralCountsX << ','
                   << candidateOutput.integralCountsY << ','
                   << candidateOutput.unlimitedCountsX << ','
                   << candidateOutput.unlimitedCountsY << ','
                   << candidateOutput.frameCountLimit << ','
                   << unlimitedToFrameLimitRatio << ','
                   << limitedToUnlimitedRatio << ',' << preGuardRequestedX << ','
                   << preGuardRequestedY << ',' << committedEndpointResidualX << ','
                   << committedEndpointResidualY << ','
                   << (committedEndpointGuardActive ? 1 : 0) << ','
                   << request.requestedCountsX << ',' << request.requestedCountsY << ','
                   << shaped.output.shapedCountsX << ','
                   << shaped.output.shapedCountsY << ',' << shaped.output.outputCountsX << ','
                   << shaped.output.outputCountsY << '\n';
        }
        previousPhysicalViewYaw = candidateCamera.yaw;
        previousPhysicalViewPitch = candidateCamera.pitch;
        previousPhysicalControlTime = controlTime;
        hasPreviousPhysicalView = true;
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
    output << "Scenario,Variant,CommandCenterMs,CommandResponseMs,KalmanAccelerationStdDps2,KalmanMovingAccelerationStdDps2,KalmanMovingRateThresholdDps,LegacyResponseMs,CandidateResponseMs,CandidateEstimatorMode,CandidateJerkStdDps3,CandidateManeuverRateThresholdDps,CandidateManeuverHoldMs,CandidateMaxCountsPerSecond,CandidateIntegralTimeMs,CandidateIntegralZoneDeg,FeedforwardGain,LeadHorizonMs,LeadStrength,ReversalFeedforwardBoost,ReversalFeedforwardMs,ReverseConfirmationErrorMultiplier,ConfirmLowSpeedReverseSettleRelease,StaticFixedTruth,CandidateViewMotionCompensation,CandidateCommittedEndpointGuard,CandidateCommandCommitHorizonMs,CandidateSettleEntryCommandGuard,CandidateSettleEntryCommandHold,TrajectoryMode,TrajectoryOutputHz,Samples,LegacyP50Deg,LegacyP95Deg,LegacyP99Deg,CandidateP50Deg,CandidateP95Deg,CandidateP99Deg,LegacyVerticalP95Deg,CandidateVerticalP95Deg,LegacyInsideBoxPercent,CandidateInsideBoxPercent,LegacyEdgeMarginP05Deg,CandidateEdgeMarginP05Deg,LegacyInterruptionPercent,CandidateInterruptionPercent,LegacyOutputFlips,CandidateOutputFlips,EstimateDirectionErrors,EstimateRateSignFlips,MeanNis,MeanCovariance,MeanFeedforwardConfidence,RequestedCounts,ShapedCounts,SentCounts,FeedforwardCounts,ReversalFeedforwardPercent,SettledPercent,SettleReleases,ReverseSuppressedPercent,VerticalCatchUpPercent,ManeuverModelPercent,TrajectoryOutputs,TrajectoryVelocityLimitedPercent,TrajectoryAccelerationLimitedPercent,TrajectoryJerkLimitedPercent,Passed,Reason\n";
    output << std::fixed << std::setprecision(6);
    for (const auto& item : comparisons)
    {
        output << item.scenario << ',' << item.variant.name << ','
               << item.variant.commandToFrameDelayMs << ','
               << item.variant.commandResponseMs << ','
               << item.kalmanAccelerationStdDegreesPerSecond2 << ','
               << item.kalmanMovingAccelerationStdDegreesPerSecond2 << ','
               << item.kalmanMovingRateThresholdDegreesPerSecond << ','
               << item.legacyResponseSeconds * 1000.0 << ','
               << item.candidateResponseSeconds * 1000.0 << ','
               << candidateEstimatorModeName(item.candidateEstimatorMode) << ','
               << item.candidateJerkStdDegreesPerSecond3 << ','
               << item.candidateManeuverRateThresholdDegreesPerSecond << ','
               << item.candidateManeuverHoldSeconds * 1000.0 << ','
               << item.candidateMaxCountsPerSecond << ','
               << item.candidateIntegralTimeSeconds * 1000.0 << ','
               << item.candidateIntegralZoneDegrees << ','
               << item.feedforwardGain << ','
               << item.leadHorizonSeconds * 1000.0 << ','
               << item.leadStrength << ','
               << item.reversalFeedforwardBoost << ','
               << item.reversalFeedforwardSeconds * 1000.0 << ','
               << item.reverseConfirmationErrorMultiplier << ','
               << (item.confirmLowSpeedReverseSettleRelease ? 1 : 0) << ','
               << (item.staticFixedTruth ? 1 : 0) << ','
               << (item.candidateViewMotionCompensation ? 1 : 0) << ','
               << (item.candidateCommittedEndpointGuard ? 1 : 0) << ','
               << item.candidateCommandCommitHorizonSeconds * 1000.0 << ','
               << (item.candidateSettleEntryCommandGuard ? 1 : 0) << ','
               << (item.candidateSettleEntryCommandHold ? 1 : 0) << ','
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
               << item.candidate.reversalFeedforwardPercent << ','
               << item.candidate.settledPercent << ',' << item.candidate.settleReleases << ','
               << item.candidate.reverseSuppressedPercent << ','
               << item.candidate.verticalCatchUpPercent << ','
               << item.candidate.maneuverModelPercent << ','
               << item.candidate.trajectoryOutputs << ','
               << item.candidate.trajectoryVelocityLimitedPercent << ','
               << item.candidate.trajectoryAccelerationLimitedPercent << ','
               << item.candidate.trajectoryJerkLimitedPercent << ','
               << (item.passed ? 1 : 0) << ',' << item.reason << '\n';
    }
}

namespace
{
bool sameLegacyCohort(const Comparison& first, const Comparison& second)
{
    const auto& a = first.legacy;
    const auto& b = second.legacy;
    return a.samples == b.samples &&
        a.errorP50Degrees == b.errorP50Degrees &&
        a.errorP95Degrees == b.errorP95Degrees &&
        a.errorP99Degrees == b.errorP99Degrees &&
        a.verticalP95Degrees == b.verticalP95Degrees &&
        a.insideBoxPercent == b.insideBoxPercent &&
        a.edgeMarginP05Degrees == b.edgeMarginP05Degrees &&
        a.interruptionPercent == b.interruptionPercent &&
        a.outputDirectionFlips == b.outputDirectionFlips &&
        a.lateHalfErrorP95Degrees == b.lateHalfErrorP95Degrees &&
        a.requestedCounts == b.requestedCounts &&
        a.sentCounts == b.sentCounts;
}
}

ResponseCounterfactual RunResponseCounterfactual(
    const SourceTrajectory& source, const Variant& variant,
    const ControllerSettings& baselineSettings,
    double counterfactualResponseSeconds,
    const std::filesystem::path& baselineFrameCsv,
    const std::filesystem::path& counterfactualFrameCsv)
{
    ResponseCounterfactual result;
    std::vector<unsigned char> detectionTimeline;
    result.baseline = RunComparison(
        source, variant, baselineSettings, baselineFrameCsv,
        nullptr, &detectionTimeline);

    ControllerSettings counterfactualSettings = baselineSettings;
    counterfactualSettings.candidateResponseSeconds =
        counterfactualResponseSeconds;
    result.counterfactual = RunComparison(
        source, variant, counterfactualSettings, counterfactualFrameCsv,
        &detectionTimeline);

    result.timelineFrames = detectionTimeline.size();
    result.detectedFrames = static_cast<size_t>(std::count(
        detectionTimeline.begin(), detectionTimeline.end(),
        static_cast<unsigned char>(1)));
    // 队列相同还不够：legacy 指标必须逐值一致，双方候选样本数也必须相同。
    // 任一条件失败都表示实现或评估边界漂移，结果不得用于参数选择。
    result.cohortStable = result.timelineFrames > 0 &&
        sameLegacyCohort(result.baseline, result.counterfactual) &&
        result.detectedFrames == result.baseline.candidate.samples &&
        result.baseline.legacy.samples == result.baseline.candidate.samples &&
        result.baseline.candidate.samples ==
            result.counterfactual.candidate.samples;
    return result;
}

void WriteResponseCounterfactualSummary(
    const std::filesystem::path& path,
    const std::vector<ResponseCounterfactual>& counterfactuals)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    output << "Scenario,Variant,CohortStable,TimelineFrames,DetectedFrames,"
              "BaselineResponseMs,CounterfactualResponseMs,BaselineSamples,"
              "CounterfactualSamples,BaselineLegacyP95Deg,CounterfactualLegacyP95Deg,"
              "BaselineCandidateP95Deg,CounterfactualCandidateP95Deg,"
              "CandidateP95DeltaPercent,BaselineOutputFlips,CounterfactualOutputFlips,"
              "BaselinePassed,CounterfactualPassed,BaselineReason,CounterfactualReason\n";
    output << std::fixed << std::setprecision(6);
    for (const auto& item : counterfactuals)
    {
        const double baselineP95 = item.baseline.candidate.errorP95Degrees;
        const double deltaPercent = baselineP95 > 1e-9
            ? 100.0 * (item.counterfactual.candidate.errorP95Degrees - baselineP95) /
                baselineP95
            : 0.0;
        output << item.baseline.scenario << ',' << item.baseline.variant.name << ','
               << (item.cohortStable ? 1 : 0) << ',' << item.timelineFrames << ','
               << item.detectedFrames << ','
               << item.baseline.candidateResponseSeconds * 1000.0 << ','
               << item.counterfactual.candidateResponseSeconds * 1000.0 << ','
               << item.baseline.candidate.samples << ','
               << item.counterfactual.candidate.samples << ','
               << item.baseline.legacy.errorP95Degrees << ','
               << item.counterfactual.legacy.errorP95Degrees << ','
               << baselineP95 << ','
               << item.counterfactual.candidate.errorP95Degrees << ','
               << deltaPercent << ','
               << item.baseline.candidate.outputDirectionFlips << ','
               << item.counterfactual.candidate.outputDirectionFlips << ','
               << (item.baseline.passed ? 1 : 0) << ','
               << (item.counterfactual.passed ? 1 : 0) << ','
               << item.baseline.reason << ',' << item.counterfactual.reason << '\n';
    }
}

std::string ClassifyAttribution(const Comparison& baseline,
                                const Comparison& oracleEstimator,
                                const Comparison& unlimitedActuator,
                                bool& cohortStable)
{
    cohortStable = sameLegacyCohort(baseline, oracleEstimator) &&
        sameLegacyCohort(baseline, unlimitedActuator) &&
        baseline.candidate.samples == oracleEstimator.candidate.samples &&
        baseline.candidate.samples == unlimitedActuator.candidate.samples;
    if (baseline.passed)
        return cohortStable ? "BASELINE_PASS" : "COHORT_CHANGED";
    if (!cohortStable)
        return "COHORT_CHANGED";
    if (oracleEstimator.passed && !unlimitedActuator.passed)
        return "ESTIMATOR_LIMITED";
    if (!oracleEstimator.passed && unlimitedActuator.passed)
        return "ACTUATOR_LIMITED";
    if (oracleEstimator.passed && unlimitedActuator.passed)
        return "EITHER_INTERVENTION_RESCUES";
    return "COUPLED_OR_GATE_LIMITED";
}

Attribution RunAttribution(const SourceTrajectory& source, const Variant& variant,
                           const ControllerSettings& settings,
                           const std::filesystem::path& frameCsv)
{
    Attribution result;
    std::vector<unsigned char> detectionTimeline;
    result.baseline = RunComparison(
        source, variant, settings, frameCsv, nullptr, &detectionTimeline);

    ControllerSettings oracleSettings = settings;
    oracleSettings.candidateEstimatorMode = CandidateEstimatorMode::OracleControlTime;
    result.oracleEstimator = RunComparison(
        source, variant, oracleSettings, {}, &detectionTimeline);

    ControllerSettings unlimitedSettings = settings;
    unlimitedSettings.candidateMaxCountsPerSecond = 100000.0;
    result.unlimitedActuator = RunComparison(
        source, variant, unlimitedSettings, {}, &detectionTimeline);

    result.classification = ClassifyAttribution(
        result.baseline, result.oracleEstimator, result.unlimitedActuator,
        result.cohortStable);
    return result;
}

void WriteAttributionSummary(const std::filesystem::path& path,
                             const std::vector<Attribution>& attributions)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    output << "Scenario,Variant,CohortStable,Classification,BaselinePassed,OraclePassed,UnlimitedPassed,BaselineReason,OracleReason,UnlimitedReason,BaselineSamples,OracleSamples,UnlimitedSamples,LegacyP95Deg,BaselineP95Deg,OracleP95Deg,UnlimitedP95Deg,LegacyVerticalP95Deg,BaselineVerticalP95Deg,OracleVerticalP95Deg,UnlimitedVerticalP95Deg,LegacyInsideBoxPercent,BaselineInsideBoxPercent,OracleInsideBoxPercent,UnlimitedInsideBoxPercent,LegacyOutputFlips,BaselineOutputFlips,OracleOutputFlips,UnlimitedOutputFlips,BaselineRequestedCounts,OracleRequestedCounts,UnlimitedRequestedCounts,BaselineSentCounts,OracleSentCounts,UnlimitedSentCounts\n";
    output << std::fixed << std::setprecision(6);
    for (const auto& item : attributions)
    {
        const auto& baseline = item.baseline;
        const auto& oracle = item.oracleEstimator;
        const auto& unlimited = item.unlimitedActuator;
        output << baseline.scenario << ',' << baseline.variant.name << ','
               << (item.cohortStable ? 1 : 0) << ',' << item.classification << ','
               << (baseline.passed ? 1 : 0) << ',' << (oracle.passed ? 1 : 0) << ','
               << (unlimited.passed ? 1 : 0) << ',' << baseline.reason << ','
               << oracle.reason << ',' << unlimited.reason << ','
               << baseline.candidate.samples << ',' << oracle.candidate.samples << ','
               << unlimited.candidate.samples << ',' << baseline.legacy.errorP95Degrees << ','
               << baseline.candidate.errorP95Degrees << ','
               << oracle.candidate.errorP95Degrees << ','
               << unlimited.candidate.errorP95Degrees << ','
               << baseline.legacy.verticalP95Degrees << ','
               << baseline.candidate.verticalP95Degrees << ','
               << oracle.candidate.verticalP95Degrees << ','
               << unlimited.candidate.verticalP95Degrees << ','
               << baseline.legacy.insideBoxPercent << ','
               << baseline.candidate.insideBoxPercent << ','
               << oracle.candidate.insideBoxPercent << ','
               << unlimited.candidate.insideBoxPercent << ','
               << baseline.legacy.outputDirectionFlips << ','
               << baseline.candidate.outputDirectionFlips << ','
               << oracle.candidate.outputDirectionFlips << ','
               << unlimited.candidate.outputDirectionFlips << ','
               << baseline.candidate.requestedCounts << ','
               << oracle.candidate.requestedCounts << ','
               << unlimited.candidate.requestedCounts << ','
               << baseline.candidate.sentCounts << ','
               << oracle.candidate.sentCounts << ','
               << unlimited.candidate.sentCounts << '\n';
    }
}
}
