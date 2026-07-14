#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "ndi_capture.h"
#include "ndi_frame_geometry.h"
#include "runtime/frame_rate_counter.h"
#include "runtime/latest_frame_queue.h"

#include <Processing.NDI.Lib.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <cstring>
#include <set>

#pragma comment(lib, "Processing.NDI.Lib.x64.lib")

std::atomic<double> NDICapture::global_declared_fps_{ 0.0 };
std::atomic<int> NDICapture::global_receive_fps_{ 0 };
std::atomic<uint64_t> NDICapture::global_received_frames_{ 0 };
std::atomic<uint64_t> NDICapture::global_dropped_frames_{ 0 };
std::atomic<int> NDICapture::global_encoded_width_{ 0 };
std::atomic<int> NDICapture::global_encoded_height_{ 0 };

// 构造函数：保存参数，初始化 NDI SDK 并启动接收线程
NDICapture::NDICapture(int width, int height, const std::string& sourceName, int frameRate,
                       int sourceWidth, int sourceHeight)
    : width_(width)
    , height_(height)
    , frame_rate_(std::max(1, std::min(120, frameRate)))
    , configured_source_width_(sourceWidth)
    , configured_source_height_(sourceHeight)
    , source_name_(sourceName)
    , ndi_find_(nullptr)
    , ndi_recv_(nullptr)
    , ndi_sdk_initialized_(false)
    , is_connected_(false)
    , initialized_(false)
    , should_stop_(false)
    , received_frames_(0)
    , dropped_frames_(0)
{
    // 新捕获会话从零开始统计，避免热重载后沿用上一个源的帧率和丢帧累计值。
    global_declared_fps_.store(0.0, std::memory_order_relaxed);
    global_receive_fps_.store(0, std::memory_order_relaxed);
    global_received_frames_.store(0, std::memory_order_relaxed);
    global_dropped_frames_.store(0, std::memory_order_relaxed);
    global_encoded_width_.store(0, std::memory_order_relaxed);
    global_encoded_height_.store(0, std::memory_order_relaxed);
    Initialize();
}

// 析构函数：停止接收线程并释放 NDI 资源
NDICapture::~NDICapture()
{
    Cleanup();
}

// 初始化 NDI SDK、创建发现实例，若指定源名称则立即尝试连接
bool NDICapture::Initialize()
{
    initialized_ = false;

    // 初始化 NDI SDK
    ndi_sdk_initialized_ = NDIlib_initialize();
    if (!ndi_sdk_initialized_)
    {
        std::cerr << "[NDI] Failed to initialize NDI SDK." << std::endl;
        return false;
    }

    // 创建 NDI 发现实例，用于在网络中查找 NDI 源
    NDIlib_find_create_t findDesc = { 0 };
    findDesc.show_local_sources = true;
    findDesc.p_groups = nullptr;
    findDesc.p_extra_ips = nullptr;

    ndi_find_ = NDIlib_find_create_v2(&findDesc);
    if (!ndi_find_)
    {
        std::cerr << "[NDI] Failed to create NDI find instance." << std::endl;
        NDIlib_destroy();
        ndi_sdk_initialized_ = false;
        return false;
    }

    // 源发现是异步的。立即读取新 finder 几乎必然为空，因此统一交给接收线程等待 mDNS 首轮结果，
    // 避免正常启动被误报为“指定源不存在”。
    should_stop_ = false;
    receive_thread_ = std::thread(&NDICapture::ReceiveThread, this);

    std::cout << "[NDI] Starting capture, source=" << source_name_ << std::endl;
    initialized_ = true;
    return true;
}

// 清理资源：停止线程、销毁 NDI 接收器、发现实例和 SDK
void NDICapture::Cleanup()
{
    should_stop_ = true;
    is_connected_ = false;
    initialized_ = false;

    if (receive_thread_.joinable())
        receive_thread_.join();

    if (ndi_recv_)
    {
        NDIlib_recv_destroy(ndi_recv_);
        ndi_recv_ = nullptr;
    }

    if (ndi_find_)
    {
        NDIlib_find_destroy(ndi_find_);
        ndi_find_ = nullptr;
    }

    if (ndi_sdk_initialized_)
    {
        NDIlib_destroy();
        ndi_sdk_initialized_ = false;
    }

    connected_source_name_.clear();
    global_declared_fps_.store(0.0, std::memory_order_relaxed);
    global_receive_fps_.store(0, std::memory_order_relaxed);
    global_received_frames_.store(0, std::memory_order_relaxed);
    global_dropped_frames_.store(0, std::memory_order_relaxed);
    global_encoded_width_.store(0, std::memory_order_relaxed);
    global_encoded_height_.store(0, std::memory_order_relaxed);
}

// 动态切换 NDI 源（在当前会话中）
void NDICapture::SetSource(const std::string& sourceName)
{
    source_name_ = sourceName;

    if (sourceName.empty() || sourceName == "None")
    {
        if (ndi_recv_)
        {
            NDIlib_recv_destroy(ndi_recv_);
            ndi_recv_ = nullptr;
        }
        is_connected_ = false;
        connected_source_name_.clear();
        return;
    }

    uint32_t numSources = 0;
    const NDIlib_source_t* sources = NDIlib_find_get_current_sources(ndi_find_, &numSources);

    for (uint32_t i = 0; i < numSources; ++i)
    {
        if (sourceName == sources[i].p_ndi_name)
        {
            if (ndi_recv_)
            {
                NDIlib_recv_destroy(ndi_recv_);
                ndi_recv_ = nullptr;
            }

            NDIlib_recv_create_v3_t recvDesc = { 0 };
            recvDesc.source_to_connect_to = sources[i];
            recvDesc.p_ndi_recv_name = "Xen NDI Receiver";
            recvDesc.color_format = NDIlib_recv_color_format_BGRX_BGRA;
            recvDesc.bandwidth = NDIlib_recv_bandwidth_highest;
            recvDesc.allow_video_fields = false;

            ndi_recv_ = NDIlib_recv_create_v3(&recvDesc);
            if (ndi_recv_)
            {
                is_connected_ = true;
                connected_source_name_ = sourceName;
                std::cout << "[NDI] Switched to source: " << sourceName << std::endl;
            }
            break;
        }
    }
}

// 获取下一帧：从帧队列取出最早的一帧并返回
cv::Mat NDICapture::GetNextFrameCpu()
{
    return GetNextFrameTimed().image;
}

CapturedFrame NDICapture::GetNextFrameTimed()
{
    std::lock_guard<std::mutex> lock(frame_mutex_);
    NetworkFrame latest;
    uint64_t skipped = 0;
    if (!TakeLatestFrame(frame_queue_, latest, skipped))
        return {};
    if (skipped > 0)
    {
        dropped_frames_.fetch_add(static_cast<int>(skipped), std::memory_order_relaxed);
        global_dropped_frames_.fetch_add(skipped, std::memory_order_relaxed);
        RecordSourceDroppedFrames(skipped);
    }
    SetSourceDimensions(latest.sourceWidth, latest.sourceHeight);
    return { std::move(latest.image), latest.timing };
}

// NDI 接收线程：持续发现和连接 NDI 源，接收视频帧并放入帧队列
void NDICapture::ReceiveThread()
{
    try
    {
        NDIlib_video_frame_v2_t videoFrame;
        int discoveryCounter = 0;
        FrameRateCounter receiveRate;
        receiveRate.reset();

        while (!should_stop_)
        {
            // 如果没有接收器，尝试发现并连接 NDI 源
            if (!ndi_recv_)
            {
                // 每 30 次循环（约 3 秒）执行一次源发现
                if (discoveryCounter % 30 == 0)
                {
                    // 首次发现和后续重试都允许 finder 等待短时间；该等待只发生在 NDI 后台线程。
                    NDIlib_find_wait_for_sources(ndi_find_, discoveryCounter == 0 ? 1000u : 100u);
                    uint32_t numSources = 0;
                    const NDIlib_source_t* sources = NDIlib_find_get_current_sources(ndi_find_, &numSources);

                    if (numSources > 0)
                    {
                        int sourceIndex = -1;
                        if (!source_name_.empty() && source_name_ != "None" && source_name_ != "Auto")
                        {
                            for (uint32_t i = 0; i < numSources; ++i)
                            {
                                if (source_name_ == sources[i].p_ndi_name)
                                {
                                    sourceIndex = static_cast<int>(i);
                                    break;
                                }
                            }
                        }
                        else if (source_name_ == "Auto" && numSources > 0)
                        {
                            sourceIndex = 0;
                        }

                        if (sourceIndex >= 0)
                        {
                            NDIlib_recv_create_v3_t recvDesc = { 0 };
                            recvDesc.source_to_connect_to = sources[sourceIndex];
                            recvDesc.p_ndi_recv_name = "Xen NDI Receiver";
                            recvDesc.color_format = NDIlib_recv_color_format_BGRX_BGRA;
                            recvDesc.bandwidth = NDIlib_recv_bandwidth_highest;
                            recvDesc.allow_video_fields = false;

                            ndi_recv_ = NDIlib_recv_create_v3(&recvDesc);
                            if (ndi_recv_)
                            {
                                connected_source_name_ = sources[sourceIndex].p_ndi_name;
                                is_connected_ = true;
                                std::cout << "[NDI] Connected to source: " << connected_source_name_ << std::endl;
                                receiveRate.reset();
                            }
                        }
                    }
                }
                discoveryCounter++;

                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            // 使用短超时（16ms ~60fps）接收视频帧。
            // 较长的超时会阻塞线程，影响叠加层请求帧的速度
            NDIlib_frame_type_e frameType = NDIlib_recv_capture_v2(ndi_recv_, &videoFrame, nullptr, nullptr, 16);

            if (frameType == NDIlib_frame_type_video)
            {
                if (videoFrame.xres > 0 && videoFrame.yres > 0)
                {
                    const double declaredFps = videoFrame.frame_rate_D > 0
                        ? static_cast<double>(videoFrame.frame_rate_N) /
                            static_cast<double>(videoFrame.frame_rate_D)
                        : 0.0;
                    global_declared_fps_.store(declaredFps, std::memory_order_relaxed);
                    global_encoded_width_.store(videoFrame.xres, std::memory_order_relaxed);
                    global_encoded_height_.store(videoFrame.yres, std::memory_order_relaxed);
                    global_received_frames_.fetch_add(1, std::memory_order_relaxed);
                    const auto receiveNow = std::chrono::steady_clock::now();
                    global_receive_fps_.store(
                        receiveRate.addFrame(receiveNow), std::memory_order_relaxed);
                    const uint64_t frameSequence = RecordSourceFrame(
                        declaredFps, videoFrame.xres, videoFrame.yres, receiveNow);

                    // 将 NDI 帧从 BGRA 转换为 BGR 格式
                    cv::Mat ndiFrame(videoFrame.yres, videoFrame.xres, CV_8UC4, videoFrame.p_data);

                    cv::Mat frame;
                    cv::cvtColor(ndiFrame, frame, cv::COLOR_BGRA2BGR);

                    // 网络完整帧与本地桌面捕获保持同一契约：从画面中心按 1:1 像素裁出检测区域。
                    // 禁止把 16:9 完整帧拉伸为正方形，否则会破坏 FOV、目标形状和鼠标坐标比例。
                    const int encodedWidth = frame.cols;
                    const int encodedHeight = frame.rows;
                    const std::string_view metadata = videoFrame.p_metadata
                        ? std::string_view(videoFrame.p_metadata)
                        : std::string_view();
                    const NdiFrameGeometry geometry = ResolveNdiFrameGeometry(
                        encodedWidth, encodedHeight, metadata,
                        configured_source_width_, configured_source_height_);
                    const int cropWidth = std::min(width_, encodedWidth);
                    const int cropHeight = std::min(height_, encodedHeight);
                    const int cropX = std::max(0, (encodedWidth - cropWidth) / 2);
                    const int cropY = std::max(0, (encodedHeight - cropHeight) / 2);
                    cv::Mat detectionFrame = frame(
                        cv::Rect(cropX, cropY, cropWidth, cropHeight)).clone();
                    if (cropWidth != width_ || cropHeight != height_)
                        cv::resize(detectionFrame, detectionFrame, cv::Size(width_, height_));

                    FrameTiming timing;
                    timing.backendReceiveTime = receiveNow;
                    timing.frameSequence = frameSequence;
                    timing.sourceWidth = geometry.sourceWidth;
                    timing.sourceHeight = geometry.sourceHeight;
                    timing.roiX = cropX;
                    timing.roiY = cropY;
                    timing.roiWidth = cropWidth;
                    timing.roiHeight = cropHeight;
                    timing.sourceTimestamp = videoFrame.timestamp;
                    timing.sourceTimecode = videoFrame.timecode;
                    timing.sourceTimestampAvailable = videoFrame.timestamp != 0;
                    timing.sourceTimecodeAvailable = videoFrame.timecode != 0;
                    timing.sourceTimestampMapped = false;

                    {
                        std::lock_guard<std::mutex> lock(frame_mutex_);
                        // 网络接收速度可能高于推理消费速度。这里只保留最新帧，避免排队增加观测延迟；
                        // 被替换的旧帧必须计入丢弃数，不能再显示为“0 丢帧”。
                        const uint64_t superseded = ReplaceWithLatestFrame(
                            frame_queue_, NetworkFrame{
                                std::move(detectionFrame), geometry.sourceWidth, geometry.sourceHeight, timing });
                        if (superseded > 0)
                        {
                            dropped_frames_.fetch_add(static_cast<int>(superseded), std::memory_order_relaxed);
                            global_dropped_frames_.fetch_add(superseded, std::memory_order_relaxed);
                            RecordSourceDroppedFrames(superseded);
                        }
                        received_frames_++;
                    }
                }

                NDIlib_recv_free_video_v2(ndi_recv_, &videoFrame);
            }
            else if (frameType == NDIlib_frame_type_error)
            {
                std::cerr << "[NDI] Connection error. Attempting to reconnect..." << std::endl;
                NDIlib_recv_destroy(ndi_recv_);
                ndi_recv_ = nullptr;
                is_connected_ = false;
                global_receive_fps_.store(0, std::memory_order_relaxed);
                receiveRate.reset();
                connected_source_name_.clear();
                discoveryCounter = 0;
            }
            else
            {
                // 超时或只收到音频/元数据时刷新过期判断，断流两秒后不再显示旧接收 FPS。
                global_receive_fps_.store(
                    receiveRate.value(std::chrono::steady_clock::now()),
                    std::memory_order_relaxed);
            }
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[NDI] Receive thread crashed: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[NDI] Receive thread crashed: unknown exception." << std::endl;
    }
}

NdiCaptureDiagnostics NDICapture::GetDiagnostics()
{
    NdiCaptureDiagnostics diagnostics;
    diagnostics.declaredFps = global_declared_fps_.load(std::memory_order_relaxed);
    diagnostics.receiveFps = global_receive_fps_.load(std::memory_order_relaxed);
    diagnostics.receivedFrames = global_received_frames_.load(std::memory_order_relaxed);
    diagnostics.droppedFrames = global_dropped_frames_.load(std::memory_order_relaxed);
    diagnostics.encodedWidth = global_encoded_width_.load(std::memory_order_relaxed);
    diagnostics.encodedHeight = global_encoded_height_.load(std::memory_order_relaxed);
    return diagnostics;
}

// 静态方法：获取当前可用的所有 NDI 源名称列表
// 用于 UI 下拉菜单或自动选择
std::vector<std::string> NDICapture::GetAvailableSources(uint32_t timeoutMs)
{
    std::vector<std::string> sources;

    // UI 使用独立发现实例，避免与捕获线程共享 finder 并触发 SDK 对同一 finder 的并发读取限制。
    // 新建 finder 的 mDNS 列表通常尚未就绪，必须等待首次变化后再读取；等待发生在 UI 异步任务中。
    NDIlib_find_instance_t pFind = nullptr;
    const bool sdkInitialized = NDIlib_initialize();
    if (sdkInitialized)
    {
        NDIlib_find_create_t findDesc = { 0 };
        findDesc.show_local_sources = true;
        pFind = NDIlib_find_create_v2(&findDesc);
    }

    if (!pFind)
    {
        if (sdkInitialized)
            NDIlib_destroy();
        return sources;
    }

    // 先读一次缓存；新建 finder 尚无结果时，最多等待 timeoutMs 让 mDNS/额外 IP 完成首轮发现。
    uint32_t numSources = 0;
    const NDIlib_source_t* pSources = NDIlib_find_get_current_sources(pFind, &numSources);
    if (numSources == 0 && timeoutMs > 0)
    {
        NDIlib_find_wait_for_sources(pFind, timeoutMs);
        pSources = NDIlib_find_get_current_sources(pFind, &numSources);
    }

    sources.reserve(numSources);
    for (uint32_t i = 0; i < numSources; ++i)
    {
        if (pSources[i].p_ndi_name)
            sources.push_back(pSources[i].p_ndi_name);
    }

    // 发现协议可能通过多个网卡返回重复源，UI 只展示稳定、排序后的唯一名称。
    std::sort(sources.begin(), sources.end());
    sources.erase(std::unique(sources.begin(), sources.end()), sources.end());

    NDIlib_find_destroy(pFind);
    NDIlib_destroy();

    return sources;
}
