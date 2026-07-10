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

private:
    int sourceWidth_ = 0;
    int sourceHeight_ = 0;
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
