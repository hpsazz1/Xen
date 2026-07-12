#ifndef PIPELINE_TRACER_H
#define PIPELINE_TRACER_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

/**
 * @brief 单帧流水线追踪记录
 *
 * 记录瞄准流水线中目标坐标经过每个处理阶段的变化，
 * 从原始检测输出到最终鼠标计数，共 8 个阶段。
 * 所有坐标均为检测分辨率像素空间（如 320×320）。
 */
struct PipelineFrame
{
    // ========== 帧标识 ==========
    int64_t                                   frameId = 0;   ///< 单调递增帧序号
    std::chrono::steady_clock::time_point     timestamp{};    ///< 记录时间戳

    // ========== Stage 1: 原始目标（tracker/sorter 输出的 pivot 坐标） ==========
    double rawPivotX = 0.0;    ///< 原始 pivot X（检测分辨率像素）
    double rawPivotY = 0.0;    ///< 原始 pivot Y
    int    targetClassId = -1; ///< 目标类别（0=body, 1=head, -1=未知）

    // ========== Stage 2: 运动补偿后 ==========
    double mcPivotX = 0.0;     ///< 减去自瞄自身旋转偏移后的 X
    double mcPivotY = 0.0;     ///< 减去自瞄自身旋转偏移后的 Y
    double mcDeltaX = 0.0;     ///< 运动补偿量 X
    double mcDeltaY = 0.0;     ///< 运动补偿量 Y

    // ========== Stage 3: 预测后（EMA 滤波 + 常速外推） ==========
    double predX = 0.0;        ///< 预测后 X
    double predY = 0.0;        ///< 预测后 Y
    double velocityX = 0.0;    ///< 估计速度 X（px/sec）
    double velocityY = 0.0;    ///< 估计速度 Y（px/sec）
    bool   hasExternalVel = false; ///< 是否使用 SOT Kalman 外部速度

    // ========== Stage 4: 速度曲线 ==========
    double distToTarget = 0.0;    ///< 目标到屏幕中心的距离（像素）
    double speedMultiplier = 0.0; ///< 三段式速度倍率

    // ========== Stage 5: Pure Pursuit 控制器输出（像素空间增量） ==========
    double ppDx = 0.0;         ///< Pure Pursuit 输出 Δx（像素）
    double ppDy = 0.0;         ///< Pure Pursuit 输出 Δy（像素）

    // ========== Stage 6: 像素 → 鼠标计数 ==========
    double countsX = 0.0;      ///< mouse counts X（含灵敏度/FOV/帧率修正）
    double countsY = 0.0;      ///< mouse counts Y

    // ========== Stage 7: EMA 输出平滑后（若未启用则与 Stage 6 相同） ==========
    double emaCountsX = 0.0;   ///< EMA 平滑后 counts X
    double emaCountsY = 0.0;   ///< EMA 平滑后 counts Y

    // ========== Stage 8: 最终输出（四舍五入取整） ==========
    int    finalMx = 0;        ///< 最终水平移动量（counts）
    int    finalMy = 0;        ///< 最终垂直移动量（counts）

    // ========== 元数据 ==========
    bool   targetDetected = false;     ///< 本帧是否检测到目标
    double observationAgeSec = 0.0;    ///< 检测延迟（秒）
    double fpsValue = 0.0;            ///< 当前帧率
    int    resolution = 0;            ///< 检测分辨率
};

/**
 * @brief 流水线追踪器
 *
 * 环形缓冲区存储最近 N 帧的流水线坐标数据，
 * 供覆盖层面板读取和可视化分析。
 *
 * 线程安全：写入在 MouseThread 主循环，读取在覆盖层渲染线程。
 * 关闭时（enabled==false）仅一次 atomic load，零开销。
 */
class PipelineTracer
{
public:
    PipelineTracer() = default;
    ~PipelineTracer() = default;

    // ========== 控制 ==========

    /** @brief 启用/禁用追踪 */
    void setEnabled(bool e) { enabled.store(e, std::memory_order_relaxed); }

    /** @brief 查询是否启用 */
    bool isEnabled() const { return enabled.load(std::memory_order_relaxed); }

    /** @brief 设置环形缓冲区最大帧数 */
    void setMaxFrames(size_t n);

    /** @brief 获取当前缓冲区最大帧数 */
    size_t getMaxFrames() const { return maxFrames; }

    // ========== 数据写入（MouseThread 调用） ==========

    /**
     * @brief 开始记录一帧
     * @param resolution 当前检测分辨率
     * @return 新帧的引用，调用者直接写入各字段
     */
    PipelineFrame& beginFrame(int resolution);

    // ========== 数据读取（覆盖层线程调用） ==========

    /** @brief 获取所有已记录帧的快照 */
    std::vector<PipelineFrame> getFrames() const;

    /** @brief 清空所有记录 */
    void clear();

    /** @brief 获取当前帧数 */
    size_t size() const;

    /** @brief 获取总帧序号 */
    int64_t getFrameCounter() const { return frameCounter.load(std::memory_order_relaxed); }

    // ========== 导出 ==========

    /** @brief 导出为 CSV 文件 */
    bool exportCSV(const std::string& path) const;

private:
    std::deque<PipelineFrame> ringBuffer;          ///< 环形缓冲区
    mutable std::mutex        mutex;               ///< 读写互斥锁
    size_t                    maxFrames = 300;     ///< 最大帧数
    std::atomic<int64_t>      frameCounter{ 0 };   ///< 全局帧序号
    std::atomic<bool>         enabled{ false };    ///< 是否启用
};

/** @brief 全局流水线追踪器单例 */
extern PipelineTracer g_pipelineTracer;

#endif // PIPELINE_TRACER_H
