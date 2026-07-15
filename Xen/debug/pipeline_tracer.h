#ifndef PIPELINE_TRACER_H
#define PIPELINE_TRACER_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "runtime/aim_pipeline_types.h"

/**
 * @brief 单帧流水线追踪记录
 *
 * 记录瞄准流水线中目标坐标经过每个处理阶段的变化，
 * 从原始检测输出到最终鼠标计数，同时记录P0并行影子链路快照。
 * 所有坐标均为检测分辨率像素空间（如 320×320）。
 */
struct PipelineFrame
{
    // ========== 帧标识 ==========
    int64_t                                   frameId = 0;   ///< 单调递增帧序号
    std::chrono::steady_clock::time_point     timestamp{};    ///< 记录时间戳
    FrameTiming                              frameTiming{};   ///< 从捕获到控制消费的同帧时间契约
    AimPipelineFrameState                    aimPipeline{};   ///< 与旧链路同帧记录的P0影子状态
    ViewCommandSample                        commandSample{}; ///< 请求入队至设备确认的命令生命周期

    // ========== Stage 1: 原始目标（tracker/sorter 输出的 pivot 坐标） ==========
    double rawPivotX = 0.0;    ///< 原始 pivot X（检测分辨率像素）
    double rawPivotY = 0.0;    ///< 原始 pivot Y
    double targetBoxX = 0.0;
    double targetBoxY = 0.0;
    double targetBoxWidth = 0.0;
    double targetBoxHeight = 0.0;
    int    targetClassId = -1; ///< 目标类别（0=body, 1=head, -1=未知）

    // ========== Stage 2: 基础观测滤波（无未来预测） ==========
    double filteredX = 0.0;
    double filteredY = 0.0;
    double observedVelocityX = 0.0; ///< 相邻原始观测水平速度，px/sec；正值向右
    double observedVelocityY = 0.0; ///< 相邻原始观测垂直速度，px/sec；正值向下
    double observedSpeed = 0.0; ///< 相邻原始观测速度，仅用于诊断
    double filterTrendSpeed = 0.0; ///< 有符号速度趋势模长，px/sec；控制移动滤波响应
    bool filterTrendActive = false; ///< 当前帧是否由趋势速度控制滤波响应
    double filterResidual = 0.0;

    // ========== Stage 3: 连续观测预测 ==========
    bool predictionApplied = false;
    bool predictionEnabled = false;
    double predictionAdditionalLeadMs = 0.0;
    double predictionVelocityTauMs = 0.0;
    double predictionStrength = 0.0;
    double predictionVelocityX = 0.0;
    double predictionVelocityY = 0.0;
    double predictionAccelerationX = 0.0;
    double predictionAccelerationY = 0.0;
    double predictionLeadMs = 0.0;
    double predictionOffsetX = 0.0;
    double predictionOffsetY = 0.0;
    double viewMotionX = 0.0;
    double viewMotionY = 0.0;
    bool predictionDirectionLocked = false;
    bool predictionSelfMotionSuppressed = false;
    bool predictionOscillationSuppressed = false; ///< 高频可靠换向门控是否撤销框外提前量
    bool predictionHighSpeedSuppressed = false; ///< 高速高加速度是否超出常速度可信范围
    double predictedX = 0.0;
    double predictedY = 0.0;

    // ========== Stage 4: 中心误差 ==========
    double errorX = 0.0;
    double errorY = 0.0;
    double errorDistance = 0.0;

    // ========== Stage 5: 基础控制器请求 ==========
    double requestedPixelX = 0.0;
    double requestedPixelY = 0.0;
    double requestedCountsX = 0.0;
    double requestedCountsY = 0.0;
    double integralCountsX = 0.0; ///< 本帧积分补偿计数；限速时按相同比例缩放
    double integralCountsY = 0.0;

    // ========== Stage 6: 请求输出（尚不代表驱动已实际发送） ==========
    int    finalMx = 0;        ///< 请求的水平移动量（counts）
    int    finalMy = 0;        ///< 请求的垂直移动量（counts）

    // ========== 被动Profile标定 ==========
    bool profileCalibrationEnabled = false; ///< 用户是否显式启用影子标定
    bool profileCalibrationValidX = false;  ///< X轴比例、方向与拟合质量是否满足有效条件
    bool profileCalibrationValidY = false;  ///< Y轴比例、方向与拟合质量是否满足有效条件
    double profileCalibrationPixelsPerCountX = 0.0;  ///< X轴实测画面响应，px/count
    double profileCalibrationPixelsPerCountY = 0.0;  ///< Y轴实测画面响应，px/count
    double profileCalibrationDegreesPerCountX = 0.0; ///< X轴按完整源FOV换算的deg/count
    double profileCalibrationDegreesPerCountY = 0.0; ///< Y轴按完整源FOV换算的deg/count
    double profileCalibrationDelayMsX = 0.0; ///< X轴设备发送到raw pivot响应的最佳对齐延迟，ms
    double profileCalibrationDelayMsY = 0.0; ///< Y轴设备发送到raw pivot响应的最佳对齐延迟，ms
    double profileCalibrationDriftX = 0.0;   ///< 拟合吸收的X轴恒定目标漂移，px/s
    double profileCalibrationDriftY = 0.0;   ///< 拟合吸收的Y轴恒定目标漂移，px/s
    double profileCalibrationRmseX = 0.0;    ///< X轴稳健回归残差均方根，px
    double profileCalibrationRmseY = 0.0;    ///< Y轴稳健回归残差均方根，px
    double profileCalibrationCorrelationX = 0.0; ///< X轴counts与去漂移画面位移相关系数
    double profileCalibrationCorrelationY = 0.0; ///< Y轴counts与去漂移画面位移相关系数
    double profileCalibrationConfidenceX = 0.0;  ///< X轴综合可信度，范围0~1
    double profileCalibrationConfidenceY = 0.0;  ///< Y轴综合可信度，范围0~1
    size_t profileCalibrationSamplesX = 0;       ///< X轴参与最佳延迟拟合的全部区间数
    size_t profileCalibrationSamplesY = 0;       ///< Y轴参与最佳延迟拟合的全部区间数
    size_t profileCalibrationActiveSamplesX = 0; ///< X轴区间counts绝对值至少0.5的有效激励数
    size_t profileCalibrationActiveSamplesY = 0; ///< Y轴区间counts绝对值至少0.5的有效激励数
    double profileCalibrationOverallConfidence = 0.0; ///< 有效轴可信度的综合结果，范围0~1

    // ========== 控制器诊断 ==========
    double responseSeconds = 0.0;
    double effectiveResponseSecondsX = 0.0; ///< 当前帧X轴实际比例响应时间
    double effectiveResponseSecondsY = 0.0; ///< 当前帧Y轴实际比例响应时间
    double integralTimeSeconds = 0.0;
    double maxCountsPerSecond = 0.0;
    double frameCountLimit = 0.0;
    double errorMotion = 0.0;          ///< 相邻有效控制观测的二维误差变化，px/observation
    double errorMotionX = 0.0;         ///< X轴相邻有效控制误差变化绝对值，px/observation
    double errorMotionY = 0.0;         ///< Y轴相邻有效控制误差变化绝对值，px/observation
    double settleMotionThreshold = 0.0;///< PI 稳定锁存快速释放阈值，px/observation
    bool   speedLimited = false;      ///< 本帧控制请求是否触发最大设备速率限制
    bool   settled = false;
    bool   settledX = false;          ///< X轴是否独立进入稳定停发
    bool   settledY = false;          ///< Y轴是否独立进入稳定停发
    bool   movingInsideSettle = false;///< 回差内误差变化是否阻止或释放稳定锁存
    bool   movingInsideSettleX = false;///< X轴回差内运动是否释放稳定锁存
    bool   movingInsideSettleY = false;///< Y轴回差内运动是否释放稳定锁存
    bool   horizontalCatchUp = false; ///< 水平大误差是否启用快速追赶响应
    bool   verticalCatchUp = false;   ///< 垂直大误差是否启用快速追赶响应
    size_t queuedMoveCount = 0;     ///< 请求入队后的待发送命令数

    // ========== 元数据 ==========
    bool   targetDetected = false;     ///< 本帧是否检测到目标
    double observationAgeSec = 0.0;    ///< 检测延迟（秒）
    double fpsValue = 0.0;            ///< 当前帧率
    int    inferenceFps = 0;           ///< 检测器实际结果发布帧率
    double dmlPreprocessMs = 0.0;      ///< DML CPU预处理耗时
    double dmlTensorSetupMs = 0.0;     ///< 输入形状与ORT张量包装耗时
    double dmlInferenceMs = 0.0;       ///< ONNX Runtime DirectML同步推理耗时
    double dmlCopyMs = 0.0;            ///< ORT返回到后处理开始之间的交接耗时
    double dmlPostprocessMs = 0.0;     ///< 输出解码、筛选和坐标缩放耗时
    double dmlNmsMs = 0.0;             ///< 后处理内NMS耗时，已包含在后处理耗时中
    double dmlTotalMs = 0.0;           ///< 预处理、推理、交接和后处理总耗时
    std::string dmlModel;               ///< DML会话实际加载的模型路径
    int dmlInputWidth = 0;              ///< DML模型实际输入宽度
    int dmlInputHeight = 0;             ///< DML模型实际输入高度
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

    /** @brief 设备线程按命令序号异步回填成功、失败或队列淘汰结果。 */
    void recordCommandResult(const ViewCommandSample& sample);
    void recordCommandDropped(uint64_t sequence);

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
    std::unordered_map<uint64_t, ViewCommandSample> pendingCommandResults; ///< 发送快于帧提交时的短期回填缓存
};

/** @brief 全局流水线追踪器单例 */
extern PipelineTracer g_pipelineTracer;

#endif // PIPELINE_TRACER_H
