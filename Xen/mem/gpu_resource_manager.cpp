#include "gpu_resource_manager.h"

#include <iostream>

// 查询空闲GPU内存，分配CUDA缓冲区，并初始化为零
bool GPUResourceManager::reserveGPUMemory(size_t reservedMemoryMB)
{
    size_t totalMemory, freeMemory;
    cudaError_t err = cudaMemGetInfo(&freeMemory, &totalMemory);
    if (err != cudaSuccess)
    {
        std::cerr << "[GPU] cudaMemGetInfo failed: " << cudaGetErrorString(err) << std::endl;
        return false;
    }

    reservedSize = reservedMemoryMB * 1024 * 1024;

    if (freeMemory < reservedSize) {
        std::cerr << "[GPU] Not enough free memory. Requested " << reservedMemoryMB
                  << " MB, free " << (freeMemory / (1024 * 1024)) << " MB." << std::endl;
        return false;
    }

    err = cudaMalloc(&reservedBuffer, reservedSize);
    if (err != cudaSuccess)
    {
        std::cerr << "[GPU] cudaMalloc failed: " << cudaGetErrorString(err) << std::endl;
        reservedBuffer = nullptr;
        return false;
    }

    err = cudaMemset(reservedBuffer, 0, reservedSize);
    if (err != cudaSuccess)
    {
        std::cerr << "[GPU] cudaMemset failed: " << cudaGetErrorString(err) << std::endl;
        cudaFree(reservedBuffer);
        reservedBuffer = nullptr;
        return false;
    }

    return true;
}

// 设置GPU的L1缓存偏好
bool GPUResourceManager::setGPUExclusiveMode()
{
    cudaError_t err = cudaDeviceSetCacheConfig(cudaFuncCachePreferL1);
    if (err != cudaSuccess)
    {
        std::cerr << "[GPU] cudaDeviceSetCacheConfig failed: " << cudaGetErrorString(err) << std::endl;
        return false;
    }
    return true;
}
