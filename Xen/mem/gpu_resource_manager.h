#pragma once

#ifdef USE_CUDA
#include <cuda_runtime.h>

// GPU 资源管理器
// 预留显存供 CUDA 使用，并可将 GPU 设置为独占模式以避免其他进程干扰
class GPUResourceManager {
public:
    // 预留指定大小的显存（MB）
    bool reserveGPUMemory(size_t reservedMemoryMB);
    // 将 GPU 设置为独占模式
    bool setGPUExclusiveMode();
    // 释放预留的显存
    void releaseReservation();

private:
    void* reservedBuffer = nullptr; // 预留显存的缓冲区指针
    size_t reservedSize = 0;        // 预留显存大小
};
#else
// 非 CUDA 编译时的空实现桩
class GPUResourceManager {
public:
    bool reserveGPUMemory(size_t) { return false; }
    bool setGPUExclusiveMode() { return false; }
    void releaseReservation() {}
};
#endif
