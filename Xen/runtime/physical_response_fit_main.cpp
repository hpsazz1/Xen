#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
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
        const double value = std::stod(optionValue(
            argc, argv, name, std::to_string(fallback)));
        return std::isfinite(value) ? value : fallback;
    }
    catch (...)
    {
        return fallback;
    }
}

std::vector<std::string> stringList(const std::string& text)
{
    std::vector<std::string> values;
    std::stringstream stream(text);
    for (std::string token; std::getline(stream, token, ',');)
    {
        if (!token.empty())
            values.push_back(token);
    }
    return values;
}

std::vector<double> doubleList(int argc, char** argv,
    const std::string& name, const std::vector<double>& fallback)
{
    const std::string text = optionValue(argc, argv, name);
    if (text.empty())
        return fallback;
    std::vector<double> values;
    for (const std::string& token : stringList(text))
    {
        try
        {
            const double value = std::stod(token);
            if (std::isfinite(value) && value >= 0.0)
                values.push_back(value);
        }
        catch (...)
        {
        }
    }
    return values.empty() ? fallback : values;
}

void writeMetric(std::ostream& output,
    const ShadowResponseReplay::PhysicalFitMetrics& metric)
{
    output << metric.source << ','
        << metric.candidate.horizontal.centerMs << ','
        << metric.candidate.horizontal.widthMs << ','
        << metric.candidate.vertical.centerMs << ','
        << metric.candidate.vertical.widthMs << ','
        << metric.anchoredSegments << ',' << metric.responseSamples << ','
        << metric.residualP50XDegrees << ',' << metric.residualP50YDegrees << ','
        << metric.residualP95XDegrees << ',' << metric.residualP95YDegrees << ','
        << metric.residualRmseXDegrees << ',' << metric.residualRmseYDegrees << ','
        << metric.scoreDegrees << '\n';
}
}

int main(int argc, char** argv)
{
    const std::filesystem::path dataRoot = optionValue(argc, argv, "--data-root");
    if (dataRoot.empty() || !std::filesystem::is_directory(dataRoot))
    {
        std::cerr << "Usage: xen_physical_response_fit --data-root <directory> "
            "[--files static.csv,static_repeat2.csv] "
            "[--axis-mode shared|split] [--centers-ms 10,15,20] "
            "[--widths-ms 0,10,20] [--x-centers-ms ... --x-widths-ms ... "
            "--y-centers-ms ... --y-widths-ms ...] "
            "[--quiet-window-ms 150] [--response-window-ms 150] "
            "[--dpc-x 0.0308 --dpc-y 0.0308] [--output <csv>]" << std::endl;
        return 2;
    }

    const std::string axisMode = optionValue(argc, argv, "--axis-mode", "shared");
    if (axisMode != "shared" && axisMode != "split")
    {
        std::cerr << "[PhysicalFit] --axis-mode must be shared or split." << std::endl;
        return 2;
    }
    const auto centers = doubleList(argc, argv, "--centers-ms", { 20.0 });
    const auto widths = doubleList(argc, argv, "--widths-ms", { 20.0 });
    const auto xCenters = doubleList(argc, argv, "--x-centers-ms", centers);
    const auto xWidths = doubleList(argc, argv, "--x-widths-ms", widths);
    const auto yCenters = doubleList(argc, argv, "--y-centers-ms", centers);
    const auto yWidths = doubleList(argc, argv, "--y-widths-ms", widths);
    const double dpcX = optionDouble(argc, argv, "--dpc-x", 0.0);
    const double dpcY = optionDouble(argc, argv, "--dpc-y", 0.0);
    const double quietWindowMs = optionDouble(
        argc, argv, "--quiet-window-ms", 150.0);
    const double responseWindowMs = optionDouble(
        argc, argv, "--response-window-ms", 150.0);
    const std::filesystem::path outputPath = optionValue(argc, argv, "--output",
        (dataRoot / "physical_response_fit_summary.csv").string());

    const std::vector<std::string> requestedFiles = stringList(
        optionValue(argc, argv, "--files"));
    if (requestedFiles.empty())
    {
        std::cerr << "[PhysicalFit] --files must list CSV files from one capture identity."
            << std::endl;
        return 2;
    }
    const std::unordered_set<std::string> requested(
        requestedFiles.begin(), requestedFiles.end());
    std::unordered_set<std::string> found;
    std::vector<ShadowResponseReplay::Timeline> timelines;
    for (const auto& entry : std::filesystem::directory_iterator(dataRoot))
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".csv" ||
            entry.path().filename() == outputPath.filename())
        {
            continue;
        }
        const std::string filename = entry.path().filename().string();
        if (!requested.empty() && requested.find(filename) == requested.end())
            continue;
        ShadowResponseReplay::Timeline timeline;
        std::string error;
        if (!ShadowResponseReplay::LoadTimelineCsv(entry.path(), timeline, error))
        {
            std::cerr << "[PhysicalFit] " << error << std::endl;
            return 3;
        }
        found.insert(filename);
        timelines.push_back(std::move(timeline));
    }
    for (const std::string& filename : requestedFiles)
    {
        if (found.find(filename) == found.end())
        {
            std::cerr << "[PhysicalFit] Requested CSV not found: "
                << filename << std::endl;
            return 3;
        }
    }
    std::sort(timelines.begin(), timelines.end(),
        [](const auto& left, const auto& right) { return left.source < right.source; });
    if (timelines.empty())
    {
        std::cerr << "[PhysicalFit] No matching pipeline CSV files found." << std::endl;
        return 3;
    }

    std::vector<ShadowResponseReplay::Candidate> candidates;
    if (axisMode == "split")
    {
        for (double xCenter : xCenters)
        for (double xWidth : xWidths)
        for (double yCenter : yCenters)
        for (double yWidth : yWidths)
            candidates.push_back({
                { xCenter, xWidth }, { yCenter, yWidth }, 0.0, 0.0 });
    }
    else
    {
        for (double center : centers)
        for (double width : widths)
            candidates.push_back({
                { center, width }, { center, width }, 0.0, 0.0 });
    }

    std::ofstream output(outputPath);
    if (!output)
    {
        std::cerr << "[PhysicalFit] Cannot write " << outputPath << std::endl;
        return 4;
    }
    output << std::fixed << std::setprecision(6);
    output << "Source,XCenterMs,XWidthMs,YCenterMs,YWidthMs,AnchoredSegments,"
        "ResponseSamples,ResidualP50XDeg,ResidualP50YDeg,ResidualP95XDeg,"
        "ResidualP95YDeg,ResidualRmseXDeg,ResidualRmseYDeg,ScoreDeg\n";

    std::vector<ShadowResponseReplay::PhysicalFitMetrics> overall;
    overall.reserve(candidates.size());
    for (const auto& candidate : candidates)
    {
        for (const auto& timeline : timelines)
        {
            writeMetric(output, ShadowResponseReplay::FitPhysicalResponse(
                timeline, candidate, dpcX, dpcY,
                quietWindowMs, responseWindowMs));
        }
        auto aggregate = ShadowResponseReplay::FitPhysicalResponse(
            timelines, candidate, dpcX, dpcY,
            quietWindowMs, responseWindowMs);
        writeMetric(output, aggregate);
        overall.push_back(std::move(aggregate));
    }
    std::sort(overall.begin(), overall.end(), [](const auto& left, const auto& right) {
        if (left.scoreDegrees != right.scoreDegrees)
            return left.scoreDegrees < right.scoreDegrees;
        return std::hypot(left.residualRmseXDegrees, left.residualRmseYDegrees) <
            std::hypot(right.residualRmseXDegrees, right.residualRmseYDegrees);
    });

    const std::size_t top = std::min<std::size_t>(20, overall.size());
    std::cout << "XCenter XWidth YCenter YWidth Segments Samples P95X P95Y Score"
        << std::endl;
    for (std::size_t index = 0; index < top; ++index)
    {
        const auto& metric = overall[index];
        std::cout << metric.candidate.horizontal.centerMs << ' '
            << metric.candidate.horizontal.widthMs << ' '
            << metric.candidate.vertical.centerMs << ' '
            << metric.candidate.vertical.widthMs << ' '
            << metric.anchoredSegments << ' ' << metric.responseSamples << ' '
            << metric.residualP95XDegrees << ' '
            << metric.residualP95YDegrees << ' '
            << metric.scoreDegrees << std::endl;
    }
    std::cout << "[PhysicalFit] Wrote " << outputPath << std::endl;
    return overall.empty() || overall.front().responseSamples == 0 ? 5 : 0;
}
