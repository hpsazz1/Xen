#ifndef CAPTURE_H
#define CAPTURE_H

// 屏幕捕获模块主头文件
// 声明全局帧队列、捕获线程函数、捕获统计信息，以及 IScreenCapture 抽象接口

#include <opencv2/opencv.hpp>
#include <atomic>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <cstdint>

#include "runtime/frame_rate_counter.h"

/**
 * @brief 各采集后端统一发布的输入源诊断
 *
 * declaredFps 只表示设备或协议声明值；receiveFps 按真实到达事件计数。droppedFrames
 * 表示应用尚未消费便被较新帧替换的旧帧，或桌面复制接口报告的累积遗漏帧。
 */
struct CaptureSourceDiagnostics
{
    double declaredFps = 0.0;
    int receiveFps = 0;
    uint64_t receivedFrames = 0;
    uint64_t droppedFrames = 0;
    int encodedWidth = 0;
    int encodedHeight = 0;
};

// 捕获参数变化标志
extern std::atomic<bool> detection_resolution_changed;
extern std::atomic<bool> capture_method_changed;
extern std::atomic<bool> capture_cursor_changed;
extern std::atomic<bool> capture_borders_changed;
extern std::atomic<bool> capture_fps_changed;
extern std::atomic<bool> capture_window_changed;
// 帧队列（多捕获源共享）
extern std::deque<cv::Mat> frameQueue;

// 捕获线程入口函数
void captureThread(int CAPTURE_WIDTH, int CAPTURE_HEIGHT);
// 屏幕原始尺寸
extern std::atomic<int> screenWidth;
extern std::atomic<int> screenHeight;

// 捕获帧数统计
extern std::atomic<int> captureFrameCount;
extern std::atomic<int> captureFps;
extern std::chrono::time_point<std::chrono::high_resolution_clock> captureFpsStartTime;

// 获取当前活动采集后端的统一输入源诊断快照。
CaptureSourceDiagnostics GetCaptureSourceDiagnostics();

// WinRT 捕获性能统计
extern std::atomic<uint64_t> captureWinrtPollAttemptsTotal;
extern std::atomic<uint64_t> captureWinrtFramesDrainedTotal;
extern std::atomic<uint64_t> captureWinrtFramesReturnedTotal;
extern std::atomic<uint64_t> captureWinrtEmptyPollsTotal;
extern std::atomic<uint64_t> captureWinrtReadbackMicrosTotal;
extern std::atomic<uint64_t> captureWinrtMapMicrosTotal;
extern std::atomic<uint64_t> captureWinrtPixelCopyMicrosTotal;

// 最新帧（互斥保护）
extern cv::Mat latestFrame;

extern std::mutex frameMutex;
extern std::condition_variable frameCV;
extern std::atomic<bool> shouldExit;
extern std::atomic<bool> show_window_changed;

// 屏幕捕获抽象基类
class IScreenCapture
{
public:
    virtual ~IScreenCapture() {}
    // 获取下一帧（CPU 内存），由子类实现
    virtual cv::Mat GetNextFrameCpu() = 0;
    // 获取后端输入侧统计；与捕获处理 FPS、检测发布 FPS 分层展示。
    virtual CaptureSourceDiagnostics GetSourceDiagnostics() const
    {
        CaptureSourceDiagnostics diagnostics;
        diagnostics.declaredFps = sourceDeclaredFps_.load(std::memory_order_relaxed);
        diagnostics.receiveFps = sourceReceiveFps_.load(std::memory_order_relaxed);
        diagnostics.receivedFrames = sourceReceivedFrames_.load(std::memory_order_relaxed);
        diagnostics.droppedFrames = sourceDroppedFrames_.load(std::memory_order_relaxed);
        diagnostics.encodedWidth = sourceEncodedWidth_.load(std::memory_order_relaxed);
        diagnostics.encodedHeight = sourceEncodedHeight_.load(std::memory_order_relaxed);

        const int64_t lastFrameNs = sourceLastFrameNs_.load(std::memory_order_relaxed);
        const int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        constexpr int64_t staleAfterNs = 2'000'000'000LL;
        if (lastFrameNs <= 0 || nowNs - lastFrameNs > staleAfterNs)
            diagnostics.receiveFps = 0;
        return diagnostics;
    }
    // 获取源图像尺寸
    bool GetSourceDimensions(int& width, int& height) const
    {
        width = sourceWidth_;
        height = sourceHeight_;
        return width > 0 && height > 0;
    }

protected:
    // 设置源图像尺寸（供子类调用）
    void SetSourceDimensions(int width, int height)
    {
        sourceWidth_ = width;
        sourceHeight_ = height;
    }

    /** @brief 记录一个真实到达或成功取得的源帧。仅允许对应后端的单一生产线程调用。 */
    void RecordSourceFrame(double declaredFps, int encodedWidth, int encodedHeight)
    {
        const auto now = std::chrono::steady_clock::now();
        if (declaredFps > 0.0)
            sourceDeclaredFps_.store(declaredFps, std::memory_order_relaxed);
        if (encodedWidth > 0 && encodedHeight > 0)
        {
            sourceEncodedWidth_.store(encodedWidth, std::memory_order_relaxed);
            sourceEncodedHeight_.store(encodedHeight, std::memory_order_relaxed);
        }
        sourceReceivedFrames_.fetch_add(1, std::memory_order_relaxed);
        sourceReceiveFps_.store(sourceRateCounter_.addFrame(now), std::memory_order_relaxed);
        sourceLastFrameNs_.store(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count(),
            std::memory_order_relaxed);
    }

    /** @brief 累加应用淘汰或采集 API 明确报告的遗漏帧。 */
    void RecordSourceDroppedFrames(uint64_t count)
    {
        if (count > 0)
            sourceDroppedFrames_.fetch_add(count, std::memory_order_relaxed);
    }

private:
    int sourceWidth_ = 0;
    int sourceHeight_ = 0;
    FrameRateCounter sourceRateCounter_;
    std::atomic<double> sourceDeclaredFps_{ 0.0 };
    std::atomic<int> sourceReceiveFps_{ 0 };
    std::atomic<uint64_t> sourceReceivedFrames_{ 0 };
    std::atomic<uint64_t> sourceDroppedFrames_{ 0 };
    std::atomic<int> sourceEncodedWidth_{ 0 };
    std::atomic<int> sourceEncodedHeight_{ 0 };
    std::atomic<int64_t> sourceLastFrameNs_{ 0 };
};

#ifdef USE_CUDA
// 获取当前检测抑制遮罩（用于排除特定区域的检测结果）
cv::Mat getCurrentDetectionSuppressionMask();
// GPU 捕获统计计数器
extern std::atomic<uint64_t> captureGpuAttemptsTotal;
extern std::atomic<uint64_t> captureGpuCapturedTotal;
extern std::atomic<uint64_t> captureGpuTimeoutTotal;
extern std::atomic<uint64_t> captureGpuAccumulatedFramesTotal;
extern std::atomic<uint64_t> captureGpuMissedFramesTotal;
extern std::atomic<uint64_t> captureGpuPresentFramesTotal;
extern std::atomic<uint64_t> captureGpuMouseOnlyEventsTotal;
extern std::atomic<uint64_t> captureGpuMetadataOnlyEventsTotal;
extern std::atomic<uint64_t> captureGpuCoalescedEventsTotal;
extern std::atomic<uint64_t> captureCpuFallbackAttemptsTotal;
extern std::atomic<uint64_t> captureCpuFallbackFramesTotal;
#endif

#endif // CAPTURE_H
