#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <iostream>

#ifdef USE_CUDA
#include <cuda_d3d11_interop.h>
#include <cuda_runtime_api.h>
#endif

#include "duplication_api_capture.h"
#include "Xen.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// 安全释放 COM 接口指针并将其置空
template <typename T>
inline void SafeRelease(T** ppInterface)
{
    if (*ppInterface)
    {
        (*ppInterface)->Release();
        *ppInterface = nullptr;
    }
}

// 帧上下文结构体，存储单次 AcquireNextFrame 调用的结果信息
struct FrameContext
{
    ID3D11Texture2D* texture = nullptr;              // 获取到的桌面纹理
    bool hasAcquiredFrame = false;                    // 是否已调用 AcquireNextFrame
    uint32_t accumulatedFrames = 0;                   // 自从上次处理后累积的帧数
    bool hasLastPresentTime = false;                   // 是否存在上一次 Present 时间戳
    bool hasLastMouseUpdateTime = false;               // 是否存在上一次鼠标更新时间戳
    int64_t lastPresentTime = 0;                       // DXGI原始QPC值，不直接参与本机年龄计算
    int64_t lastMouseUpdateTime = 0;                   // DXGI原始鼠标更新时间
    bool pointerVisible = false;                       // 鼠标指针是否可见
    bool rectsCoalesced = false;                       // 脏矩形是否被合并
    uint32_t totalMetadataBufferSize = 0;              // 总元数据缓冲区大小
    uint32_t pointerShapeBufferSize = 0;               // 指针形状缓冲区大小
};

// 管理 DXGI Output Duplication API 的生命周期，封装 D3D 设备和复制接口的创建与销毁
class DDAManager
{
public:
    DDAManager()
        : m_device(nullptr)
        , m_context(nullptr)
        , m_duplication(nullptr)
        , m_output1(nullptr)
        , m_frameAcquired(false)
    {
        ZeroMemory(&m_duplDesc, sizeof(m_duplDesc));
    }

    ~DDAManager()
    {
        Release();
    }

    // 初始化 DXGI 工厂、适配器、输出和 DuplicateOutput 接口
    // monitorIndex: 目标显示器索引
    // outScreenWidth/outScreenHeight: 输出屏幕尺寸
    // outDevice/outContext: 输出 D3D11 设备和上下文指针
    HRESULT Initialize(
        int monitorIndex,
        int /*captureWidth*/,
        int /*captureHeight*/,
        int& outScreenWidth,
        int& outScreenHeight,
        ID3D11Device** outDevice,
        ID3D11DeviceContext** outContext)
    {
        HRESULT hr = S_OK;

        IDXGIFactory1* factory = nullptr;
        hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);
        if (FAILED(hr))
        {
            std::cerr << "[DDA] CreateDXGIFactory1 failed hr=" << std::hex << hr << std::endl;
            return hr;
        }

        IDXGIAdapter1* adapter = nullptr;
        IDXGIOutput* output = nullptr;
        const int targetMonitorIndex = std::max(0, monitorIndex);

        int currentMonitorIndex = 0;
        bool foundOutput = false;
        for (UINT adapterIdx = 0; ; ++adapterIdx)
        {
            IDXGIAdapter1* candidateAdapter = nullptr;
            hr = factory->EnumAdapters1(adapterIdx, &candidateAdapter);
            if (hr == DXGI_ERROR_NOT_FOUND)
                break;
            if (FAILED(hr))
            {
                std::cerr << "[DDA] EnumAdapters1 failed hr=" << std::hex << hr << std::endl;
                SafeRelease(&factory);
                return hr;
            }

            for (UINT outputIdx = 0; ; ++outputIdx)
            {
                IDXGIOutput* candidateOutput = nullptr;
                hr = candidateAdapter->EnumOutputs(outputIdx, &candidateOutput);
                if (hr == DXGI_ERROR_NOT_FOUND)
                    break;
                if (FAILED(hr))
                {
                    std::cerr << "[DDA] EnumOutputs failed hr=" << std::hex << hr << std::endl;
                    SafeRelease(&candidateAdapter);
                    SafeRelease(&factory);
                    return hr;
                }

                if (currentMonitorIndex == targetMonitorIndex)
                {
                    adapter = candidateAdapter;
                    output = candidateOutput;
                    foundOutput = true;
                    break;
                }

                ++currentMonitorIndex;
                candidateOutput->Release();
            }

            if (foundOutput)
                break;

            candidateAdapter->Release();
        }

        if (!foundOutput || !adapter || !output)
        {
            std::cerr << "[DDA] No monitor with index " << targetMonitorIndex << std::endl;
            SafeRelease(&adapter);
            SafeRelease(&output);
            SafeRelease(&factory);
            return DXGI_ERROR_NOT_FOUND;
        }

        {
            D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
            UINT createDeviceFlags = 0;

            hr = D3D11CreateDevice(
                adapter,
                D3D_DRIVER_TYPE_UNKNOWN,
                nullptr,
                createDeviceFlags,
                featureLevels,
                1,
                D3D11_SDK_VERSION,
                &m_device,
                nullptr,
                &m_context
            );
            if (FAILED(hr))
            {
                std::cerr << "[DDA] D3D11CreateDevice failed hr=" << std::hex << hr << std::endl;
                SafeRelease(&output);
                SafeRelease(&adapter);
                SafeRelease(&factory);
                return hr;
            }

            IDXGIDevice1* dxgiDevice = nullptr;
            if (SUCCEEDED(m_device->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxgiDevice)))
            {
                dxgiDevice->SetMaximumFrameLatency(1);
                dxgiDevice->Release();
            }
        }

        hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&m_output1);
        if (FAILED(hr))
        {
            std::cerr << "[DDA] QueryInterface(IDXGIOutput1) failed hr=" << std::hex << hr << std::endl;
            SafeRelease(&m_context);
            SafeRelease(&m_device);
            SafeRelease(&output);
            SafeRelease(&adapter);
            SafeRelease(&factory);
            return hr;
        }

        hr = m_output1->DuplicateOutput(m_device, &m_duplication);
        if (FAILED(hr))
        {
            std::cerr << "[DDA] DuplicateOutput failed hr=" << std::hex << hr << std::endl;
            SafeRelease(&m_output1);
            SafeRelease(&m_context);
            SafeRelease(&m_device);
            SafeRelease(&output);
            SafeRelease(&adapter);
            SafeRelease(&factory);
            return hr;
        }

        m_duplication->GetDesc(&m_duplDesc);

        DXGI_OUTPUT_DESC oDesc{};
        output->GetDesc(&oDesc);
        outScreenWidth = oDesc.DesktopCoordinates.right - oDesc.DesktopCoordinates.left;
        outScreenHeight = oDesc.DesktopCoordinates.bottom - oDesc.DesktopCoordinates.top;

        SafeRelease(&output);
        SafeRelease(&adapter);
        SafeRelease(&factory);

        if (outDevice)  *outDevice = m_device;
        if (outContext) *outContext = m_context;

        return hr;
    }

    // 获取下一帧（CPU 回读路径）：
    // 1. 调用 AcquireNextFrame 获取桌面纹理
    // 2. 使用 CopySubresourceRegion 将指定区域拷贝到 staging 纹理
    // 3. 通过 Map/Unmap 将 GPU 纹理数据映射到 CPU 可读内存
    // 4. 以 cv::Mat (CV_8UC4) 格式返回帧数据
    // 返回空 Mat 表示无新帧或出错
    HRESULT AcquireFrame(FrameContext& frameCtx, UINT timeout = 100)
    {
        frameCtx.texture = nullptr;
        frameCtx.hasAcquiredFrame = false;
        if (!m_duplication) return E_FAIL;

        DXGI_OUTDUPL_FRAME_INFO frameInfo{};
        IDXGIResource* resource = nullptr;

        HRESULT hr = m_duplication->AcquireNextFrame(timeout, &frameInfo, &resource);
        if (FAILED(hr)) return hr;

        frameCtx.hasAcquiredFrame = true;
        frameCtx.accumulatedFrames = frameInfo.AccumulatedFrames;
        frameCtx.hasLastPresentTime = frameInfo.LastPresentTime.QuadPart != 0;
        frameCtx.hasLastMouseUpdateTime = frameInfo.LastMouseUpdateTime.QuadPart != 0;
        frameCtx.lastPresentTime = frameInfo.LastPresentTime.QuadPart;
        frameCtx.lastMouseUpdateTime = frameInfo.LastMouseUpdateTime.QuadPart;
        frameCtx.pointerVisible = frameInfo.PointerPosition.Visible != FALSE;
        frameCtx.rectsCoalesced = frameInfo.RectsCoalesced != FALSE;
        frameCtx.totalMetadataBufferSize = frameInfo.TotalMetadataBufferSize;
        frameCtx.pointerShapeBufferSize = frameInfo.PointerShapeBufferSize;
        m_frameAcquired = true;

        if (resource)
        {
            hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&frameCtx.texture);
            resource->Release();
        }
        return hr;
    }

    // 释放当前帧，允许 Duplication API 继续输出新帧
    void ReleaseFrame()
    {
        if (!m_duplication || !m_frameAcquired)
            return;

        m_duplication->ReleaseFrame();
        m_frameAcquired = false;
    }

    // 释放所有 D3D/DDM 资源
    void Release()
    {
        if (m_duplication)
        {
            ReleaseFrame();
            m_duplication->Release();
            m_duplication = nullptr;
        }
        SafeRelease(&m_output1);
        SafeRelease(&m_context);
        SafeRelease(&m_device);
    }

public:
    ID3D11Device* m_device;                 // D3D11 设备
    ID3D11DeviceContext* m_context;         // D3D11 设备上下文
    IDXGIOutputDuplication* m_duplication;  // DXGI 输出复制接口
    IDXGIOutput1* m_output1;                // DXGI 输出 (v1) 接口
    DXGI_OUTDUPL_DESC m_duplDesc;           // 输出复制描述
    bool m_frameAcquired;                   // 是否已获取帧未释放
};

// 构造函数：初始化 DDA 管理器，创建 staging 纹理和 CUDA 互操作纹理
DuplicationAPIScreenCapture::DuplicationAPIScreenCapture(int desiredWidth, int desiredHeight, int monitorIndex)
    : d3dDevice(nullptr)
    , d3dContext(nullptr)
    , deskDupl(nullptr)
    , output1(nullptr)
    , sharedTexture(nullptr)
    , stagingTextureCPU(nullptr)
    , screenWidth(0)
    , screenHeight(0)
    , regionWidth(desiredWidth)
    , regionHeight(desiredHeight)
{
    m_ddaManager = std::make_unique<DDAManager>();

    HRESULT hr = m_ddaManager->Initialize(
        monitorIndex,
        regionWidth,
        regionHeight,
        screenWidth,
        screenHeight,
        &d3dDevice,
        &d3dContext
    );
    if (FAILED(hr))
    {
        std::cerr << "[DDA] DDAManager Initialize failed hr=0x" << std::hex << hr << std::endl;
        return;
    }

    regionWidth = std::clamp(regionWidth, 1, std::max(1, screenWidth));
    regionHeight = std::clamp(regionHeight, 1, std::max(1, screenHeight));
    SetSourceDimensions(screenWidth, screenHeight);

    initialized_ = createStagingTextureCPU();
    if (!initialized_)
        return;
#ifdef USE_CUDA
    createCudaInteropTexture();
#endif
}

// 析构函数：释放 CUDA 互操作资源、DDA 管理器、staging 纹理和 D3D 资源
DuplicationAPIScreenCapture::~DuplicationAPIScreenCapture()
{
#ifdef USE_CUDA
    releaseCudaInteropTexture();
#endif
    if (m_ddaManager)
    {
        m_ddaManager->Release();
        m_ddaManager.reset();
    }
    SafeRelease(&stagingTextureCPU);
    SafeRelease(&sharedTexture);

    d3dDevice = nullptr;
    d3dContext = nullptr;
    deskDupl = nullptr;
    output1 = nullptr;
}

// 获取下一帧（CPU 回读路径）：
// 1. 调用 AcquireNextFrame 获取桌面纹理
// 2. 使用 CopySubresourceRegion 将指定区域拷贝到 staging 纹理
// 3. 通过 Map/Unmap 将 GPU 纹理数据映射到 CPU 可读内存
// 4. 以 cv::Mat (CV_8UC4) 格式返回帧数据
// 返回空 Mat 表示无新帧或出错
cv::Mat DuplicationAPIScreenCapture::GetNextFrameCpu()
{
    if (!m_ddaManager || !m_ddaManager->m_duplication || !stagingTextureCPU)
        return cv::Mat();

    FrameContext frameCtx;
    HRESULT hr = m_ddaManager->AcquireFrame(frameCtx, 0);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT)
    {
        return cv::Mat();
    }
    else if (hr == DXGI_ERROR_ACCESS_LOST ||
        hr == DXGI_ERROR_DEVICE_RESET ||
        hr == DXGI_ERROR_DEVICE_REMOVED ||
        hr == DXGI_ERROR_INVALID_CALL)
    {
        capture_method_changed.store(true);
        return cv::Mat();
    }
    else if (FAILED(hr))
    {
        std::cerr << "[DuplicationAPIScreenCapture] AcquireNextFrame (CPU) failed hr=0x"
            << std::hex << hr << std::endl;
        if (frameCtx.hasAcquiredFrame)
            m_ddaManager->ReleaseFrame();
        return cv::Mat();
    }

    if (!frameCtx.texture)
    {
        if (frameCtx.hasAcquiredFrame)
            m_ddaManager->ReleaseFrame();
        return cv::Mat();
    }

    const int copyWidth = std::min(regionWidth, std::max(1, screenWidth));
    const int copyHeight = std::min(regionHeight, std::max(1, screenHeight));
    const int left = std::max(0, (screenWidth - copyWidth) / 2);
    const int top = std::max(0, (screenHeight - copyHeight) / 2);

    D3D11_BOX sourceRegion;
    sourceRegion.left = left;
    sourceRegion.top = top;
    sourceRegion.front = 0;
    sourceRegion.right = sourceRegion.left + copyWidth;
    sourceRegion.bottom = sourceRegion.top + copyHeight;
    sourceRegion.back = 1;

    d3dContext->CopySubresourceRegion(
        stagingTextureCPU,
        0,
        0, 0, 0,
        frameCtx.texture,
        0,
        &sourceRegion
    );

    m_ddaManager->ReleaseFrame();
    frameCtx.texture->Release();

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hrMap = d3dContext->Map(stagingTextureCPU, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hrMap))
    {
        std::cerr << "[DDA] Map stagingTextureCPU failed hr=" << std::hex << hrMap << std::endl;
        if (hrMap == DXGI_ERROR_DEVICE_REMOVED || hrMap == DXGI_ERROR_DEVICE_RESET)
            capture_method_changed.store(true);
        return cv::Mat();
    }

    cv::Mat cpuFrame(regionHeight, regionWidth, CV_8UC4);
    for (int y = 0; y < regionHeight; y++)
    {
        unsigned char* dstRow = cpuFrame.ptr<unsigned char>(y);
        unsigned char* srcRow = (unsigned char*)mapped.pData + y * mapped.RowPitch;
        memcpy(dstRow, srcRow, regionWidth * 4);
    }

    d3dContext->Unmap(stagingTextureCPU, 0);
    RecordSourceFrame(
        0.0, screenWidth, screenHeight, FrameTiming::Clock::now(),
        frameCtx.lastPresentTime, frameCtx.hasLastPresentTime);
    if (frameCtx.accumulatedFrames > 1)
        RecordSourceDroppedFrames(static_cast<uint64_t>(frameCtx.accumulatedFrames - 1));
    return cpuFrame;
}

#ifdef USE_CUDA
// 设置 GPU 捕获状态枚举值（辅助函数）
static void SetGpuCaptureStatus(GpuCaptureStatus* status, GpuCaptureStatus value)
{
    if (status)
        *status = value;
}

// 将 DDA 帧上下文信息复制到 DdaCaptureFrameInfo 输出结构体
static void SetDdaCaptureFrameInfo(DdaCaptureFrameInfo* outInfo, const FrameContext& frameCtx)
{
    if (!outInfo)
        return;

    outInfo->accumulatedFrames = frameCtx.accumulatedFrames;
    outInfo->hasLastPresentTime = frameCtx.hasLastPresentTime;
    outInfo->hasLastMouseUpdateTime = frameCtx.hasLastMouseUpdateTime;
    outInfo->lastPresentTime = frameCtx.lastPresentTime;
    outInfo->lastMouseUpdateTime = frameCtx.lastMouseUpdateTime;
    outInfo->pointerVisible = frameCtx.pointerVisible;
    outInfo->rectsCoalesced = frameCtx.rectsCoalesced;
    outInfo->totalMetadataBufferSize = frameCtx.totalMetadataBufferSize;
    outInfo->pointerShapeBufferSize = frameCtx.pointerShapeBufferSize;
}

// 重置 DdaCaptureFrameInfo 结构体为默认值
static void ResetDdaCaptureFrameInfo(DdaCaptureFrameInfo* outInfo)
{
    if (outInfo)
        *outInfo = DdaCaptureFrameInfo{};
}

// 获取下一帧（GPU 路径）：
// 1. 调用 AcquireNextFrame 获取桌面纹理
// 2. 使用 CopySubresourceRegion 将指定区域拷贝到 CUDA 互操作纹理
// 3. 使用 cudaGraphicsMapResources / cudaGraphicsSubResourceGetMappedArray 将 D3D 纹理映射为 CUDA 可访问资源
// 4. 使用 cudaMemcpy2DFromArray 将数据拷贝到 GpuMat 中返回
// 5. 返回 true 表示成功，gpuFrameBgra 包含 BGRA 格式的 GPU 图像数据
// 6. status 输出捕获状态码
// 7. frameInfo 输出 DDA 帧元数据（累积帧数、Present 时间等）
bool DuplicationAPIScreenCapture::GetNextFrameGpu(
    cv::cuda::GpuMat& gpuFrameBgra,
    GpuCaptureStatus* status,
    uint32_t* accumulatedFrames,
    DdaCaptureFrameInfo* frameInfo)
{
    if (!m_ddaManager || !m_ddaManager->m_duplication || !interopTextureGPU || !cudaInteropResource || !cudaInteropReady)
    {
        SetGpuCaptureStatus(status, GpuCaptureStatus::NotReady);
        if (accumulatedFrames)
            *accumulatedFrames = 0;
        ResetDdaCaptureFrameInfo(frameInfo);
        return false;
    }

    FrameContext frameCtx;
    HRESULT hr = m_ddaManager->AcquireFrame(frameCtx, 0);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT)
    {
        SetGpuCaptureStatus(status, GpuCaptureStatus::Timeout);
        if (accumulatedFrames)
            *accumulatedFrames = 0;
        ResetDdaCaptureFrameInfo(frameInfo);
        return false;
    }
    else if (hr == DXGI_ERROR_ACCESS_LOST ||
        hr == DXGI_ERROR_DEVICE_RESET ||
        hr == DXGI_ERROR_DEVICE_REMOVED ||
        hr == DXGI_ERROR_INVALID_CALL)
    {
        capture_method_changed.store(true);
        SetGpuCaptureStatus(status, GpuCaptureStatus::DeviceLost);
        if (accumulatedFrames)
            *accumulatedFrames = 0;
        ResetDdaCaptureFrameInfo(frameInfo);
        return false;
    }
    else if (FAILED(hr))
    {
        std::cerr << "[DuplicationAPIScreenCapture] AcquireNextFrame (GPU) failed hr=0x"
            << std::hex << hr << std::endl;
        if (frameCtx.hasAcquiredFrame)
            m_ddaManager->ReleaseFrame();
        SetGpuCaptureStatus(status, GpuCaptureStatus::AcquireFailed);
        if (accumulatedFrames)
            *accumulatedFrames = 0;
        ResetDdaCaptureFrameInfo(frameInfo);
        return false;
    }

    if (!frameCtx.texture)
    {
        if (frameCtx.hasAcquiredFrame)
            m_ddaManager->ReleaseFrame();
        SetGpuCaptureStatus(status, GpuCaptureStatus::MissingTexture);
        if (accumulatedFrames)
            *accumulatedFrames = 0;
        SetDdaCaptureFrameInfo(frameInfo, frameCtx);
        return false;
    }

    const int copyWidth = std::min(regionWidth, std::max(1, screenWidth));
    const int copyHeight = std::min(regionHeight, std::max(1, screenHeight));
    const int left = std::max(0, (screenWidth - copyWidth) / 2);
    const int top = std::max(0, (screenHeight - copyHeight) / 2);

    D3D11_BOX sourceRegion;
    sourceRegion.left = left;
    sourceRegion.top = top;
    sourceRegion.front = 0;
    sourceRegion.right = sourceRegion.left + copyWidth;
    sourceRegion.bottom = sourceRegion.top + copyHeight;
    sourceRegion.back = 1;

    d3dContext->CopySubresourceRegion(
        interopTextureGPU,
        0,
        0, 0, 0,
        frameCtx.texture,
        0,
        &sourceRegion
    );

    m_ddaManager->ReleaseFrame();
    frameCtx.texture->Release();

    cudaError_t cuErr = cudaGraphicsMapResources(1, &cudaInteropResource, 0);
    if (cuErr != cudaSuccess)
    {
        std::cerr << "[DDA] cudaGraphicsMapResources failed: " << cudaGetErrorString(cuErr) << std::endl;
        cudaInteropReady = false;
        SetGpuCaptureStatus(status, GpuCaptureStatus::CudaMapFailed);
        if (accumulatedFrames)
            *accumulatedFrames = frameCtx.accumulatedFrames;
        SetDdaCaptureFrameInfo(frameInfo, frameCtx);
        return false;
    }

    cudaArray_t mappedArray = nullptr;
    cuErr = cudaGraphicsSubResourceGetMappedArray(&mappedArray, cudaInteropResource, 0, 0);
    if (cuErr != cudaSuccess)
    {
        std::cerr << "[DDA] cudaGraphicsSubResourceGetMappedArray failed: " << cudaGetErrorString(cuErr) << std::endl;
        cudaGraphicsUnmapResources(1, &cudaInteropResource, 0);
        cudaInteropReady = false;
        SetGpuCaptureStatus(status, GpuCaptureStatus::CudaArrayFailed);
        if (accumulatedFrames)
            *accumulatedFrames = frameCtx.accumulatedFrames;
        SetDdaCaptureFrameInfo(frameInfo, frameCtx);
        return false;
    }

    gpuFrameBgra.create(regionHeight, regionWidth, CV_8UC4);

    cuErr = cudaMemcpy2DFromArray(
        gpuFrameBgra.ptr<unsigned char>(),
        gpuFrameBgra.step,
        mappedArray,
        0, 0,
        static_cast<size_t>(regionWidth) * 4,
        static_cast<size_t>(regionHeight),
        cudaMemcpyDeviceToDevice
    );

    cudaError_t unmapErr = cudaGraphicsUnmapResources(1, &cudaInteropResource, 0);
    if (unmapErr != cudaSuccess)
    {
        std::cerr << "[DDA] cudaGraphicsUnmapResources failed: " << cudaGetErrorString(unmapErr) << std::endl;
        cudaInteropReady = false;
    }

    if (cuErr != cudaSuccess)
    {
        std::cerr << "[DDA] cudaMemcpy2DFromArray failed: " << cudaGetErrorString(cuErr) << std::endl;
        cudaInteropReady = false;
        SetGpuCaptureStatus(status, GpuCaptureStatus::CudaCopyFailed);
        if (accumulatedFrames)
            *accumulatedFrames = frameCtx.accumulatedFrames;
        SetDdaCaptureFrameInfo(frameInfo, frameCtx);
        return false;
    }

    SetGpuCaptureStatus(status, GpuCaptureStatus::Captured);
    RecordSourceFrame(
        0.0, screenWidth, screenHeight, FrameTiming::Clock::now(),
        frameCtx.lastPresentTime, frameCtx.hasLastPresentTime);
    if (frameCtx.accumulatedFrames > 1)
        RecordSourceDroppedFrames(static_cast<uint64_t>(frameCtx.accumulatedFrames - 1));
    if (accumulatedFrames)
        *accumulatedFrames = frameCtx.accumulatedFrames;
    SetDdaCaptureFrameInfo(frameInfo, frameCtx);
    return true;
}

// 创建与 D3D11 纹理共享的 CUDA 互操作纹理，实现 GPU 路径的零拷贝捕获
bool DuplicationAPIScreenCapture::createCudaInteropTexture()
{
    if (!d3dDevice)
        return false;

    releaseCudaInteropTexture();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = regionWidth;
    desc.Height = regionHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;

    HRESULT hr = d3dDevice->CreateTexture2D(&desc, nullptr, &interopTextureGPU);
    if (FAILED(hr))
    {
        std::cerr << "[DDA] CreateTexture2D(interop) failed hr=" << std::hex << hr << std::endl;
        return false;
    }

    cudaError_t cuErr = cudaGraphicsD3D11RegisterResource(
        &cudaInteropResource,
        interopTextureGPU,
        cudaGraphicsRegisterFlagsNone
    );

    if (cuErr != cudaSuccess)
    {
        std::cerr << "[DDA] cudaGraphicsD3D11RegisterResource failed: "
            << cudaGetErrorString(cuErr) << std::endl;
        SafeRelease(&interopTextureGPU);
        cudaInteropResource = nullptr;
        cudaInteropReady = false;
        return false;
    }

    cudaInteropReady = true;
    return true;
}

// 释放 CUDA 互操作纹理资源
void DuplicationAPIScreenCapture::releaseCudaInteropTexture()
{
    if (cudaInteropResource)
    {
        cudaError_t cuErr = cudaGraphicsUnregisterResource(cudaInteropResource);
        if (cuErr != cudaSuccess)
        {
            std::cerr << "[DDA] cudaGraphicsUnregisterResource failed: "
                << cudaGetErrorString(cuErr) << std::endl;
        }
        cudaInteropResource = nullptr;
    }

    SafeRelease(&interopTextureGPU);
    cudaInteropReady = false;
}
#endif

// 创建用于 CPU 回读的 staging 纹理（D3D11_USAGE_STAGING, CPU_ACCESS_READ），
// 用于将 GPU 桌面纹理拷贝到 CPU 可读内存
bool DuplicationAPIScreenCapture::createStagingTextureCPU()
{
    if (!d3dDevice) return false;

    SafeRelease(&stagingTextureCPU);

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
    desc.MiscFlags = 0;

    HRESULT hr = d3dDevice->CreateTexture2D(&desc, nullptr, &stagingTextureCPU);
    if (FAILED(hr))
    {
        std::cerr << "[DDA] CreateTexture2D(staging) failed hr=" << std::hex << hr << std::endl;
        return false;
    }
    return true;
}
