#include "shadow_response_replay.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace ShadowResponseReplay
{
namespace
{
std::vector<std::string> parseCsvLine(const std::string& line)
{
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;
    for (std::size_t index = 0; index < line.size(); ++index)
    {
        const char ch = line[index];
        if (ch == '"')
        {
            if (quoted && index + 1 < line.size() && line[index + 1] == '"')
            {
                field.push_back('"');
                ++index;
            }
            else
            {
                quoted = !quoted;
            }
        }
        else if (ch == ',' && !quoted)
        {
            fields.push_back(field);
            field.clear();
        }
        else if (ch != '\r')
        {
            field.push_back(ch);
        }
    }
    fields.push_back(field);
    return fields;
}

bool parseDouble(const std::string& value, double& result)
{
    try
    {
        std::size_t consumed = 0;
        result = std::stod(value, &consumed);
        return consumed == value.size() && std::isfinite(result);
    }
    catch (...)
    {
        return false;
    }
}

bool parseInt64(const std::string& value, std::int64_t& result)
{
    try
    {
        std::size_t consumed = 0;
        result = std::stoll(value, &consumed);
        return consumed == value.size();
    }
    catch (...)
    {
        return false;
    }
}

bool parseUInt64(const std::string& value, std::uint64_t& result)
{
    try
    {
        std::size_t consumed = 0;
        result = std::stoull(value, &consumed);
        return consumed == value.size();
    }
    catch (...)
    {
        return false;
    }
}

bool parseInt(const std::string& value, int& result)
{
    std::int64_t wide = 0;
    if (!parseInt64(value, wide) ||
        wide < std::numeric_limits<int>::min() ||
        wide > std::numeric_limits<int>::max())
    {
        return false;
    }
    result = static_cast<int>(wide);
    return true;
}

bool parseFlag(const std::string& value, bool& result)
{
    if (value == "1" || value == "true" || value == "True")
    {
        result = true;
        return true;
    }
    if (value == "0" || value == "false" || value == "False")
    {
        result = false;
        return true;
    }
    return false;
}

double responseFraction(std::int64_t queryTimeNs, std::int64_t sendTimeNs,
    const AxisResponse& response)
{
    const double centerMs = std::clamp(response.centerMs, 0.0, 250.0);
    const double widthMs = std::clamp(response.widthMs, 0.0, 100.0);
    const double startNs = static_cast<double>(sendTimeNs) +
        std::max(0.0, centerMs - widthMs * 0.5) * 1'000'000.0;
    const double queryNs = static_cast<double>(queryTimeNs);
    if (queryNs < startNs)
        return 0.0;
    if (widthMs <= 1e-9)
        return 1.0;
    return std::clamp((queryNs - startNs) / (widthMs * 1'000'000.0), 0.0, 1.0);
}

std::pair<double, double> cameraAt(const Timeline& timeline,
    std::int64_t queryTimeNs, const Candidate& candidate,
    double degreesPerCountXOverride, double degreesPerCountYOverride)
{
    double yaw = 0.0;
    double pitch = 0.0;
    for (const Command& command : timeline.commands)
    {
        if (command.sendTimeNs > queryTimeNs)
            break;
        const double degreesPerCountX = degreesPerCountXOverride > 0.0
            ? degreesPerCountXOverride : command.degreesPerCountX;
        const double degreesPerCountY = degreesPerCountYOverride > 0.0
            ? degreesPerCountYOverride : command.degreesPerCountY;
        yaw += static_cast<double>(command.countsX) * degreesPerCountX *
            responseFraction(queryTimeNs, command.sendTimeNs, candidate.horizontal);
        pitch += static_cast<double>(command.countsY) * degreesPerCountY *
            responseFraction(queryTimeNs, command.sendTimeNs, candidate.vertical);
    }
    return { yaw, pitch };
}

std::pair<double, double> cameraRateAt(const Timeline& timeline,
    std::int64_t queryTimeNs, const Candidate& candidate,
    double degreesPerCountXOverride, double degreesPerCountYOverride)
{
    double yawRate = 0.0;
    double pitchRate = 0.0;
    for (const Command& command : timeline.commands)
    {
        if (command.sendTimeNs > queryTimeNs)
            break;
        const AxisResponse responses[]{ candidate.horizontal, candidate.vertical };
        const int counts[]{ command.countsX, command.countsY };
        const double configuredDegreesPerCount[]{
            degreesPerCountXOverride > 0.0
                ? degreesPerCountXOverride : command.degreesPerCountX,
            degreesPerCountYOverride > 0.0
                ? degreesPerCountYOverride : command.degreesPerCountY
        };
        double* rates[]{ &yawRate, &pitchRate };
        for (int axis = 0; axis < 2; ++axis)
        {
            const double centerMs = std::clamp(
                responses[axis].centerMs, 0.0, 250.0);
            const double widthMs = std::clamp(
                responses[axis].widthMs, 0.0, 100.0);
            if (widthMs <= 1e-9)
                continue;
            const double startNs = static_cast<double>(command.sendTimeNs) +
                std::max(0.0, centerMs - widthMs * 0.5) * 1'000'000.0;
            const double endNs = startNs + widthMs * 1'000'000.0;
            const double uncertaintyTailMs = std::isfinite(
                candidate.maneuverUncertaintyTailMs)
                ? std::clamp(candidate.maneuverUncertaintyTailMs, 0.0, 100.0)
                : 0.0;
            const double uncertaintyEndNs = endNs +
                uncertaintyTailMs * 1'000'000.0;
            const double queryNs = static_cast<double>(queryTimeNs);
            if (queryNs >= startNs && queryNs < uncertaintyEndNs)
            {
                *rates[axis] += static_cast<double>(counts[axis]) *
                    configuredDegreesPerCount[axis] / (widthMs / 1000.0);
            }
        }
    }
    return { yawRate, pitchRate };
}

std::string joinMissing(const std::vector<std::string>& missing)
{
    std::ostringstream stream;
    for (std::size_t index = 0; index < missing.size(); ++index)
    {
        if (index != 0)
            stream << ", ";
        stream << missing[index];
    }
    return stream.str();
}

double percentile(std::vector<double> values, double quantile)
{
    if (values.empty())
        return 0.0;
    std::sort(values.begin(), values.end());
    const double position = std::clamp(quantile, 0.0, 1.0) *
        static_cast<double>(values.size() - 1);
    const std::size_t lower = static_cast<std::size_t>(std::floor(position));
    const std::size_t upper = std::min(values.size() - 1, lower + 1);
    const double fraction = position - static_cast<double>(lower);
    return values[lower] + (values[upper] - values[lower]) * fraction;
}
}

bool LoadTimelineCsv(const std::filesystem::path& path,
    Timeline& timeline, std::string& error)
{
    timeline = {};
    timeline.source = path.filename().string();
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        error = "cannot open CSV: " + path.string();
        return false;
    }

    std::string line;
    if (!std::getline(input, line))
    {
        error = "CSV is empty: " + path.string();
        return false;
    }
    if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xef &&
        static_cast<unsigned char>(line[1]) == 0xbb &&
        static_cast<unsigned char>(line[2]) == 0xbf)
    {
        line.erase(0, 3);
    }
    const std::vector<std::string> header = parseCsvLine(line);
    std::unordered_map<std::string, std::size_t> columns;
    for (std::size_t index = 0; index < header.size(); ++index)
        columns.emplace(header[index], index);

    const std::vector<std::string> required{
        "BackendReceiveNs", "ControlTimeNs", "AimPipelineResetGeneration",
        "AimPipelineTargetId", "MeasuredLosYawDegrees",
        "MeasuredLosPitchDownDegrees", "AimPipelineConfidence",
        "AimPipelineOutputPaused", "Settled", "AimPipelineManeuverModelActive",
        "CommandToFrameDelayMs", "CommandResponseMs", "CommandSendSucceeded",
        "CommandSequence", "CommandDeviceSendNs", "CommandAppliedCountsX",
        "CommandAppliedCountsY", "DegreesPerCountX", "DegreesPerCountY"
    };
    std::vector<std::string> missing;
    for (const std::string& name : required)
    {
        if (columns.find(name) == columns.end())
            missing.push_back(name);
    }
    if (!missing.empty())
    {
        error = "CSV missing required columns: " + joinMissing(missing);
        return false;
    }

    std::unordered_set<std::int64_t> commandSequences;
    std::size_t rowNumber = 1;
    while (std::getline(input, line))
    {
        ++rowNumber;
        if (line.empty())
            continue;
        const std::vector<std::string> fields = parseCsvLine(line);
        if (fields.size() != header.size())
        {
            error = "CSV column count mismatch at row " + std::to_string(rowNumber);
            return false;
        }
        const auto value = [&](const char* name) -> const std::string& {
            return fields[columns.at(name)];
        };

        Sample sample;
        double confidence = 0.0;
        if (!parseInt64(value("BackendReceiveNs"), sample.observationTimeNs) ||
            !parseInt64(value("ControlTimeNs"), sample.controlTimeNs) ||
            !parseUInt64(value("AimPipelineResetGeneration"), sample.resetGeneration) ||
            !parseInt(value("AimPipelineTargetId"), sample.targetId) ||
            !parseDouble(value("MeasuredLosYawDegrees"), sample.measuredYawDegrees) ||
            !parseDouble(value("MeasuredLosPitchDownDegrees"), sample.measuredPitchDownDegrees) ||
            !parseDouble(value("AimPipelineConfidence"), confidence) ||
            !parseFlag(value("AimPipelineOutputPaused"), sample.outputPaused) ||
            !parseFlag(value("Settled"), sample.settled) ||
            !parseFlag(value("AimPipelineManeuverModelActive"),
                sample.recordedManeuverActive))
        {
            error = "CSV has invalid observation data at row " +
                std::to_string(rowNumber);
            return false;
        }
        sample.confidence = static_cast<float>(confidence);
        timeline.samples.push_back(sample);

        double centerMs = 0.0;
        double widthMs = 0.0;
        if (!parseDouble(value("CommandToFrameDelayMs"), centerMs) ||
            !parseDouble(value("CommandResponseMs"), widthMs))
        {
            error = "CSV has invalid response identity at row " +
                std::to_string(rowNumber);
            return false;
        }
        if (timeline.samples.size() == 1)
        {
            timeline.recordedCenterMs = centerMs;
            timeline.recordedWidthMs = widthMs;
        }
        else if (std::abs(timeline.recordedCenterMs - centerMs) > 1e-6 ||
                 std::abs(timeline.recordedWidthMs - widthMs) > 1e-6)
        {
            error = "CSV response identity changes at row " +
                std::to_string(rowNumber);
            return false;
        }

        bool sendSucceeded = false;
        if (!parseFlag(value("CommandSendSucceeded"), sendSucceeded))
        {
            error = "CSV has invalid command flag at row " +
                std::to_string(rowNumber);
            return false;
        }
        if (sendSucceeded)
        {
            Command command;
            if (!parseInt64(value("CommandSequence"), command.sequence) ||
                !parseInt64(value("CommandDeviceSendNs"), command.sendTimeNs) ||
                !parseInt(value("CommandAppliedCountsX"), command.countsX) ||
                !parseInt(value("CommandAppliedCountsY"), command.countsY) ||
                !parseDouble(value("DegreesPerCountX"), command.degreesPerCountX) ||
                !parseDouble(value("DegreesPerCountY"), command.degreesPerCountY))
            {
                error = "CSV has invalid command data at row " +
                    std::to_string(rowNumber);
                return false;
            }
            if (!commandSequences.insert(command.sequence).second)
            {
                error = "CSV repeats command sequence " +
                    std::to_string(command.sequence);
                return false;
            }
            timeline.commands.push_back(command);
        }
    }
    if (timeline.samples.empty())
    {
        error = "CSV contains no samples: " + path.string();
        return false;
    }
    std::sort(timeline.commands.begin(), timeline.commands.end(),
        [](const Command& left, const Command& right) {
            return left.sendTimeNs < right.sendTimeNs;
        });
    return true;
}

Metrics Evaluate(const Timeline& timeline, const Candidate& candidate,
    const ManeuverLosEstimator::Settings& settings,
    double degreesPerCountXOverride, double degreesPerCountYOverride,
    bool compareRecordedSelection, std::vector<TraceRow>* trace)
{
    Metrics metrics;
    metrics.source = timeline.source;
    metrics.candidate = candidate;
    metrics.rows = timeline.samples.size();

    ManeuverLosEstimator estimator;
    bool haveIdentity = false;
    std::uint64_t previousGeneration = 0;
    int previousTargetId = 0;
    bool previousRunningActive = false;
    bool wasRunning = false;
    std::size_t trial = 0;
    bool trialFailed = false;

    std::size_t row = 0;
    for (const Sample& sample : timeline.samples)
    {
        ++row;
        if (!haveIdentity || sample.resetGeneration != previousGeneration ||
            sample.targetId != previousTargetId)
        {
            estimator.reset();
            haveIdentity = true;
            previousGeneration = sample.resetGeneration;
            previousTargetId = sample.targetId;
            previousRunningActive = false;
        }
        const auto observationCamera = cameraAt(timeline,
            sample.observationTimeNs, candidate,
            degreesPerCountXOverride, degreesPerCountYOverride);
        const ManeuverLosEstimator::TimePoint observationTime{
            std::chrono::nanoseconds(sample.observationTimeNs) };
        const ManeuverLosEstimator::TimePoint controlTime{
            std::chrono::nanoseconds(sample.controlTimeNs) };
        const auto cameraRate = cameraRateAt(timeline,
            sample.observationTimeNs, candidate,
            degreesPerCountXOverride, degreesPerCountYOverride);
        ManeuverLosEstimator::Settings sampleSettings = settings;
        const double uncertaintyGain = std::isfinite(
            candidate.maneuverUncertaintyGain)
            ? std::clamp(candidate.maneuverUncertaintyGain, 0.0, 10.0) : 0.0;
        sampleSettings.maneuverRateUncertaintyXDegreesPerSecond =
            std::abs(cameraRate.first) * uncertaintyGain;
        sampleSettings.maneuverRateUncertaintyYDegreesPerSecond =
            std::abs(cameraRate.second) * uncertaintyGain;
        estimator.update(
            sample.measuredYawDegrees + observationCamera.first,
            sample.measuredPitchDownDegrees + observationCamera.second,
            sample.confidence, observationTime, controlTime, sampleSettings);
        const auto& diagnostics = estimator.diagnostics();
        const auto& estimate = estimator.selectedEstimate();
        const bool active = diagnostics.maneuverModelActive;
        if (trace)
        {
            trace->push_back({
                row, trial + (sample.outputPaused ? 0U : 1U),
                sample.observationTimeNs, sample.outputPaused, sample.settled, active,
                cameraRate.first, cameraRate.second,
                sampleSettings.maneuverRateUncertaintyXDegreesPerSecond,
                sampleSettings.maneuverRateUncertaintyYDegreesPerSecond,
                estimate.x.rateDegreesPerSecond, estimate.y.rateDegreesPerSecond,
                diagnostics.maneuverRateEvidenceDegreesPerSecond,
                diagnostics.maneuverHoldRemainingSeconds
            });
        }
        metrics.maxManeuverUncertainty = std::max(
            metrics.maxManeuverUncertainty,
            std::hypot(
                sampleSettings.maneuverRateUncertaintyXDegreesPerSecond,
                sampleSettings.maneuverRateUncertaintyYDegreesPerSecond));
        metrics.maxManeuverEvidence = std::max(metrics.maxManeuverEvidence,
            diagnostics.maneuverRateEvidenceDegreesPerSecond);
        metrics.recordedSelectionCompared = compareRecordedSelection;
        if (compareRecordedSelection && active != sample.recordedManeuverActive)
            ++metrics.recordedSelectionMismatches;
        if (diagnostics.selectionChanged)
            ++metrics.selectionChanges;

        if (sample.outputPaused)
        {
            ++metrics.pausedSamples;
            if (active)
                ++metrics.pausedActiveSamples;
            if (wasRunning && trialFailed)
                metrics.failedTrials.push_back(trial);
            wasRunning = false;
            previousRunningActive = false;
            trialFailed = false;
            continue;
        }

        if (!wasRunning)
        {
            ++trial;
            wasRunning = true;
            trialFailed = false;
        }
        ++metrics.runningSamples;
        if (active)
        {
            ++metrics.runningActiveSamples;
            trialFailed = true;
            if (!previousRunningActive)
                ++metrics.runningActiveSegments;
            if (sample.settled)
                ++metrics.settledActiveSamples;
            metrics.maxAbsRateX = std::max(metrics.maxAbsRateX,
                std::abs(estimate.x.rateDegreesPerSecond));
            metrics.maxAbsRateY = std::max(metrics.maxAbsRateY,
                std::abs(estimate.y.rateDegreesPerSecond));
        }
        previousRunningActive = active;
    }
    if (wasRunning && trialFailed)
        metrics.failedTrials.push_back(trial);
    return metrics;
}

static PhysicalFitMetrics fitPhysicalResponse(
    const std::vector<const Timeline*>& timelines,
    const Candidate& candidate,
    double degreesPerCountXOverride,
    double degreesPerCountYOverride,
    double quietWindowMs,
    double responseWindowMs)
{
    PhysicalFitMetrics metrics;
    metrics.source = timelines.size() == 1 ? timelines.front()->source : "all";
    metrics.candidate = candidate;
    const std::int64_t quietWindowNs = static_cast<std::int64_t>(
        std::clamp(quietWindowMs, 1.0, 1000.0) * 1'000'000.0);
    const std::int64_t responseWindowNs = static_cast<std::int64_t>(
        std::clamp(responseWindowMs, 1.0, 1000.0) * 1'000'000.0);

    std::vector<double> absoluteResidualsX;
    std::vector<double> absoluteResidualsY;
    double squaredResidualsX = 0.0;
    double squaredResidualsY = 0.0;

    struct SegmentSample
    {
        double worldYaw = 0.0;
        double worldPitch = 0.0;
        std::int64_t commandAgeNs = std::numeric_limits<std::int64_t>::max();
        bool outputPaused = false;
    };

    for (const Timeline* timelinePointer : timelines)
    {
        const Timeline& timeline = *timelinePointer;
        std::vector<SegmentSample> segment;
        std::uint64_t generation = 0;
        int targetId = 0;
        bool haveIdentity = false;
        bool segmentHasRunningSamples = false;
        bool previousOutputPaused = false;
        std::size_t commandIndex = 0;
        std::int64_t latestCommandTimeNs = std::numeric_limits<std::int64_t>::min();

        const auto finishSegment = [&]() {
            std::vector<double> anchorX;
            std::vector<double> anchorY;
            for (const SegmentSample& sample : segment)
            {
                if (!sample.outputPaused && sample.commandAgeNs > quietWindowNs)
                {
                    anchorX.push_back(sample.worldYaw);
                    anchorY.push_back(sample.worldPitch);
                }
            }
            if (anchorX.size() < 3 || anchorY.size() < 3)
            {
                segment.clear();
                return;
            }
            const double referenceX = percentile(anchorX, 0.5);
            const double referenceY = percentile(anchorY, 0.5);
            std::vector<double> segmentResidualsX;
            std::vector<double> segmentResidualsY;
            for (const SegmentSample& sample : segment)
            {
                if (sample.outputPaused || sample.commandAgeNs < 0 ||
                    sample.commandAgeNs > responseWindowNs)
                {
                    continue;
                }
                const double residualX = sample.worldYaw - referenceX;
                const double residualY = sample.worldPitch - referenceY;
                segmentResidualsX.push_back(residualX);
                segmentResidualsY.push_back(residualY);
            }
            if (segmentResidualsX.size() >= 3)
            {
                ++metrics.anchoredSegments;
                metrics.responseSamples += segmentResidualsX.size();
                for (double residual : segmentResidualsX)
                {
                    absoluteResidualsX.push_back(std::abs(residual));
                    squaredResidualsX += residual * residual;
                }
                for (double residual : segmentResidualsY)
                {
                    absoluteResidualsY.push_back(std::abs(residual));
                    squaredResidualsY += residual * residual;
                }
            }
            segment.clear();
        };

        for (const Sample& sample : timeline.samples)
        {
            const bool identityChanged = haveIdentity &&
                (sample.resetGeneration != generation || sample.targetId != targetId);
            if (identityChanged)
            {
                finishSegment();
                segmentHasRunningSamples = false;
            }
            // 九点测试可在同一目标身份内连续完成多轮。暂停后的下一次运行是新点位，
            // 前一段暂停尾部只为前一点提供安静锚点，不能与下一点的世界角混合。
            if (!identityChanged && !sample.outputPaused && previousOutputPaused &&
                segmentHasRunningSamples)
            {
                finishSegment();
                segmentHasRunningSamples = false;
            }
            if (!haveIdentity || sample.resetGeneration != generation ||
                sample.targetId != targetId)
            {
                generation = sample.resetGeneration;
                targetId = sample.targetId;
                haveIdentity = true;
            }
            while (commandIndex < timeline.commands.size() &&
                   timeline.commands[commandIndex].sendTimeNs <= sample.observationTimeNs)
            {
                latestCommandTimeNs = timeline.commands[commandIndex].sendTimeNs;
                ++commandIndex;
            }
            const auto camera = cameraAt(timeline, sample.observationTimeNs,
                candidate, degreesPerCountXOverride, degreesPerCountYOverride);
            const std::int64_t commandAgeNs =
                latestCommandTimeNs == std::numeric_limits<std::int64_t>::min()
                ? std::numeric_limits<std::int64_t>::max()
                : sample.observationTimeNs - latestCommandTimeNs;
            segment.push_back({
                sample.measuredYawDegrees + camera.first,
                sample.measuredPitchDownDegrees + camera.second,
                commandAgeNs,
                sample.outputPaused });
            if (!sample.outputPaused)
                segmentHasRunningSamples = true;
            previousOutputPaused = sample.outputPaused;
        }
        if (haveIdentity && segmentHasRunningSamples)
            finishSegment();
    }

    if (metrics.responseSamples == 0)
    {
        metrics.scoreDegrees = std::numeric_limits<double>::infinity();
        return metrics;
    }
    metrics.residualP50XDegrees = percentile(absoluteResidualsX, 0.5);
    metrics.residualP50YDegrees = percentile(absoluteResidualsY, 0.5);
    metrics.residualP95XDegrees = percentile(absoluteResidualsX, 0.95);
    metrics.residualP95YDegrees = percentile(absoluteResidualsY, 0.95);
    metrics.residualRmseXDegrees = std::sqrt(
        squaredResidualsX / static_cast<double>(metrics.responseSamples));
    metrics.residualRmseYDegrees = std::sqrt(
        squaredResidualsY / static_cast<double>(metrics.responseSamples));
    metrics.scoreDegrees = std::hypot(
        metrics.residualP95XDegrees, metrics.residualP95YDegrees);
    return metrics;
}

PhysicalFitMetrics FitPhysicalResponse(
    const Timeline& timeline,
    const Candidate& candidate,
    double degreesPerCountXOverride,
    double degreesPerCountYOverride,
    double quietWindowMs,
    double responseWindowMs)
{
    return fitPhysicalResponse({ &timeline }, candidate,
        degreesPerCountXOverride, degreesPerCountYOverride,
        quietWindowMs, responseWindowMs);
}

PhysicalFitMetrics FitPhysicalResponse(
    const std::vector<Timeline>& timelines,
    const Candidate& candidate,
    double degreesPerCountXOverride,
    double degreesPerCountYOverride,
    double quietWindowMs,
    double responseWindowMs)
{
    std::vector<const Timeline*> timelinePointers;
    timelinePointers.reserve(timelines.size());
    for (const Timeline& timeline : timelines)
        timelinePointers.push_back(&timeline);
    return fitPhysicalResponse(timelinePointers, candidate,
        degreesPerCountXOverride, degreesPerCountYOverride,
        quietWindowMs, responseWindowMs);
}
}
