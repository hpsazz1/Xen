#ifndef DUPLICATION_API_CAPTURE_H
#define DUPLICATION_API_CAPTURE_H

// Duplication API 屏幕捕获实现
// 使用 Windows Desktop Duplication API 进行高性能屏幕采集，支持 CUDA 互操作

#include <d3d11.h>
#include <dxgi1_2.h>
#include <opencv2/opencv.hpp>
#include <memory>
#include <cstdint>

#ifdef USE_CUDA
#include <cuda_runtime_api.h>
#include <opencv2/core/cuda.hpp>
struct cudaGraphicsResource;

// DDA 捕获帧信息结构体（仅 CUDA 模式使用）
struct DdaCaptureFrameInfo
{
    uint32_t accumulatedFrames = 0;           // 累积帧数
    bool hasLastPresentTime = false;          // 是否有上次呈现时间
    bool hasLastMouseUpdateTime = false;      // 是否有上次鼠标更新时间
    bool pointerVisible = false;              // 鼠标指针是否可见
    bool rectsCoalesced = false;              // 脏矩形是否已合并
    uint32_t totalMetadataBufferSize = 0;     // 元数据缓冲区总大小
    uint32_t pointerShapeBufferSize = 0;      // 指针形状缓冲区大小
};

// GPU 捕获状态枚举
enum class GpuCaptureStatus
{
    Captured,       // 成功捕获
    NotReady,       // 未就绪
    Timeout,        // 超时
    NoPresent,      // 无呈现事件
    DeviceLost,     // 设备丢失
    AcquireFailed,  // 获取帧失败
    MissingTexture, // 纹理丢失
    CudaMapFailed,  // CUDA 映射失败
    CudaArrayFailed,// CUDA 数组失败
    CudaCopyFailed  // CUDA 拷贝失败
};
#endif

#include "capture.h"

class DDAManager;

// Desktop Duplication API 屏幕捕获类
class DuplicationAPIScreenCapture : public IScreenCapture
{
public:
    DuplicationAPIScreenCapture(int desiredWidth, int desiredHeight, int monitorIndex);
    ~DuplicationAPIScreenCapture();

    cv::Mat GetNextFrameCpu() override;
    // 是否已成功初始化
    bool isInitialized() const { return initialized_; }
#ifdef USE_CUDA
    // 获取 GPU 帧（CUDA GpuMat），避免 CPU 回读开销
    bool GetNextFrameGpu(
        cv::cuda::GpuMat& gpuFrameBgra,
        GpuCaptureStatus* status = nullptr,
        uint32_t* accumulatedFrames = nullptr,
        DdaCaptureFrameInfo* frameInfo = nullptr);
#endif

private:
    std::unique_ptr<DDAManager> m_ddaManager;

    ID3D11Device* d3dDevice = nullptr;
    ID3D11DeviceContext* d3dContext = nullptr;
    IDXGIOutputDuplication* deskDupl = nullptr;
    IDXGIOutput1* output1 = nullptr;

    ID3D11Texture2D* sharedTexture = nullptr;     // 共享纹理（GPU 端）

    ID3D11Texture2D* stagingTextureCPU = nullptr;  // CPU 回读暂存纹理
#ifdef USE_CUDA
    ID3D11Texture2D* interopTextureGPU = nullptr;  // CUDA 互操作纹理
    cudaGraphicsResource* cudaInteropResource = nullptr;
    bool cudaInteropReady = false;
#endif

    int screenWidth = 0;
    int screenHeight = 0;
    int regionWidth = 0;
    int regionHeight = 0;
    bool initialized_ = false;

    bool createStagingTextureCPU();
#ifdef USE_CUDA
    bool createCudaInteropTexture();
    void releaseCudaInteropTexture();
#endif
};

#endif // DUPLICATION_API_CAPTURE_H
