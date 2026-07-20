#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include "recovery_speed_device_protocol.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>

namespace
{
bool parseInteger(const std::string& text, int64_t& value)
{
    try
    {
        size_t used = 0;
        value = std::stoll(text, &used);
        return used == text.size();
    }
    catch (...) { return false; }
}

bool readKeyValues(const std::filesystem::path& path,
                   std::map<std::string, std::string>& values,
                   std::string& error)
{
    std::ifstream input(path);
    if (!input)
    {
        error = "manifest_open_failed";
        return false;
    }
    std::string line;
    size_t lineNumber = 0;
    while (std::getline(input, line))
    {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (lineNumber == 1 && line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF)
            line.erase(0, 3);
        const auto separator = line.find('=');
        if (separator == std::string::npos || separator == 0 ||
            !values.emplace(line.substr(0, separator), line.substr(separator + 1)).second)
        {
            error = "manifest_invalid_line:" + std::to_string(lineNumber);
            return false;
        }
    }
    return true;
}

bool parseCsvRow(const std::string& line, std::vector<std::string>& fields)
{
    fields.clear();
    size_t index = 0;
    while (index <= line.size())
    {
        std::string field;
        if (index < line.size() && line[index] == '"')
        {
            ++index;
            bool closed = false;
            while (index < line.size())
            {
                if (line[index] != '"') { field.push_back(line[index++]); continue; }
                if (index + 1 < line.size() && line[index + 1] == '"')
                {
                    field.push_back('"');
                    index += 2;
                    continue;
                }
                ++index;
                closed = true;
                break;
            }
            if (!closed) return false;
        }
        else
        {
            while (index < line.size() && line[index] != ',') field.push_back(line[index++]);
        }
        fields.push_back(std::move(field));
        if (index == line.size()) break;
        if (line[index] != ',') return false;
        ++index;
    }
    return true;
}

bool sha256File(const std::filesystem::path& path, std::string& digest, std::string& error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) { error = "sha256_open_failed"; return false; }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0;
    DWORD hashLength = 0;
    DWORD size = 0;
    auto cleanup = [&]() {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    };
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &size, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength), &size, 0) < 0)
    {
        error = "sha256_initialization_failed";
        cleanup();
        return false;
    }
    std::vector<unsigned char> object(objectLength);
    std::vector<unsigned char> bytes(hashLength);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0) < 0)
    {
        error = "sha256_create_failed";
        cleanup();
        return false;
    }
    std::array<char, 65536> buffer{};
    while (input)
    {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0 && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()),
                                        static_cast<ULONG>(count), 0) < 0)
        {
            error = "sha256_update_failed";
            cleanup();
            return false;
        }
    }
    if (!input.eof() || BCryptFinishHash(hash, bytes.data(), hashLength, 0) < 0)
    {
        error = "sha256_finish_failed";
        cleanup();
        return false;
    }
    cleanup();
    std::ostringstream stream;
    stream << std::uppercase << std::hex << std::setfill('0');
    for (const auto value : bytes) stream << std::setw(2) << static_cast<int>(value);
    digest = stream.str();
    return true;
}

double median(std::vector<double> values)
{
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const size_t middle = values.size() / 2;
    return values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) * 0.5;
}
}

bool LoadRecoverySpeedDevicePlan(
    const std::filesystem::path& planDirectory,
    const std::string& expectedBackend,
    const std::string& expectedRevision,
    int expectedControllerRevision,
    RecoverySpeedDevicePlan& plan,
    std::string& error)
{
    const auto manifestPath = planDirectory / "recovery_speed_device_manifest.txt";
    const auto csvPath = planDirectory / "recovery_speed_device_plan.csv";
    std::map<std::string, std::string> values;
    if (!readKeyValues(manifestPath, values, error)) return false;
    const std::vector<std::string> required{
        "ProtocolVersion", "PlanId", "BuildBackend", "BuildRevision", "ControllerRevision",
        "DeviceId", "ConfigPath", "ConfigSha256", "NdiSource", "Roi", "FrameRateHz",
        "RatesCountsPerSecond", "PulseFrames", "MaximumExcursionCounts",
        "MaximumAbsoluteCountsPerTrial", "BaselineMs", "StopObservationMs", "TailObservationMs",
        "InterTrialMs", "PlanSha256", "ExecutionEnabled", "PhysicalExecutionAuthorized",
        "ZeroTargetRiskConfirmed", "AutomaticConfigurationWrite", "AutomaticActiveEnable" };
    for (const auto& name : required)
    {
        if (values.find(name) == values.end()) { error = "manifest_missing:" + name; return false; }
    }
    int64_t controller = 0;
    if (!parseInteger(values["ControllerRevision"], controller) ||
        values["ProtocolVersion"] != "xen-recovery-speed-device-v1" ||
        values["BuildBackend"] != expectedBackend || expectedRevision.empty() ||
        values["BuildRevision"].rfind(expectedRevision, 0) != 0 ||
        controller != expectedControllerRevision || values["FrameRateHz"] != "240" ||
        values["RatesCountsPerSecond"] != "1440,1800" || values["PulseFrames"] != "8" ||
        values["MaximumExcursionCounts"] != "60" || values["MaximumAbsoluteCountsPerTrial"] != "120" ||
        values["BaselineMs"] != "500" || values["StopObservationMs"] != "1000" ||
        values["TailObservationMs"] != "1000" || values["InterTrialMs"] != "1000")
    {
        error = "manifest_identity_or_envelope_mismatch";
        return false;
    }
    for (const auto* name : { "ExecutionEnabled", "PhysicalExecutionAuthorized", "ZeroTargetRiskConfirmed",
                              "AutomaticConfigurationWrite", "AutomaticActiveEnable" })
    {
        if (values[name] != "0") { error = std::string("manifest_unsafe_flag:") + name; return false; }
    }
    std::string configDigest;
    std::string planDigest;
    if (!sha256File(values["ConfigPath"], configDigest, error) || configDigest != values["ConfigSha256"])
    {
        error = "config_hash_mismatch";
        return false;
    }
    if (!sha256File(csvPath, planDigest, error) || planDigest != values["PlanSha256"])
    {
        error = "plan_hash_mismatch";
        return false;
    }
    std::vector<std::string> roiFields;
    std::stringstream roiStream(values["Roi"]);
    std::string roiField;
    while (std::getline(roiStream, roiField, ',')) roiFields.push_back(roiField);
    if (roiFields.size() != 4) { error = "roi_invalid"; return false; }
    std::array<int64_t, 4> roi{};
    for (size_t i = 0; i < roi.size(); ++i)
        if (!parseInteger(roiFields[i], roi[i])) { error = "roi_invalid"; return false; }

    std::ifstream input(csvPath);
    std::string line;
    std::vector<std::string> fields;
    if (!std::getline(input, line) || !parseCsvRow(line, fields) || fields.size() != 16)
    {
        error = "plan_header_invalid";
        return false;
    }
    std::vector<RecoverySpeedDeviceCommand> commands;
    while (std::getline(input, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!parseCsvRow(line, fields) || fields.size() != 16)
        {
            error = "plan_row_invalid";
            return false;
        }
        std::array<int64_t, 13> numeric{};
        const std::array<size_t, 13> columns{ 2,3,4,6,7,8,9,10,11,12,13,14,15 };
        for (size_t i = 0; i < columns.size(); ++i)
            if (!parseInteger(fields[columns[i]], numeric[i])) { error = "plan_number_invalid"; return false; }
        RecoverySpeedDeviceCommand command;
        command.trial = static_cast<int>(numeric[0]);
        command.rateCountsPerSecond = static_cast<int>(numeric[1]);
        command.leadingDirection = static_cast<int>(numeric[2]);
        command.segment = fields[5];
        command.command = static_cast<int>(numeric[3]);
        command.frameIndex = static_cast<int>(numeric[4]);
        command.trialStartOffsetUs = numeric[5];
        command.relativeOffsetUs = numeric[6];
        command.scheduledOffsetUs = numeric[7];
        command.deltaX = static_cast<int>(numeric[8]);
        command.deltaY = static_cast<int>(numeric[9]);
        command.cumulativeX = static_cast<int>(numeric[10]);
        command.trialAbsoluteCounts = static_cast<int>(numeric[11]);
        command.maximumExcursionCounts = static_cast<int>(numeric[12]);
        if (fields[0] != values["ProtocolVersion"] || fields[1] != values["PlanId"])
        {
            error = "plan_identity_mismatch";
            return false;
        }
        commands.push_back(std::move(command));
    }
    if (commands.size() != 64) { error = "plan_row_count"; return false; }

    // 不信任CSV的累计列，按固定四trial序列逐行重构并比对。
    size_t row = 0;
    int64_t trialStartUs = 0;
    for (const int rate : { 1440, 1800 })
    {
        for (const int direction : { 1, -1 })
        {
            const int trial = static_cast<int>(row / 16) + 1;
            std::vector<int> pulse = rate == 1440
                ? std::vector<int>{6,6,6,6,6,6,6,6}
                : std::vector<int>{7,8,7,8,7,8,7,8};
            const int excursion = rate == 1440 ? 48 : 60;
            int cumulative = 0;
            int absolute = 0;
            for (int index = 0; index < 16; ++index, ++row)
            {
                const bool returning = index >= 8;
                const int frame = index % 8;
                const int pulseIndex = returning ? 7 - frame : frame;
                const int delta = (returning ? -direction : direction) * pulse[pulseIndex];
                cumulative += delta;
                absolute += std::abs(delta);
                const int64_t relativeUs = (returning ? 1533333LL : 500000LL) +
                    static_cast<int64_t>(std::llround(frame * 1000000.0 / 240.0));
                const auto& actual = commands[row];
                if (actual.trial != trial || actual.rateCountsPerSecond != rate ||
                    actual.leadingDirection != direction || actual.segment != (returning ? "return" : "forward") ||
                    actual.command != index + 1 || actual.frameIndex != frame ||
                    actual.trialStartOffsetUs != trialStartUs || actual.relativeOffsetUs != relativeUs ||
                    actual.scheduledOffsetUs != trialStartUs + relativeUs || actual.deltaX != delta ||
                    actual.deltaY != 0 || actual.cumulativeX != cumulative ||
                    actual.trialAbsoluteCounts != absolute || actual.maximumExcursionCounts != excursion)
                {
                    error = "plan_schedule_mismatch:" + std::to_string(trial) + "/" + std::to_string(index + 1);
                    return false;
                }
            }
            if (cumulative != 0 || absolute > 120) { error = "plan_envelope_mismatch"; return false; }
            trialStartUs += 3566667LL;
        }
    }
    plan.protocolVersion = values["ProtocolVersion"];
    plan.planId = values["PlanId"];
    plan.buildBackend = values["BuildBackend"];
    plan.buildRevision = values["BuildRevision"];
    plan.controllerRevision = static_cast<int>(controller);
    plan.deviceId = values["DeviceId"];
    plan.configPath = values["ConfigPath"];
    plan.configSha256 = values["ConfigSha256"];
    plan.ndiSource = values["NdiSource"];
    plan.roiX = static_cast<int>(roi[0]);
    plan.roiY = static_cast<int>(roi[1]);
    plan.roiWidth = static_cast<int>(roi[2]);
    plan.roiHeight = static_cast<int>(roi[3]);
    plan.commands = std::move(commands);
    return true;
}

RecoverySpeedTrialResult AnalyzeRecoverySpeedTrial(
    int trial, int rateCountsPerSecond, int leadingDirection, int expectedExcursionCounts,
    int64_t trialStartNs, const std::vector<PhysicalResponseSample>& samples,
    const std::vector<RecoverySpeedCommandRecord>& commands)
{
    RecoverySpeedTrialResult result;
    result.trial = trial;
    result.rateCountsPerSecond = rateCountsPerSecond;
    result.leadingDirection = leadingDirection;
    result.expectedExcursionCounts = expectedExcursionCounts;
    result.samples = samples.size();
    result.commands = commands.size();
    result.minimumTrackingQuality = 1.0;
    std::vector<double> baselineX;
    std::vector<double> baselineY;
    for (const auto& sample : samples)
    {
        if (!sample.valid) { result.reason = "tracking_invalid"; return result; }
        result.minimumTrackingQuality = std::min(result.minimumTrackingQuality, sample.trackingQuality);
        const double relativeMs = static_cast<double>(sample.receiveNs - trialStartNs) / 1e6;
        if (relativeMs >= 0.0 && relativeMs < 450.0)
        {
            baselineX.push_back(sample.displacementX);
            baselineY.push_back(sample.displacementY);
        }
    }
    if (baselineX.size() < 20 || commands.size() != 16)
    {
        result.reason = "baseline_or_command_count";
        return result;
    }
    for (const auto& command : commands)
    {
        if (!command.succeeded) { result.reason = "device_command_failed"; return result; }
        result.maximumCommandJitterMs = std::max(result.maximumCommandJitterMs,
            std::abs(static_cast<double>(command.attemptNs - command.scheduledNs)) / 1e6);
    }
    const double originX = median(std::move(baselineX));
    const double originY = median(std::move(baselineY));
    const double responseSign = static_cast<double>(-leadingDirection);
    const int64_t firstCommandConfirmedNs = commands.front().confirmedNs;
    const int64_t lastForwardCommandConfirmedNs = commands[7].confirmedNs;
    int64_t responseOnsetNs = 0;
    for (const auto& sample : samples)
    {
        if (sample.receiveNs < firstCommandConfirmedNs)
            continue;
        const double primary = (sample.displacementX - originX) * responseSign;
        if (primary >= 1.0)
        {
            responseOnsetNs = sample.receiveNs;
            break;
        }
    }
    if (responseOnsetNs == 0)
    {
        result.reason = "response_onset_missing";
        return result;
    }
    result.visualResponseLatencyMs =
        static_cast<double>(responseOnsetNs - firstCommandConfirmedNs) / 1e6;
    if (result.visualResponseLatencyMs < 0.0 || result.visualResponseLatencyMs > 100.0)
    {
        result.reason = "visual_response_latency";
        return result;
    }
    // 命令确认时间与NDI画面属于不同时间基准。用同一trial的首次可见响应延迟对齐
    // 最后一条正向命令，避免把尚未显示完的命令响应误计为停止后的额外位移。
    const int64_t stopAnchorNs = lastForwardCommandConfirmedNs +
        (responseOnsetNs - firstCommandConfirmedNs);
    std::vector<double> forwardTail;
    std::vector<double> finalX;
    bool haveStopAnchor = false;
    double peak = 0.0;
    double crossPeak = 0.0;
    for (const auto& sample : samples)
    {
        const double relativeMs = static_cast<double>(sample.receiveNs - trialStartNs) / 1e6;
        const double primary = (sample.displacementX - originX) * responseSign;
        const double cross = std::abs(sample.displacementY - originY);
        if (!haveStopAnchor && sample.receiveNs >= stopAnchorNs)
        {
            result.stopAnchorDisplacementPx = primary;
            haveStopAnchor = true;
        }
        if (sample.receiveNs >= responseOnsetNs && relativeMs < 1533.333)
        {
            peak = std::max(peak, primary);
            crossPeak = std::max(crossPeak, cross);
            if (relativeMs >= 1300.0) forwardTail.push_back(primary);
        }
        if (relativeMs >= 2350.0) finalX.push_back(sample.displacementX - originX);
    }
    if (!haveStopAnchor || forwardTail.size() < 20 || finalX.size() < 20)
    {
        result.reason = "observation_tail_incomplete";
        return result;
    }
    result.forwardDisplacementPx = median(std::move(forwardTail));
    result.peakDisplacementPx = peak;
    // 鼠标增量停止后，命令到画面的固定延迟仍会继续显示已提交位移；该部分不是物理过冲。
    // 以长稳态尾段作为期望终点，峰值超过稳态终点的部分才计入停止距离。
    result.stopDistancePx = std::max(0.0, peak - result.forwardDisplacementPx);
    result.finalResidualPx = std::abs(median(std::move(finalX)));
    result.pixelsPerCount = result.forwardDisplacementPx / expectedExcursionCounts;
    result.crossAxisLeakagePercent = peak > 1e-9 ? 100.0 * crossPeak / peak : 1000.0;
    if (result.minimumTrackingQuality < 0.75) result.reason = "tracking_quality";
    else if (result.maximumCommandJitterMs > 4.2) result.reason = "command_jitter";
    else if (result.pixelsPerCount < 0.25 || result.pixelsPerCount > 0.75) result.reason = "pixels_per_count";
    else if (result.peakDisplacementPx <= 0.0 || result.peakDisplacementPx > 48.0) result.reason = "peak_excursion";
    else if (result.stopDistancePx > 12.0) result.reason = "stop_distance";
    else if (result.finalResidualPx > 3.0) result.reason = "final_residual";
    else if (result.crossAxisLeakagePercent > 10.0) result.reason = "cross_axis_leakage";
    else { result.passed = true; result.reason = "passed"; }
    return result;
}
