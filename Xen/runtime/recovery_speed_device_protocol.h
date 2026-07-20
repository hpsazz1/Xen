#pragma once

#include "physical_response_probe.h"

#include <filesystem>
#include <string>
#include <vector>

struct RecoverySpeedDeviceCommand
{
    int trial = 0;
    int rateCountsPerSecond = 0;
    int leadingDirection = 0;
    std::string segment;
    int command = 0;
    int frameIndex = 0;
    int64_t trialStartOffsetUs = 0;
    int64_t relativeOffsetUs = 0;
    int64_t scheduledOffsetUs = 0;
    int deltaX = 0;
    int deltaY = 0;
    int cumulativeX = 0;
    int trialAbsoluteCounts = 0;
    int maximumExcursionCounts = 0;
};

struct RecoverySpeedDevicePlan
{
    std::string protocolVersion;
    std::string planId;
    std::string buildBackend;
    std::string buildRevision;
    int controllerRevision = 0;
    std::string deviceId;
    std::filesystem::path configPath;
    std::string configSha256;
    std::string ndiSource;
    int roiX = 0;
    int roiY = 0;
    int roiWidth = 0;
    int roiHeight = 0;
    std::vector<RecoverySpeedDeviceCommand> commands;
};

struct RecoverySpeedCommandRecord
{
    int trial = 0;
    int command = 0;
    int deltaX = 0;
    int64_t scheduledNs = 0;
    int64_t attemptNs = 0;
    int64_t confirmedNs = 0;
    bool succeeded = false;
};

struct RecoverySpeedTrialResult
{
    int trial = 0;
    int rateCountsPerSecond = 0;
    int leadingDirection = 0;
    int expectedExcursionCounts = 0;
    size_t samples = 0;
    size_t commands = 0;
    double minimumTrackingQuality = 0.0;
    double forwardDisplacementPx = 0.0;
    double peakDisplacementPx = 0.0;
    double stopDistancePx = 0.0;
    double finalResidualPx = 0.0;
    double pixelsPerCount = 0.0;
    double crossAxisLeakagePercent = 0.0;
    double maximumCommandJitterMs = 0.0;
    bool passed = false;
    std::string reason;
};

bool LoadRecoverySpeedDevicePlan(
    const std::filesystem::path& planDirectory,
    const std::string& expectedBackend,
    const std::string& expectedRevision,
    int expectedControllerRevision,
    RecoverySpeedDevicePlan& plan,
    std::string& error);

RecoverySpeedTrialResult AnalyzeRecoverySpeedTrial(
    int trial,
    int rateCountsPerSecond,
    int leadingDirection,
    int expectedExcursionCounts,
    int64_t trialStartNs,
    const std::vector<PhysicalResponseSample>& samples,
    const std::vector<RecoverySpeedCommandRecord>& commands);
