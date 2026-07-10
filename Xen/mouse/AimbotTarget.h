#ifndef AIMBOTTARGET_H
#define AIMBOTTARGET_H

#include <opencv2/opencv.hpp>
#include <vector>
#include <chrono>

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
    AimbotTarget(int x, int y, int w, int h, int classId, double pivotX = 0.0, double pivotY = 0.0);
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
 * @brief 多目标跟踪器类
 *
 * 负责在帧之间关联检测到的目标，维持目标 ID 的一致性，
 * 并通过帧间匹配实现目标锁定和跟踪。
 */
class MultiTargetTracker
{
public:
    /** @brief 重置跟踪器状态 */
    void reset();
    /**
     * @brief 更新跟踪器状态
     * @param boxes 当前帧检测到的边界框
     * @param classes 对应的类别 ID
     * @param screenWidth 屏幕宽度
     * @param screenHeight 屏幕高度
     * @param disableHeadshot 是否禁用爆头
     * @param keepCurrentLock 是否保持当前锁定目标
     * @param observationTime 观测时间戳
     */
    void update(
        const std::vector<cv::Rect>& boxes,
        const std::vector<int>& classes,
        int screenWidth,
        int screenHeight,
        bool disableHeadshot,
        bool keepCurrentLock,
        std::chrono::steady_clock::time_point observationTime = {}
    );
    /** @brief 获取当前锁定的目标信息，返回是否成功 */
    bool getLockedTarget(LockedTargetInfo& out) const;
    /** @brief 获取当前锁定目标的跟踪 ID */
    int getLockedTrackId() const { return lockedTrackId_; }
    /** @brief 获取所有跟踪轨道的调试信息 */
    std::vector<TrackDebugInfo> getDebugTracks() const;

private:
    /** @brief 内部跟踪轨道状态 */
    struct TrackState
    {
        int id = -1;                                  ///< 跟踪 ID
        cv::Rect2f box;                              ///< 边界框
        cv::Point2f velocity = { 0.0f, 0.0f };       ///< 速度
        int classId = -1;                             ///< 类别 ID
        int hits = 0;                                 ///< 命中次数
        int missed = 0;                               ///< 丢失次数
        bool observedThisFrame = false;               ///< 本帧是否观测到
        double pivotX = 0.0;                          ///< 枢轴点 X
        double pivotY = 0.0;                          ///< 枢轴点 Y
        std::chrono::steady_clock::time_point lastUpdate;  ///< 最后更新时间
    };

    /** @brief 检测候选结构 */
    struct DetectionCandidate
    {
        cv::Rect2f box;                              ///< 边界框
        int classId = -1;                             ///< 类别 ID
        double pivotX = 0.0;                          ///< 枢轴点 X
        double pivotY = 0.0;                          ///< 枢轴点 Y
    };

    /** @brief 计算两个边界框的交并比 (IoU) */
    static float iou(const cv::Rect2f& a, const cv::Rect2f& b);
    /** @brief 根据 ID 查找跟踪轨道索引 */
    int findTrackIndexById(int id) const;
    /** @brief 根据屏幕中心选择最佳跟踪轨道 */
    int chooseBestTrack(int screenWidth, int screenHeight) const;
    /** @brief 计算允许的最大丢失帧数 */
    int allowedMissedFrames(const TrackState& t) const;
    /** @brief 清除已死亡的跟踪轨道 */
    void pruneDeadTracks();

    std::vector<TrackState> tracks_;                ///< 所有跟踪轨道
    int nextId_ = 1;                                 ///< 下一个可用 ID
    int lockedTrackId_ = -1;                          ///< 当前锁定目标的跟踪 ID
    int maxMissedFrames_ = 6;                        ///< 最大允许丢失帧数
    double frameDtMeanSec_ = 1.0 / 60.0;            ///< 帧间平均时间（秒）
    std::chrono::steady_clock::time_point lastTrackerFrameTime_{};  ///< 上一帧跟踪时间
};

#endif // AIMBOTTARGET_H
