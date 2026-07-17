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
    bool compareRecordedSelection)
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

    for (const Sample& sample : timeline.samples)
    {
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
        estimator.update(
            sample.measuredYawDegrees + observationCamera.first,
            sample.measuredPitchDownDegrees + observationCamera.second,
            sample.confidence, observationTime, controlTime, settings);
        const auto& diagnostics = estimator.diagnostics();
        const auto& estimate = estimator.selectedEstimate();
        const bool active = diagnostics.maneuverModelActive;
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
}
