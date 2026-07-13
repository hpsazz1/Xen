#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <iostream>
#include <atomic>
#include <thread>
#include <mutex>
#include <algorithm>
#include <chrono>
#include <timeapi.h>
#include <condition_variable>
#include <filesystem>
#include <cstdint>
#include <memory>
#include <optional>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "capture.h"
#ifdef USE_CUDA
#include "trt_detector.h"
#include "depth/depth_anything_trt.h"
#include "depth/depth_mask.h"
#include "tensorrt/nvinf.h"
#endif
#include "Xen.h"
#include "keycodes.h"
#include "keyboard_listener.h"
#include "other_tools.h"
#include "duplication_api_capture.h"
#include "winrt_capture.h"
#include "virtual_camera.h"
#include "udp_capture.h"
#include "ndi_capture.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowsapp.lib")

// 最新捕获的帧（CPU 端 cv::Mat），供叠加层预览或处理
cv::Mat latestFrame;
// 保护 latestFrame 和 frameQueue 的互斥锁
std::mutex frameMutex;

// 屏幕源宽度和高度（由当前捕获后端报告）
std::atomic<int> screenWidth(0);
std::atomic<int> screenHeight(0);

// 用于 FPS 统计的帧计数器和计算值
std::atomic<int> captureFrameCount(0);
std::atomic<int> captureFps(0);
// FPS 统计周期的起始时间
std::chrono::time_point<std::chrono::high_resolution_clock> captureFpsStartTime;

namespace
{
std::mutex g_captureSourceDiagnosticsMutex;
CaptureSourceDiagnostics g_captureSourceDiagnostics;
}

CaptureSourceDiagnostics GetCaptureSourceDiagnostics()
{
    std::lock_guard<std::mutex> lock(g_captureSourceDiagnosticsMutex);
    return g_captureSourceDiagnostics;
}

// WinRT 捕获性能统计计数器（用于诊断和调优）

// WinRT TryGetNextFrame 的总调用次数
std::atomic<uint64_t> captureWinrtPollAttemptsTotal(0);
// WinRT 从帧池中排出的总帧数
std::atomic<uint64_t> captureWinrtFramesDrainedTotal(0);
// WinRT 成功返回给调用者的总帧数
std::atomic<uint64_t> captureWinrtFramesReturnedTotal(0);
// WinRT TryGetNextFrame 返回 nullptr（无新帧）的次数
std::atomic<uint64_t> captureWinrtEmptyPollsTotal(0);
// WinRT 整个回读流程（CopySubresourceRegion + Map + pixel copy + Unmap）总耗时（微秒）
std::atomic<uint64_t> captureWinrtReadbackMicrosTotal(0);
// WinRT Map 操作总耗时（微秒）
std::atomic<uint64_t> captureWinrtMapMicrosTotal(0);
// WinRT 像素拷贝总耗时（微秒）
std::atomic<uint64_t> captureWinrtPixelCopyMicrosTotal(0);

// 帧队列，存储最近捕获的帧供其他模块批量获取
std::deque<cv::Mat> frameQueue;

#ifdef USE_CUDA
namespace
{
// 保护远距离检测抑制掩码的互斥锁
std::mutex g_detectionSuppressionMaskMutex;
// 远距离检测抑制掩码，用于屏蔽由深度信息确定的远距离区域，减少误检
cv::Mat g_detectionSuppressionMask;
}

// GPU 捕获路径全局性能计数器

// GPU 捕获尝试总次数
std::atomic<uint64_t> captureGpuAttemptsTotal(0);
// GPU 捕获成功总次数
std::atomic<uint64_t> captureGpuCapturedTotal(0);
// GPU 捕获超时总次数
std::atomic<uint64_t> captureGpuTimeoutTotal(0);
// GPU 累积帧数（DuplicateOutput 可能一次返回多帧累积的增量）
std::atomic<uint64_t> captureGpuAccumulatedFramesTotal(0);
// GPU 丢帧总数（累积帧数 - 1，因每次只取最新帧）
std::atomic<uint64_t> captureGpuMissedFramesTotal(0);
// GPU 包含新 Present 事件的帧数
std::atomic<uint64_t> captureGpuPresentFramesTotal(0);
// GPU 仅包含鼠标更新事件的帧数
std::atomic<uint64_t> captureGpuMouseOnlyEventsTotal(0);
// GPU 仅包含元数据更新事件的帧数
std::atomic<uint64_t> captureGpuMetadataOnlyEventsTotal(0);
// GPU 合并矩形事件数
std::atomic<uint64_t> captureGpuCoalescedEventsTotal(0);
// GPU 捕获失败后回退到 CPU 路径的尝试次数
std::atomic<uint64_t> captureCpuFallbackAttemptsTotal(0);
// GPU 回退到 CPU 路径后成功获取的帧数
std::atomic<uint64_t> captureCpuFallbackFramesTotal(0);

// 更新远距离检测抑制掩码（线程安全）
static void UpdateDetectionSuppressionMask(const cv::Mat& mask)
{
    std::lock_guard<std::mutex> lock(g_detectionSuppressionMaskMutex);
    if (!mask.empty() && mask.type() == CV_8UC1)
        g_detectionSuppressionMask = mask.clone();
    else
        g_detectionSuppressionMask.release();
}

// 获取当前的远距离检测抑制掩码副本（线程安全）
cv::Mat getCurrentDetectionSuppressionMask()
{
    std::lock_guard<std::mutex> lock(g_detectionSuppressionMaskMutex);
    return g_detectionSuppressionMask.clone();
}
#endif

namespace
{
// 捕获线程的配置快照结构体，用于在锁定状态下原子读取所有配置项，
// 避免在捕获循环中频繁加锁读取全局配置
struct CaptureThreadConfig
{
    std::string capture_method;          // 捕获方法：duplication_api / winrt / virtual_camera / udp_capture / ndi
    int capture_fps = 0;                 // 目标捕获帧率（0 表示不限）
    int detection_resolution = 0;        // 检测分辨率（宽高相同，正方形输入）
    int monitor_idx = 0;                 // 要捕获的显示器索引
    bool circle_fov_enabled = false;     // 是否启用圆形视场（FOV）裁剪
    int circle_fov_radius_percent = 100; // 圆形视场半径百分比
    bool capture_borders = true;         // 是否捕获窗口边框（仅 WinRT 窗口目标有效）
    bool capture_cursor = true;          // 是否捕获鼠标指针
    std::string capture_target;          // WinRT 捕获目标：monitor / window
    std::string capture_window_title;    // WinRT 窗口捕获的目标窗口标题
    std::string virtual_camera_name;     // 虚拟摄像头名称
    int virtual_camera_width = 0;        // 虚拟摄像头宽度
    int virtual_camera_height = 0;       // 虚拟摄像头高度
    std::string udp_ip;                  // UDP 捕获的 IP 地址
    int udp_port = 0;                    // UDP 捕获的端口
    std::string ndi_source_name;         // NDI 源名称
    int ndi_source_width = 0;            // NDI 预裁剪 ROI 对应的完整游戏 FOV 宽度
    int ndi_source_height = 0;           // NDI 预裁剪 ROI 对应的完整游戏 FOV 高度
    std::string backend;                 // AI 推理后端：TRT / DML
    std::vector<std::string> screenshot_button; // 截图快捷键按键列表
    int screenshot_delay = 0;            // 截图最小间隔（毫秒）
    bool show_window = false;            // 是否显示捕获预览窗口
    bool verbose = false;                // 是否输出详细日志
#ifdef USE_CUDA
    bool depth_inference_enabled = false;   // 是否启用深度推理
    bool depth_mask_enabled = false;        // 是否启用深度掩码
    std::string depth_model_path;           // 深度模型路径
    int depth_mask_fps = 0;                 // 深度掩码更新帧率
    int depth_mask_near_percent = 0;        // 深度掩码近处百分比阈值
    int depth_mask_expand = 0;              // 深度掩码膨胀像素数
    bool depth_mask_invert = false;         // 是否反转深度掩码
    bool capture_use_cuda = true;           // 是否使用 CUDA 进行捕获
#endif
};

// 在互斥锁保护下，从全局配置中快照读取当前捕获相关配置，返回一致的配置快照
CaptureThreadConfig SnapshotCaptureConfig()
{
    std::lock_guard<std::mutex> cfgLock(configMutex);
    CaptureThreadConfig snapshot;
    snapshot.capture_method = config.capture_method;
    snapshot.capture_fps = config.capture_fps;
    snapshot.detection_resolution = config.detection_resolution;
    snapshot.monitor_idx = config.monitor_idx;
    snapshot.circle_fov_enabled = config.circle_fov_enabled;
    snapshot.circle_fov_radius_percent = config.circle_fov_radius_percent;
    snapshot.capture_borders = config.capture_borders;
    snapshot.capture_cursor = config.capture_cursor;
    snapshot.capture_target = config.capture_target;
    snapshot.capture_window_title = config.capture_window_title;
    snapshot.virtual_camera_name = config.virtual_camera_name;
    snapshot.virtual_camera_width = config.virtual_camera_width;
    snapshot.virtual_camera_height = config.virtual_camera_height;
    snapshot.udp_ip = config.udp_ip;
    snapshot.udp_port = config.udp_port;
    snapshot.ndi_source_name = config.ndi_source_name;
    snapshot.ndi_source_width = config.ndi_source_width;
    snapshot.ndi_source_height = config.ndi_source_height;
    snapshot.backend = config.backend;
    snapshot.screenshot_button = config.screenshot_button;
    snapshot.screenshot_delay = config.screenshot_delay;
    snapshot.show_window = config.show_window;
    snapshot.verbose = config.verbose;
#ifdef USE_CUDA
    snapshot.depth_inference_enabled = config.depth_inference_enabled;
    snapshot.depth_mask_enabled = config.depth_mask_enabled;
    snapshot.depth_model_path = config.depth_model_path;
    snapshot.depth_mask_fps = config.depth_mask_fps;
    snapshot.depth_mask_near_percent = config.depth_mask_near_percent;
    snapshot.depth_mask_expand = config.depth_mask_expand;
    snapshot.depth_mask_invert = config.depth_mask_invert;
    snapshot.capture_use_cuda = config.capture_use_cuda;
#endif
    return snapshot;
}

#ifdef USE_CUDA
// CUDA 捕获诊断数据结构，记录 GPU 捕获路径的各类事件计数，用于日志输出和性能诊断
struct CudaCaptureDiagnostics
{
    uint64_t gpuAttempts = 0;               // GPU 捕获尝试总次数
    uint64_t gpuCaptured = 0;               // GPU 捕获成功次数
    uint64_t gpuTimeout = 0;                // GPU 捕获超时次数
    uint64_t gpuNotReady = 0;               // GPU 未就绪次数
    uint64_t gpuDeviceLost = 0;             // GPU 设备丢失次数
    uint64_t gpuAcquireFailed = 0;          // 获取帧失败次数
    uint64_t gpuMissingTexture = 0;         // 缺少纹理次数
    uint64_t gpuCudaMapFailed = 0;          // CUDA 资源映射失败次数
    uint64_t gpuCudaArrayFailed = 0;        // CUDA 数组获取失败次数
    uint64_t gpuCudaCopyFailed = 0;         // CUDA 拷贝失败次数
    uint64_t gpuNoPresent = 0;              // 无新 Present 事件次数
    uint64_t gpuAccumulatedFrames = 0;      // 累积帧数总计
    uint64_t gpuMissedFrames = 0;           // 丢帧总计
    uint64_t gpuPresentFrames = 0;          // 含 Present 事件的帧数
    uint64_t gpuMouseOnlyEvents = 0;        // 仅鼠标更新的事件数
    uint64_t gpuMetadataOnlyEvents = 0;     // 仅元数据更新的事件数
    uint64_t gpuCoalescedEvents = 0;        // 合并矩形事件数
    uint64_t gpuSubmitted = 0;              // 提交给 TRT 检测器的 GPU 帧数
    uint64_t gpuCpuCopies = 0;              // GPU->CPU 回拷次数
    uint64_t cpuFallbackAttempts = 0;       // CPU 回退尝试次数
    uint64_t cpuFallbackFrames = 0;         // CPU 回退成功帧数
    uint64_t cpuFallbackEmpty = 0;          // CPU 回退返回空帧次数
    uint64_t cpuPathFrames = 0;             // 直接走 CPU 路径的帧数
    uint64_t trtCpuSubmitted = 0;           // 提交给 TRT 检测器的 CPU 帧数
    bool lastPreferGpu = false;             // 上一次是否偏好 GPU 路径
    bool lastNeedCpuCopy = false;           // 上一次是否需要 CPU 拷贝
    std::chrono::steady_clock::time_point lastLog = std::chrono::steady_clock::now(); // 上次日志时间
};

// 统计 GPU 捕获状态，更新本地诊断计数器和对应的全局原子计数器
void CountGpuCaptureStatus(CudaCaptureDiagnostics& diag, GpuCaptureStatus status)
{
    switch (status)
    {
    case GpuCaptureStatus::Captured:
        diag.gpuCaptured++;
        captureGpuCapturedTotal.fetch_add(1, std::memory_order_relaxed);
        break;
    case GpuCaptureStatus::NotReady: diag.gpuNotReady++; break;
    case GpuCaptureStatus::Timeout:
        diag.gpuTimeout++;
        captureGpuTimeoutTotal.fetch_add(1, std::memory_order_relaxed);
        break;
    case GpuCaptureStatus::NoPresent: diag.gpuNoPresent++; break;
    case GpuCaptureStatus::DeviceLost: diag.gpuDeviceLost++; break;
    case GpuCaptureStatus::AcquireFailed: diag.gpuAcquireFailed++; break;
    case GpuCaptureStatus::MissingTexture: diag.gpuMissingTexture++; break;
    case GpuCaptureStatus::CudaMapFailed: diag.gpuCudaMapFailed++; break;
    case GpuCaptureStatus::CudaArrayFailed: diag.gpuCudaArrayFailed++; break;
    case GpuCaptureStatus::CudaCopyFailed: diag.gpuCudaCopyFailed++; break;
    }
}

// 条件性记录 CUDA 捕获诊断日志（每 2 秒最多输出一次，仅当 verbose 模式启用时）
void MaybeLogCudaCaptureDiagnostics(CudaCaptureDiagnostics& diag, const CaptureThreadConfig& cfg)
{
    if (!cfg.verbose)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (now - diag.lastLog < std::chrono::seconds(2))
        return;

    diag.lastLog = now;
    std::cout
        << "[CaptureDiag] backend=" << cfg.backend
        << " method=" << cfg.capture_method
        << " cap_fps=" << cfg.capture_fps
        << " use_cuda=" << (cfg.capture_use_cuda ? "true" : "false")
        << " show_window=" << (cfg.show_window ? "true" : "false")
        << " circle_fov=" << (cfg.circle_fov_enabled ? "true" : "false")
        << " circle_fov_radius=" << cfg.circle_fov_radius_percent
        << " depth_mask=" << ((cfg.depth_inference_enabled && cfg.depth_mask_enabled) ? "true" : "false")
        << " prefer_gpu=" << (diag.lastPreferGpu ? "true" : "false")
        << " need_cpu_copy=" << (diag.lastNeedCpuCopy ? "true" : "false")
        << " gpu_attempts=" << diag.gpuAttempts
        << " gpu_captured=" << diag.gpuCaptured
        << " gpu_timeout=" << diag.gpuTimeout
        << " gpu_no_present=" << diag.gpuNoPresent
        << " gpu_accumulated=" << diag.gpuAccumulatedFrames
        << " gpu_missed=" << diag.gpuMissedFrames
        << " gpu_present=" << diag.gpuPresentFrames
        << " gpu_mouse_only=" << diag.gpuMouseOnlyEvents
        << " gpu_metadata_only=" << diag.gpuMetadataOnlyEvents
        << " gpu_coalesced=" << diag.gpuCoalescedEvents
        << " gpu_not_ready=" << diag.gpuNotReady
        << " gpu_lost=" << diag.gpuDeviceLost
        << " gpu_acquire_failed=" << diag.gpuAcquireFailed
        << " gpu_missing_tex=" << diag.gpuMissingTexture
        << " cuda_map_failed=" << diag.gpuCudaMapFailed
        << " cuda_array_failed=" << diag.gpuCudaArrayFailed
        << " cuda_copy_failed=" << diag.gpuCudaCopyFailed
        << " trt_gpu_submitted=" << diag.gpuSubmitted
        << " gpu_cpu_copies=" << diag.gpuCpuCopies
        << " cpu_fallback_attempts=" << diag.cpuFallbackAttempts
        << " cpu_fallback_frames=" << diag.cpuFallbackFrames
        << " cpu_fallback_empty=" << diag.cpuFallbackEmpty
        << " cpu_path_frames=" << diag.cpuPathFrames
        << " trt_cpu_submitted=" << diag.trtCpuSubmitted
        << std::endl;
}
#endif

// 校验并规范化捕获方法名称。若传入方法名不合法，回退到 duplication_api
std::string NormalizeCaptureMethod(const std::string& method)
{
    if (method == "duplication_api" || method == "winrt" || method == "virtual_camera" || method == "udp_capture" || method == "ndi")
        return method;
    return "duplication_api";
}

// 判断当前配置是否为 WinRT 窗口目标捕获模式
bool IsWinrtWindowTarget(const CaptureThreadConfig& cfg)
{
    return NormalizeCaptureMethod(cfg.capture_method) == "winrt" && cfg.capture_target == "window";
}

// 判断 WinRT 窗口目标是否缺失（窗口不存在或标题为空）
bool IsWinrtWindowTargetMissing(const CaptureThreadConfig& cfg)
{
    if (!IsWinrtWindowTarget(cfg))
        return false;

    if (OtherTools::TrimAscii(cfg.capture_window_title).empty())
        return true;

    return FindCaptureWindowByTitle(cfg.capture_window_title) == nullptr;
}

// RAII 辅助类：启用/禁用 Windows 高精度定时器（1ms 精度），
// 确保 Sleep 和等待操作能达到毫秒级精度
class TimerResolutionGuard
{
public:
    // 启用 1ms 定时器分辨率
    void Enable()
    {
        if (!enabled_)
        {
            timeBeginPeriod(1);
            enabled_ = true;
        }
    }

    // 恢复默认定时器分辨率
    void Disable()
    {
        if (enabled_)
        {
            timeEndPeriod(1);
            enabled_ = false;
        }
    }

    // 析构时自动恢复定时器分辨率
    ~TimerResolutionGuard()
    {
        Disable();
    }

private:
    bool enabled_{ false };
};

// RAII 辅助类：管理 WinRT COM 公寓初始化/反初始化，
// 确保在 WinRT 捕获前后正确设置多线程公寓模式
class WinrtApartmentGuard
{
public:
    // 按需初始化或反初始化 WinRT 公寓
    void Ensure(bool required)
    {
        if (required && !initialized_)
        {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            initialized_ = true;
        }
        else if (!required && initialized_)
        {
            winrt::uninit_apartment();
            initialized_ = false;
        }
    }

    // 析构时自动反初始化
    ~WinrtApartmentGuard()
    {
        if (initialized_)
            winrt::uninit_apartment();
    }

private:
    bool initialized_{ false };
};

// 异步截图写入器。将截图保存操作放入后台线程执行，避免阻塞捕获主循环
class ScreenshotWriter
{
public:
    ScreenshotWriter()
    {
        writerThread_ = std::thread([this]() { Run(); });
    }

    ~ScreenshotWriter()
    {
        Stop();
    }

    // 将截图任务加入队列，由后台线程处理写入磁盘
    // filename: 输出文件名；frame: 要保存的图像帧
    void Enqueue(const std::string& filename, cv::Mat frame)
    {
        if (filename.empty() || frame.empty())
            return;

        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= maxPendingFrames_)
            queue_.pop();
        queue_.emplace(filename, std::move(frame));
        cv_.notify_one();
    }

private:
    void Stop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_one();

        if (writerThread_.joinable())
            writerThread_.join();
    }

    // 后台线程主循环：等待截图任务，将图像写入 screenshots 目录
    void Run()
    {
        while (true)
        {
            std::pair<std::string, cv::Mat> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this]() { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty())
                    break;

                job = std::move(queue_.front());
                queue_.pop();
            }

            try
            {
                const std::filesystem::path screenshotsDir("screenshots");
                std::error_code ec;
                std::filesystem::create_directories(screenshotsDir, ec);
                if (ec)
                {
                    std::cerr << "[Capture] Screenshot folder creation failed: " << ec.message() << std::endl;
                    continue;
                }

                const std::filesystem::path outputPath = screenshotsDir / job.first;
                cv::imwrite(outputPath.string(), job.second);
            }
            catch (const std::exception& e)
            {
                std::cerr << "[Capture] Screenshot save failed: " << e.what() << std::endl;
            }
            catch (...)
            {
                std::cerr << "[Capture] Screenshot save failed: unknown exception." << std::endl;
            }
        }
    }

private:
    // 最大等待帧数，防止内存无限增长
    static constexpr size_t maxPendingFrames_ = 8;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::pair<std::string, cv::Mat>> queue_;
    std::thread writerThread_;
    bool stop_{ false };
};
} // namespace

// 从帧队列中批量获取指定数量的帧（从队列尾部取连续帧），用于批处理推理
std::vector<cv::Mat> getBatchFromQueue(int batch_size)
{
    std::vector<cv::Mat> batch;
    std::lock_guard<std::mutex> lk(frameMutex);
    const size_t target_size = (batch_size > 0) ? static_cast<size_t>(batch_size) : 0;
    const size_t n = std::min(frameQueue.size(), target_size);

    for (size_t i = 0; i < n; ++i)
        batch.push_back(frameQueue[frameQueue.size() - n + i]);

    while (batch.size() < target_size && !batch.empty())
        batch.push_back(batch.back().clone());
    return batch;
}

// 捕获线程主函数。负责整个屏幕捕获的生命周期管理：
// 1. 按配置创建合适的捕获后端
// 2. 在主循环中持续捕获帧
// 3. 处理配置热变更（分辨率切换、后端切换等）
// 4. 支持 GPU 直接路径（CUDA）和 CPU 回退路径
// 5. 截图、FPS 统计、深度掩码等功能
void captureThread(int CAPTURE_WIDTH, int CAPTURE_HEIGHT)
{
    try
    {
        CaptureThreadConfig currentCfg = SnapshotCaptureConfig();
        if (currentCfg.verbose)
            std::cout << "[Capture] OpenCV version: " << CV_VERSION << std::endl;

        int captureWidth = std::max(1, CAPTURE_WIDTH);
        int captureHeight = std::max(1, CAPTURE_HEIGHT);
        if (currentCfg.detection_resolution > 0)
        {
            captureWidth = currentCfg.detection_resolution;
            captureHeight = currentCfg.detection_resolution;
        }

#ifdef USE_CUDA
        depth_anything::DepthAnythingTrt depthMaskFallbackModel;
        std::string depthMaskFallbackModelPath;
#endif

        WinrtApartmentGuard winrtApartment;
        // 根据配置创建捕获后端实例（工厂 lambda）
        auto createCapturer = [&](const CaptureThreadConfig& cfg, int width, int height) -> std::unique_ptr<IScreenCapture>
        {
            try
            {
                const std::string method = NormalizeCaptureMethod(cfg.capture_method);
                if (method != cfg.capture_method)
                    std::cout << "[Capture] Unknown capture method '" << cfg.capture_method << "'. Falling back to duplication_api." << std::endl;

                if (method == "duplication_api")
                {
                    if (cfg.verbose)
                        std::cout << "[Capture] Using Duplication API" << std::endl;
                    auto capture = std::make_unique<DuplicationAPIScreenCapture>(width, height, cfg.monitor_idx);
                    if (!capture->isInitialized())
                        return nullptr;
                    return capture;
                }

                if (method == "winrt")
                {
                    if (cfg.verbose)
                        std::cout << "[Capture] Using WinRT" << std::endl;

                    WinRTScreenCapture::Options options;
                    options.target = cfg.capture_target;
                    options.windowTitle = cfg.capture_window_title;
                    options.monitorIndex = cfg.monitor_idx;
                    options.captureBorders = cfg.capture_borders;
                    options.captureCursor = cfg.capture_cursor;

                    return std::make_unique<WinRTScreenCapture>(width, height, options);
                }

                if (method == "virtual_camera")
                {
                    if (cfg.verbose)
                        std::cout << "[Capture] Using virtual camera" << std::endl;
                    return std::make_unique<VirtualCameraCapture>(
                        cfg.virtual_camera_width,
                        cfg.virtual_camera_height,
                        cfg.virtual_camera_name,
                        cfg.capture_fps,
                        cfg.verbose
                    );
                }

                if (method == "ndi")
                {
                    if (cfg.verbose)
                        std::cout << "[Capture] Using NDI: " << cfg.ndi_source_name << std::endl;
                    return std::make_unique<NDICapture>(
                        width, height, cfg.ndi_source_name, cfg.capture_fps,
                        cfg.ndi_source_width, cfg.ndi_source_height);
                }

                if (cfg.verbose)
                    std::cout << "[Capture] Using UDP capture" << std::endl;
                auto capture = std::make_unique<UDPCapture>(width, height, cfg.udp_ip, cfg.udp_port);
                if (!capture->isInitialized())
                    return nullptr;
                return capture;
            }
            catch (const std::exception& e)
            {
                std::cerr << "[Capture] Initialization failed for method '" << cfg.capture_method
                    << "': " << e.what() << std::endl;
                return nullptr;
            }
        };

        // 从捕获后端获取源帧尺寸并更新全局屏幕尺寸变量
        auto publishCaptureSourceSize = [](const IScreenCapture* capture)
        {
            int sourceWidth = 0;
            int sourceHeight = 0;
            if (capture && capture->GetSourceDimensions(sourceWidth, sourceHeight) &&
                sourceWidth > 0 && sourceHeight > 0)
            {
                screenWidth.store(sourceWidth, std::memory_order_relaxed);
                screenHeight.store(sourceHeight, std::memory_order_relaxed);
            }
            else
            {
                screenWidth.store(0, std::memory_order_relaxed);
                screenHeight.store(0, std::memory_order_relaxed);
            }
        };

        // 将捕获对象内部的输入侧统计发布给 UI 和流水线追踪线程。
        // 捕获对象只由本线程持有，其他线程始终读取这一份值语义快照。
        auto publishCaptureSourceDiagnostics = [](const IScreenCapture* capture)
        {
            const CaptureSourceDiagnostics diagnostics = capture
                ? capture->GetSourceDiagnostics()
                : CaptureSourceDiagnostics{};
            std::lock_guard<std::mutex> lock(g_captureSourceDiagnosticsMutex);
            g_captureSourceDiagnostics = diagnostics;
        };

        std::string desiredCaptureMethod = NormalizeCaptureMethod(currentCfg.capture_method);
        winrtApartment.Ensure(desiredCaptureMethod == "winrt");

        std::unique_ptr<IScreenCapture> capturer = createCapturer(currentCfg, captureWidth, captureHeight);
        publishCaptureSourceSize(capturer.get());
        publishCaptureSourceDiagnostics(capturer.get());
        std::string activeCapturerMethod = capturer ? desiredCaptureMethod : std::string();
        auto lastCapturerCreateAttempt = std::chrono::steady_clock::now();
        bool waitingForWinrtWindowTarget = !capturer && IsWinrtWindowTargetMissing(currentCfg);

        // 清除所有缓存帧
        auto clearCaptureFrames = [&]()
        {
            std::lock_guard<std::mutex> lock(frameMutex);
            latestFrame.release();
            frameQueue.clear();
        };

        // 清除检测结果缓冲区
        auto clearDetections = [&]()
        {
            detectionBuffer.clear();
        };

        // 标记捕获不可用：清除帧和检测结果，通知等待线程
        auto markCaptureUnavailable = [&]()
        {
            clearCaptureFrames();
            clearDetections();
            frameCV.notify_one();
        };

        bool captureUnavailable = false;
        // 设置捕获不可用状态（幂等）
        auto setCaptureUnavailable = [&]()
        {
            if (captureUnavailable)
                return;
            captureUnavailable = true;
            markCaptureUnavailable();
        };
        // 设置捕获可用状态
        auto setCaptureAvailable = [&]()
        {
            captureUnavailable = false;
        };

        // 初始状态设为不可用，避免旧状态下的过时帧和检测被使用
        setCaptureUnavailable();

        TimerResolutionGuard timerResolution;
        // Always request 1 ms timer resolution for the lifetime of the capture thread.
        // The loop performs short sleeps or yields in the "no new frame" backoff paths
        // (when capture_fps == 0 / unlimited) to avoid 100% CPU spin while waiting for
        // Duplication API / WinRT to deliver a present. It also needs precise sleeps for
        // the frame limiter when a cap is active.
        // Without timeBeginPeriod(1), these round up to the default Windows timer
        // resolution (~15.6 ms), throttling the capture loop (and reported captureFps)
        // unless something else in the process (the overlay/GUI D3D presents + short
        // sleep loops) has already bumped the resolution as a side effect.
        timerResolution.Enable();

        // 当前帧间隔持续时间（以 steady_clock duration 表示），
        // 用于实现帧率限制。当 captureFps == 0 时无限制。
        std::optional<std::chrono::steady_clock::duration> frameDuration;
        // 根据 captureFps 设置更新帧间隔持续时间
        auto updateFrameDuration = [&](int captureFpsSetting)
        {
            if (captureFpsSetting > 0)
            {
                const auto frameMs = std::chrono::duration<double, std::milli>(1000.0 / captureFpsSetting);
                frameDuration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(frameMs);
            }
            else
            {
                frameDuration.reset();
                // Do NOT Disable() the timer resolution here. The 1 ms backoff sleeps
                // (see the four sites below that check !frameDuration.has_value()) and
                // the applyFrameLimiter pacing still require it when running unlimited.
                // The guard's destructor will call timeEndPeriod(1) on thread exit.
            }
        };
        updateFrameDuration(currentCfg.capture_fps);

        captureFpsStartTime = std::chrono::high_resolution_clock::now();

        // 帧率限制器：按配置的 FPS 进行睡眠节流，确保不超过目标帧率
        auto frameStartTime = std::chrono::steady_clock::now();
        auto applyFrameLimiter = [&]()
        {
            if (frameDuration.has_value())
            {
                const auto now = std::chrono::steady_clock::now();
                const auto elapsed = now - frameStartTime;
                if (elapsed < frameDuration.value())
                {
                    std::this_thread::sleep_for(frameDuration.value() - elapsed);
                }
            }
            frameStartTime = std::chrono::steady_clock::now();
        };

        ScreenshotWriter screenshotWriter;
#ifdef USE_CUDA
        CudaCaptureDiagnostics cudaDiag;
#endif
        auto lastSaveTime = std::chrono::steady_clock::now();
        auto lastSuccessfulFrameTime = std::chrono::steady_clock::now();
        // 帧过时超时：如果超过 500ms 没有获取到新帧，标记捕获不可用
        constexpr auto staleFrameTimeout = std::chrono::milliseconds(500);

        // === 主捕获循环 ===
        while (!shouldExit)
        {
            try
            {
                // 1. 快照当前配置
                currentCfg = SnapshotCaptureConfig();
                publishCaptureSourceDiagnostics(capturer.get());

            // 2. 检查是否需要重新初始化捕获后端
            if (capture_fps_changed.exchange(false))
            {
                updateFrameDuration(currentCfg.capture_fps);
            }

            // 检测分辨率是否变更
            const bool resolutionChanged =
                currentCfg.detection_resolution > 0 &&
                (currentCfg.detection_resolution != captureWidth ||
                 currentCfg.detection_resolution != captureHeight);

            // 需要重新初始化的条件：检测分辨率、捕获方法、光标/边框/窗口变更
            const bool needsReinit =
                detection_resolution_changed.exchange(false) ||
                resolutionChanged ||
                capture_method_changed.exchange(false) ||
                capture_cursor_changed.exchange(false) ||
                capture_borders_changed.exchange(false) ||
                capture_window_changed.exchange(false);

            // 3. 需要重新初始化捕获后端
            if (needsReinit)
            {
                // 标记捕获不可用，清除旧帧
                setCaptureUnavailable();
                waitingForWinrtWindowTarget = false;

                if (currentCfg.detection_resolution > 0)
                {
                    captureWidth = currentCfg.detection_resolution;
                    captureHeight = currentCfg.detection_resolution;
                }

                const std::string nextMethod = NormalizeCaptureMethod(currentCfg.capture_method);
                desiredCaptureMethod = nextMethod;
                const bool nextNeedsWinrt = (nextMethod == "winrt");

                // 先销毁旧后端，避免新旧捕获对象重叠。
                // WinRT 必须在公寓反初始化之前销毁。
                if (capturer)
                {
                    const bool activeWasWinrt = (activeCapturerMethod == "winrt");
                    capturer.reset();
                    publishCaptureSourceDiagnostics(nullptr);
                    activeCapturerMethod.clear();
                    if (activeWasWinrt && !nextNeedsWinrt)
                        winrtApartment.Ensure(false);
                }

                winrtApartment.Ensure(nextNeedsWinrt);

                // 虚拟摄像头需要预扫描可用设备
                if (nextMethod == "virtual_camera")
                    VirtualCameraCapture::GetAvailableVirtualCameras(true);

                // 创建新捕获后端
                capturer = createCapturer(currentCfg, captureWidth, captureHeight);
                publishCaptureSourceSize(capturer.get());
                publishCaptureSourceDiagnostics(capturer.get());
                if (capturer)
                    activeCapturerMethod = nextMethod;
                else
                {
                    activeCapturerMethod.clear();
                    waitingForWinrtWindowTarget = IsWinrtWindowTargetMissing(currentCfg);
                }

                lastCapturerCreateAttempt = std::chrono::steady_clock::now();
                if (currentCfg.verbose)
                    std::cout << "[Capture] Capture backend re-initialized." << std::endl;
            }

            // 4. 捕获后端不可用时的处理逻辑
            if (!capturer)
            {
                // 等待 WinRT 窗口目标出现
                if (waitingForWinrtWindowTarget && IsWinrtWindowTarget(currentCfg))
                {
                    setCaptureUnavailable();
                    if (!frameDuration.has_value())
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    applyFrameLimiter();
                    continue;
                }

                // 每 1 秒尝试重新创建捕获后端
                const auto now = std::chrono::steady_clock::now();
                if (now - lastCapturerCreateAttempt >= std::chrono::seconds(1))
                {
                    desiredCaptureMethod = NormalizeCaptureMethod(currentCfg.capture_method);
                    winrtApartment.Ensure(desiredCaptureMethod == "winrt");

                    if (desiredCaptureMethod == "virtual_camera")
                        VirtualCameraCapture::GetAvailableVirtualCameras(true);

                    capturer = createCapturer(currentCfg, captureWidth, captureHeight);
                    publishCaptureSourceSize(capturer.get());
                    publishCaptureSourceDiagnostics(capturer.get());
                    lastCapturerCreateAttempt = now;

                    if (capturer)
                    {
                        waitingForWinrtWindowTarget = false;
                        activeCapturerMethod = desiredCaptureMethod;
                        lastSuccessfulFrameTime = now;
                        if (currentCfg.verbose)
                            std::cout << "[Capture] Capture backend restored." << std::endl;
                    }
                    else
                    {
                        activeCapturerMethod.clear();
                        waitingForWinrtWindowTarget = IsWinrtWindowTargetMissing(currentCfg);
                    }
                }

                setCaptureUnavailable();
                if (!frameDuration.has_value())
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                applyFrameLimiter();
                continue;
            }

            // 5. 检查截图请求状态
            const bool screenshotEnabled =
                !currentCfg.screenshot_button.empty() && currentCfg.screenshot_button[0] != "None";
            const auto screenshotNow = std::chrono::steady_clock::now();
            const auto screenshotElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                screenshotNow - lastSaveTime
            ).count();
            const bool screenshotRequested =
                screenshotEnabled &&
                isAnyKeyPressed(currentCfg.screenshot_button) &&
                screenshotElapsedMs >= currentCfg.screenshot_delay;
#ifdef USE_CUDA
            // 需要从 GPU 拷贝到 CPU 的两种场景：截图或显示预览窗口
            const bool needCpuCopyFromGpu = screenshotRequested || currentCfg.show_window;
#endif

            cv::Mat screenshotCpu;
            cv::Mat detectionFrame;
            std::chrono::steady_clock::time_point frameTimestamp{};
            bool frameSubmittedToDetector = false;
            bool skipCpuFallbackThisFrame = false;
            bool keepCaptureAliveThisFrame = false;

#ifdef USE_CUDA
            static bool lastDepthInferenceEnabled = true;
            if (!currentCfg.depth_inference_enabled)
            {
                if (lastDepthInferenceEnabled)
                {
                    auto& depthMask = depth_anything::GetDepthMaskGenerator();
                    depthMask.reset();
                    depthMaskFallbackModel.reset();
                    depthMaskFallbackModelPath.clear();
                }
                UpdateDetectionSuppressionMask(cv::Mat());
                lastDepthInferenceEnabled = false;
            }
            else
            {
                lastDepthInferenceEnabled = true;
            }

            // 8. 判断深度掩码启用状态和 GPU 捕获路径偏好
            // GPU 路径条件：TRT 后端 + duplication_api + 启用 CUDA 捕获 + 未启用深度掩码
            const bool depthMaskEnabled = currentCfg.depth_inference_enabled && currentCfg.depth_mask_enabled;
            const bool preferGpuCapturePath =
                currentCfg.backend == "TRT" &&
                NormalizeCaptureMethod(currentCfg.capture_method) == "duplication_api" &&
                currentCfg.capture_use_cuda &&
                !depthMaskEnabled;

            cudaDiag.lastPreferGpu = preferGpuCapturePath;
            cudaDiag.lastNeedCpuCopy = needCpuCopyFromGpu;

            // 9. GPU 捕获路径：直接通过 CUDA 从 DXGI 获取 GPU 图像，避免 CPU 回读开销
            if (preferGpuCapturePath)
            {
                auto* duplicationCapture = dynamic_cast<DuplicationAPIScreenCapture*>(capturer.get());
                if (duplicationCapture)
                {
                    cv::cuda::GpuMat screenshotGpu;
                    GpuCaptureStatus gpuStatus = GpuCaptureStatus::NotReady;
                    uint32_t accumulatedFrames = 0;
                    DdaCaptureFrameInfo ddaFrameInfo;
                    // 统计 DDA 帧信息：区分 Present 事件、鼠标更新和元数据更新
                    auto countDdaFrameInfo = [&](const DdaCaptureFrameInfo& info)
                    {
                        if (info.hasLastPresentTime)
                        {
                            cudaDiag.gpuPresentFrames++;
                            captureGpuPresentFramesTotal.fetch_add(1, std::memory_order_relaxed);
                        }
                        else if (info.hasLastMouseUpdateTime)
                        {
                            cudaDiag.gpuMouseOnlyEvents++;
                            captureGpuMouseOnlyEventsTotal.fetch_add(1, std::memory_order_relaxed);
                        }
                        else
                        {
                            cudaDiag.gpuMetadataOnlyEvents++;
                            captureGpuMetadataOnlyEventsTotal.fetch_add(1, std::memory_order_relaxed);
                        }
                        if (info.rectsCoalesced)
                        {
                            cudaDiag.gpuCoalescedEvents++;
                            captureGpuCoalescedEventsTotal.fetch_add(1, std::memory_order_relaxed);
                        }
                    };

                    // 尝试从 GPU 获取帧
                    cudaDiag.gpuAttempts++;
                    captureGpuAttemptsTotal.fetch_add(1, std::memory_order_relaxed);
                    if (duplicationCapture->GetNextFrameGpu(screenshotGpu, &gpuStatus, &accumulatedFrames, &ddaFrameInfo))
                    {
                        // GPU 捕获成功：记录统计、提交给 TRT 检测器
                        CountGpuCaptureStatus(cudaDiag, gpuStatus);
                        const uint64_t accumulated = accumulatedFrames;
                        const uint64_t missed = accumulated > 0 ? accumulated - 1 : 0;
                        cudaDiag.gpuAccumulatedFrames += accumulated;
                        cudaDiag.gpuMissedFrames += missed;
                        captureGpuAccumulatedFramesTotal.fetch_add(accumulated, std::memory_order_relaxed);
                        captureGpuMissedFramesTotal.fetch_add(missed, std::memory_order_relaxed);
                        countDdaFrameInfo(ddaFrameInfo);
                        frameTimestamp = std::chrono::steady_clock::now();
                        trt_detector.processFrameGpu(screenshotGpu, frameTimestamp);
                        cudaDiag.gpuSubmitted++;
                        frameSubmittedToDetector = true;

                        // 需要时下载到 CPU（截图或预览）
                        if (needCpuCopyFromGpu)
                        {
                            screenshotGpu.download(screenshotCpu);
                            cudaDiag.gpuCpuCopies++;
                        }
                    }
                    else
                    {
                        // GPU 捕获失败：根据状态决定是否回退到 CPU 路径
                        CountGpuCaptureStatus(cudaDiag, gpuStatus);
                        if (gpuStatus == GpuCaptureStatus::NoPresent)
                        {
                            // 无新帧，保持捕获存活但不回退
                            countDdaFrameInfo(ddaFrameInfo);
                            skipCpuFallbackThisFrame = true;
                            keepCaptureAliveThisFrame = true;
                        }
                        else if (gpuStatus == GpuCaptureStatus::Timeout)
                            skipCpuFallbackThisFrame = true;
                    }
                }
                else
                {
                    // 当前捕获器不支持 GPU 路径
                    cudaDiag.gpuAttempts++;
                    CountGpuCaptureStatus(cudaDiag, GpuCaptureStatus::NotReady);
                }
            }
#endif

            // 10. CPU 捕获路径：GPU 路径未启用或失败时使用
            if (!frameSubmittedToDetector)
            {
                // GPU 返回 NoPresent/Timeout 时跳过 CPU 回退
                if (skipCpuFallbackThisFrame)
                {
                    const auto now = std::chrono::steady_clock::now();
                    if (keepCaptureAliveThisFrame)
                    {
                        lastSuccessfulFrameTime = now;
                        publishCaptureSourceSize(capturer.get());
                        setCaptureAvailable();
                    }
                    else if (now - lastSuccessfulFrameTime >= staleFrameTimeout)
                        setCaptureUnavailable();

                    if (!frameDuration.has_value())
                        std::this_thread::yield();
#ifdef USE_CUDA
                    MaybeLogCudaCaptureDiagnostics(cudaDiag, currentCfg);
#endif
                    applyFrameLimiter();
                    continue;
                }

#ifdef USE_CUDA
                // GPU 路径回退到 CPU 路径的统计
                const bool cpuFallbackFromGpu = cudaDiag.lastPreferGpu;
                if (cpuFallbackFromGpu)
                {
                    cudaDiag.cpuFallbackAttempts++;
                    captureCpuFallbackAttemptsTotal.fetch_add(1, std::memory_order_relaxed);
                }
#endif
                // 从捕获后端获取 CPU 帧
                screenshotCpu = capturer->GetNextFrameCpu();
                frameTimestamp = std::chrono::steady_clock::now();

                // CPU 帧为空时的处理
                if (screenshotCpu.empty())
                {
#ifdef USE_CUDA
                    if (cpuFallbackFromGpu)
                        cudaDiag.cpuFallbackEmpty++;
#endif
                    const auto now = std::chrono::steady_clock::now();
                    if (now - lastSuccessfulFrameTime >= staleFrameTimeout)
                        setCaptureUnavailable();

                    if (!frameDuration.has_value())
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
#ifdef USE_CUDA
                    MaybeLogCudaCaptureDiagnostics(cudaDiag, currentCfg);
#endif
                    applyFrameLimiter();
                    continue;
                }
#ifdef USE_CUDA
                if (cpuFallbackFromGpu)
                {
                    cudaDiag.cpuFallbackFrames++;
                    captureCpuFallbackFramesTotal.fetch_add(1, std::memory_order_relaxed);
                }
                else
                    cudaDiag.cpuPathFrames++;
#endif

                // 11. 虚拟摄像头特殊处理：裁剪并缩放到目标尺寸
                if (NormalizeCaptureMethod(currentCfg.capture_method) == "virtual_camera")
                {
                    const int targetW = std::max(1, captureWidth);
                    const int targetH = std::max(1, captureHeight);
                    const int roiW = std::min(targetW, screenshotCpu.cols);
                    const int roiH = std::min(targetH, screenshotCpu.rows);

                    if (roiW <= 0 || roiH <= 0)
                    {
#ifdef USE_CUDA
                        MaybeLogCudaCaptureDiagnostics(cudaDiag, currentCfg);
#endif
                        applyFrameLimiter();
                        continue;
                    }

                    const int x = std::max(0, (screenshotCpu.cols - roiW) / 2);
                    const int y = std::max(0, (screenshotCpu.rows - roiH) / 2);
                    cv::Mat centered = screenshotCpu(cv::Rect(x, y, roiW, roiH));

                    if (roiW != targetW || roiH != targetH)
                    {
                        cv::resize(centered, screenshotCpu, cv::Size(targetW, targetH), 0, 0, cv::INTER_LINEAR);
                    }
                    else
                    {
                        screenshotCpu = centered;
                    }
                }

                detectionFrame = screenshotCpu;
#ifdef USE_CUDA
                // 12. 深度掩码处理：通过深度估计模型生成抑制掩码，屏蔽远距离/不相关区域
                if (currentCfg.depth_inference_enabled && currentCfg.depth_mask_enabled)
                {
                    if (currentCfg.verbose)
                    {
                        static auto lastMaskLog = std::chrono::steady_clock::time_point::min();
                        auto now = std::chrono::steady_clock::now();
                        if (now - lastMaskLog > std::chrono::seconds(2))
                        {
                            std::cout << "[DepthMask] update frame " << screenshotCpu.cols << "x" << screenshotCpu.rows
                                      << " model=" << currentCfg.depth_model_path
                                      << " fps=" << currentCfg.depth_mask_fps
                                      << " near_pct=" << currentCfg.depth_mask_near_percent
                                      << " expand=" << currentCfg.depth_mask_expand
                                      << " invert=" << (currentCfg.depth_mask_invert ? "true" : "false")
                                      << std::endl;
                            lastMaskLog = now;
                        }
                    }

                    cv::Mat mask;
                    depth_anything::DepthMaskOptions maskOptions;
                    maskOptions.enabled = currentCfg.depth_mask_enabled;
                    maskOptions.fps = currentCfg.depth_mask_fps;
                    maskOptions.near_percent = currentCfg.depth_mask_near_percent;
                    maskOptions.expand = currentCfg.depth_mask_expand;
                    maskOptions.invert = currentCfg.depth_mask_invert;

                    auto& depthMask = depth_anything::GetDepthMaskGenerator();
                    depthMask.update(screenshotCpu, maskOptions, currentCfg.depth_model_path, gLogger);
                    mask = depthMask.getMask();

                    if (!mask.empty() && mask.size() != screenshotCpu.size())
                        mask.release();

                    if (mask.empty())
                    {
                        if (currentCfg.depth_model_path.empty())
                        {
                            if (depthMaskFallbackModel.ready())
                                depthMaskFallbackModel.reset();
                            depthMaskFallbackModelPath.clear();
                        }
                        else if (depthMaskFallbackModelPath != currentCfg.depth_model_path || !depthMaskFallbackModel.ready())
                        {
                            if (depthMaskFallbackModel.initialize(currentCfg.depth_model_path, gLogger))
                            {
                                depthMaskFallbackModelPath = currentCfg.depth_model_path;
                            }
                        }

                        if (depthMaskFallbackModel.ready())
                        {
                            cv::Mat depthLocal = depthMaskFallbackModel.predictDepth(screenshotCpu);
                            if (!depthLocal.empty())
                            {
                                const int nearPercent = std::clamp(currentCfg.depth_mask_near_percent, 1, 100);
                                const bool invertMask = currentCfg.depth_mask_invert;
                                mask = depth_anything::generateDepthMaskFallback(
                                    depthLocal, nearPercent, currentCfg.depth_mask_expand, invertMask);
                            }
                        }
                    }

                    UpdateDetectionSuppressionMask(mask);
                    if (!mask.empty() && mask.size() == screenshotCpu.size())
                    {
                        detectionFrame = screenshotCpu.clone();
                        detectionFrame.setTo(cv::Scalar(0, 0, 0), mask);
                    }
                }
                else
                {
                    UpdateDetectionSuppressionMask(cv::Mat());
                }
#endif

                // 13. 提交帧给检测器进行 AI 推理
#ifdef USE_CUDA
                if (currentCfg.backend == "TRT")
                {
                    trt_detector.processFrame(detectionFrame, screenshotCpu, frameTimestamp);
                    cudaDiag.trtCpuSubmitted++;
                }
#else
                if (dml_detector)
                {
                    dml_detector->processFrame(detectionFrame, screenshotCpu, frameTimestamp);
                }
#endif
            }

            // 14. 更新捕获状态和帧数据
            if (frameSubmittedToDetector || !screenshotCpu.empty())
            {
                lastSuccessfulFrameTime = std::chrono::steady_clock::now();
                publishCaptureSourceSize(capturer.get());
                setCaptureAvailable();
            }

            if (!screenshotCpu.empty())
            {
                std::lock_guard<std::mutex> lock(frameMutex);
                latestFrame = screenshotCpu;
                if (frameQueue.size() >= 1)
                    frameQueue.pop_front();
                frameQueue.push_back(latestFrame);
            }
            frameCV.notify_one();

            // 15. 处理截图请求（快捷键触发）
            if (screenshotRequested)
            {
                cv::Mat saveMat = screenshotCpu.clone();
                if (!saveMat.empty())
                {
                    auto epoch_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()
                    ).count();
                    std::string filename = std::to_string(epoch_time) + ".jpg";
                    screenshotWriter.Enqueue(filename, std::move(saveMat));
                    lastSaveTime = screenshotNow;
                }
            }

            // 16. FPS 统计（每秒更新一次）
            captureFrameCount++;
            auto currentTime = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsedTime = currentTime - captureFpsStartTime;
            if (elapsedTime.count() >= 1.0)
            {
                captureFps = static_cast<int>(captureFrameCount / elapsedTime.count());
                captureFrameCount = 0;
                captureFpsStartTime = currentTime;
            }

#ifdef USE_CUDA
                MaybeLogCudaCaptureDiagnostics(cudaDiag, currentCfg);
#endif
                applyFrameLimiter();
            }
            // 17. 异常处理：捕获循环中的异常导致后端重置
            catch (const std::exception& e)
            {
                std::cerr << "[Capture] Loop exception: " << e.what() << std::endl;
                capturer.reset();
                publishCaptureSourceSize(nullptr);
                publishCaptureSourceDiagnostics(nullptr);
                activeCapturerMethod.clear();
                winrtApartment.Ensure(false);
                setCaptureUnavailable();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            catch (...)
            {
                std::cerr << "[Capture] Loop exception: unknown." << std::endl;
                capturer.reset();
                publishCaptureSourceSize(nullptr);
                publishCaptureSourceDiagnostics(nullptr);
                activeCapturerMethod.clear();
                winrtApartment.Ensure(false);
                setCaptureUnavailable();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    }
    // 18. 顶层异常处理：未捕获的异常会重新抛出
    catch (const std::exception& e)
    {
        std::cerr << "[Capture] Unhandled exception: " << e.what() << std::endl;
        throw;
    }
}
