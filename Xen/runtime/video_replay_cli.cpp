#include "runtime/video_replay_cli.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <stdexcept>
#include <vector>

#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include "Xen.h"
#include "mouse/AimbotTarget.h"
#include "runtime/aim_coordinate_space.h"
#include "runtime/basic_aim_controller.h"
#include "runtime/basic_target_filter.h"
#include "runtime/command_trajectory_shaper.h"
#include "runtime/cross_domain_replay.h"
#include "runtime/target_predictor.h"
#include "runtime/video_replay_math.h"

#ifndef USE_CUDA
#include "detector/dml_detector.h"
#endif

namespace VideoReplay
{
namespace
{
    struct Options
    {
        std::filesystem::path videoRoot;
        std::filesystem::path modelPath;
        std::filesystem::path outputRoot;
        int cropX = 1120;
        int cropY = 560;
        int cropWidth = 320;
        int cropHeight = 320;
        double inferenceFps = 94.0;
        std::vector<double> observationAgesMs{ 10.0, 15.0, 20.0 };
        double responseMs = 80.0;
        double candidateResponseMs = 0.0;
        double candidateResponseCounterfactualMs = 0.0;
        CrossDomainReplay::CandidateEstimatorMode candidateEstimatorMode =
            CrossDomainReplay::CandidateEstimatorMode::Kalman;
        double candidateJerkStdDegreesPerSecond3 = 8000.0;
        double candidateManeuverRateThresholdDegreesPerSecond = 12.0;
        double candidateManeuverHoldSeconds = 0.120;
        double kalmanAccelerationStdDegreesPerSecond2 = 90.0;
        double kalmanMovingAccelerationStdDegreesPerSecond2 = 360.0;
        double kalmanMovingRateThresholdDegreesPerSecond = 8.0;
        double verticalCatchUpErrorDegrees = 0.8;
        double maxCountsPerSecond = 1440.0;
        double integralMs = 320.0;
        double candidateIntegralMs = 0.0;
        double candidateIntegralZoneDegrees = 1.0;
        double sensitivity = 1.4;
        double yaw = 0.022;
        double pitch = 0.022;
        double fovX = 106.0;
        double fovY = 74.0;
        bool crossDomain = false;
        bool crossDomainAttribution = false;
        std::vector<std::string> crossDomainDetailVariants{
            "domain_fov106_2560x1440_94fps_20ms_1x"
        };
        double settleErrorDegrees = 0.080;
        double settleRateDegreesPerSecond = 1.200;
        double reverseConfirmationSeconds = 0.080;
        double reverseConfirmationErrorMultiplier = 1.5;
        bool confirmLowSpeedReverseSettleRelease = false;
        bool staticFixedTruth = false;
        bool candidateViewMotionCompensation = false;
        double candidateCommandCommitHorizonMs = 0.0;
        double physicalCommandCenterMs = 60.0;
        double physicalCommandResponseMs = 0.0;
        bool candidateSettleEntryCommandGuard = false;
        bool candidateSettleEntryCommandHold = false;
        double feedforwardGain = 0.0;
        double leadHorizonSeconds = 0.0;
        double leadStrength = 0.0;
        double reversalFeedforwardBoost = 0.0;
        double reversalFeedforwardSeconds = 0.0;
        TrajectoryShaperMode trajectoryMode = TrajectoryShaperMode::Off;
        double trajectoryOutputHz = 240.0;
        double trajectoryMaxAccelerationCountsPerSecond2 = 60000.0;
        double trajectoryMaxJerkCountsPerSecond3 = 4000000.0;
    };

    struct ScenarioTrajectory
    {
        std::string name;
        std::filesystem::path path;
        int sourceWidth = 0;
        int sourceHeight = 0;
        double videoFps = 0.0;
        double frameCount = 0.0;
        std::vector<TrajectoryPoint> points;
    };

    struct Metrics
    {
        size_t samples = 0;
        size_t outsideCrop = 0;
        double meanAbsX = 0.0;
        double p95AbsX = 0.0;
        double meanAbsY = 0.0;
        double p95AbsY = 0.0;
    };

    struct Candidate
    {
        bool enabled = false;
        double leadMs = 0.0;
        double tauMs = 35.0;
        double maxCountsPerSecond = 1440.0;
        double predictionStrength = 0.0;
    };

    struct CandidateResult
    {
        Candidate candidate;
        std::map<std::string, Metrics> worstByScenario;
        double movingP95 = std::numeric_limits<double>::infinity();
        double reverseP95 = std::numeric_limits<double>::infinity();
        double staticP95X = std::numeric_limits<double>::infinity();
        double staticP95Y = std::numeric_limits<double>::infinity();
        double jumpP95 = std::numeric_limits<double>::infinity();
        double improvementPercent = 0.0;
        double jumpImprovementPercent = 0.0;
        bool valid = false;
        double score = std::numeric_limits<double>::infinity();
    };

    std::string lower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    }

    std::optional<std::string> optionValue(int argc, char** argv, const std::string& name)
    {
        for (int index = 1; index + 1 < argc; ++index)
        {
            if (argv[index] && name == argv[index])
                return std::string(argv[index + 1]);
        }
        return std::nullopt;
    }

    double optionDouble(int argc, char** argv, const std::string& name, double fallback)
    {
        const auto value = optionValue(argc, argv, name);
        if (!value)
            return fallback;
        try
        {
            return std::stod(*value);
        }
        catch (...)
        {
            return fallback;
        }
    }

    std::vector<double> parseDoubleList(const std::string& value)
    {
        std::vector<double> result;
        std::stringstream stream(value);
        std::string token;
        while (std::getline(stream, token, ','))
        {
            try
            {
                const double number = std::stod(token);
                if (std::isfinite(number) && number >= 0.0)
                    result.push_back(number);
            }
            catch (...)
            {
            }
        }
        return result;
    }

    std::vector<std::string> parseStringList(const std::string& value)
    {
        std::vector<std::string> result;
        std::stringstream stream(value);
        for (std::string token; std::getline(stream, token, ',');)
        {
            token.erase(std::remove_if(token.begin(), token.end(), [](unsigned char ch) {
                return std::isspace(ch) != 0;
            }), token.end());
            if (!token.empty() && std::find(result.begin(), result.end(), token) == result.end())
                result.push_back(std::move(token));
        }
        return result;
    }

    std::filesystem::path resolveDefaultModel()
    {
        const std::vector<std::filesystem::path> candidates{
            std::filesystem::path("models") / config.ai_model,
            std::filesystem::path("..") / ".." / ".." / "x64" / "DML" / "models" / config.ai_model
        };
        for (const auto& candidate : candidates)
        {
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error))
                return std::filesystem::absolute(candidate, error);
        }
        return {};
    }

    Options parseOptions(int argc, char** argv)
    {
        Options options;
        if (const auto root = optionValue(argc, argv, "--cross-domain-replay"))
        {
            options.videoRoot = *root;
            options.crossDomain = true;
        }
        else if (const auto root = optionValue(argc, argv, "--video-replay"))
            options.videoRoot = *root;
        if (const auto attribution = optionValue(
                argc, argv, "--cross-domain-attribution"))
        {
            const std::string normalized = lower(*attribution);
            options.crossDomainAttribution = normalized != "0" &&
                normalized != "false" && normalized != "off";
        }
        if (const auto detailVariant = optionValue(
                argc, argv, "--cross-domain-detail-variant"))
        {
            options.crossDomainDetailVariants = parseStringList(*detailVariant);
            if (options.crossDomainDetailVariants.empty())
                throw std::runtime_error("--cross-domain-detail-variant requires a name.");
        }
        if (const auto model = optionValue(argc, argv, "--video-model"))
            options.modelPath = *model;
        else
            options.modelPath = resolveDefaultModel();
        if (const auto output = optionValue(argc, argv, "--video-output"))
            options.outputRoot = *output;
        else
            options.outputRoot = options.videoRoot / "analysis";

        options.cropX = static_cast<int>(optionDouble(argc, argv, "--crop-x", options.cropX));
        options.cropY = static_cast<int>(optionDouble(argc, argv, "--crop-y", options.cropY));
        options.cropWidth = static_cast<int>(optionDouble(argc, argv, "--crop-width", options.cropWidth));
        options.cropHeight = static_cast<int>(optionDouble(argc, argv, "--crop-height", options.cropHeight));
        options.inferenceFps = optionDouble(argc, argv, "--inference-fps", options.inferenceFps);
        options.responseMs = optionDouble(argc, argv, "--response-ms", options.responseMs);
        options.candidateResponseMs = optionDouble(
            argc, argv, "--candidate-response-ms", options.candidateResponseMs);
        options.candidateResponseCounterfactualMs = optionDouble(
            argc, argv, "--candidate-response-counterfactual-ms",
            options.candidateResponseCounterfactualMs);
        if (const auto estimator = optionValue(argc, argv, "--candidate-estimator"))
        {
            const std::string normalized = lower(*estimator);
            if (normalized == "ca" || normalized == "constant_acceleration")
            {
                options.candidateEstimatorMode =
                    CrossDomainReplay::CandidateEstimatorMode::ConstantAcceleration;
            }
            else if (normalized == "gated_ca" ||
                     normalized == "maneuver_gated_ca")
            {
                options.candidateEstimatorMode = CrossDomainReplay::
                    CandidateEstimatorMode::ManeuverGatedConstantAcceleration;
            }
        }
        options.candidateJerkStdDegreesPerSecond3 = optionDouble(
            argc, argv, "--candidate-jerk-std-dps3",
            options.candidateJerkStdDegreesPerSecond3);
        options.candidateManeuverRateThresholdDegreesPerSecond = optionDouble(
            argc, argv, "--candidate-maneuver-rate-threshold-dps",
            options.candidateManeuverRateThresholdDegreesPerSecond);
        options.candidateManeuverHoldSeconds = optionDouble(
            argc, argv, "--candidate-maneuver-hold-ms",
            options.candidateManeuverHoldSeconds * 1000.0) / 1000.0;
        options.kalmanAccelerationStdDegreesPerSecond2 = optionDouble(
            argc, argv, "--kalman-accel-std-dps2",
            options.kalmanAccelerationStdDegreesPerSecond2);
        options.kalmanMovingAccelerationStdDegreesPerSecond2 = optionDouble(
            argc, argv, "--kalman-moving-accel-std-dps2",
            options.kalmanMovingAccelerationStdDegreesPerSecond2);
        options.kalmanMovingRateThresholdDegreesPerSecond = optionDouble(
            argc, argv, "--kalman-moving-rate-threshold-dps",
            options.kalmanMovingRateThresholdDegreesPerSecond);
        options.verticalCatchUpErrorDegrees = optionDouble(
            argc, argv, "--vertical-catch-up-deg",
            options.verticalCatchUpErrorDegrees);
        options.maxCountsPerSecond = optionDouble(argc, argv, "--max-cps", options.maxCountsPerSecond);
        options.integralMs = optionDouble(argc, argv, "--integral-ms", options.integralMs);
        options.candidateIntegralMs = optionDouble(
            argc, argv, "--candidate-integral-ms", options.candidateIntegralMs);
        options.candidateIntegralZoneDegrees = optionDouble(
            argc, argv, "--candidate-integral-zone-deg",
            options.candidateIntegralZoneDegrees);
        options.sensitivity = optionDouble(argc, argv, "--sensitivity", options.sensitivity);
        options.yaw = optionDouble(argc, argv, "--yaw", options.yaw);
        options.pitch = optionDouble(argc, argv, "--pitch", options.pitch);
        options.fovX = optionDouble(argc, argv, "--fov-x", options.fovX);
        options.fovY = optionDouble(argc, argv, "--fov-y", options.fovY);
        options.settleErrorDegrees = optionDouble(
            argc, argv, "--settle-error-deg", options.settleErrorDegrees);
        options.settleRateDegreesPerSecond = optionDouble(
            argc, argv, "--settle-rate-dps", options.settleRateDegreesPerSecond);
        options.reverseConfirmationSeconds = optionDouble(
            argc, argv, "--reverse-confirm-ms",
            options.reverseConfirmationSeconds * 1000.0) / 1000.0;
        options.reverseConfirmationErrorMultiplier = optionDouble(
            argc, argv, "--reverse-confirm-error-multiplier",
            options.reverseConfirmationErrorMultiplier);
        if (const auto confirmRelease = optionValue(
                argc, argv, "--confirm-low-speed-reverse-settle-release"))
        {
            const std::string normalized = lower(*confirmRelease);
            options.confirmLowSpeedReverseSettleRelease = normalized != "0" &&
                normalized != "false" && normalized != "off";
        }
        if (const auto fixedTruth = optionValue(argc, argv, "--static-fixed-truth"))
        {
            const std::string normalized = lower(*fixedTruth);
            options.staticFixedTruth = normalized != "0" &&
                normalized != "false" && normalized != "off";
        }
        if (const auto compensation = optionValue(
                argc, argv, "--candidate-view-motion-compensation"))
        {
            const std::string normalized = lower(*compensation);
            options.candidateViewMotionCompensation = normalized != "0" &&
                normalized != "false" && normalized != "off";
        }
        options.candidateCommandCommitHorizonMs = optionDouble(
            argc, argv, "--candidate-command-commit-horizon-ms",
            options.candidateCommandCommitHorizonMs);
        options.physicalCommandCenterMs = optionDouble(
            argc, argv, "--physical-command-center-ms", options.physicalCommandCenterMs);
        options.physicalCommandResponseMs = optionDouble(
            argc, argv, "--physical-command-response-ms", options.physicalCommandResponseMs);
        if (const auto guard = optionValue(
                argc, argv, "--candidate-settle-entry-command-guard"))
        {
            const std::string normalized = lower(*guard);
            options.candidateSettleEntryCommandGuard = normalized != "0" &&
                normalized != "false" && normalized != "off";
        }
        if (const auto hold = optionValue(
                argc, argv, "--candidate-settle-entry-command-hold"))
        {
            const std::string normalized = lower(*hold);
            options.candidateSettleEntryCommandHold = normalized != "0" &&
                normalized != "false" && normalized != "off";
        }
        options.feedforwardGain = optionDouble(
            argc, argv, "--feedforward-gain", options.feedforwardGain);
        options.leadHorizonSeconds = optionDouble(
            argc, argv, "--lead-horizon-ms",
            options.leadHorizonSeconds * 1000.0) / 1000.0;
        options.leadStrength = optionDouble(
            argc, argv, "--lead-strength", options.leadStrength);
        options.reversalFeedforwardBoost = optionDouble(
            argc, argv, "--reversal-ff-boost", options.reversalFeedforwardBoost);
        options.reversalFeedforwardSeconds = optionDouble(
            argc, argv, "--reversal-ff-ms",
            options.reversalFeedforwardSeconds * 1000.0) / 1000.0;
        if (const auto mode = optionValue(argc, argv, "--trajectory-mode"))
            options.trajectoryMode = parseTrajectoryShaperMode(*mode);
        options.trajectoryOutputHz = optionDouble(
            argc, argv, "--trajectory-output-hz", options.trajectoryOutputHz);
        options.trajectoryMaxAccelerationCountsPerSecond2 = optionDouble(
            argc, argv, "--trajectory-max-accel-cps2",
            options.trajectoryMaxAccelerationCountsPerSecond2);
        options.trajectoryMaxJerkCountsPerSecond3 = optionDouble(
            argc, argv, "--trajectory-max-jerk-cps3",
            options.trajectoryMaxJerkCountsPerSecond3);
        if (const auto ages = optionValue(argc, argv, "--observation-ages-ms"))
        {
            const auto parsed = parseDoubleList(*ages);
            if (!parsed.empty())
                options.observationAgesMs = parsed;
        }
        return options;
    }

    double percentile95(std::vector<double> values)
    {
        if (values.empty())
            return 0.0;
        std::sort(values.begin(), values.end());
        const size_t index = static_cast<size_t>(
            std::floor(0.95 * static_cast<double>(values.size() - 1)));
        return values[index];
    }

    cv::Rect centeredCrop(double centerX, double centerY, int width, int height, int sourceWidth, int sourceHeight)
    {
        const int x = std::clamp(
            static_cast<int>(std::round(centerX - width * 0.5)), 0, std::max(0, sourceWidth - width));
        const int y = std::clamp(
            static_cast<int>(std::round(centerY - height * 0.5)), 0, std::max(0, sourceHeight - height));
        return { x, y, width, height };
    }

#ifndef USE_CUDA
    std::optional<AimbotTarget> selectTarget(const std::vector<Detection>& detections, int width, int height)
    {
        std::vector<cv::Rect> boxes;
        std::vector<int> classes;
        boxes.reserve(detections.size());
        classes.reserve(detections.size());
        for (const auto& detection : detections)
        {
            boxes.push_back(detection.box);
            classes.push_back(detection.classId);
        }
        const auto target = sortTargets(boxes, classes, width, height, config.disable_headshot);
        if (!target)
            return std::nullopt;
        return *target;
    }

    ScenarioTrajectory extractTrajectory(
        DirectMLDetector& detector,
        const std::filesystem::path& path,
        const Options& options)
    {
        ScenarioTrajectory trajectory;
        trajectory.name = lower(path.stem().string());
        trajectory.path = path;

        cv::VideoCapture capture(path.string(), cv::CAP_ANY);
        if (!capture.isOpened())
            throw std::runtime_error("Unable to open video: " + path.string());

        trajectory.sourceWidth = static_cast<int>(capture.get(cv::CAP_PROP_FRAME_WIDTH));
        trajectory.sourceHeight = static_cast<int>(capture.get(cv::CAP_PROP_FRAME_HEIGHT));
        trajectory.videoFps = capture.get(cv::CAP_PROP_FPS);
        trajectory.frameCount = capture.get(cv::CAP_PROP_FRAME_COUNT);
        if (trajectory.sourceWidth < options.cropWidth || trajectory.sourceHeight < options.cropHeight ||
            !std::isfinite(trajectory.videoFps) || trajectory.videoFps <= 0.0)
        {
            throw std::runtime_error("Invalid video geometry or FPS: " + path.string());
        }

        double expectedX = options.cropX + options.cropWidth * 0.5;
        double expectedY = options.cropY + options.cropHeight * 0.5;
        double velocityX = 0.0;
        double velocityY = 0.0;
        double previousDetectedTime = 0.0;
        double previousDetectedX = expectedX;
        double previousDetectedY = expectedY;
        double nextSampleTime = 0.0;
        double lastFullWidthSearchTime = -1.0;
        size_t decodedFrame = 0;
        size_t detectedCount = 0;

        while (capture.grab())
        {
            const double timeSeconds = static_cast<double>(decodedFrame) / trajectory.videoFps;
            ++decodedFrame;
            if (timeSeconds + 1e-9 < nextSampleTime)
                continue;
            nextSampleTime += 1.0 / std::max(1.0, options.inferenceFps);

            cv::Mat frame;
            if (!capture.retrieve(frame) || frame.empty())
                continue;

            const double elapsed = trajectory.points.empty()
                ? 0.0 : timeSeconds - trajectory.points.back().timeSeconds;
            expectedX += velocityX * std::max(0.0, elapsed);
            expectedY += velocityY * std::max(0.0, elapsed);
            cv::Rect cropRect = centeredCrop(
                expectedX, expectedY, options.cropWidth, options.cropHeight,
                frame.cols, frame.rows);
            const cv::Mat crop = frame(cropRect);
            const auto selected = selectTarget(detector.detect(crop), options.cropWidth, options.cropHeight);
            std::optional<std::pair<double, double>> selectedGlobal;
            std::optional<AimbotTarget> selectedMetadata;
            if (selected)
            {
                selectedMetadata = *selected;
                selectedGlobal = std::make_pair(
                    cropRect.x + selected->pivotX,
                    cropRect.y + selected->pivotY);
            }
            else if (timeSeconds - lastFullWidthSearchTime >= 0.25)
            {
                lastFullWidthSearchTime = timeSeconds;
                const int searchY = centeredCrop(
                    frame.cols * 0.5, expectedY,
                    options.cropWidth, options.cropHeight,
                    frame.cols, frame.rows).y;
                const int stride = std::max(1, options.cropWidth * 3 / 4);
                double bestDistance = std::numeric_limits<double>::infinity();
                for (int searchX = 0;; searchX += stride)
                {
                    searchX = std::min(searchX, frame.cols - options.cropWidth);
                    const cv::Rect searchRect(
                        searchX, searchY, options.cropWidth, options.cropHeight);
                    const auto reacquired = selectTarget(
                        detector.detect(frame(searchRect)),
                        options.cropWidth, options.cropHeight);
                    if (reacquired)
                    {
                        const double globalX = searchRect.x + reacquired->pivotX;
                        const double globalY = searchRect.y + reacquired->pivotY;
                        const double distance = std::hypot(globalX - expectedX, globalY - expectedY);
                        if (distance < bestDistance)
                        {
                            bestDistance = distance;
                            selectedMetadata = *reacquired;
                            selectedGlobal = std::make_pair(globalX, globalY);
                        }
                    }
                    if (searchX >= frame.cols - options.cropWidth)
                        break;
                }
            }

            TrajectoryPoint point;
            point.timeSeconds = timeSeconds;
            if (selectedGlobal)
            {
                point.detected = true;
                point.globalX = selectedGlobal->first;
                point.globalY = selectedGlobal->second;
                // 框尺寸和置信度是P0-5框内比例、边缘余量与Kalman测量噪声的输入，
                // 必须与支点来自同一次目标选择，不能在回放时用固定框伪造。
                if (selectedMetadata)
                {
                    point.boxWidth = selectedMetadata->w;
                    point.boxHeight = selectedMetadata->h;
                    point.confidence = selectedMetadata->confidence;
                }
                const double dt = timeSeconds - previousDetectedTime;
                if (detectedCount > 0 && dt > 0.0 && dt <= 0.15)
                {
                    const double sampleVelocityX = (point.globalX - previousDetectedX) / dt;
                    const double sampleVelocityY = (point.globalY - previousDetectedY) / dt;
                    velocityX = velocityX * 0.65 + sampleVelocityX * 0.35;
                    velocityY = velocityY * 0.65 + sampleVelocityY * 0.35;
                }
                else
                {
                    velocityX = velocityY = 0.0;
                }
                expectedX = point.globalX;
                expectedY = point.globalY;
                previousDetectedTime = timeSeconds;
                previousDetectedX = point.globalX;
                previousDetectedY = point.globalY;
                ++detectedCount;
            }
            else
            {
                point.globalX = expectedX;
                point.globalY = expectedY;
            }
            trajectory.points.push_back(point);
        }

        std::cout << "[VideoReplay] " << trajectory.name
                  << " " << trajectory.sourceWidth << 'x' << trajectory.sourceHeight
                  << " fps=" << std::fixed << std::setprecision(3) << trajectory.videoFps
                  << " frames=" << static_cast<long long>(trajectory.frameCount)
                  << " samples=" << trajectory.points.size()
                  << " detected=" << detectedCount << std::endl;
        return trajectory;
    }
#endif

    std::string scenarioKey(const std::string& name);

    Metrics simulate(
        const ScenarioTrajectory& trajectory,
        const Options& options,
        const Candidate& candidate,
        double observationAgeMs)
    {
        BasicTargetFilter filter;
        TargetPredictor predictor;
        BasicAimController controller;
        TargetPredictor::Settings predictorSettings;
        predictorSettings.enabled = candidate.enabled;
        predictorSettings.additionalLeadSeconds = candidate.leadMs / 1000.0;
        predictorSettings.velocityTimeConstantSeconds = candidate.tauMs / 1000.0;
        predictorSettings.predictionStrength = candidate.predictionStrength;
        BasicAimController::Settings controllerSettings;
        controllerSettings.responseSeconds = options.responseMs / 1000.0;
        controllerSettings.maxCountsPerSecond = candidate.maxCountsPerSecond;
        controllerSettings.integralTimeSeconds = options.integralMs / 1000.0;
        controllerSettings.settleRadiusPixels = std::max(2.0, options.cropWidth / 64.0);
        controllerSettings.releaseRadiusPixels = controllerSettings.settleRadiusPixels * 1.6;

        const double countsPerPixelX = AimCoordinateSpace::countsPerSourcePixel(
            options.fovX, trajectory.sourceWidth, options.sensitivity * options.yaw);
        const double countsPerPixelY = AimCoordinateSpace::countsPerSourcePixel(
            options.fovY, trajectory.sourceHeight, options.sensitivity * options.pitch);
        double cameraX = 0.0;
        double cameraY = 0.0;
        double previousObservationTime = 0.0;
        double previousTrajectoryTime = 0.0;
        double previousGlobalX = 0.0;
        double previousGlobalY = 0.0;
        bool hasPreviousDetection = false;
        bool segmentAcquired = false;
        std::vector<double> absErrorsX;
        std::vector<double> absErrorsY;
        struct TimedError
        {
            double timeSeconds = 0.0;
            double absX = 0.0;
            double absY = 0.0;
        };
        std::vector<TimedError> segmentErrors;
        const std::string scenario = scenarioKey(trajectory.name);
        const bool steadyWindowScenario = scenario == "left" || scenario == "right";
        auto flushSegmentErrors = [&]() {
            if (segmentErrors.empty())
                return;
            const double threshold = steadyWindowScenario
                ? segmentErrors.back().timeSeconds - 1.0
                : segmentErrors.front().timeSeconds + 1.0;
            for (const auto& error : segmentErrors)
            {
                if (error.timeSeconds + 1e-9 < threshold)
                    continue;
                absErrorsX.push_back(error.absX);
                absErrorsY.push_back(error.absY);
            }
            segmentErrors.clear();
        };
        size_t outsideCrop = 0;

        for (size_t index = 0; index < trajectory.points.size(); ++index)
        {
            const auto& point = trajectory.points[index];
            if (!point.detected)
                continue;

            const TrajectoryPoint previousPoint{
                previousTrajectoryTime, previousGlobalX, previousGlobalY, hasPreviousDetection
            };
            const bool newSegment = hasPreviousDetection && IsNewTrajectorySegment(
                previousPoint, point, 0.10, std::max(64.0, options.cropWidth * 0.20));
            if (newSegment)
            {
                flushSegmentErrors();
                filter.reset();
                predictor.reset();
                controller.reset();
                cameraX = cameraY = 0.0;
                previousObservationTime = 0.0;
                segmentAcquired = false;
            }
            previousGlobalX = point.globalX;
            previousGlobalY = point.globalY;
            previousTrajectoryTime = point.timeSeconds;
            hasPreviousDetection = true;

            const double rawX = ObservedCoordinate(point.globalX, cameraX, options.cropX);
            const double rawY = ObservedCoordinate(point.globalY, cameraY, options.cropY);
            if (rawX < 0.0 || rawX >= options.cropWidth || rawY < 0.0 || rawY >= options.cropHeight)
            {
                if (segmentAcquired)
                {
                    filter.reset();
                    predictor.reset();
                    controller.reset();
                    previousObservationTime = 0.0;
                    ++outsideCrop;
                }
                continue;
            }
            if (!segmentAcquired)
            {
                segmentAcquired = true;
            }

            const auto observationTime = std::chrono::steady_clock::time_point(
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(point.timeSeconds + 1.0)));
            const auto controlTime = observationTime + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double, std::milli>(observationAgeMs));
            const double fallbackSeconds = 1.0 / std::max(1.0, options.inferenceFps);
            const auto filtered = filter.update(
                point.globalX, point.globalY,
                observationTime, fallbackSeconds, options.cropWidth);
            const auto predicted = predictor.update(
                filtered.x, filtered.y, observationTime, controlTime,
                options.cropWidth, predictorSettings);

            double dt = fallbackSeconds;
            if (previousObservationTime > 0.0)
                dt = std::clamp(point.timeSeconds - previousObservationTime, 1.0 / 1000.0, 0.050);
            previousObservationTime = point.timeSeconds;
            const auto output = controller.update(
                predicted.x - cameraX - (options.cropX + options.cropWidth * 0.5),
                predicted.y - cameraY - (options.cropY + options.cropHeight * 0.5),
                dt, countsPerPixelX, countsPerPixelY, controllerSettings);

            const double currentTime = point.timeSeconds + observationAgeMs / 1000.0;
            const double actualGlobalX = InterpolateCoordinate(trajectory.points, index, currentTime, true);
            const double actualGlobalY = InterpolateCoordinate(trajectory.points, index, currentTime, false);
            segmentErrors.push_back({
                point.timeSeconds,
                std::abs(actualGlobalX - cameraX - (options.cropX + options.cropWidth * 0.5)),
                std::abs(actualGlobalY - cameraY - (options.cropY + options.cropHeight * 0.5))
            });

            cameraX += std::round(output.countsX) / countsPerPixelX;
            cameraY += std::round(output.countsY) / countsPerPixelY;
        }
        flushSegmentErrors();

        Metrics metrics;
        metrics.samples = absErrorsX.size();
        metrics.outsideCrop = outsideCrop;
        if (!absErrorsX.empty())
        {
            metrics.meanAbsX = std::accumulate(absErrorsX.begin(), absErrorsX.end(), 0.0) / absErrorsX.size();
            metrics.meanAbsY = std::accumulate(absErrorsY.begin(), absErrorsY.end(), 0.0) / absErrorsY.size();
            metrics.p95AbsX = percentile95(absErrorsX);
            metrics.p95AbsY = percentile95(absErrorsY);
        }
        return metrics;
    }

    std::vector<Candidate> candidates(double baselineMaxCountsPerSecond)
    {
        std::vector<double> speedLimits{
            baselineMaxCountsPerSecond,
            std::min(4000.0, baselineMaxCountsPerSecond * 1.25),
            3200.0,
            4000.0
        };
        std::sort(speedLimits.begin(), speedLimits.end());
        speedLimits.erase(std::unique(speedLimits.begin(), speedLimits.end()), speedLimits.end());

        std::vector<Candidate> result;
        for (double maxCountsPerSecond : speedLimits)
        {
            result.push_back({ false, 0.0, 50.0, maxCountsPerSecond, 0.0 });
            for (double lead : { 0.0, 10.0, 20.0, 30.0, 35.0, 40.0, 50.0 })
            {
                for (double tau : { 40.0, 50.0, 60.0, 75.0, 100.0 })
                {
                    for (double strength : { 1.0, 1.5, 2.0, 2.5, 3.0 })
                        result.push_back({ true, lead, tau, maxCountsPerSecond, strength });
                }
            }
        }
        return result;
    }

    std::string scenarioKey(const std::string& name)
    {
        if (name.find("static") != std::string::npos) return "static";
        if (name.find("reverse") != std::string::npos) return "reverse";
        if (name.find("jump") != std::string::npos) return "jump";
        if (name.find("left") != std::string::npos) return "left";
        if (name.find("right") != std::string::npos) return "right";
        return name;
    }

    void updateWorst(Metrics& worst, const Metrics& value)
    {
        if (worst.samples == 0)
            worst = value;
        else
        {
            worst.samples = std::min(worst.samples, value.samples);
            worst.outsideCrop = std::max(worst.outsideCrop, value.outsideCrop);
            worst.meanAbsX = std::max(worst.meanAbsX, value.meanAbsX);
            worst.p95AbsX = std::max(worst.p95AbsX, value.p95AbsX);
            worst.meanAbsY = std::max(worst.meanAbsY, value.meanAbsY);
            worst.p95AbsY = std::max(worst.p95AbsY, value.p95AbsY);
        }
    }

    double metricP95(const CandidateResult& result, const std::string& scenario, bool horizontal)
    {
        const auto found = result.worstByScenario.find(scenario);
        if (found == result.worstByScenario.end() || found->second.samples == 0)
            return std::numeric_limits<double>::infinity();
        return horizontal ? found->second.p95AbsX : found->second.p95AbsY;
    }
}

bool IsRequested(int argc, char** argv)
{
    return optionValue(argc, argv, "--video-replay").has_value() ||
        optionValue(argc, argv, "--cross-domain-replay").has_value();
}

int Run(int argc, char** argv)
{
#ifdef USE_CUDA
    (void)argc;
    (void)argv;
    std::cerr << "[VideoReplay] Video replay parameter search is available in the DML build." << std::endl;
    return 2;
#else
    try
    {
        const Options options = parseOptions(argc, argv);
        // 物理相机响应仅用于离线契约敏感性分析。这里先统一归一化，确保实际执行、
        // summary与decision记录的是同一组有限值，避免异常输入污染回放身份。
        const double physicalCommandCenterMs =
            std::isfinite(options.physicalCommandCenterMs)
            ? std::clamp(options.physicalCommandCenterMs, 0.0, 250.0)
            : 60.0;
        const double physicalCommandResponseMs =
            std::isfinite(options.physicalCommandResponseMs)
            ? std::clamp(options.physicalCommandResponseMs, 0.0, 100.0)
            : 0.0;
        if (!std::filesystem::is_directory(options.videoRoot))
            throw std::runtime_error("Video root does not exist: " + options.videoRoot.string());
        if (!std::filesystem::is_regular_file(options.modelPath))
            throw std::runtime_error("DML model does not exist. Use --video-model with an absolute path.");
        std::vector<CrossDomainReplay::Variant> crossDomainVariants;
        if (options.crossDomain)
        {
            crossDomainVariants = CrossDomainReplay::BuildRequiredVariants();
            for (auto& variant : crossDomainVariants)
            {
                variant.commandToFrameDelayMs = physicalCommandCenterMs;
                variant.commandResponseMs = physicalCommandResponseMs;
            }
            for (const std::string& detailVariant : options.crossDomainDetailVariants)
            {
                const bool detailVariantExists = std::any_of(
                    crossDomainVariants.begin(), crossDomainVariants.end(),
                    [&](const auto& variant) {
                        return variant.name == detailVariant;
                    });
                if (!detailVariantExists)
                {
                    throw std::runtime_error(
                        "Unknown --cross-domain-detail-variant: " + detailVariant);
                }
            }
        }
        std::filesystem::create_directories(options.outputRoot);

        config.detection_resolution = options.cropWidth;
        config.confidence_threshold = std::clamp(config.confidence_threshold, 0.01f, 1.0f);
        DirectMLDetector detector(options.modelPath.string());
        if (!detector.isReady())
            throw std::runtime_error("DML detector failed to initialize for video replay.");

        std::vector<std::filesystem::path> videos;
        for (const auto& entry : std::filesystem::directory_iterator(options.videoRoot))
        {
            if (entry.is_regular_file() && lower(entry.path().extension().string()) == ".mp4")
                videos.push_back(entry.path());
        }
        std::sort(videos.begin(), videos.end());
        if (videos.empty())
            throw std::runtime_error("No MP4 videos were found.");

        std::vector<ScenarioTrajectory> trajectories;
        for (const auto& video : videos)
            trajectories.push_back(extractTrajectory(detector, video, options));

        std::ofstream observations(options.outputRoot / "video_observations.csv");
        observations << "Scenario,TimeSeconds,Detected,GlobalX,GlobalY,BoxWidth,BoxHeight,Confidence\n";
        for (const auto& trajectory : trajectories)
        {
            for (const auto& point : trajectory.points)
            {
                observations << trajectory.name << ',' << std::fixed << std::setprecision(6)
                             << point.timeSeconds << ',' << (point.detected ? 1 : 0) << ','
                             << point.globalX << ',' << point.globalY << ','
                             << point.boxWidth << ',' << point.boxHeight << ','
                             << point.confidence << '\n';
            }
        }

        if (options.crossDomain)
        {
            std::vector<CrossDomainReplay::Comparison> comparisons;
            std::vector<CrossDomainReplay::Attribution> attributions;
            std::vector<CrossDomainReplay::ResponseCounterfactual>
                responseCounterfactuals;
            std::vector<CrossDomainReplay::Comparison>
                responseCounterfactualComparisons;
            const bool responseCounterfactualEnabled =
                std::isfinite(options.candidateResponseCounterfactualMs) &&
                options.candidateResponseCounterfactualMs > 0.0;
            if (responseCounterfactualEnabled && options.crossDomainAttribution)
            {
                throw std::runtime_error(
                    "Response counterfactual and attribution modes are mutually exclusive.");
            }
            const auto& variants = crossDomainVariants;
            CrossDomainReplay::ControllerSettings settings;
            settings.kalmanAccelerationStdDegreesPerSecond2 =
                options.kalmanAccelerationStdDegreesPerSecond2;
            settings.kalmanMovingAccelerationStdDegreesPerSecond2 =
                options.kalmanMovingAccelerationStdDegreesPerSecond2;
            settings.kalmanMovingRateThresholdDegreesPerSecond =
                options.kalmanMovingRateThresholdDegreesPerSecond;
            settings.responseSeconds = options.responseMs / 1000.0;
            settings.candidateResponseSeconds = options.candidateResponseMs > 0.0
                ? options.candidateResponseMs / 1000.0 : 0.0;
            settings.candidateEstimatorMode = options.candidateEstimatorMode;
            settings.candidateJerkStdDegreesPerSecond3 =
                options.candidateJerkStdDegreesPerSecond3;
            settings.candidateManeuverRateThresholdDegreesPerSecond =
                options.candidateManeuverRateThresholdDegreesPerSecond;
            settings.candidateManeuverHoldSeconds =
                options.candidateManeuverHoldSeconds;
            settings.verticalCatchUpErrorDegrees =
                options.verticalCatchUpErrorDegrees;
            settings.maxCountsPerSecond = options.maxCountsPerSecond;
            settings.integralTimeSeconds = options.candidateIntegralMs > 0.0
                ? options.candidateIntegralMs / 1000.0 : 0.0;
            settings.integralZoneDegrees = options.candidateIntegralZoneDegrees;
            settings.legacyPredictionLeadSeconds = 0.050;
            settings.legacyPredictionWindowSeconds = 0.050;
            settings.legacyPredictionStrength = 1.0;
            settings.degreesPerCountX = options.sensitivity * options.yaw;
            settings.degreesPerCountY = options.sensitivity * options.pitch;
            settings.settleErrorDegrees = options.settleErrorDegrees;
            settings.settleRateDegreesPerSecond = options.settleRateDegreesPerSecond;
            settings.reverseConfirmationSeconds = options.reverseConfirmationSeconds;
            settings.reverseConfirmationErrorMultiplier =
                options.reverseConfirmationErrorMultiplier;
            settings.confirmLowSpeedReverseSettleRelease =
                options.confirmLowSpeedReverseSettleRelease;
            settings.staticFixedTruth = options.staticFixedTruth;
            settings.candidateViewMotionCompensation =
                options.candidateViewMotionCompensation;
            settings.candidateCommandCommitHorizonSeconds =
                options.candidateCommandCommitHorizonMs / 1000.0;
            settings.candidateSettleEntryCommandGuard =
                options.candidateSettleEntryCommandGuard;
            settings.candidateSettleEntryCommandHold =
                options.candidateSettleEntryCommandHold;
            settings.feedforwardGain = options.feedforwardGain;
            settings.leadHorizonSeconds = options.leadHorizonSeconds;
            settings.leadStrength = options.leadStrength;
            settings.reversalFeedforwardBoost = options.reversalFeedforwardBoost;
            settings.reversalFeedforwardSeconds = options.reversalFeedforwardSeconds;
            settings.trajectoryMode = options.trajectoryMode;
            settings.trajectoryOutputHz = options.trajectoryOutputHz;
            settings.trajectoryMaxAccelerationCountsPerSecond2 =
                options.trajectoryMaxAccelerationCountsPerSecond2;
            settings.trajectoryMaxJerkCountsPerSecond3 =
                options.trajectoryMaxJerkCountsPerSecond3;
            const auto framePath = options.outputRoot / "cross_domain_frames.csv";
            const auto counterfactualFramePath = options.outputRoot /
                "cross_domain_response_counterfactual_frames.csv";
            std::error_code removeError;
            std::filesystem::remove(framePath, removeError);
            std::filesystem::remove(counterfactualFramePath, removeError);
            size_t passed = 0;
            size_t counterfactualPassed = 0;
            size_t responseChangedCohorts = 0;
            for (const auto& trajectory : trajectories)
            {
                CrossDomainReplay::SourceTrajectory source;
                source.scenario = trajectory.name;
                source.sourceWidth = trajectory.sourceWidth;
                source.sourceHeight = trajectory.sourceHeight;
                source.sourceFovXDegrees = options.fovX;
                source.sourceFovYDegrees = options.fovY;
                source.centerX = options.cropX + options.cropWidth * 0.5;
                source.centerY = options.cropY + options.cropHeight * 0.5;
                source.detectionWidth = options.cropWidth;
                source.detectionHeight = options.cropHeight;
                source.points = trajectory.points;
                for (const auto& variant : variants)
                {
                    // 全矩阵保留汇总指标；逐帧诊断默认只记录实机近似基准域，也允许
                    // 用一个或多个精确名称选择失败物理域；Variant列保持每行来源可审计。
                    const bool detailedVariant = std::find(
                        options.crossDomainDetailVariants.begin(),
                        options.crossDomainDetailVariants.end(),
                        variant.name) != options.crossDomainDetailVariants.end();
                    CrossDomainReplay::Comparison comparison;
                    if (responseCounterfactualEnabled)
                    {
                        auto responseCounterfactual =
                            CrossDomainReplay::RunResponseCounterfactual(
                                source, variant, settings,
                                options.candidateResponseCounterfactualMs / 1000.0,
                                detailedVariant ? framePath : std::filesystem::path{},
                                detailedVariant ? counterfactualFramePath :
                                    std::filesystem::path{});
                        comparison = responseCounterfactual.baseline;
                        counterfactualPassed +=
                            responseCounterfactual.counterfactual.passed ? 1U : 0U;
                        responseChangedCohorts +=
                            responseCounterfactual.cohortStable ? 0U : 1U;
                        responseCounterfactualComparisons.push_back(
                            responseCounterfactual.counterfactual);
                        responseCounterfactuals.push_back(
                            std::move(responseCounterfactual));
                    }
                    else if (options.crossDomainAttribution)
                    {
                        auto attribution = CrossDomainReplay::RunAttribution(
                            source, variant, settings,
                            detailedVariant ? framePath : std::filesystem::path{});
                        comparison = attribution.baseline;
                        attributions.push_back(std::move(attribution));
                    }
                    else
                    {
                        comparison = CrossDomainReplay::RunComparison(
                            source, variant, settings,
                            detailedVariant ? framePath : std::filesystem::path{});
                    }
                    passed += comparison.passed ? 1U : 0U;
                    comparisons.push_back(std::move(comparison));
                }
            }
            CrossDomainReplay::WriteSummary(
                options.outputRoot / "cross_domain_summary.csv", comparisons);
            if (responseCounterfactualEnabled)
            {
                CrossDomainReplay::WriteSummary(
                    options.outputRoot /
                        "cross_domain_response_counterfactual_matrix.csv",
                    responseCounterfactualComparisons);
                CrossDomainReplay::WriteResponseCounterfactualSummary(
                    options.outputRoot /
                        "cross_domain_response_counterfactual.csv",
                    responseCounterfactuals);
            }
            if (options.crossDomainAttribution)
            {
                CrossDomainReplay::WriteAttributionSummary(
                    options.outputRoot / "cross_domain_attribution.csv",
                    attributions);
            }
            std::map<std::string, size_t> attributionCounts;
            size_t oraclePassed = 0;
            size_t unlimitedPassed = 0;
            size_t changedCohorts = 0;
            for (const auto& attribution : attributions)
            {
                ++attributionCounts[attribution.classification];
                oraclePassed += attribution.oracleEstimator.passed ? 1U : 0U;
                unlimitedPassed += attribution.unlimitedActuator.passed ? 1U : 0U;
                changedCohorts += attribution.cohortStable ? 0U : 1U;
            }
            std::ofstream decision(options.outputRoot / "cross_domain_decision.txt");
            decision << "KalmanAccelerationStdDps2="
                     << (std::isfinite(options.kalmanAccelerationStdDegreesPerSecond2)
                             ? std::clamp(
                                 options.kalmanAccelerationStdDegreesPerSecond2,
                                 1.0, 2000.0)
                             : 90.0) << '\n'
                     << "KalmanMovingAccelerationStdDps2="
                     << (std::isfinite(options.kalmanMovingAccelerationStdDegreesPerSecond2)
                             ? std::clamp(
                                 options.kalmanMovingAccelerationStdDegreesPerSecond2,
                                 1.0, 2000.0)
                             : 360.0) << '\n'
                     << "KalmanMovingRateThresholdDps="
                     << (std::isfinite(options.kalmanMovingRateThresholdDegreesPerSecond)
                             ? std::clamp(
                                 options.kalmanMovingRateThresholdDegreesPerSecond,
                                 0.1, 1000.0)
                             : 8.0) << '\n'
                     << "LegacyResponseMs="
                     << (std::isfinite(options.responseMs)
                             ? std::clamp(options.responseMs, 10.0, 500.0)
                             : 80.0) << '\n'
                     << "CandidateResponseMs="
                     << (std::isfinite(options.candidateResponseMs) &&
                             options.candidateResponseMs > 0.0
                             ? std::clamp(options.candidateResponseMs, 10.0, 500.0)
                             : (std::isfinite(options.responseMs)
                                 ? std::clamp(options.responseMs, 10.0, 500.0)
                                 : 80.0)) << '\n'
                     << "CandidateResponseCounterfactualEnabled="
                     << (responseCounterfactualEnabled ? 1 : 0) << '\n'
                     << "CandidateResponseCounterfactualMs="
                     << (responseCounterfactualEnabled
                             ? std::clamp(
                                 options.candidateResponseCounterfactualMs,
                                 10.0, 500.0)
                             : 0.0) << '\n'
                     << "CandidateEstimatorMode="
                     << CrossDomainReplay::candidateEstimatorModeName(
                         options.candidateEstimatorMode) << '\n'
                     << "CandidateJerkStdDps3="
                     << (std::isfinite(options.candidateJerkStdDegreesPerSecond3)
                             ? std::clamp(options.candidateJerkStdDegreesPerSecond3,
                                 1.0, 100000.0)
                             : 8000.0) << '\n'
                     << "CandidateManeuverRateThresholdDps="
                     << (std::isfinite(
                             options.candidateManeuverRateThresholdDegreesPerSecond)
                             ? std::clamp(
                                 options.candidateManeuverRateThresholdDegreesPerSecond,
                                 0.1, 1000.0)
                             : 12.0) << '\n'
                     << "CandidateManeuverHoldMs="
                     << (std::isfinite(options.candidateManeuverHoldSeconds)
                             ? std::clamp(options.candidateManeuverHoldSeconds,
                                 0.0, 1.0) * 1000.0
                             : 120.0) << '\n'
                     << "CandidateIntegralTimeMs="
                     << (std::isfinite(options.candidateIntegralMs) &&
                             options.candidateIntegralMs > 0.0
                             ? std::clamp(options.candidateIntegralMs, 50.0, 2000.0)
                             : 0.0) << '\n'
                     << "CandidateIntegralZoneDeg="
                     << (std::isfinite(options.candidateIntegralZoneDegrees)
                             ? std::clamp(options.candidateIntegralZoneDegrees, 0.0, 10.0)
                             : 1.0) << '\n'
                     << "FeedforwardGain="
                     << (std::isfinite(options.feedforwardGain)
                             ? std::clamp(options.feedforwardGain, 0.0, 2.0)
                             : 0.0) << '\n'
                     << "LeadHorizonMs="
                     << (std::isfinite(options.leadHorizonSeconds)
                             ? std::clamp(options.leadHorizonSeconds, 0.0, 0.250) * 1000.0
                             : 0.0) << '\n'
                     << "LeadStrength="
                     << (std::isfinite(options.leadStrength)
                             ? std::clamp(options.leadStrength, 0.0, 4.0)
                             : 0.0) << '\n'
                     << "ReversalFeedforwardBoost="
                     << (std::isfinite(options.reversalFeedforwardBoost)
                             ? std::clamp(options.reversalFeedforwardBoost, 0.0, 2.0)
                             : 0.0) << '\n'
                     << "ReversalFeedforwardMs="
                     << (std::isfinite(options.reversalFeedforwardSeconds)
                             ? std::clamp(options.reversalFeedforwardSeconds, 0.0, 0.500) * 1000.0
                             : 0.0) << '\n'
                     << "ReverseConfirmationErrorMultiplier="
                     << (std::isfinite(options.reverseConfirmationErrorMultiplier)
                             ? std::clamp(options.reverseConfirmationErrorMultiplier, 1.5, 2.0)
                             : 1.5) << '\n'
                     << "ConfirmLowSpeedReverseSettleRelease="
                     << (options.confirmLowSpeedReverseSettleRelease ? 1 : 0) << '\n'
                     << "StaticFixedTruth=" << (options.staticFixedTruth ? 1 : 0) << '\n'
                     << "CandidateViewMotionCompensation="
                     << (options.candidateViewMotionCompensation ? 1 : 0) << '\n'
                     << "CandidateCommandCommitHorizonMs="
                     << options.candidateCommandCommitHorizonMs << '\n'
                     << "PhysicalCommandCenterMs="
                     << physicalCommandCenterMs << '\n'
                     << "PhysicalCommandResponseMs="
                     << physicalCommandResponseMs << '\n'
                     << "CandidateSettleEntryCommandGuard="
                     << (options.candidateSettleEntryCommandGuard ? 1 : 0) << '\n'
                     << "CandidateSettleEntryCommandHold="
                     << (options.candidateSettleEntryCommandHold ? 1 : 0) << '\n'
                     << "TrajectoryMode="
                     << trajectoryShaperModeName(options.trajectoryMode) << '\n'
                     << "TrajectoryOutputHz=" << options.trajectoryOutputHz << '\n'
                     << "TrajectoryMaxAccelerationCountsPerSecond2="
                     << options.trajectoryMaxAccelerationCountsPerSecond2 << '\n'
                     << "TrajectoryMaxJerkCountsPerSecond3="
                     << options.trajectoryMaxJerkCountsPerSecond3 << '\n'
                     << "DetailedVariant=";
            for (size_t index = 0; index < options.crossDomainDetailVariants.size(); ++index)
            {
                if (index > 0)
                    decision << ',';
                decision << options.crossDomainDetailVariants[index];
            }
            decision << '\n'
                     << "AttributionEnabled="
                     << (options.crossDomainAttribution ? 1 : 0) << '\n'
                     << "AttributionOraclePassed=" << oraclePassed << '\n'
                     << "AttributionUnlimitedPassed=" << unlimitedPassed << '\n'
                     << "AttributionChangedCohorts=" << changedCohorts << '\n';
            for (const auto& [classification, count] : attributionCounts)
                decision << "Attribution_" << classification << '=' << count << '\n';
            decision
                     << "ResponseCounterfactualPassed="
                     << counterfactualPassed << '\n'
                     << "ResponseCounterfactualChangedCohorts="
                     << responseChangedCohorts << '\n'
                     << "Comparisons=" << comparisons.size() << '\n'
                     << "Passed=" << passed << '\n'
                     << "Failed=" << comparisons.size() - passed << '\n'
                     << "Promotion=" << (passed == comparisons.size() ? "PASS" : "HOLD_SHADOW") << '\n';
            std::cout << "[CrossDomainReplay] " << passed << '/' << comparisons.size()
                      << " gates passed; promotion="
                      << (passed == comparisons.size() ? "PASS" : "HOLD_SHADOW") << std::endl;
            if (responseCounterfactualEnabled)
            {
                std::cout << "[ResponseCounterfactual] "
                          << counterfactualPassed << '/' << comparisons.size()
                          << " gates passed; changed_cohorts="
                          << responseChangedCohorts << std::endl;
                if (responseChangedCohorts != 0)
                    return 4;
            }
            return passed == comparisons.size() ? 0 : 3;
        }

        std::ofstream grid(options.outputRoot / "prediction_grid.csv");
        grid << "Enabled,LeadMs,TauMs,MaxCountsPerSecond,PredictionStrength,ObservationAgeMs,Scenario,Samples,OutsideCrop,MeanAbsX,P95AbsX,MeanAbsY,P95AbsY\n";
        std::vector<CandidateResult> results;
        for (const Candidate& candidate : candidates(options.maxCountsPerSecond))
        {
            CandidateResult result;
            result.candidate = candidate;
            for (double ageMs : options.observationAgesMs)
            {
                for (const auto& trajectory : trajectories)
                {
                    const Metrics metrics = simulate(trajectory, options, candidate, ageMs);
                    const std::string scenario = scenarioKey(trajectory.name);
                    updateWorst(result.worstByScenario[scenario], metrics);
                    grid << (candidate.enabled ? 1 : 0) << ',' << candidate.leadMs << ',' << candidate.tauMs << ','
                         << candidate.maxCountsPerSecond << ',' << candidate.predictionStrength << ','
                         << ageMs << ',' << scenario << ','
                         << metrics.samples << ',' << metrics.outsideCrop << ','
                         << metrics.meanAbsX << ',' << metrics.p95AbsX << ','
                         << metrics.meanAbsY << ',' << metrics.p95AbsY << '\n';
                }
            }
            const double left = metricP95(result, "left", true);
            const double right = metricP95(result, "right", true);
            result.movingP95 = (left + right) * 0.5;
            result.reverseP95 = metricP95(result, "reverse", true);
            result.staticP95X = metricP95(result, "static", true);
            result.staticP95Y = metricP95(result, "static", false);
            result.jumpP95 = metricP95(result, "jump", true);
            results.push_back(result);
        }

        if (results.empty() || !std::isfinite(results.front().movingP95))
            throw std::runtime_error("Required static/left/right/reverse scenarios were not extracted.");
        const CandidateResult baseline = results.front();
        const bool hasJumpScenario = std::isfinite(baseline.jumpP95);
        for (auto& result : results)
        {
            result.improvementPercent = baseline.movingP95 > 0.0
                ? (baseline.movingP95 - result.movingP95) / baseline.movingP95 * 100.0 : 0.0;
            result.jumpImprovementPercent = hasJumpScenario && baseline.jumpP95 > 0.0
                ? (baseline.jumpP95 - result.jumpP95) / baseline.jumpP95 * 100.0 : 0.0;
            size_t outside = 0;
            for (const auto& item : result.worstByScenario)
                outside += item.second.outsideCrop;
            size_t baselineOutside = 0;
            for (const auto& item : baseline.worstByScenario)
                baselineOutside += item.second.outsideCrop;
            result.valid = std::isfinite(result.movingP95) &&
                result.improvementPercent >= 10.0 &&
                (!hasJumpScenario || result.jumpImprovementPercent >= 10.0) &&
                result.staticP95X <= baseline.staticP95X + 0.5 &&
                result.staticP95Y <= baseline.staticP95Y + 0.5 &&
                result.reverseP95 <= baseline.reverseP95 * 1.10 &&
                outside <= baselineOutside;
            result.score = result.valid
                ? result.movingP95 + result.reverseP95 * 0.5 +
                  (hasJumpScenario ? result.jumpP95 * 0.5 : 0.0) +
                  result.staticP95X + result.staticP95Y : std::numeric_limits<double>::infinity();
        }
        const auto best = std::min_element(results.begin() + 1, results.end(), [](const auto& left, const auto& right) {
            return left.score < right.score;
        });

        std::ofstream summary(options.outputRoot / "prediction_recommendation.txt");
        summary << std::fixed << std::setprecision(3)
                << "Model=" << options.modelPath.string() << '\n'
                << "InferenceFps=" << options.inferenceFps << '\n'
                << "ObservationAgesMs=";
        for (size_t index = 0; index < options.observationAgesMs.size(); ++index)
            summary << (index == 0 ? "" : ",") << options.observationAgesMs[index];
        summary << '\n'
                << "BaselineMovingP95=" << baseline.movingP95 << '\n'
                << "BaselineReverseP95=" << baseline.reverseP95 << '\n'
                << "BaselineStaticP95X=" << baseline.staticP95X << '\n'
                << "BaselineStaticP95Y=" << baseline.staticP95Y << '\n';
        if (hasJumpScenario)
            summary << "BaselineJumpP95=" << baseline.jumpP95 << '\n';
        if (best != results.end() && best->valid && std::isfinite(best->score))
        {
            summary << "RecommendedPredictionEnabled=" << (best->candidate.enabled ? 1 : 0) << '\n'
                    << "RecommendedLeadMs=" << best->candidate.leadMs << '\n'
                    << "RecommendedTauMs=" << best->candidate.tauMs << '\n'
                    << "RecommendedMaxCountsPerSecond=" << best->candidate.maxCountsPerSecond << '\n'
                    << "RecommendedPredictionStrength=" << best->candidate.predictionStrength << '\n'
                    << "MovingP95=" << best->movingP95 << '\n'
                    << "ReverseP95=" << best->reverseP95 << '\n'
                    << "StaticP95X=" << best->staticP95X << '\n'
                    << "StaticP95Y=" << best->staticP95Y << '\n'
                    << "MovingImprovementPercent=" << best->improvementPercent << '\n';
            if (hasJumpScenario)
            {
                summary << "JumpP95=" << best->jumpP95 << '\n'
                        << "JumpImprovementPercent=" << best->jumpImprovementPercent << '\n';
            }
            std::cout << "[VideoReplay] Recommended lead=" << best->candidate.leadMs
                      << " ms tau=" << best->candidate.tauMs
                      << " ms max_cps=" << best->candidate.maxCountsPerSecond
                      << " strength=" << best->candidate.predictionStrength
                      << " counts/s, moving improvement=" << best->improvementPercent << "%" << std::endl;
        }
        else
        {
            summary << "Recommendation=PredictionOff\nReason=No candidate passed static, reverse and crop-loss constraints.\n";
            std::cout << "[VideoReplay] No prediction candidate passed safety constraints." << std::endl;
        }
        std::cout << "[VideoReplay] Results: " << options.outputRoot.string() << std::endl;
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "[VideoReplay] " << error.what() << std::endl;
        return 1;
    }
#endif
}
}
