#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "ndi_capture.h"

#include <Processing.NDI.Lib.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <cstring>
#include <set>

#pragma comment(lib, "Processing.NDI.Lib.x64.lib")

// 构造函数：保存参数，初始化 NDI SDK 并启动接收线程
NDICapture::NDICapture(int width, int height, const std::string& sourceName, int frameRate)
    : width_(width)
    , height_(height)
    , frame_rate_(std::max(1, std::min(120, frameRate)))
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
    SetSourceDimensions(width_, height_);
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

    // 如果指定了源名称，尝试立即连接
    if (!source_name_.empty() && source_name_ != "None" && source_name_ != "Auto")
    {
        uint32_t numSources = 0;
        const NDIlib_source_t* sources = NDIlib_find_get_current_sources(ndi_find_, &numSources);

        for (uint32_t i = 0; i < numSources; ++i)
        {
            if (source_name_ == sources[i].p_ndi_name)
            {
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
                    connected_source_name_ = source_name_;
                    std::cout << "[NDI] Connected to source: " << source_name_ << std::endl;
                }
                break;
            }
        }

        if (!ndi_recv_)
        {
            std::cerr << "[NDI] Specified source '" << source_name_ << "' not found. Will retry in receive thread." << std::endl;
        }
    }

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
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (frame_queue_.empty())
        return cv::Mat();

    cv::Mat frame = frame_queue_.front();
    frame_queue_.pop();
    return frame;
}

// NDI 接收线程：持续发现和连接 NDI 源，接收视频帧并放入帧队列
void NDICapture::ReceiveThread()
{
    try
    {
        NDIlib_video_frame_v2_t videoFrame;
        int discoveryCounter = 0;

        while (!should_stop_)
        {
            // 如果没有接收器，尝试发现并连接 NDI 源
            if (!ndi_recv_)
            {
                // 每 30 次循环（约 3 秒）执行一次源发现
                if (discoveryCounter % 30 == 0)
                {
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
                    // 将 NDI 帧从 BGRA 转换为 BGR 格式
                    cv::Mat ndiFrame(videoFrame.yres, videoFrame.xres, CV_8UC4, videoFrame.p_data);

                    cv::Mat frame;
                    cv::cvtColor(ndiFrame, frame, cv::COLOR_BGRA2BGR);

                    // 按需调整帧尺寸
                    if (width_ > 0 && height_ > 0 &&
                        (videoFrame.xres != width_ || videoFrame.yres != height_))
                    {
                        cv::resize(frame, frame, cv::Size(width_, height_));
                    }

                    {
                        std::lock_guard<std::mutex> lock(frame_mutex_);
                        while (frame_queue_.size() >= MAX_QUEUE_SIZE)
                        {
                            frame_queue_.pop();
                            dropped_frames_++;
                        }
                        frame_queue_.push(frame);
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
                connected_source_name_.clear();
                discoveryCounter = 0;
            }
            // 超时或收到音频/元数据 -> 继续循环
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

// 静态方法：获取当前可用的所有 NDI 源名称列表
// 用于 UI 下拉菜单或自动选择
std::vector<std::string> NDICapture::GetAvailableSources()
{
    std::vector<std::string> sources;

    // 使用轻量级发现实例用于 UI 查询。
    // 无阻塞睡眠 - get_current_sources 立即返回。
    // 如果源尚未被发现，返回空列表，用户可点击刷新。
    NDIlib_find_instance_t pFind = nullptr;

    if (NDIlib_initialize())
    {
        NDIlib_find_create_t findDesc = { 0 };
        findDesc.show_local_sources = true;
        pFind = NDIlib_find_create_v2(&findDesc);
    }

    if (!pFind)
    {
        NDIlib_destroy();
        return sources;
    }

    // 非阻塞：返回当前已发现的所有源
    uint32_t numSources = 0;
    const NDIlib_source_t* pSources = NDIlib_find_get_current_sources(pFind, &numSources);

    sources.reserve(numSources);
    for (uint32_t i = 0; i < numSources; ++i)
    {
        if (pSources[i].p_ndi_name)
            sources.push_back(pSources[i].p_ndi_name);
    }

    NDIlib_find_destroy(pFind);
    NDIlib_destroy();

    return sources;
}
