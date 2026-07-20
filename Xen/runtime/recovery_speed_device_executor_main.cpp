#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "capture/ndi_capture.h"
#include "config/config.h"
#include "mouse/MouseInput.h"
#include "runtime/basic_aim_controller.h"
#include "runtime/build_identity.h"
#include "runtime/physical_response_probe.h"
#include "runtime/recovery_speed_device_protocol.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

std::mutex configMutex;
std::atomic<bool> aiming{ false };
std::atomic<bool> shooting{ false };
std::atomic<bool> zooming{ false };

namespace
{
using Clock = std::chrono::steady_clock;
std::atomic<bool> stopRequested{ false };

struct Options
{
    std::filesystem::path planDirectory;
    std::filesystem::path outputDirectory;
    std::string confirmedPlanId;
    bool confirmedDeviceMotion = false;
    bool confirmedZeroTargetRisk = false;
    bool confirmedEmergencyStop = false;
    bool validateOnly = false;
};

struct TrialCapture
{
    int trial = 0;
    int rate = 0;
    int direction = 0;
    int64_t startNs = 0;
    std::vector<PhysicalResponseSample> samples;
    std::vector<RecoverySpeedCommandRecord> commands;
    RecoverySpeedTrialResult result;
};

int64_t ns(Clock::time_point value)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count();
}
void signalHandler(int)
{
    stopRequested.store(true, std::memory_order_relaxed);
}

bool parseOptions(int argc, char** argv, Options& options, std::string& error)
{
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        auto next = [&]() -> std::string { return index + 1 < argc ? argv[++index] : std::string{}; };
        if (argument == "--plan-dir") options.planDirectory = next();
        else if (argument == "--output-dir") options.outputDirectory = next();
        else if (argument == "--confirm-plan-id") options.confirmedPlanId = next();
        else if (argument == "--confirm-device-motion") options.confirmedDeviceMotion = next() == "YES";
        else if (argument == "--confirm-zero-target-risk") options.confirmedZeroTargetRisk = next() == "YES";
        else if (argument == "--confirm-emergency-stop-ready") options.confirmedEmergencyStop = next() == "YES";
        else if (argument == "--validate-only") options.validateOnly = true;
        else { error = "未知参数:" + argument; return false; }
    }
    if (options.planDirectory.empty() || !options.planDirectory.is_absolute()) error = "--plan-dir必须是绝对路径";
    else if (!options.validateOnly &&
             (options.outputDirectory.empty() || !options.outputDirectory.is_absolute()))
        error = "--output-dir必须是不存在的绝对路径";
    else if (!options.validateOnly &&
             (!options.confirmedDeviceMotion || !options.confirmedZeroTargetRisk ||
              !options.confirmedEmergencyStop))
        error = "物理执行必须同时确认device-motion、zero-target-risk和emergency-stop-ready";
    return error.empty();
}

bool takeFrame(NDICapture& capture, CapturedFrame& frame, int timeoutMs)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!stopRequested.load(std::memory_order_relaxed) && Clock::now() < deadline)
    {
        frame = capture.GetNextFrameTimed();
        if (!frame.image.empty()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

std::unique_ptr<NDICapture> startStableCapture(const Config& config,
                                               const RecoverySpeedDevicePlan& plan,
                                               CapturedFrame& current,
                                               std::string& error)
{
    auto capture = std::make_unique<NDICapture>(
        config.detection_resolution, config.detection_resolution, plan.ndiSource, 240,
        config.ndi_source_width, config.ndi_source_height,
        NdiFrameDeliveryMode::PreserveAllBounded, 2048);
    if (!takeFrame(*capture, current, 10000))
    {
        error = "ndi_first_frame_timeout";
        return {};
    }
    const auto stabilityDeadline = Clock::now() + std::chrono::seconds(2);
    while (Clock::now() < stabilityDeadline)
    {
        if (!takeFrame(*capture, current, 100))
        {
            error = "ndi_stability_timeout";
            return {};
        }
    }
    const auto diagnostics = NDICapture::GetDiagnostics();
    if (diagnostics.declaredFps < 200.0 || diagnostics.receiveFps < 200 ||
        diagnostics.droppedFrames != 0)
    {
        error = "ndi_requires_native_240hz_zero_drop";
        return {};
    }
    return capture;
}

void writeOutputs(const std::filesystem::path& outputDirectory,
                  const RecoverySpeedDevicePlan& plan,
                  const std::vector<TrialCapture>& trials,
                  const std::string& recommendation,
                  const std::string& issues)
{
    std::filesystem::create_directories(outputDirectory);
    std::ofstream frames(outputDirectory / "recovery_speed_device_frames.csv");
    std::ofstream commands(outputDirectory / "recovery_speed_device_commands.csv");
    std::ofstream summary(outputDirectory / "recovery_speed_device_summary.csv");
    frames << "BuildBackend,BuildRevision,BuildTimestampUtc,ControllerRevision,PlanId,Trial,RateCountsPerSecond,"
              "LeadingDirection,TrialStartNs,FrameReceiveNs,RelativeMs,DisplacementX,DisplacementY,TrackingQuality,Valid\n";
    commands << "BuildBackend,BuildRevision,BuildTimestampUtc,ControllerRevision,PlanId,Trial,Command,DeltaX,"
                "ScheduledNs,AttemptNs,ConfirmedNs,AttemptJitterMs,ConfirmLatencyMs,Succeeded\n";
    summary << "BuildBackend,BuildRevision,BuildTimestampUtc,ControllerRevision,PlanId,Trial,RateCountsPerSecond,"
               "LeadingDirection,ExpectedExcursionCounts,Samples,Commands,MinimumTrackingQuality,ForwardDisplacementPx,"
               "PeakDisplacementPx,VisualResponseLatencyMs,StopAnchorDisplacementPx,StopDistancePx,FinalResidualPx,"
               "PixelsPerCount,CrossAxisLeakagePercent,"
               "MaximumCommandJitterMs,Passed,Reason\n";
    frames << std::fixed << std::setprecision(6);
    commands << std::fixed << std::setprecision(6);
    summary << std::fixed << std::setprecision(6);
    for (const auto& trial : trials)
    {
        for (const auto& sample : trial.samples)
            frames << BuildIdentity::backend() << ',' << BuildIdentity::revision() << ',' << BuildIdentity::timestampUtc() << ','
                   << kBasicAimControllerRevision << ',' << plan.planId << ',' << trial.trial << ',' << trial.rate << ','
                   << trial.direction << ',' << trial.startNs << ',' << sample.receiveNs << ','
                   << static_cast<double>(sample.receiveNs - trial.startNs) / 1e6 << ','
                   << sample.displacementX << ',' << sample.displacementY << ',' << sample.trackingQuality << ','
                   << sample.valid << '\n';
        for (const auto& command : trial.commands)
            commands << BuildIdentity::backend() << ',' << BuildIdentity::revision() << ',' << BuildIdentity::timestampUtc() << ','
                     << kBasicAimControllerRevision << ',' << plan.planId << ',' << command.trial << ',' << command.command << ','
                     << command.deltaX << ',' << command.scheduledNs << ',' << command.attemptNs << ',' << command.confirmedNs << ','
                     << static_cast<double>(command.attemptNs - command.scheduledNs) / 1e6 << ','
                     << static_cast<double>(command.confirmedNs - command.attemptNs) / 1e6 << ',' << command.succeeded << '\n';
        const auto& result = trial.result;
        summary << BuildIdentity::backend() << ',' << BuildIdentity::revision() << ',' << BuildIdentity::timestampUtc() << ','
                << kBasicAimControllerRevision << ',' << plan.planId << ',' << result.trial << ','
                << result.rateCountsPerSecond << ',' << result.leadingDirection << ',' << result.expectedExcursionCounts << ','
                << result.samples << ',' << result.commands << ',' << result.minimumTrackingQuality << ','
                << result.forwardDisplacementPx << ',' << result.peakDisplacementPx << ','
                << result.visualResponseLatencyMs << ',' << result.stopAnchorDisplacementPx << ','
                << result.stopDistancePx << ','
                << result.finalResidualPx << ',' << result.pixelsPerCount << ',' << result.crossAxisLeakagePercent << ','
                << result.maximumCommandJitterMs << ',' << result.passed << ',' << result.reason << '\n';
    }
    std::ofstream decision(outputDirectory / "recovery_speed_device_decision.txt");
    decision << "ProtocolCompleted=" << (trials.size() == 4) << '\n'
             << "PlanId=" << plan.planId << '\n'
             << "Recommendation=" << recommendation << '\n'
             << "ConfigurationAutoWrite=0\nActiveAutoEnable=0\n"
             << "Issues=" << issues << '\n';
}

bool directionSymmetryPassed(const RecoverySpeedTrialResult& first,
                             const RecoverySpeedTrialResult& second)
{
    const double center = (first.pixelsPerCount + second.pixelsPerCount) * 0.5;
    return center > 1e-9 && 100.0 * std::abs(first.pixelsPerCount - second.pixelsPerCount) / center <= 15.0;
}
}

int main(int argc, char** argv)
{
    Options options;
    std::string error;
    if (!parseOptions(argc, argv, options, error))
    {
        std::cerr << error << "\nUsage: xen_recovery_speed_device_executor --plan-dir <absolute> --output-dir <absolute> "
                     "--confirm-plan-id <id> --confirm-device-motion YES --confirm-zero-target-risk YES "
                     "--confirm-emergency-stop-ready YES [--validate-only]\n";
        return 2;
    }
    RecoverySpeedDevicePlan plan;
    if (!LoadRecoverySpeedDevicePlan(options.planDirectory, BuildIdentity::backend(),
                                     BuildIdentity::revision(), kBasicAimControllerRevision, plan, error))
    {
        std::cerr << "计划校验失败:" << error << '\n';
        return 3;
    }
    if (options.validateOnly)
    {
        std::cout << "PlanValidationPassed=1\nPlanId=" << plan.planId << "\nExecutionPerformed=0\n";
        return 0;
    }
    if (options.confirmedPlanId != plan.planId)
    {
        std::cerr << "--confirm-plan-id与已审计计划不一致\n";
        return 4;
    }
    if (std::filesystem::exists(options.outputDirectory))
    {
        std::cerr << "输出目录已存在，拒绝覆盖:" << options.outputDirectory << '\n';
        return 5;
    }
    Config config;
    if (!config.loadConfig(plan.configPath.string()) || config.input_method != "KMBOX_NET" ||
        plan.deviceId != "KMBOX_NET:" + config.kmbox_net_uuid || config.ndi_source_name != plan.ndiSource)
    {
        std::cerr << "配置中的设备或NDI身份与计划不一致\n";
        return 6;
    }
    std::signal(SIGINT, signalHandler);
    auto mouse = CreateMouseInputDevice(config);
    if (!mouse)
    {
        std::cerr << "设备创建失败\n";
        return 7;
    }
    const auto readyDeadline = Clock::now() + std::chrono::seconds(10);
    while (!mouse->isReadyForMotion() && !stopRequested.load() && Clock::now() < readyDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!mouse->isReadyForMotion() || !mouse->prepareForMotion())
    {
        std::cerr << "设备未就绪或动作通道预热失败\n";
        return 7;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    CapturedFrame current;
    auto capture = startStableCapture(config, plan, current, error);
    if (!capture)
    {
        std::cerr << "NDI启动失败:" << error << '\n';
        return 8;
    }

    std::filesystem::create_directories(options.outputDirectory);
    std::vector<TrialCapture> trials;
    std::string issues;
    for (int trialNumber = 1; trialNumber <= 4 && !stopRequested.load(); ++trialNumber)
    {
        if (trialNumber == 3)
        {
            if (trials.size() != 2 || !trials[0].result.passed || !trials[1].result.passed ||
                !directionSymmetryPassed(trials[0].result, trials[1].result))
            {
                issues = "1440_control_gate_failed";
                break;
            }
            // 人工确认时间无上限。确认前关闭逐帧会话，防止后台接收在无人消费时填满
            // 有界队列；确认后用新会话清零统计并重新验证原生240 Hz和零丢帧。
            capture.reset();
            std::cout << "1440对照通过。确认现场仍无目标风险且紧急停止可用后，输入 ENTER-1800:"
                      << plan.planId << " 继续：" << std::flush;
            std::string confirmation;
            if (!std::getline(std::cin, confirmation) || confirmation != "ENTER-1800:" + plan.planId)
            {
                issues = "second_stage_confirmation_missing";
                break;
            }
            capture = startStableCapture(config, plan, current, error);
            if (!capture)
            {
                issues = "ndi_restart:" + error;
                break;
            }
        }
        const auto firstCommand = std::find_if(plan.commands.begin(), plan.commands.end(),
            [&](const auto& command) { return command.trial == trialNumber; });
        if (firstCommand == plan.commands.end()) { issues = "trial_plan_missing"; break; }
        TrialCapture trial;
        trial.trial = trialNumber;
        trial.rate = firstCommand->rateCountsPerSecond;
        trial.direction = firstCommand->leadingDirection;
        PhysicalResponseTracker tracker;
        if (!tracker.initialize(current.image, cv::Rect(plan.roiX, plan.roiY, plan.roiWidth, plan.roiHeight), error))
        {
            issues = "tracker_initialize:" + error;
            break;
        }
        const auto start = Clock::now();
        trial.startNs = ns(start);
        std::atomic<bool> trialAbort{ false };
        std::mutex recordsMutex;
        std::thread scheduler([&]() {
            for (const auto& command : plan.commands)
            {
                if (command.trial != trialNumber) continue;
                const auto scheduled = start + std::chrono::microseconds(command.relativeOffsetUs);
                while (!stopRequested.load() && !trialAbort.load() && Clock::now() < scheduled)
                {
                    const auto remaining = scheduled - Clock::now();
                    if (remaining > std::chrono::milliseconds(2)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    else std::this_thread::yield();
                }
                if (stopRequested.load() || trialAbort.load()) break;
                RecoverySpeedCommandRecord record;
                record.trial = trialNumber;
                record.command = command.command;
                record.deltaX = command.deltaX;
                record.scheduledNs = ns(scheduled);
                record.attemptNs = ns(Clock::now());
                record.succeeded = mouse->move(command.deltaX, 0);
                record.confirmedNs = ns(Clock::now());
                {
                    std::lock_guard<std::mutex> lock(recordsMutex);
                    trial.commands.push_back(record);
                }
                if (!record.succeeded) { trialAbort.store(true); break; }
            }
        });
        const auto deadline = start + std::chrono::microseconds(2566667);
        while (!stopRequested.load() && !trialAbort.load() && Clock::now() < deadline)
        {
            if (!takeFrame(*capture, current, 100)) { trialAbort.store(true); issues = "ndi_frame_timeout"; break; }
            auto sample = tracker.update(current.image, ns(current.timing.backendReceiveTime));
            trial.samples.push_back(sample);
            if (!sample.valid || std::abs(sample.displacementX) > 48.0 || std::abs(sample.displacementY) > 12.0)
            {
                trialAbort.store(true);
                issues = sample.valid ? "live_excursion_limit" : "live_tracking_invalid";
                break;
            }
        }
        trialAbort.store(true);
        scheduler.join();
        {
            std::lock_guard<std::mutex> lock(recordsMutex);
            trial.result = AnalyzeRecoverySpeedTrial(trialNumber, trial.rate, trial.direction,
                firstCommand->maximumExcursionCounts, trial.startNs, trial.samples, trial.commands);
        }
        if (NDICapture::GetDiagnostics().droppedFrames != 0 && issues.empty()) issues = "ndi_dropped_frames";
        if (!trial.result.passed && issues.empty()) issues = trial.result.reason;
        trials.push_back(std::move(trial));
        if (!issues.empty() || stopRequested.load()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

    bool protocolPassed = trials.size() == 4;
    for (const auto& trial : trials) protocolPassed = protocolPassed && trial.result.passed;
    if (protocolPassed)
    {
        protocolPassed = directionSymmetryPassed(trials[0].result, trials[1].result) &&
                         directionSymmetryPassed(trials[2].result, trials[3].result);
        for (size_t index = 2; index < 4 && protocolPassed; ++index)
        {
            const auto& baseline = trials[index - 2].result;
            const auto& candidate = trials[index].result;
            const double allowedStop = baseline.stopDistancePx + std::max(2.0, baseline.stopDistancePx * 0.25);
            if (candidate.stopDistancePx > allowedStop) protocolPassed = false;
        }
        if (!protocolPassed && issues.empty()) issues = "cross_rate_safety_gate";
    }
    if (stopRequested.load() && issues.empty()) issues = "ctrl_c";
    writeOutputs(options.outputDirectory, plan, trials,
                 protocolPassed ? "MANUAL_REVIEW_ONLY" : "HOLD_DIAGNOSTIC", issues);
    std::cout << "结果已保存:" << options.outputDirectory << '\n';
    return protocolPassed ? 0 : 11;
}
