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

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
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
    std::filesystem::path config;
    std::filesystem::path outputDirectory;
    std::string source;
    cv::Rect roi;
    int counts = 16;
    int repeats = 10;
    int baselineMs = 300;
    int tailMs = 500;
    int intervalMs = 500;
    std::string axis = "both";
    bool allowWin32 = false;
};

struct Trial
{
    int number = 0;
    char axis = 'x';
    int counts = 0;
    int64_t attemptNs = 0;
    int64_t confirmedNs = 0;
    bool commandSucceeded = false;
    std::vector<PhysicalResponseSample> samples;
};

int64_t ns(Clock::time_point value)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count();
}

void signalHandler(int)
{
    stopRequested.store(true, std::memory_order_relaxed);
}

bool parseInt(const char* value, int& output)
{
    try
    {
        size_t used = 0;
        output = std::stoi(value, &used);
        return used == std::string(value).size();
    }
    catch (...) { return false; }
}

bool parseOptions(int argc, char** argv, Options& options, std::string& error)
{
    std::string confirmation;
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        auto value = [&]() -> const char* {
            return index + 1 < argc ? argv[++index] : nullptr;
        };
        if (argument == "--config") { const char* v = value(); if (v) options.config = v; else error = "--config缺少值"; }
        else if (argument == "--ndi-source") { const char* v = value(); if (v) options.source = v; else error = "--ndi-source缺少值"; }
        else if (argument == "--output-dir") { const char* v = value(); if (v) options.outputDirectory = v; else error = "--output-dir缺少值"; }
        else if (argument == "--axis") { const char* v = value(); if (v) options.axis = v; else error = "--axis缺少值"; }
        else if (argument == "--confirm-device-motion") { const char* v = value(); if (v) confirmation = v; else error = "确认参数缺少值"; }
        else if (argument == "--allow-win32") { const char* v = value(); options.allowWin32 = v && std::string(v) == "YES"; }
        else if (argument == "--counts") { const char* v = value(); if (!v || !parseInt(v, options.counts)) error = "--counts无效"; }
        else if (argument == "--repeats") { const char* v = value(); if (!v || !parseInt(v, options.repeats)) error = "--repeats无效"; }
        else if (argument == "--baseline-ms") { const char* v = value(); if (!v || !parseInt(v, options.baselineMs)) error = "--baseline-ms无效"; }
        else if (argument == "--tail-ms") { const char* v = value(); if (!v || !parseInt(v, options.tailMs)) error = "--tail-ms无效"; }
        else if (argument == "--interval-ms") { const char* v = value(); if (!v || !parseInt(v, options.intervalMs)) error = "--interval-ms无效"; }
        else if (argument == "--roi-x") { const char* v = value(); if (!v || !parseInt(v, options.roi.x)) error = "--roi-x无效"; }
        else if (argument == "--roi-y") { const char* v = value(); if (!v || !parseInt(v, options.roi.y)) error = "--roi-y无效"; }
        else if (argument == "--roi-width") { const char* v = value(); if (!v || !parseInt(v, options.roi.width)) error = "--roi-width无效"; }
        else if (argument == "--roi-height") { const char* v = value(); if (!v || !parseInt(v, options.roi.height)) error = "--roi-height无效"; }
        else { error = "未知参数: " + argument; }
        if (!error.empty()) return false;
    }
    if (confirmation != "YES") error = "必须传入 --confirm-device-motion YES";
    else if (options.config.empty() || !options.config.is_absolute()) error = "--config必须是绝对路径";
    else if (options.outputDirectory.empty()) error = "--output-dir必填";
    else if (options.source.empty() || options.source == "Auto" || options.source == "None") error = "--ndi-source必须是精确源名称";
    else if (options.roi.width < 8 || options.roi.height < 8) error = "ROI宽高必须至少8像素";
    else if (options.counts <= 0 || options.repeats <= 0) error = "counts和repeats必须为正数";
    else if (options.baselineMs < 300 || options.tailMs < 500 || options.intervalMs < 500) error = "baseline/tail/interval不得低于300/500/500 ms";
    else if (options.axis != "x" && options.axis != "y" && options.axis != "both") error = "--axis必须为x、y或both";
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

bool collectUntil(NDICapture& capture, PhysicalResponseTracker& tracker,
                  Clock::time_point deadline, std::vector<PhysicalResponseSample>& samples,
                  std::string& error)
{
    while (!stopRequested.load(std::memory_order_relaxed) && Clock::now() < deadline)
    {
        CapturedFrame frame;
        if (!takeFrame(capture, frame, 100))
        {
            error = "NDI连续100 ms无帧";
            return false;
        }
        const auto receiveNs = ns(frame.timing.backendReceiveTime);
        auto sample = tracker.update(frame.image, receiveNs);
        samples.push_back(sample);
        if (!sample.valid)
        {
            error = "稀疏光流质量不足";
            return false;
        }
    }
    return !stopRequested.load(std::memory_order_relaxed);
}

void writeOutputs(const Options& options, const std::vector<Trial>& trials)
{
    std::filesystem::create_directories(options.outputDirectory);
    std::ofstream frames(options.outputDirectory / "probe_frames.csv");
    std::ofstream summary(options.outputDirectory / "probe_summary.csv");
    frames << "BuildBackend,BuildRevision,BuildTimestampUtc,ControllerRevision,Trial,Axis,SignedCounts,"
              "FrameReceiveNs,CommandAttemptNs,CommandConfirmedNs,CommandSucceeded,RelativePixelX,RelativePixelY,TrackingQuality,Valid\n";
    summary << "BuildBackend,BuildRevision,BuildTimestampUtc,ControllerRevision,Trial,Axis,SignedCounts,"
               "BaselineSamples,TailSamples,FinalDisplacementPx,T10Ms,T50Ms,T90Ms,T99Ms,Valid,Reason\n";
    frames << std::fixed << std::setprecision(6);
    summary << std::fixed << std::setprecision(6);
    for (const auto& trial : trials)
    {
        for (const auto& sample : trial.samples)
            frames << BuildIdentity::backend() << ',' << BuildIdentity::revision() << ',' << BuildIdentity::timestampUtc() << ','
                   << kBasicAimControllerRevision << ',' << trial.number << ',' << trial.axis << ',' << trial.counts << ','
                   << sample.receiveNs << ',' << trial.attemptNs << ',' << trial.confirmedNs << ',' << trial.commandSucceeded << ','
                   << sample.displacementX << ',' << sample.displacementY << ',' << sample.trackingQuality << ',' << sample.valid << '\n';
        const auto result = AnalyzePhysicalResponse(trial.samples, trial.confirmedNs,
                                                     trial.axis == 'x', options.baselineMs, options.tailMs);
        summary << BuildIdentity::backend() << ',' << BuildIdentity::revision() << ',' << BuildIdentity::timestampUtc() << ','
                << kBasicAimControllerRevision << ',' << trial.number << ',' << trial.axis << ',' << trial.counts << ','
                << result.baselineSamples << ',' << result.tailSamples << ',' << result.finalDisplacement << ','
                << result.t10Ms << ',' << result.t50Ms << ',' << result.t90Ms << ',' << result.t99Ms << ','
                << result.valid << ',' << result.reason << '\n';
    }
}
}

int main(int argc, char** argv)
{
    Options options;
    std::string error;
    if (!parseOptions(argc, argv, options, error))
    {
        std::cerr << error << "\nUsage: xen_physical_response_probe --config <absolute.ini> --ndi-source <exact> "
          "--output-dir <dir> --roi-x N --roi-y N --roi-width N --roi-height N --confirm-device-motion YES "
          "[--axis x|y|both] [--counts 16] [--repeats 10]\n";
        return 2;
    }
    std::signal(SIGINT, signalHandler);
    Config config;
    if (!std::filesystem::exists(options.config) || !config.loadConfig(options.config.string()))
    {
        std::cerr << "无法加载配置: " << options.config << '\n';
        return 3;
    }
    const auto method = ParseMouseInputMethod(config.input_method);
    if (!method)
    {
        std::cerr << "配置中的input_method无效，拒绝回退WIN32\n";
        return 4;
    }
    if (*method == MouseInputMethod::Win32 && !options.allowWin32)
    {
        std::cerr << "默认拒绝WIN32输出；确需桌面注入时额外传入 --allow-win32 YES\n";
        return 5;
    }
    auto mouse = CreateMouseInputDevice(config);
    if (!mouse || !mouse->isOpen())
    {
        std::cerr << "鼠标设备未打开\n";
        return 6;
    }

    NDICapture capture(config.detection_resolution, config.detection_resolution, options.source, 240,
                       config.ndi_source_width, config.ndi_source_height,
                       NdiFrameDeliveryMode::PreserveAllBounded, 2048);
    CapturedFrame first;
    if (!takeFrame(capture, first, 10000))
    {
        std::cerr << "10秒内未收到指定NDI源画面\n";
        return 7;
    }
    const auto stabilityDeadline = Clock::now() + std::chrono::seconds(2);
    while (Clock::now() < stabilityDeadline)
    {
        CapturedFrame drain;
        if (!takeFrame(capture, drain, 100)) { std::cerr << "NDI稳定性检查断流\n"; return 8; }
        first = std::move(drain);
    }
    const auto diagnostics = NDICapture::GetDiagnostics();
    if (diagnostics.declaredFps < 200.0 || diagnostics.receiveFps < 200)
    {
        std::cerr << "拒绝采集：需要原始240 Hz链路，当前声明/接收FPS="
                  << diagnostics.declaredFps << '/' << diagnostics.receiveFps << '\n';
        return 9;
    }
    PhysicalResponseTracker tracker;
    if (!tracker.initialize(first.image, options.roi, error))
    {
        std::cerr << "跟踪初始化失败: " << error << '\n';
        return 10;
    }

    std::vector<std::pair<char, int>> commands;
    for (int repeat = 0; repeat < options.repeats; ++repeat)
    {
        if (options.axis == "x" || options.axis == "both")
        {
            commands.emplace_back('x', options.counts);
            commands.emplace_back('x', -options.counts);
        }
        if (options.axis == "y" || options.axis == "both")
        {
            commands.emplace_back('y', options.counts);
            commands.emplace_back('y', -options.counts);
        }
    }

    std::vector<Trial> trials;
    std::cout << "开始孤立脉冲采集，共" << commands.size() << "轮；Ctrl+C可停止。\n";
    for (size_t index = 0; index < commands.size() && !stopRequested.load(); ++index)
    {
        Trial trial;
        trial.number = static_cast<int>(index + 1);
        trial.axis = commands[index].first;
        trial.counts = commands[index].second;
        if (!collectUntil(capture, tracker, Clock::now() + std::chrono::milliseconds(options.baselineMs), trial.samples, error))
            break;
        const auto attempt = Clock::now();
        trial.attemptNs = ns(attempt);
        trial.commandSucceeded = mouse->move(trial.axis == 'x' ? trial.counts : 0,
                                             trial.axis == 'y' ? trial.counts : 0);
        trial.confirmedNs = ns(Clock::now());
        if (!trial.commandSucceeded)
        {
            error = "设备move返回失败";
            trials.push_back(std::move(trial));
            break;
        }
        if (!collectUntil(capture, tracker, Clock::now() + std::chrono::milliseconds(options.tailMs), trial.samples, error))
        {
            trials.push_back(std::move(trial));
            break;
        }
        const auto trialSummary = AnalyzePhysicalResponse(
            trial.samples, trial.confirmedNs, trial.axis == 'x', options.baselineMs, options.tailMs);
        if (!trialSummary.valid)
        {
            error = "本轮响应无效: " + trialSummary.reason;
            trials.push_back(std::move(trial));
            break;
        }
        trials.push_back(std::move(trial));
        std::vector<PhysicalResponseSample> ignored;
        if (!collectUntil(capture, tracker, Clock::now() + std::chrono::milliseconds(options.intervalMs), ignored, error))
            break;
        if (NDICapture::GetDiagnostics().droppedFrames != 0)
        {
            error = "逐帧队列发生丢帧";
            break;
        }
    }
    writeOutputs(options, trials);
    if (!error.empty())
    {
        std::cerr << "采集停止: " << error << "；已保留已完成数据。\n";
        return 11;
    }
    std::cout << "采集完成: " << options.outputDirectory << '\n';
    return 0;
}
