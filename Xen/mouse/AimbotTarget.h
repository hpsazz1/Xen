#ifndef AIMBOTTARGET_H
#define AIMBOTTARGET_H

#include <opencv2/opencv.hpp>
#include <vector>
#include <chrono>
#include <string>

#include "estimation.h"

/**
 * @brief 瞄准目标数据结构
 *
 * 表示一个检测到的目标，包含位置、大小、类别 ID 和瞄准枢轴点。
 */
class AimbotTarget
{
public:
    AimbotTarget();
    int x, y, w, h;         ///< 目标边界框（左上角坐标和宽高）
    int classId;             ///< 目标类别 ID
    float confidence = 1.0f; ///< 当前目标观测置信度，范围0~1

    double pivotX;           ///< 瞄准枢轴点 X（目标上的瞄准参考点）
    double pivotY;           ///< 瞄准枢轴点 Y

    /**
     * @brief 构造函数
     * @param x 边界框左上角 X
     * @param y 边界框左上角 Y
     * @param w 边界框宽度
     * @param h 边界框高度
     * @param classId 类别 ID
     * @param pivotX 枢轴点 X（默认中心）
     * @param pivotY 枢轴点 Y（默认中心）
     */
    AimbotTarget(int x, int y, int w, int h, int classId, double pivotX = 0.0,
        double pivotY = 0.0, float confidence = 1.0f);
};

/**
 * @brief 从检测结果中排序并选择最佳瞄准目标
 * @param boxes 检测到的边界框列表
 * @param classes 对应的类别 ID 列表
 * @param screenWidth 屏幕宽度
 * @param screenHeight 屏幕高度
 * @param disableHeadshot 是否禁用爆头瞄准
 * @return 指向最佳目标的 unique_ptr，若无合适目标则返回 nullptr
 */
std::unique_ptr<AimbotTarget> sortTargets(
    const std::vector<cv::Rect>& boxes,
    const std::vector<int>& classes,
    int screenWidth,
    int screenHeight,
    bool disableHeadshot
);

/**
 * @brief 锁定目标信息结构
 *
 * 用于在帧之间跟踪已锁定的目标状态，包括跟踪 ID、观测状态和丢失帧数。
 */
struct LockedTargetInfo
{
    int trackId = -1;                      ///< 跟踪 ID
    bool observedThisFrame = false;        ///< 本帧是否观测到
    int missedFrames = 0;                 ///< 连续丢失的帧数
    AimbotTarget target;                   ///< 当前目标数据
};

/**
 * @brief 跟踪调试信息结构
 *
 * 用于可视化调试每个跟踪轨道的状态。
 */
struct TrackDebugInfo
{
    int trackId = -1;                                          ///< 跟踪 ID
    int classId = -1;                                          ///< 类别 ID
    cv::Rect box;                                              ///< 边界框
    double pivotX = 0.0;                                       ///< 枢轴点 X
    double pivotY = 0.0;                                       ///< 枢轴点 Y
    double velocityX = 0.0;                                    ///< X 方向速度
    double velocityY = 0.0;                                    ///< Y 方向速度
    std::chrono::steady_clock::time_point lastUpdate{};        ///< 最后更新时间
    bool observedThisFrame = false;                            ///< 本帧是否被观测到
    int missedFrames = 0;                                      ///< 丢失帧数
    bool isLocked = false;                                     ///< 是否被锁定
};

/**
 * @brief 检测候选结构（跟踪器内部使用，头-身合并的中间表示）
 */
struct DetectionCandidate
{
    cv::Rect2f box;                              ///< 边界框
    int classId = -1;                             ///< 类别 ID
    float confidence = 1.0f;                     ///< 归一化观测置信度
    double pivotX = 0.0;                          ///< 枢轴点 X
    double pivotY = 0.0;                          ///< 枢轴点 Y
};

/**
 * @brief 头-身合并逻辑（共享函数）
 *
 * 当头部检测框落在对应身体检测框的容忍区域内时，认为两者属于同一实体。
 * 剔除冗余的头部候选，将瞄准支点迁移至身体候选上（使用头部支点位置）。
 *
 * @param dets          检测候选列表（原地修改）
 * @param classPlayer   身体类别 ID
 * @param classHead     头部类别 ID
 * @param headOffset    头部 Y 偏移比例
 * @param disableHeadshot 是否禁用爆头
 */
void mergeHeadToPlayer(
    std::vector<DetectionCandidate>& dets,
    int classPlayer, int classHead,
    float headOffset, bool disableHeadshot);

/**
 * @brief 基础观测锁定器：只关联当前检测，不做速度预测或丢失滑行。
 */
class BasicTargetTracker
{
public:
    void update(
        const std::vector<cv::Rect>& boxes,
        const std::vector<int>& classes,
        const std::vector<float>& confidences,
        int screenWidth,
        int screenHeight,
        bool disableHeadshot,
        std::chrono::steady_clock::time_point observationTime = {});

    void reset();
    bool getLockedTarget(LockedTargetInfo& out) const;
    int getLockedTrackId() const { return locked_ ? trackId_ : -1; }
    std::vector<TrackDebugInfo> getDebugTracks() const;

private:
    bool locked_ = false;
    int trackId_ = -1;
    int nextTrackId_ = 1;
    DetectionCandidate lockedCandidate_{};
    std::chrono::steady_clock::time_point lastUpdate_{};
};

/**
 * @brief 基于移动控制库的多目标跟踪器包装类
 *
 * 内部使用 estimation::MultiTargetTracker（匈牙利匹配 + 7状态卡尔曼）
 * 和 estimation::SingleTargetTracker（SOT 状态机：Locked→Coasting→Unlocked）。
 *
 * 特性：
 *   - 匈牙利全局最优匹配（而非贪心 IoU）
 *   - SOT 状态机支持遮挡滑行和重捕获评分
 *   - 7 状态 Kalman 滤波（vs 旧版简单 EMA 速度估计）
 *   - 多策略目标选择（最近/最高置信度/指定 ID）
 */
class MotionLibTargetTracker
{
public:
    MotionLibTargetTracker();

    /** @brief 从配置初始化跟踪器参数 */
    void configure(
        int confirmThreshold,
        int terminationFrames,
        float noiseVx, float noiseVy,
        float noiseW, float noiseH,
        float measurementStdDev,
        int coastFramesLimit,
        const std::string& selectionStrategy,
        float recaptureIoUThreshold,
        float recaptureDistanceMultiplier,
        float coastVelocityDecay);

    /** @brief 重置跟踪器状态 */
    void reset();

    /**
     * @brief 更新跟踪器状态（与旧 MultiTargetTracker 接口兼容）
     * @param boxes 当前帧的检测框列表
     * @param classes 每个检测框的类别ID
     * @param screenWidth 屏幕宽度
     * @param screenHeight 屏幕高度
     * @param disableHeadshot 是否禁用爆头模式
     * @param keepCurrentLock 是否保持当前锁定目标不变
     * @param observationTime 可选的时间戳
     */
    void update(
        const std::vector<cv::Rect>& boxes,
        const std::vector<int>& classes,
        const std::vector<float>& confidences,
        int screenWidth,
        int screenHeight,
        bool disableHeadshot,
        bool keepCurrentLock,
        std::chrono::steady_clock::time_point observationTime = {});

    /** @brief 获取当前锁定的目标信息，返回是否成功 */
    bool getLockedTarget(LockedTargetInfo& out) const;

    /** @brief 获取当前锁定目标的跟踪 ID */
    int getLockedTrackId() const { return lockedTrackId_; }

    /** @brief 获取所有跟踪轨道的调试信息 */
    std::vector<TrackDebugInfo> getDebugTracks() const;

    /** @brief 获取 SOT 估计的速度，返回 (vx, vy) 像素/帧。 */
    std::pair<float, float> getLockedVelocity() const;

    /** @brief 是否已初始化 */
    bool isConfigured() const { return configured_; }

private:
    // 内部移动控制库跟踪器实例
    estimation::MultiTargetTracker motTracker_;
    estimation::SingleTargetTracker sotTracker_;
    bool configured_ = false;

    // 缓存的锁定状态（预留：未来可用于 getLockedTarget 惰性缓存优化）
    int lockedTrackId_ = -1;
    mutable LockedTargetInfo cachedLockedTarget_;

    // 配置参数
    int confirmThreshold_ = 2;
    int terminationFrames_ = 8;
    float noiseVx_ = 1.0f, noiseVy_ = 1.0f;
    float noiseW_ = 0.01f, noiseH_ = 0.01f;
    float measurementStdDev_ = 5.0f;
    int coastFramesLimit_ = 15;
    float recaptureIoUThreshold_ = 0.3f;
    float recaptureDistanceMultiplier_ = 2.5f;
    float coastVelocityDecay_ = 1.0f;

    // 缓存的类别/偏移配置（避免 update() 每帧加锁 configMutex）
    int cachedClassPlayer_ = 0;
    int cachedClassHead_ = 1;
    float cachedBodyOffset_ = 0.15f;
    float cachedHeadOffset_ = 0.05f;

    // 帧时间跟踪
    std::chrono::steady_clock::time_point lastUpdateTime_{};
    double frameDtMeanSec_ = 1.0 / 60.0;

    /** @brief 将 cv::Rect 转换为 estimation::BoundingBox */
    static estimation::BoundingBox toBoundingBox(const cv::Rect& r);

    /** @brief 将 estimation::DetectedObject 转换为 AimbotTarget */
    static AimbotTarget toAimbotTarget(const estimation::DetectedObject& obj);
};

#endif // AIMBOTTARGET_H
