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
 * 从原始检测输出到最终鼠标计数，仅记录当前基础链路。
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

    // ========== Stage 2: 基础观测滤波（无未来预测） ==========
    double filteredX = 0.0;
    double filteredY = 0.0;
    double observedSpeed = 0.0; ///< 相邻原始观测速度，仅用于诊断
    double filterResidual = 0.0;

    // ========== Stage 3: 中心误差 ==========
    double errorX = 0.0;
    double errorY = 0.0;
    double errorDistance = 0.0;

    // ========== Stage 4: 基础控制器请求 ==========
    double requestedPixelX = 0.0;
    double requestedPixelY = 0.0;
    double requestedCountsX = 0.0;
    double requestedCountsY = 0.0;

    // ========== Stage 5: 请求输出（尚不代表驱动已实际发送） ==========
    int    finalMx = 0;        ///< 请求的水平移动量（counts）
    int    finalMy = 0;        ///< 请求的垂直移动量（counts）

    // ========== 控制器诊断 ==========
    double responseSeconds = 0.0;
    double maxCountsPerSecond = 0.0;
    double frameCountLimit = 0.0;
    bool   settled = false;
    size_t queuedMoveCount = 0;     ///< 请求入队后的待发送命令数

    // ========== 元数据 ==========
    bool   targetDetected = false;     ///< 本帧是否检测到目标
    double observationAgeSec = 0.0;    ///< 检测延迟（秒）
    double fpsValue = 0.0;            ///< 当前帧率
    int    inferenceFps = 0;           ///< 检测器实际结果发布帧率
    double sourceDeclaredFps = 0.0;    ///< 当前采集源/设备声明帧率；协议不提供时为 0
    int    sourceReceiveFps = 0;       ///< 当前采集后端真实取得或收到的输入帧率
    uint64_t sourceReceivedFrames = 0; ///< 当前采集会话累计取得或收到的源帧数
    uint64_t sourceDroppedFrames = 0;  ///< 累计被新帧替换或由采集 API 报告遗漏的源帧数
    double ndiDeclaredFps = 0.0;       ///< NDI 发送端帧头声明帧率
    int    ndiReceiveFps = 0;          ///< NDI 接收线程实际收到的帧率
    uint64_t ndiReceivedFrames = 0;    ///< 当前 NDI 会话累计收到的视频帧数
    uint64_t ndiDroppedFrames = 0;     ///< NDI 接收队列累计丢弃的旧帧数
    int    resolution = 0;            ///< 检测分辨率
    int    sourceWidth = 0;            ///< 捕获后端报告的完整源宽度，用于 FOV 换算审计
    int    sourceHeight = 0;           ///< 捕获后端报告的完整源高度，用于 FOV 换算审计
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
    size_t getMaxFrames() const;

    // ========== 数据写入（MouseThread 调用） ==========

    /**
     * @brief 开始记录一帧
     * @param resolution 当前检测分辨率
     * @return 可在调用线程安全填写的本地帧；填写完后调用 commitFrame。
     */
    PipelineFrame beginFrame(int resolution);

    /** @brief 提交一条完整的帧记录到环形缓冲区。 */
    void commitFrame(PipelineFrame frame);

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
