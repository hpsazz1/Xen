#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "shadow_response_replay.h"

namespace
{
std::string optionValue(int argc, char** argv,
    const std::string& name, const std::string& fallback = {})
{
    for (int index = 1; index + 1 < argc; ++index)
    {
        if (argv[index] && name == argv[index])
            return argv[index + 1];
    }
    return fallback;
}

double optionDouble(int argc, char** argv,
    const std::string& name, double fallback)
{
    try
    {
        return std::stod(optionValue(argc, argv, name, std::to_string(fallback)));
    }
    catch (...)
    {
        return fallback;
    }
}

std::vector<double> optionList(int argc, char** argv,
    const std::string& name, const std::vector<double>& fallback)
{
    const std::string text = optionValue(argc, argv, name);
    if (text.empty())
        return fallback;
    std::vector<double> values;
    std::stringstream stream(text);
    std::string token;
    while (std::getline(stream, token, ','))
    {
        try
        {
            const double value = std::stod(token);
            if (std::isfinite(value))
                values.push_back(value);
        }
        catch (...)
        {
        }
    }
    return values.empty() ? fallback : values;
}

std::string failedTrials(const std::vector<std::size_t>& trials)
{
    std::ostringstream stream;
    for (std::size_t index = 0; index < trials.size(); ++index)
    {
        if (index != 0)
            stream << ';';
        stream << trials[index];
    }
    return stream.str();
}

void writeMetric(std::ostream& output,
    const ShadowResponseReplay::Metrics& metric)
{
    output << metric.source << ','
        << metric.candidate.horizontal.centerMs << ','
        << metric.candidate.horizontal.widthMs << ','
        << metric.candidate.vertical.centerMs << ','
        << metric.candidate.vertical.widthMs << ','
        << metric.candidate.maneuverUncertaintyGain << ','
        << metric.candidate.maneuverUncertaintyTailMs << ','
        << metric.rows << ',' << metric.runningSamples << ','
        << metric.pausedSamples << ',' << metric.runningActiveSamples << ','
        << metric.pausedActiveSamples << ',' << metric.runningActiveSegments << ','
        << metric.settledActiveSamples << ',' << metric.selectionChanges << ','
        << (metric.recordedSelectionCompared ? 1 : 0) << ','
        << metric.recordedSelectionMismatches << ','
        << metric.maxAbsRateX << ',' << metric.maxAbsRateY << ','
        << metric.maxManeuverUncertainty << ','
        << metric.maxManeuverEvidence << ','
        << failedTrials(metric.failedTrials) << '\n';
}

void writeTrace(std::ostream& output, const std::string& source,
    const ShadowResponseReplay::Candidate& candidate,
    const std::vector<ShadowResponseReplay::TraceRow>& rows)
{
    for (const auto& row : rows)
    {
        output << source << ','
            << candidate.horizontal.centerMs << ','
            << candidate.horizontal.widthMs << ','
            << candidate.vertical.centerMs << ','
            << candidate.vertical.widthMs << ','
            << candidate.maneuverUncertaintyGain << ','
            << candidate.maneuverUncertaintyTailMs << ','
            << row.row << ',' << row.trial << ',' << row.observationTimeNs << ','
            << (row.outputPaused ? 1 : 0) << ',' << (row.settled ? 1 : 0) << ','
            << (row.active ? 1 : 0) << ',' << row.cameraRateX << ','
            << row.cameraRateY << ',' << row.uncertaintyX << ','
            << row.uncertaintyY << ',' << row.estimatedRateX << ','
            << row.estimatedRateY << ',' << row.maneuverEvidence << ','
            << row.holdRemainingSeconds << '\n';
    }
}
}

int main(int argc, char** argv)
{
    const std::filesystem::path dataRoot = optionValue(argc, argv, "--data-root");
    if (dataRoot.empty() || !std::filesystem::is_directory(dataRoot))
    {
        std::cerr << "Usage: xen_shadow_response_replay --data-root <directory> "
            "[--output <csv>] [--axis-mode shared|split] "
            "[--centers-ms 10,15,20] [--widths-ms 0,10,20] "
            "[--x-centers-ms ... --x-widths-ms ... "
            "--y-centers-ms ... --y-widths-ms ...] "
            "[--uncertainty-gains 0,0.25,0.5] "
            "[--uncertainty-tail-ms 0,10,20] "
            "[--trace-output <csv>] "
            "[--dpc-x 0.0308 --dpc-y 0.0308]" << std::endl;
        return 2;
    }
    const std::string axisMode = optionValue(argc, argv, "--axis-mode", "shared");
    const std::vector<double> centers = optionList(
        argc, argv, "--centers-ms", { 20.0 });
    const std::vector<double> widths = optionList(
        argc, argv, "--widths-ms", { 20.0 });
    const std::vector<double> xCenters = optionList(
        argc, argv, "--x-centers-ms", centers);
    const std::vector<double> xWidths = optionList(
        argc, argv, "--x-widths-ms", widths);
    const std::vector<double> yCenters = optionList(
        argc, argv, "--y-centers-ms", centers);
    const std::vector<double> yWidths = optionList(
        argc, argv, "--y-widths-ms", widths);
    const std::vector<double> uncertaintyGains = optionList(
        argc, argv, "--uncertainty-gains", { 0.0 });
    const std::vector<double> uncertaintyTailsMs = optionList(
        argc, argv, "--uncertainty-tail-ms", { 0.0 });
    const double dpcX = optionDouble(argc, argv, "--dpc-x", 0.0);
    const double dpcY = optionDouble(argc, argv, "--dpc-y", 0.0);
    const std::filesystem::path outputPath = optionValue(
        argc, argv, "--output",
        (dataRoot / "shadow_response_replay_summary.csv").string());
    const std::filesystem::path traceOutputPath = optionValue(
        argc, argv, "--trace-output");

    std::vector<ShadowResponseReplay::Timeline> timelines;
    for (const auto& entry : std::filesystem::directory_iterator(dataRoot))
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".csv" ||
            entry.path().filename() == outputPath.filename() ||
            entry.path().filename() == "shadow_pipeline_summary.csv")
        {
            continue;
        }
        ShadowResponseReplay::Timeline timeline;
        std::string error;
        if (!ShadowResponseReplay::LoadTimelineCsv(entry.path(), timeline, error))
        {
            std::cerr << "[ShadowReplay] " << error << std::endl;
            return 3;
        }
        timelines.push_back(std::move(timeline));
    }
    std::sort(timelines.begin(), timelines.end(),
        [](const auto& left, const auto& right) { return left.source < right.source; });
    if (timelines.empty())
    {
        std::cerr << "[ShadowReplay] No pipeline CSV files found." << std::endl;
        return 3;
    }

    std::vector<ShadowResponseReplay::AxisResponse> sharedResponses;
    for (double center : centers)
    for (double width : widths)
        sharedResponses.push_back({ center, width });
    std::vector<ShadowResponseReplay::Candidate> candidates;
    if (axisMode == "split")
    {
        for (double xCenter : xCenters)
        for (double xWidth : xWidths)
        for (double yCenter : yCenters)
        for (double yWidth : yWidths)
        for (double uncertaintyGain : uncertaintyGains)
        for (double uncertaintyTailMs : uncertaintyTailsMs)
            candidates.push_back({
                { xCenter, xWidth }, { yCenter, yWidth }, uncertaintyGain,
                uncertaintyTailMs });
    }
    else
    {
        for (const auto& response : sharedResponses)
        for (double uncertaintyGain : uncertaintyGains)
        for (double uncertaintyTailMs : uncertaintyTailsMs)
            candidates.push_back({ response, response, uncertaintyGain,
                uncertaintyTailMs });
    }

    ManeuverLosEstimator::Settings settings;
    settings.mode = ManeuverLosEstimatorMode::ManeuverGatedConstantAcceleration;
    settings.jerkStdDegreesPerSecond3 = optionDouble(
        argc, argv, "--jerk-std-dps3", 8000.0);
    settings.maneuverRateThresholdDegreesPerSecond = optionDouble(
        argc, argv, "--maneuver-rate-threshold-dps", 12.0);
    settings.maneuverHoldSeconds = optionDouble(
        argc, argv, "--maneuver-hold-ms", 120.0) / 1000.0;

    std::ofstream output(outputPath);
    if (!output)
    {
        std::cerr << "[ShadowReplay] Cannot write " << outputPath << std::endl;
        return 4;
    }
    output << std::fixed << std::setprecision(3);
    output << "Source,XCenterMs,XWidthMs,YCenterMs,YWidthMs,UncertaintyGain,UncertaintyTailMs,"
        "Rows,RunningSamples,"
        "PausedSamples,RunningActiveSamples,PausedActiveSamples,RunningActiveSegments,"
        "SettledActiveSamples,SelectionChanges,RecordedSelectionCompared,"
        "RecordedSelectionMismatches,"
        "MaxAbsRateX,MaxAbsRateY,MaxManeuverUncertainty,"
        "MaxManeuverEvidence,FailedTrials\n";

    std::ofstream traceOutput;
    if (!traceOutputPath.empty())
    {
        traceOutput.open(traceOutputPath);
        if (!traceOutput)
        {
            std::cerr << "[ShadowReplay] Cannot write " << traceOutputPath << std::endl;
            return 4;
        }
        traceOutput << std::fixed << std::setprecision(6);
        traceOutput << "Source,XCenterMs,XWidthMs,YCenterMs,YWidthMs,UncertaintyGain,UncertaintyTailMs,"
            "Row,Trial,ObservationTimeNs,OutputPaused,Settled,Active,CameraRateX,"
            "CameraRateY,UncertaintyX,UncertaintyY,EstimatedRateX,EstimatedRateY,"
            "ManeuverEvidence,HoldRemainingSeconds\n";
    }

    std::vector<ShadowResponseReplay::Metrics> overall;
    overall.reserve(candidates.size());
    for (const auto& candidate : candidates)
    {
        ShadowResponseReplay::Metrics aggregate;
        aggregate.source = "all";
        aggregate.candidate = candidate;
        aggregate.recordedSelectionCompared = true;
        for (const auto& timeline : timelines)
        {
            const bool compareRecorded =
                std::abs(candidate.horizontal.centerMs - timeline.recordedCenterMs) <= 1e-6 &&
                std::abs(candidate.horizontal.widthMs - timeline.recordedWidthMs) <= 1e-6 &&
                std::abs(candidate.vertical.centerMs - timeline.recordedCenterMs) <= 1e-6 &&
                std::abs(candidate.vertical.widthMs - timeline.recordedWidthMs) <= 1e-6 &&
                std::abs(candidate.maneuverUncertaintyGain) <= 1e-9;
            std::vector<ShadowResponseReplay::TraceRow> trace;
            const auto metric = ShadowResponseReplay::Evaluate(
                timeline, candidate, settings, dpcX, dpcY, compareRecorded,
                traceOutput ? &trace : nullptr);
            writeMetric(output, metric);
            if (traceOutput)
                writeTrace(traceOutput, timeline.source, candidate, trace);
            aggregate.rows += metric.rows;
            aggregate.runningSamples += metric.runningSamples;
            aggregate.pausedSamples += metric.pausedSamples;
            aggregate.runningActiveSamples += metric.runningActiveSamples;
            aggregate.pausedActiveSamples += metric.pausedActiveSamples;
            aggregate.runningActiveSegments += metric.runningActiveSegments;
            aggregate.settledActiveSamples += metric.settledActiveSamples;
            aggregate.selectionChanges += metric.selectionChanges;
            aggregate.recordedSelectionCompared =
                aggregate.recordedSelectionCompared && metric.recordedSelectionCompared;
            aggregate.recordedSelectionMismatches += metric.recordedSelectionMismatches;
            aggregate.maxAbsRateX = std::max(aggregate.maxAbsRateX, metric.maxAbsRateX);
            aggregate.maxAbsRateY = std::max(aggregate.maxAbsRateY, metric.maxAbsRateY);
            aggregate.maxManeuverUncertainty = std::max(
                aggregate.maxManeuverUncertainty, metric.maxManeuverUncertainty);
            aggregate.maxManeuverEvidence = std::max(
                aggregate.maxManeuverEvidence, metric.maxManeuverEvidence);
        }
        writeMetric(output, aggregate);
        overall.push_back(aggregate);
    }
    std::sort(overall.begin(), overall.end(), [](const auto& left, const auto& right) {
        if (left.runningActiveSamples != right.runningActiveSamples)
            return left.runningActiveSamples < right.runningActiveSamples;
        if (left.settledActiveSamples != right.settledActiveSamples)
            return left.settledActiveSamples < right.settledActiveSamples;
        return left.runningActiveSegments < right.runningActiveSegments;
    });

    const std::size_t top = std::min<std::size_t>(20, overall.size());
    std::cout << "XCenter XWidth YCenter YWidth Gain Tail Active Settled Segments Mismatch" << std::endl;
    for (std::size_t index = 0; index < top; ++index)
    {
        const auto& metric = overall[index];
        std::cout << metric.candidate.horizontal.centerMs << ' '
            << metric.candidate.horizontal.widthMs << ' '
            << metric.candidate.vertical.centerMs << ' '
            << metric.candidate.vertical.widthMs << ' '
            << metric.candidate.maneuverUncertaintyGain << ' '
            << metric.candidate.maneuverUncertaintyTailMs << ' '
            << metric.runningActiveSamples << ' '
            << metric.settledActiveSamples << ' '
            << metric.runningActiveSegments << ' '
            << metric.recordedSelectionMismatches << std::endl;
    }
    std::cout << "[ShadowReplay] Wrote " << outputPath << std::endl;
    return 0;
}
