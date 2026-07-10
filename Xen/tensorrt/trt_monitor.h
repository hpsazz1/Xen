#ifdef USE_CUDA
#pragma once
#include <atomic>
#include <string>
#include <mutex>
#include <map>
#include <chrono>

/**
 * @brief 进度阶段结构
 *
 * 表示 TensorRT 引擎构建过程中的一个阶段，
 * 包含阶段名称和当前/最大进度值。
 */
struct ProgressPhase {
    std::string name;    ///< 阶段名称
    int current = 0;     ///< 当前进度
    int max = 0;         ///< 总进度
};

inline std::mutex gProgressMutex;                              ///< 进度数据互斥锁
inline std::map<std::string, ProgressPhase> gProgressPhases;   ///< 各阶段进度映射表
inline std::atomic<bool> gIsTrtExporting = false;              ///< 是否正在导出 TensorRT 引擎
inline std::atomic<bool> gTrtExportCancelRequested = false;    ///< 是否已请求取消导出
inline std::atomic<long long> gTrtExportLastUpdateMs = 0;      ///< 上次更新时间的毫秒时间戳

/**
 * @brief 获取当前毫秒时间戳
 * @return 从 steady_clock epoch 开始的毫秒数
 */
inline long long TrtNowMs() noexcept
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

/** @brief 重置 TensorRT 导出状态 */
inline void TrtExportResetState() noexcept
{
    gTrtExportCancelRequested = false;
    gTrtExportLastUpdateMs = TrtNowMs();
}

/**
 * @brief ImGui 进度监视器类
 *
 * 继承自 nvinfer1::IProgressMonitor，用于接收 TensorRT
 * 引擎构建过程中的阶段和进度回调，并在 ImGui UI 中显示。
 */
class ImGuiProgressMonitor : public nvinfer1::IProgressMonitor {
public:
    /** @brief 阶段开始回调 */
    void phaseStart(const char* phaseName, const char* parentPhase, int32_t nbSteps) noexcept override {
        std::lock_guard<std::mutex> lock(gProgressMutex);
        ProgressPhase phase;
        phase.name = phaseName;
        phase.current = 0;
        phase.max = nbSteps;
        gProgressPhases[phaseName] = phase;
        gIsTrtExporting = true;
        gTrtExportLastUpdateMs = TrtNowMs();
    }

    /** @brief 步骤完成回调，返回 false 可取消导出 */
    bool stepComplete(const char* phaseName, int32_t step) noexcept override {
        std::lock_guard<std::mutex> lock(gProgressMutex);
        gProgressPhases[phaseName].current = step;
        gTrtExportLastUpdateMs = TrtNowMs();
        return !gTrtExportCancelRequested.load();
    }

    /** @brief 阶段结束回调 */
    void phaseFinish(const char* phaseName) noexcept override {
        std::lock_guard<std::mutex> lock(gProgressMutex);
        gProgressPhases.erase(phaseName);
        if (gProgressPhases.empty())
            gIsTrtExporting = false;
        gTrtExportLastUpdateMs = TrtNowMs();
    }
};
#endif
