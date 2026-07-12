#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <cmath>
#include <limits>
#include <algorithm>
#include <numeric>
#include <memory>
#include <mutex>
#include <opencv2/opencv.hpp>

#include "Xen.h"
#include "AimbotTarget.h"
#include "config.h"

/**
 * AimbotTarget 默认构造函数
 * 初始化所有成员变量为零值或默认值。
 */
AimbotTarget::AimbotTarget()
    : x(0), y(0), w(0), h(0), classId(0), pivotX(0.0), pivotY(0.0)
{
}

/**
 * AimbotTarget 带参构造函数
 * 使用给定的检测框位置、尺寸、类别ID和瞄准支点坐标初始化目标对象。
 *
 * @param x_   检测框左上角 X 坐标
 * @param y_   检测框左上角 Y 坐标
 * @param w_   检测框宽度
 * @param h_   检测框高度
 * @param cls  目标类别ID（例如 0=玩家身体, 1=玩家头部）
 * @param px   瞄准支点 X 坐标（用于计算瞄准向量）
 * @param py   瞄准支点 Y 坐标
 */
AimbotTarget::AimbotTarget(int x_, int y_, int w_, int h_, int cls, double px, double py)
    : x(x_), y(y_), w(w_), h(h_), classId(cls), pivotX(px), pivotY(py)
{
}

/**
 * sortTargets — 目标选择函数
 *
 * 从检测结果中选出距离屏幕中心最近的目标，并返回一个 AimbotTarget 对象。
 * 策略：优先选择头部目标（classHead），若禁用爆头或未检测到头部则退选身体目标（classPlayer）。
 *
 * 选择逻辑：
 *   1. 非禁用爆头时，遍历所有 classHead 目标，计算其头部偏移点距屏幕中心的欧几里得距离，
 *      选取距离最小的头部目标。
 *   2. 若禁用爆头或未找到头部目标，则遍历 classPlayer 目标，计算身体偏移点距屏幕中心的距离，
 *      选取最近的身体目标。
 *   3. 最终返回的 AimbotTarget 包含原始框坐标和调整后的瞄准支点坐标（pivotX/pivotY）。
 *
 * 偏移控制：
 *   headOffset — 头部检测框高度方向上的偏移比例（默认 0.05），用于确定头部的精确瞄准点。
 *   bodyOffset — 身体检测框高度方向上的偏移比例（默认 0.15），用于确定身体的精确瞄准点。
 *
 * @param boxes          当前帧的所有检测框
 * @param classes        每个检测框对应的类别ID
 * @param screenWidth    屏幕宽度（像素），用于计算屏幕中心点
 * @param screenHeight   屏幕高度（像素）
 * @param disableHeadshot 是否禁用爆头模式（true=仅瞄准身体）
 * @return               指向选中目标的 unique_ptr，若无合适目标则返回 nullptr
 */
std::unique_ptr<AimbotTarget> sortTargets(
    const std::vector<cv::Rect>& boxes,
    const std::vector<int>& classes,
    int screenWidth,
    int screenHeight,
    bool disableHeadshot)
{
    const size_t inputCount = std::min(boxes.size(), classes.size());
    if (inputCount == 0)
    {
        return nullptr;
    }

    cv::Point center(screenWidth / 2, screenHeight / 2);
    int classHead = 1;
    int classPlayer = 0;
    float headOffset = 0.05f;
    float bodyOffset = 0.15f;
    {
        std::lock_guard<std::mutex> lock(configMutex);
        classHead = config.class_head;
        classPlayer = config.class_player;
        headOffset = config.head_y_offset;
        bodyOffset = config.body_y_offset;
    }

    double minDistance = std::numeric_limits<double>::max();
    int nearestIdx = -1;
    int targetY = 0;

    // 第一轮：优先搜索头部目标（classHead），取距屏幕中心最近的头部
    if (!disableHeadshot)
    {
        for (size_t i = 0; i < inputCount; i++)
        {
            if (boxes[i].width <= 0 || boxes[i].height <= 0)
                continue;

            if (classes[i] == classHead)
            {
                int headOffsetY = static_cast<int>(boxes[i].height * headOffset);
                cv::Point targetPoint(boxes[i].x + boxes[i].width / 2, boxes[i].y + headOffsetY);
                double dx = targetPoint.x - center.x;
                double dy = targetPoint.y - center.y;
                double distance = dx * dx + dy * dy;
                if (distance < minDistance)
                {
                    minDistance = distance;
                    nearestIdx = static_cast<int>(i);
                    targetY = targetPoint.y;
                }
            }
        }
    }

    // 第二轮：禁用爆头或未找到头部目标时，退选身体目标（classPlayer）
    if (disableHeadshot || nearestIdx == -1)
    {
        minDistance = std::numeric_limits<double>::max();
        for (size_t i = 0; i < inputCount; i++)
        {
            if (boxes[i].width <= 0 || boxes[i].height <= 0)
                continue;

            if (disableHeadshot && classes[i] == classHead)
                continue;

            if (classes[i] == classPlayer)
            {
                int offsetY = static_cast<int>(boxes[i].height * bodyOffset);
                cv::Point targetPoint(boxes[i].x + boxes[i].width / 2, boxes[i].y + offsetY);
                double dx = targetPoint.x - center.x;
                double dy = targetPoint.y - center.y;
                double distance = dx * dx + dy * dy;
                if (distance < minDistance)
                {
                    minDistance = distance;
                    nearestIdx = static_cast<int>(i);
                    targetY = targetPoint.y;
                }
            }
        }
    }

    if (nearestIdx == -1)
    {
        return nullptr;
    }

    // 调整最终瞄准框的 Y 坐标：头部目标保持头部偏移，身体目标上移半个框高
    int finalY = 0;
    if (classes[nearestIdx] == classHead)
    {
        int headOffsetY = static_cast<int>(boxes[nearestIdx].height * headOffset);
        finalY = boxes[nearestIdx].y + headOffsetY - boxes[nearestIdx].height / 2;
    }
    else
    {
        finalY = targetY - boxes[nearestIdx].height / 2;
    }

    int finalX = boxes[nearestIdx].x;
    int finalW = boxes[nearestIdx].width;
    int finalH = boxes[nearestIdx].height;
    int finalClass = classes[nearestIdx];

    // 计算瞄准参考支点（pivot），作为后续瞄准向量计算的基准点
    double pivotX = finalX + (finalW / 2.0);
    double pivotY = finalY + (finalH / 2.0);

    return std::make_unique<AimbotTarget>(finalX, finalY, finalW, finalH, finalClass, pivotX, pivotY);
}

/**
 * mergeHeadToPlayer — 共享头-身合并函数
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
    float headOffset, bool disableHeadshot)
{
    if (disableHeadshot || dets.empty())
        return;

    // 收集所有身体类别检测的索引
    std::vector<size_t> playerIdx;
    playerIdx.reserve(dets.size());
    for (size_t i = 0; i < dets.size(); ++i)
    {
        if (dets[i].classId == classPlayer)
            playerIdx.push_back(i);
    }

    if (playerIdx.empty())
        return;

    std::vector<char> dropHead(dets.size(), 0);
    std::vector<char> playerHasHeadPivot(dets.size(), 0);
    std::vector<double> playerHeadPivotX(dets.size(), 0.0);
    std::vector<double> playerHeadPivotY(dets.size(), 0.0);
    std::vector<double> playerHeadPivotDist(dets.size(), std::numeric_limits<double>::max());

    // 对每个头部检测，寻找包含它的最近身体检测
    for (size_t hi = 0; hi < dets.size(); ++hi)
    {
        const auto& h = dets[hi];
        if (h.classId != classHead)
            continue;

        const double headCx = h.box.x + h.box.width * 0.5;
        const double headCy = h.box.y + h.box.height * 0.5;

        size_t bestPlayer = static_cast<size_t>(-1);
        double bestDist = std::numeric_limits<double>::max();

        for (size_t pi : playerIdx)
        {
            const auto& p = dets[pi].box;
            // 定义身体框的容忍区域：水平外扩15%，垂直上扩20%、下缩至65%高度
            const double px1 = p.x - p.width * 0.15;
            const double px2 = p.x + p.width * 1.15;
            const double py1 = p.y - p.height * 0.20;
            const double py2 = p.y + p.height * 0.65;

            if (!(headCx >= px1 && headCx <= px2 && headCy >= py1 && headCy <= py2))
                continue;

            const double pCx = p.x + p.width * 0.5;
            const double pCy = p.y + p.height * 0.5;
            const double d = std::hypot(headCx - pCx, headCy - pCy);
            if (d < bestDist)
            {
                bestDist = d;
                bestPlayer = pi;
            }
        }

        // 找到匹配的身体检测：标记头部丢弃，将头部的瞄准支点信息保存到身体上
        if (bestPlayer != static_cast<size_t>(-1))
        {
            dropHead[hi] = 1;
            if (!playerHasHeadPivot[bestPlayer] || bestDist < playerHeadPivotDist[bestPlayer])
            {
                playerHasHeadPivot[bestPlayer] = 1;
                playerHeadPivotDist[bestPlayer] = bestDist;
                playerHeadPivotX[bestPlayer] = h.box.x + h.box.width * 0.5;
                playerHeadPivotY[bestPlayer] = h.box.y + h.box.height * headOffset;
            }
        }
    }

    // 构建过滤后的候选列表：丢弃被合并的头部，将命中的身体的支点更新为头部支点
    std::vector<DetectionCandidate> filtered;
    filtered.reserve(dets.size());

    for (size_t i = 0; i < dets.size(); ++i)
    {
        if (dropHead[i])
            continue;

        DetectionCandidate d = dets[i];
        if (d.classId == classPlayer && playerHasHeadPivot[i])
        {
            d.pivotX = playerHeadPivotX[i];
            d.pivotY = playerHeadPivotY[i];
        }
        filtered.push_back(d);
    }

    dets.swap(filtered);
}

// ============================================================================
// MotionLibTargetTracker — 基于移动控制库的跟踪器包装类实现
// ============================================================================

/**
 * MotionLibTargetTracker 默认构造函数
 */
MotionLibTargetTracker::MotionLibTargetTracker()
{
    // MOT 和 SOT 在 configure() 中初始化
}

/**
 * configure — 从外部配置初始化跟踪器参数
 *
 * 同时初始化内部 MOT（多目标跟踪）和 SOT（单目标锁定）实例。
 *
 * @param confirmThreshold       MOT 确认帧数（跟踪需连续命中多少帧才输出）
 * @param terminationFrames      MOT 终止帧数（连续丢失多少帧后删除轨迹）
 * @param noiseVx, noiseVy       速度过程噪声
 * @param noiseW, noiseH         尺寸过程噪声
 * @param measurementStdDev      测量标准差
 * @param coastFramesLimit       SOT 滑行帧数限制（遮挡后维持锁定的最大帧数）
 * @param selectionStrategy      目标选择策略："nearest" / "confidence" / "track_id"
 * @param recaptureIoUThreshold  重捕获 IoU 阈值
 * @param recaptureDistanceMultiplier 重捕获距离乘数
 * @param coastVelocityDecay     滑行速度衰减系数
 */
void MotionLibTargetTracker::configure(
    int confirmThreshold,
    int terminationFrames,
    float noiseVx, float noiseVy,
    float noiseW, float noiseH,
    float measurementStdDev,
    int coastFramesLimit,
    const std::string& selectionStrategy,
    float recaptureIoUThreshold,
    float recaptureDistanceMultiplier,
    float coastVelocityDecay)
{
    confirmThreshold_ = confirmThreshold;
    terminationFrames_ = terminationFrames;
    noiseVx_ = noiseVx;
    noiseVy_ = noiseVy;
    noiseW_ = noiseW;
    noiseH_ = noiseH;
    measurementStdDev_ = measurementStdDev;
    coastFramesLimit_ = coastFramesLimit;
    recaptureIoUThreshold_ = recaptureIoUThreshold;
    recaptureDistanceMultiplier_ = recaptureDistanceMultiplier;
    coastVelocityDecay_ = coastVelocityDecay;

    // 初始化 MOT
    motTracker_.initialize(
        confirmThreshold_, terminationFrames_,
        noiseVx_, noiseVy_, noiseW_, noiseH_, measurementStdDev_);

    // SOT 固定使用 NearestDistance 策略（最近目标优先，最适合自瞄场景）
    sotTracker_.initialize(
        estimation::SingleTargetTracker::SelectionStrategy::NearestDistance,
        coastFramesLimit_,
        -1,  // specifiedId 不使用
        recaptureIoUThreshold_,
        recaptureDistanceMultiplier_,
        coastVelocityDecay_);

    configured_ = true;
    lockedTrackId_ = -1;
    frameDtMeanSec_ = 1.0 / 60.0;
    lastUpdateTime_ = {};

    // 缓存类别/偏移配置（调用者必须持有 configMutex）
    cachedClassPlayer_ = config.class_player;
    cachedClassHead_   = config.class_head;
    cachedBodyOffset_  = config.body_y_offset;
    cachedHeadOffset_  = config.head_y_offset;
}

/**
 * reset — 重置所有跟踪器内部状态
 */
void MotionLibTargetTracker::reset()
{
    motTracker_.reset(true);
    sotTracker_.reset();
    lockedTrackId_ = -1;
    frameDtMeanSec_ = 1.0 / 60.0;
    lastUpdateTime_ = {};
}

/**
 * toBoundingBox — 类型转换：cv::Rect → estimation::BoundingBox
 */
estimation::BoundingBox MotionLibTargetTracker::toBoundingBox(const cv::Rect& r)
{
    return {
        static_cast<float>(r.x),
        static_cast<float>(r.y),
        static_cast<float>(r.width),
        static_cast<float>(r.height)
    };
}

/**
 * toAimbotTarget — 类型转换：estimation::DetectedObject → AimbotTarget
 */
AimbotTarget MotionLibTargetTracker::toAimbotTarget(const estimation::DetectedObject& obj)
{
    return AimbotTarget(
        static_cast<int>(std::lround(obj.box.x)),
        static_cast<int>(std::lround(obj.box.y)),
        static_cast<int>(std::lround(obj.box.w)),
        static_cast<int>(std::lround(obj.box.h)),
        obj.classId,
        obj.box.x + obj.box.w * 0.5f,  // pivotX = 框中心 X
        obj.box.y + obj.box.h * 0.5f   // pivotY = 框中心 Y
    );
}

/**
 * update — 多目标跟踪主更新函数
 *
 * 完整流程：
 *   1. 帧时间管理（EMA 平滑帧间隔）
 *   2. 检测候选构建（cv::Rect → DetectionCandidate）
 *   3. 头-身合并（复用旧逻辑）
 *   4. 转换为 estimation::DetectedObject 列表
 *   5. 调用 MOT predictAndUpdate（匈牙利匹配 + 7状态 Kalman）
 *   6. 调用 SOT update（状态机：Locked→Coasting→Unlocked）
 *   7. 管理锁定/解锁切换
 */
void MotionLibTargetTracker::update(
    const std::vector<cv::Rect>& boxes,
    const std::vector<int>& classes,
    int screenWidth,
    int screenHeight,
    bool disableHeadshot,
    bool keepCurrentLock,
    std::chrono::steady_clock::time_point observationTime)
{
    if (!configured_)
        return;

    auto now = (observationTime.time_since_epoch().count() != 0)
        ? observationTime
        : std::chrono::steady_clock::now();

    // 帧时间管理
    if (lastUpdateTime_.time_since_epoch().count() != 0)
    {
        double frameDt = std::chrono::duration<double>(now - lastUpdateTime_).count();
        if (!std::isfinite(frameDt) || frameDt <= 0.0)
            frameDt = std::clamp(frameDtMeanSec_, 1.0 / 500.0, 0.25);
        frameDt = std::clamp(frameDt, 1.0 / 500.0, 0.25);
        frameDtMeanSec_ = frameDtMeanSec_ * 0.88 + frameDt * 0.12;
    }
    lastUpdateTime_ = now;

    // 读取类别配置（使用缓存值，configure() 时已初始化）
    const int classPlayer = cachedClassPlayer_;
    const int classHead   = cachedClassHead_;
    const float bodyOffset = cachedBodyOffset_;
    const float headOffset = cachedHeadOffset_;

    const size_t inputCount = std::min(boxes.size(), classes.size());

    // 构建检测候选列表
    std::vector<DetectionCandidate> dets;
    dets.reserve(inputCount);
    for (size_t i = 0; i < inputCount; ++i)
    {
        const int cls = classes[i];
        if (disableHeadshot)
        {
            if (cls != classPlayer) continue;
        }
        else
        {
            if (cls != classPlayer && cls != classHead) continue;
        }

        const cv::Rect& b = boxes[i];
        if (b.width <= 0 || b.height <= 0) continue;

        const double yOffset = (cls == classHead) ? headOffset : bodyOffset;
        DetectionCandidate d;
        d.box = cv::Rect2f(
            static_cast<float>(b.x), static_cast<float>(b.y),
            static_cast<float>(b.width), static_cast<float>(b.height));
        d.classId = cls;
        d.pivotX = b.x + b.width * 0.5;
        d.pivotY = b.y + b.height * yOffset;
        dets.push_back(d);
    }

    // 头-身合并
    mergeHeadToPlayer(dets, classPlayer, classHead, headOffset, disableHeadshot);

    // 转换为 motion_lib 的 DetectedObject 列表
    std::vector<estimation::DetectedObject> motInput;
    motInput.reserve(dets.size());
    for (const auto& d : dets)
    {
        estimation::DetectedObject obj;
        obj.box = { d.box.x, d.box.y, d.box.width, d.box.height };
        obj.classId = d.classId;
        obj.confidence = 1.0f;  // Xen 当前不传递置信度
        motInput.push_back(obj);
    }

    // MOT 预测+更新（匈牙利匹配 + 7状态 Kalman）
    std::vector<estimation::DetectedObject> motOutput;
    motTracker_.predictAndUpdate(motInput, motOutput, frameDtMeanSec_);

    // SOT 更新
    if (sotTracker_.hasTarget())
    {
        // 已有锁定目标时：注入外部速度（从 MOT Kalman 获取），然后尝试重捕获
        // 在 MOT 输出中查找匹配的 trackId
        int sotId = sotTracker_.trackId();
        bool foundInMot = false;
        for (const auto& obj : motOutput)
        {
            if (obj.trackId == sotId)
            {
                sotTracker_.setExternalVelocity(obj.velocityX, obj.velocityY, true);
                foundInMot = true;
                break;
            }
        }
        if (!foundInMot)
        {
            // MOT 中未找到 — 目标可能被遮挡，设置零速度让它滑行
            // 如果轨迹在 MOT 中未被匹配（遮挡/丢失），让 SOT 以自身 EMA
            // 速度继续滑行，而非强制清零。速度可能略偏但仍比冻结位置合理。
            sotTracker_.setExternalVelocity(0.0f, 0.0f, false);
        }
    }


    // 当 SOT 处于解锁状态时，将参考中心设为屏幕中心，
    // 确保 NearestDistance 策略选择离准星最近的目标（而非左上角(0,0)）
    {
        sotTracker_.setReferenceCenter(
            static_cast<float>(screenWidth) * 0.5f,
            static_cast<float>(screenHeight) * 0.5f);
    }

    sotTracker_.update(motOutput);

    // 管理锁定切换
    if (sotTracker_.hasTarget())
    {
        lockedTrackId_ = sotTracker_.trackId();
    }
    else
    {
        // SOT 丢失目标：如果允许保留锁定则保留旧的 lockedTrackId_
        // 注意：keepCurrentLock 仅保留 trackId 记录，不影响 getLockedTarget() 返回值
        // （getLockedTarget 以 sotTracker_.hasTarget() 为准）
        if (!keepCurrentLock)
            lockedTrackId_ = -1;
    }

    // 如果 SOT 之前没有目标但现在有了（新锁定），且策略是 SpecifiedTrackId
    // 无需额外操作 — SOT 已自动处理
}

/**
 * getLockedTarget — 获取当前锁定的目标信息
 */
bool MotionLibTargetTracker::getLockedTarget(LockedTargetInfo& out) const
{
    if (!configured_ || lockedTrackId_ < 0)
        return false;

    if (!sotTracker_.hasTarget())
        return false;

    // 从 SOT 获取状态
    float cx, cy, w, h, vx, vy;
    if (!sotTracker_.getState(cx, cy, w, h, vx, vy))
        return false;

    out.trackId = sotTracker_.trackId();
    out.observedThisFrame = sotTracker_.isLocked();  // locked=当帧有观测, coasting=无观测
    out.missedFrames = sotTracker_.coastCount();

    // 构建 AimbotTarget
    out.target = AimbotTarget(
        static_cast<int>(std::lround(cx - w * 0.5f)),
        static_cast<int>(std::lround(cy - h * 0.5f)),
        static_cast<int>(std::lround(w)),
        static_cast<int>(std::lround(h)),
        sotTracker_.classId(),
        static_cast<double>(cx),
        static_cast<double>(cy)
    );

    return true;
}

/**
 * getDebugTracks — 获取所有活跃轨迹的调试信息
 *
 * 从 MOT 的输出中提取轨迹信息用于覆层可视化。
 * 注意：MOT 的轨迹接口与旧版不同，这里使用 SOT 状态 + MOT 输出组合。
 */
std::vector<TrackDebugInfo> MotionLibTargetTracker::getDebugTracks() const
{
    std::vector<TrackDebugInfo> out;

    if (!configured_)
        return out;

    // 主要返回 SOT 锁定的目标（如果存在）
    if (sotTracker_.hasTarget())
    {
        float cx, cy, w, h, vx, vy;
        if (sotTracker_.getState(cx, cy, w, h, vx, vy))
        {
            TrackDebugInfo d;
            d.trackId = sotTracker_.trackId();
            d.classId = sotTracker_.classId();
            d.box = cv::Rect(
                static_cast<int>(std::lround(cx - w * 0.5f)),
                static_cast<int>(std::lround(cy - h * 0.5f)),
                static_cast<int>(std::lround(w)),
                static_cast<int>(std::lround(h)));
            d.pivotX = static_cast<double>(cx);
            d.pivotY = static_cast<double>(cy);
            d.velocityX = static_cast<double>(vx);
            d.velocityY = static_cast<double>(vy);
            d.lastUpdate = lastUpdateTime_;
            d.observedThisFrame = sotTracker_.isLocked();
            d.missedFrames = sotTracker_.coastCount();
            d.isLocked = true;
            out.push_back(d);
        }
    }

    return out;
}

/**
 * getLockedVelocity — 获取 SOT 估计的速度（像素/秒）
 *
 * 用于外部预测模块。
 *
 * @return (velocityX, velocityY) 像素/秒
 */
std::pair<float, float> MotionLibTargetTracker::getLockedVelocity() const
{
    if (!configured_ || !sotTracker_.hasTarget())
        return { 0.0f, 0.0f };

    float cx, cy, w, h, vx, vy;
    if (sotTracker_.getState(cx, cy, w, h, vx, vy))
        return { vx, vy };

    return { 0.0f, 0.0f };
}
