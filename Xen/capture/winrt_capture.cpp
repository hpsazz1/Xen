#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>

#include "winrt_capture.h"
#include "Xen.h"
#include "other_tools.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

// 计算两个时间点之间的微秒差（辅助函数）
namespace
{
uint64_t ElapsedMicros(std::chrono::steady_clock::time_point start,
                       std::chrono::steady_clock::time_point end)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}
}

// 获取 WinRT GraphicsCaptureItem 互操作工厂（单例），用于创建窗口/显示器的捕获项
winrt::com_ptr<IGraphicsCaptureItemInterop> GetInteropFactory()
{
    static winrt::com_ptr<IGraphicsCaptureItemInterop> s_factory = [] {
        auto factory = winrt::get_activation_factory<
            GraphicsCaptureItem,
            IGraphicsCaptureItemInterop>();
        return factory.as<IGraphicsCaptureItemInterop>();
    }();
    return s_factory;
}

// 根据窗口标题子串查找目标窗口
HWND WinRTScreenCapture::FindWindowByTitleSubstring(const std::string& title_substr)
{
    return FindCaptureWindowByTitle(title_substr);
}

// 构造函数：初始化 D3D11 设备、WinRT 捕获项、帧池和捕获会话
// desiredWidth/desiredHeight: 期望的捕获区域尺寸
// options: 捕获选项（目标类型、窗口标题、显示器索引、边框/光标捕获等）
WinRTScreenCapture::WinRTScreenCapture(int desiredWidth, int desiredHeight, const Options& options)
    : desiredRegionWidth(desiredWidth)
    , desiredRegionHeight(desiredHeight)
    , regionWidth(desiredWidth)
    , regionHeight(desiredHeight)
{
    // 1. 创建 D3D11 设备和上下文
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
    UINT createDeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    winrt::check_hresult(
        D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            0,
            createDeviceFlags,
            featureLevels,
            ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION,
            d3dDevice.put(),
            nullptr,
            d3dContext.put()
        )
    );

    winrt::com_ptr<IDXGIDevice> dxgiDevice;
    winrt::check_hresult(d3dDevice->QueryInterface(IID_PPV_ARGS(dxgiDevice.put())));
    winrt::com_ptr<IDXGIDevice1> dxgiDevice1;
    if (SUCCEEDED(dxgiDevice->QueryInterface(IID_PPV_ARGS(dxgiDevice1.put()))))
    {
        dxgiDevice1->SetMaximumFrameLatency(1);
    }

    // 2. 创建 IDirect3DDevice（WinRT 封装）
    device = CreateDirect3DDevice(dxgiDevice.get());
    if (!device)
    {
        throw std::runtime_error("[WinRTCapture] Failed to create IDirect3DDevice.");
    }

    // 3. 根据目标类型（窗口/显示器）创建捕获项
    if (options.target == "window")
    {
        if (options.windowTitle.empty())
        {
            throw std::runtime_error("[WinRTCapture] capture_target=window but capture_window_title is empty.");
        }
        HWND hwnd = FindWindowByTitleSubstring(options.windowTitle);
        if (!hwnd)
        {
            throw std::runtime_error("[WinRTCapture] Target window not found by title substring: " + options.windowTitle);
        }
        captureItem = CreateCaptureItemForWindow(hwnd);
    }
    else
    {
        HMONITOR hMonitor = GetMonitorHandleByIndex(options.monitorIndex);
        if (!hMonitor)
        {
            throw std::runtime_error("[WinRTCapture] Invalid monitor index in config.");
        }
        captureItem = CreateCaptureItemForMonitor(hMonitor);
    }
    if (!captureItem)
    {
        throw std::runtime_error("[WinRTCapture] CreateCaptureItemForMonitor failed.");
    }

    screenWidth = captureItem.Size().Width;
    screenHeight = captureItem.Size().Height;
    SetSourceDimensions(screenWidth, screenHeight);
    desiredRegionWidth = std::max(1, desiredRegionWidth);
    desiredRegionHeight = std::max(1, desiredRegionHeight);
    regionWidth = std::clamp(desiredRegionWidth, 1, std::max(1, screenWidth));
    regionHeight = std::clamp(desiredRegionHeight, 1, std::max(1, screenHeight));

    regionX = (screenWidth - regionWidth) / 2;
    regionY = (screenHeight - regionHeight) / 2;

    // 4. 创建帧池和捕获会话
    framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
        device,
        DirectXPixelFormat::B8G8R8A8UIntNormalized,
        3,
        captureItem.Size()
    );

    session = framePool.CreateCaptureSession(captureItem);
    // 设置最小更新间隔为 0，以最快速度获取帧
    if (auto session5 = session.try_as<IGraphicsCaptureSession5>())
    {
        session5.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{ 0 });
    }

    // 根据配置设置边框和光标捕获
    if (!options.captureBorders)
    {
        session.IsBorderRequired(false);
    }

    if (!options.captureCursor)
    {
        session.IsCursorCaptureEnabled(false);
    }

    // 5. 创建 CPU staging 纹理
    if (!createStagingTextureCPU())
    {
        throw std::runtime_error("[WinRTCapture] createStagingTextureCPU() failed.");
    }

    // 6. 开始捕获
    session.StartCapture();
}

// 析构函数：关闭捕获会话和帧池，释放 D3D 资源
WinRTScreenCapture::~WinRTScreenCapture()
{
    if (session)
        session.Close();
    if (framePool)
        framePool.Close();

    stagingTextureCPU = nullptr;
    sharedTexture = nullptr;
    d3dContext = nullptr;
    d3dDevice = nullptr;
}

// 创建用于 CPU 回读的 staging 纹理
bool WinRTScreenCapture::createStagingTextureCPU()
{
    stagingTextureCPU = nullptr;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = regionWidth;
    desc.Height = regionHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    HRESULT hr = d3dDevice->CreateTexture2D(&desc, nullptr, stagingTextureCPU.put());
    if (FAILED(hr))
    {
        std::cerr << "[WinRTCapture] CreateTexture2D(staging) failed hr=" << std::hex << hr << std::endl;
        return false;
    }
    return true;
}

// 获取下一帧（CPU 回读路径）：
// 1. 从帧池尝试获取最新帧（TryGetNextFrame）
// 2. 如果内容尺寸变化，重新创建帧池和 staging 纹理
// 3. 从帧 Surface 获取 DXGI 纹理，拷贝指定区域到 staging 纹理
// 4. 通过 Map/Unmap 将像素数据拷贝到 cv::Mat 返回
// 5. 记录详细性能统计数据（轮询次数、空轮询、回读耗时等）
cv::Mat WinRTScreenCapture::GetNextFrameCpu()
{
    if (!framePool || !stagingTextureCPU)
        return cv::Mat();

    captureWinrtPollAttemptsTotal.fetch_add(1, std::memory_order_relaxed);

    Direct3D11CaptureFrame lastFrame = framePool.TryGetNextFrame();
    if (!lastFrame)
    {
        captureWinrtEmptyPollsTotal.fetch_add(1, std::memory_order_relaxed);
        return cv::Mat();
    }
    captureWinrtFramesDrainedTotal.fetch_add(1, std::memory_order_relaxed);

    const auto contentSize = lastFrame.ContentSize();
    if (contentSize.Width > 0 && contentSize.Height > 0 &&
        (contentSize.Width != screenWidth || contentSize.Height != screenHeight))
    {
        screenWidth = contentSize.Width;
        screenHeight = contentSize.Height;
        SetSourceDimensions(screenWidth, screenHeight);

        regionWidth = std::clamp(desiredRegionWidth, 1, std::max(1, screenWidth));
        regionHeight = std::clamp(desiredRegionHeight, 1, std::max(1, screenHeight));
        regionX = (screenWidth - regionWidth) / 2;
        regionY = (screenHeight - regionHeight) / 2;

        framePool.Recreate(
            device,
            DirectXPixelFormat::B8G8R8A8UIntNormalized,
            3,
            contentSize
        );

        if (!createStagingTextureCPU())
            return cv::Mat();
    }

    auto frameSurface = lastFrame.Surface();
    auto frameTexture = GetDXGIInterfaceFromObject<ID3D11Texture2D>(frameSurface);
    if (!frameTexture)
        return cv::Mat();

    D3D11_BOX sourceRegion;
    sourceRegion.left = regionX;
    sourceRegion.top = regionY;
    sourceRegion.front = 0;
    sourceRegion.right = regionX + regionWidth;
    sourceRegion.bottom = regionY + regionHeight;
    sourceRegion.back = 1;

    const auto readbackStart = std::chrono::steady_clock::now();
    d3dContext->CopySubresourceRegion(
        stagingTextureCPU.get(),
        0,
        0, 0, 0,
        frameTexture.get(),
        0,
        &sourceRegion
    );

    D3D11_MAPPED_SUBRESOURCE mapped;
    const auto mapStart = std::chrono::steady_clock::now();
    HRESULT hrMap = d3dContext->Map(stagingTextureCPU.get(), 0, D3D11_MAP_READ, 0, &mapped);
    const auto mapEnd = std::chrono::steady_clock::now();
    captureWinrtMapMicrosTotal.fetch_add(ElapsedMicros(mapStart, mapEnd), std::memory_order_relaxed);
    if (FAILED(hrMap))
    {
        std::cerr << "[WinRTCapture] Map stagingTextureCPU failed hr=" << std::hex << hrMap << std::endl;
        if (hrMap == DXGI_ERROR_DEVICE_REMOVED || hrMap == DXGI_ERROR_DEVICE_RESET)
            capture_method_changed.store(true);
        return cv::Mat();
    }

    cv::Mat cpuFrame(regionHeight, regionWidth, CV_8UC4);
    const auto pixelCopyStart = std::chrono::steady_clock::now();
    for (int y = 0; y < regionHeight; y++)
    {
        unsigned char* dstRow = cpuFrame.ptr<unsigned char>(y);
        unsigned char* srcRow = (unsigned char*)mapped.pData + y * mapped.RowPitch;
        memcpy(dstRow, srcRow, regionWidth * 4);
    }
    const auto pixelCopyEnd = std::chrono::steady_clock::now();
    d3dContext->Unmap(stagingTextureCPU.get(), 0);
    const auto readbackEnd = std::chrono::steady_clock::now();

    captureWinrtPixelCopyMicrosTotal.fetch_add(
        ElapsedMicros(pixelCopyStart, pixelCopyEnd),
        std::memory_order_relaxed);
    captureWinrtReadbackMicrosTotal.fetch_add(
        ElapsedMicros(readbackStart, readbackEnd),
        std::memory_order_relaxed);
    captureWinrtFramesReturnedTotal.fetch_add(1, std::memory_order_relaxed);

    RecordSourceFrame(0.0, screenWidth, screenHeight);
    return cpuFrame;
}

// 为指定显示器创建 GraphicsCaptureItem
winrt::Windows::Graphics::Capture::GraphicsCaptureItem
WinRTScreenCapture::CreateCaptureItemForMonitor(HMONITOR hMonitor)
{
    auto interopFactory = GetInteropFactory();
    GraphicsCaptureItem item{ nullptr };
    HRESULT hr = interopFactory->CreateForMonitor(
        hMonitor,
        winrt::guid_of<GraphicsCaptureItem>(),
        winrt::put_abi(item)
    );
    if (FAILED(hr))
    {
        throw std::runtime_error("[WinRTCapture] CreateForMonitor failed, HR=" + std::to_string(hr));
    }
    return item;
}

// 为指定窗口创建 GraphicsCaptureItem
winrt::Windows::Graphics::Capture::GraphicsCaptureItem
WinRTScreenCapture::CreateCaptureItemForWindow(HWND hWnd)
{
    auto interopFactory = GetInteropFactory();
    GraphicsCaptureItem item{ nullptr };
    HRESULT hr = interopFactory->CreateForWindow(
        hWnd,
        winrt::guid_of<GraphicsCaptureItem>(),
        winrt::put_abi(item)
    );
    if (FAILED(hr))
    {
        throw std::runtime_error("[WinRTCapture] CreateForWindow failed, HR=" + std::to_string(hr));
    }
    return item;
}

// 从 IDXGIDevice 创建 IDirect3DDevice（WinRT 封装）
winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice
WinRTScreenCapture::CreateDirect3DDevice(IDXGIDevice* dxgiDevice)
{
    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(
        CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice, inspectable.put())
    );
    return inspectable.as<IDirect3DDevice>();
}

// 从 WinRT IInspectable 对象中获取底层 DXGI 接口（模板方法）
template<typename T>
winrt::com_ptr<T> WinRTScreenCapture::GetDXGIInterfaceFromObject(
    winrt::Windows::Foundation::IInspectable const& object)
{
    auto dxgiInterfaceAccess = object.as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<T> result = nullptr;
    winrt::check_hresult(
        dxgiInterfaceAccess->GetInterface(winrt::guid_of<T>(), result.put_void())
    );
    return result;
}
