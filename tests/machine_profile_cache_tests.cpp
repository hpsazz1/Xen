#include "runtime/machine_profile_cache.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace
{
int failures = 0;

void expectTrue(bool condition, const char* name)
{
    if (condition)
        return;
    std::cerr << name << ": condition was false\n";
    ++failures;
}

void expectLevel(const MachineProfileDecision& decision,
                 MachineProfileLevel expected, const char* name)
{
    if (decision.level == expected)
        return;
    std::cerr << name << ": expected " << machineProfileLevelName(expected)
              << ", got " << machineProfileLevelName(decision.level)
              << " (" << decision.reason << ")\n";
    ++failures;
}

MachineProfileRecord stableRecord()
{
    MachineProfileRecord record;
    record.key.gameProfile = "CS";
    record.key.aimMode = "hipfire";
    record.key.captureMethod = "ndi";
    record.key.captureSource = "HPSAZZ (main)";
    record.key.sourceWidth = 2560;
    record.key.sourceHeight = 1440;
    record.key.roiX = 1120;
    record.key.roiY = 560;
    record.key.roiWidth = 320;
    record.key.roiHeight = 320;
    record.key.inferenceBackend = "DML";
    record.key.inputMethod = "KMBOX_NET";
    record.key.inputDeviceIdentity = "7679E04E";
    record.key.sensitivity = 1.4;
    record.key.yaw = 0.022;
    record.key.pitch = 0.022;
    record.key.fovXDegrees = 106.0;
    record.key.fovYDegrees = 74.0;
    record.key.fovScaled = false;
    record.key.baseFovDegrees = 0.0;
    record.key.controllerRevision = 64;
    record.evidence.probeRoiX = 120;
    record.evidence.probeRoiY = 100;
    record.evidence.probeRoiWidth = 80;
    record.evidence.probeRoiHeight = 80;
    record.evidence.pixelsPerCountX = 0.515625;
    record.evidence.pixelsPerCountY = 0.5;
    record.evidence.degreesPerCountX = 0.021354;
    record.evidence.degreesPerCountY = 0.018991;
    record.evidence.t50MsX = 14.1322;
    record.evidence.t50MsY = 14.0608;
    record.evidence.t90MsX = 14.50823;
    record.evidence.t90MsY = 14.49659;
    record.evidence.confidence = 1.0;
    record.evidence.trialCount = 360;
    record.evidence.protocol = "active_profile_v1";
    record.evidence.buildIdentity = "DML|13f67257f5c2|r64";
    record.evidence.evidenceDigest = "synthetic-test-digest";
    return record;
}
}

int main()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "xen_machine_profile_cache_test.ini";
    std::error_code ec;
    std::filesystem::remove(path, ec);

    const MachineProfileRecord record = stableRecord();
    std::string error;
    expectTrue(MachineProfileCache::saveNew(path.string(), record, error),
               "explicit cache save succeeds");
    expectTrue(!MachineProfileCache::saveNew(path.string(), record, error) &&
               error == "cache_already_exists",
               "cache save refuses overwrite");

    MachineProfileCache cache;
    expectTrue(cache.load(path.string()), "saved cache loads");
    const MachineProfileDecision exact = cache.evaluate(record.key, true, true);
    expectLevel(exact, MachineProfileLevel::CalibratedAngle,
                "exact key selects calibrated angle");
    expectTrue(exact.cacheMatched && exact.calibratedViewResponseEnabled &&
               exact.feedforwardConfidenceScale == 1.0 &&
               exact.predictionEnabled && exact.integralEnabled &&
               exact.commandToFrameDelayMs > 14.0 &&
               exact.commandToFrameDelayMs < 14.2 &&
               exact.commandResponseMs == 0.0,
               "calibrated level exposes bounded feature policy");

    MachineProfileKey changedSource = record.key;
    changedSource.captureSource = "different NDI source";
    const MachineProfileDecision sourceMismatch =
        cache.evaluate(changedSource, true, true);
    expectLevel(sourceMismatch, MachineProfileLevel::ConservativeAngle,
                "NDI source mismatch degrades to user profile");
    expectTrue(sourceMismatch.reason == "cache_key_mismatch:captureSource" &&
               !sourceMismatch.cacheMatched,
               "NDI source mismatch is auditable");

    MachineProfileKey changedController = record.key;
    changedController.controllerRevision = 65;
    const MachineProfileDecision controllerMismatch =
        cache.evaluate(changedController, true, true);
    expectLevel(controllerMismatch, MachineProfileLevel::ConservativeAngle,
                "controller revision mismatch invalidates cache");

    const MachineProfileDecision disabled = cache.evaluate(record.key, false, true);
    expectLevel(disabled, MachineProfileLevel::ConservativeAngle,
                "disabled cache keeps conservative user angle mode");
    expectTrue(disabled.feedforwardConfidenceScale == 0.25 &&
               disabled.predictionEnabled && !disabled.integralEnabled &&
               disabled.reason == "cache_disabled",
               "conservative level applies feedforward ceiling");

    const MachineProfileDecision normalized = cache.evaluate(record.key, true, false);
    expectLevel(normalized, MachineProfileLevel::NormalizedImage,
                "invalid user profile uses normalized geometry");
    expectTrue(normalized.normalizedImageEnabled &&
               normalized.feedforwardConfidenceScale == 0.0 &&
               !normalized.predictionEnabled && !normalized.integralEnabled,
               "normalized mode disables angular feedforward");

    MachineProfileKey noGeometry = record.key;
    noGeometry.sourceWidth = 0;
    const MachineProfileDecision safe = cache.evaluate(noGeometry, true, false);
    expectLevel(safe, MachineProfileLevel::SafetyDirectPursuit,
                "invalid profile and geometry selects safety pursuit");
    expectTrue(!safe.predictionEnabled && !safe.integralEnabled,
               "safety pursuit disables stateful enhancements");

    const MachineProfileDecision severe = cache.evaluate(record.key, true, true, true);
    expectLevel(severe, MachineProfileLevel::SafetyDirectPursuit,
                "severe runtime mismatch has highest priority");

    std::filesystem::remove(path, ec);
    if (failures != 0)
    {
        std::cerr << failures << " machine profile cache test(s) failed\n";
        return 1;
    }
    std::cout << "machine profile cache tests passed\n";
    return 0;
}
