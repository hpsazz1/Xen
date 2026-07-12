#pragma once
#include <windows.h>

// CPU 核心亲和性与系统内存管理器
// 负责预留 CPU 核心（防止系统调度干扰）和预留物理内存
class CPUAffinityManager {
public:
    // 预留指定数量的 CPU 核心
    bool reserveCPUCores(int numCores);
    // 预留指定大小的系统内存（MB）
    bool reserveSystemMemory(size_t reservedMemoryMB);

private:
    DWORD_PTR originalMask;         // 保存原始 CPU 亲和性掩码
    // 注：亲和性和预留内存在进程生命周期内有意不恢复，以保证实时性能
    // DWORD_PTR 在 >64 逻辑处理器系统上有限制（单处理器组）
    void* reservedMemory = nullptr; // 预留内存的指针
};