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
 * MultiTargetTracker::iou — 计算两个矩形框的交并比（Intersection over Union）
 *
 * 用于衡量检测框与预测框的重叠程度，是数据关联中的关键指标之一。
 * IOU 值域为 [0.0, 1.0]，值越大说明两个框重叠越充分。
 *
 * @param a 第一个矩形框（Rect2f 类型）
 * @param b 第二个矩形框（Rect2f 类型）
 * @return  两个框的交并比，若分母接近零则返回 0.0
 */
float MultiTargetTracker::iou(const cv::Rect2f& a, const cv::Rect2f& b)
{
    const float x1 = std::max(a.x, b.x);
    const float y1 = std::max(a.y, b.y);
    const float x2 = std::min(a.x + a.width, b.x + b.width);
    const float y2 = std::min(a.y + a.height, b.y + b.height);
    const float w = std::max(0.0f, x2 - x1);
    const float h = std::max(0.0f, y2 - y1);
    const float inter = w * h;
    const float ua = a.width * a.height + b.width * b.height - inter;
    if (ua <= 1e-6f) return 0.0f;
    return inter / ua;
}

/**
 * 匿名命名空间 — 封装匈牙利算法的最小代价分配实现
 *
 * 该算法用于解决「指派问题」：给定 N 条轨迹和 M 个检测结果，
 * 为每条轨迹找到最优的检测匹配，使总代价最小。
 * 该版本的匈牙利算法在处理非方阵时自动处理未匹配项（填充固定代价 kUnassignedCost）。
 */
namespace
{
/**
 * hungarianMinimize — 匈牙利算法（最小化版本）
 *
 * 基于 Jonker-Volgenant 算法的经典 O(n^3) 实现，用于求解二分图的最小权匹配。
 * 原理：通过引入对偶变量 u（行）和 v（列），在增广路径过程中不断调整对偶变量，
 * 使得满足互补松弛条件，最终找到最优指派。
 *
 * 算法步骤：
 *   1. 对每一行 i，初始化列 j0=0，寻找增广路径。
 *   2. 维护每列的最小值数组 minv[] 和路径记录数组 way[]。
 *   3. 每次迭代找到最小值所在的列 j1，更新对偶变量。
 *   4. 当找到增广路径终点（p[j0]==0）时，沿 way[] 回溯更新匹配关系。
 *   5. 输出一个长度为 n 的 assignment 数组，其中 assignment[i] = j 表示第 i 行指派给第 j 列，
 *      若未匹配则为 -1。
 *
 * @param costs  n x m 代价矩阵，costs[i][j] 表示第 i 个轨迹与第 j 个检测的关联代价
 * @return       长度为 n 的整数向量，assignment[i] = 匹配到的列索引，未匹配为 -1
 */
std::vector<int> hungarianMinimize(const std::vector<std::vector<double>>& costs)
{
    const int n = static_cast<int>(costs.size());
    if (n == 0)
        return {};
    const int m = static_cast<int>(costs[0].size());
    if (m == 0)
        return std::vector<int>(n, -1);

    // 对偶变量 u: 行分量，v: 列分量；p: 当前匹配；way: 增广路径回溯记录
    std::vector<double> u(n + 1, 0.0);
    std::vector<double> v(m + 1, 0.0);
    std::vector<int> p(m + 1, 0);
    std::vector<int> way(m + 1, 0);

    for (int i = 1; i <= n; ++i)
    {
        p[0] = i;
        int j0 = 0;
        std::vector<double> minv(m + 1, std::numeric_limits<double>::infinity());
        std::vector<char> used(m + 1, 0);

        do
        {
            used[j0] = 1;
            const int i0 = p[j0];
            double delta = std::numeric_limits<double>::infinity();
            int j1 = 0;

            // 更新当前行 i0 到所有未使用列 j 的缩减代价，记录最小值
            for (int j = 1; j <= m; ++j)
            {
                if (used[j])
                    continue;

                const double cur = costs[i0 - 1][j - 1] - u[i0] - v[j];
                if (cur < minv[j])
                {
                    minv[j] = cur;
                    way[j] = j0;
                }
                if (minv[j] < delta)
                {
                    delta = minv[j];
                    j1 = j;
                }
            }

            // 根据 delta 更新对偶变量，保持互补松弛条件
            for (int j = 0; j <= m; ++j)
            {
                if (used[j])
                {
                    u[p[j]] += delta;
                    v[j] -= delta;
                }
                else
                {
                    minv[j] -= delta;
                }
            }

            j0 = j1;
        } while (p[j0] != 0);

        // 沿 way[] 回溯，更新匹配关系
        do
        {
            const int j1 = way[j0];
            p[j0] = p[j1];
            j0 = j1;
        } while (j0 != 0);
    }

    // 将匹配结果转为 n 长度的 assignment 数组
    std::vector<int> assignment(n, -1);
    for (int j = 1; j <= m; ++j)
    {
        if (p[j] > 0)
            assignment[p[j] - 1] = j - 1;
    }
    return assignment;
}
}

/**
 * MultiTargetTracker::findTrackIndexById — 按 ID 查找轨迹在内部向量中的索引
 *
 * @param id 目标的轨迹 ID
 * @return   轨迹在 tracks_ 向量中的下标，若未找到则返回 -1
 */
int MultiTargetTracker::findTrackIndexById(int id) const
{
    for (size_t i = 0; i < tracks_.size(); ++i)
    {
        if (tracks_[i].id == id)
            return static_cast<int>(i);
    }
    return -1;
}

/**
 * MultiTargetTracker::allowedMissedFrames — 计算轨迹允许的最大丢失帧数
 *
 * 已锁定的目标可以获得额外的丢失容忍帧数（lockedBonus），
 * 使其在短暂遮挡或高速移动时不被立即销毁，提高锁定的鲁棒性。
 *
 * @param t 目标轨迹状态引用
 * @return  该轨迹允许的最大丢失帧数
 */
int MultiTargetTracker::allowedMissedFrames(const TrackState& t) const
{
    // 已锁定的目标额外获得 8 帧的丢失容忍，以应对短暂遮挡或快速运动
    const int lockedBonus = (t.id == lockedTrackId_) ? 8 : 0;
    return maxMissedFrames_ + lockedBonus;
}

/**
 * MultiTargetTracker::pruneDeadTracks — 清理死亡轨迹
 *
 * 遍历所有轨迹，移除丢失帧数超过 allowedMissedFrames 的轨迹，
 * 避免轨迹列表无限增长。
 */
void MultiTargetTracker::pruneDeadTracks()
{
    tracks_.erase(
        std::remove_if(tracks_.begin(), tracks_.end(), [&](const TrackState& t) {
            return t.missed > allowedMissedFrames(t);
            }),
        tracks_.end());
}

/**
 * MultiTargetTracker::chooseBestTrack — 从所有活跃轨迹中选出最优锁定目标
 *
 * 评分策略综合考虑距离（距屏幕中心）、命中次数（hitBonus，鼓励已稳定跟踪的目标）
 * 和丢失次数（missPenalty，惩罚不稳定目标）。
 * 评分越低表示目标越适合锁定。
 *
 * @param screenWidth   屏幕宽度
 * @param screenHeight  屏幕高度
 * @return              最优轨迹在 tracks_ 中的下标，无合适目标则返回 -1
 */
int MultiTargetTracker::chooseBestTrack(int screenWidth, int screenHeight) const
{
    if (tracks_.empty())
        return -1;

    const double cx = screenWidth * 0.5;
    const double cy = screenHeight * 0.5;

    int bestIdx = -1;
    double bestScore = std::numeric_limits<double>::max();

    for (size_t i = 0; i < tracks_.size(); ++i)
    {
        const auto& t = tracks_[i];
        if (t.missed > allowedMissedFrames(t))
            continue;

        const double dx = t.pivotX - cx;
        const double dy = t.pivotY - cy;
        const double dist = std::hypot(dx, dy);
        const double hitBonus = std::min(5, t.hits) * 4.0;
        const double missPenalty = t.missed * 50.0;
        const double score = dist + missPenalty - hitBonus;

        if (score < bestScore)
        {
            bestScore = score;
            bestIdx = static_cast<int>(i);
        }
    }

    return bestIdx;
}

/**
 * MultiTargetTracker::reset — 重置多目标跟踪器
 *
 * 清空所有轨迹、重置 ID 计数器、清除锁定状态，并将帧间隔估算恢复为默认值（1/60 秒）。
 */
void MultiTargetTracker::reset()
{
    tracks_.clear();
    nextId_ = 1;
    lockedTrackId_ = -1;
    frameDtMeanSec_ = 1.0 / 60.0;
    lastTrackerFrameTime_ = {};
}

/**
 * MultiTargetTracker::update — 多目标跟踪主更新函数
 *
 * 该函数是整个多目标跟踪系统的核心，负责每帧执行以下完整流程：
 *
 * 1. 时间管理
 *    - 计算当前帧与上一帧的时间差（dt），通过指数移动平均平滑帧间隔波动。
 *    - 对异常 dt 值进行裁剪和回退处理，防止数值不稳定。
 *
 * 2. 检测候选构建
 *    - 从 YOLO 检测结果中筛选有效候选（根据 disableHeadshot 过滤类别）。
 *    - 为每个候选计算瞄准支点坐标（pivotX/pivotY），头部用 headOffset，身体用 bodyOffset。
 *
 * 3. 头-身合并（Head-to-Player Merging）
 *    - 当头部检测框落在对应身体检测框的容忍区域内时，认为两者属于同一实体。
 *    - 剔除冗余的头部候选，将瞄准支点迁移至身体候选上（使用头部支点位置），
 *      在保持跟踪稳定性的同时允许精确爆头瞄准。
 *
 * 4. 数据关联（匈牙利算法）
 *    - 构建 N x M 代价矩阵，每个元素通过 computeMatchScore 计算轨迹与检测的匹配代价。
 *    - 调用 hungarianMinimize 求解最优一一匹配，未匹配轨迹视为丢失帧。
 *
 * 5. 轨迹状态更新
 *    - 匹配成功的轨迹：更新位置、计算速度（EMA平滑）、重置丢失计数。
 *    - 未匹配的轨迹：基于速度进行位置外推预测、速度衰减、丢失计数递增。
 *    - 未匹配的检测：创建新轨迹，分配新 ID。
 *
 * 6. 轨迹生命周期管理
 *    - 调用 pruneDeadTracks 清理超过丢失容忍阈值的死亡轨迹。
 *    - 若锁定目标已死亡则清除锁定状态。
 *
 * 7. 自动锁定选择
 *    - keepCurrentLock=true 且已有锁定时，保持当前锁定不变。
 *    - 否则调用 chooseBestTrack 从所有活跃轨迹中选出最优目标自动锁定。
 *
 * @param boxes          当前帧的检测框列表
 * @param classes        每个检测框的类别ID
 * @param screenWidth    屏幕宽度
 * @param screenHeight   屏幕高度
 * @param disableHeadshot 是否禁用爆头模式
 * @param keepCurrentLock 是否保持当前锁定目标不变
 * @param observationTime 可选的时间戳，用于外部传入观测时间（如图像采集时间）
 */
void MultiTargetTracker::update(
    const std::vector<cv::Rect>& boxes,
    const std::vector<int>& classes,
    int screenWidth,
    int screenHeight,
    bool disableHeadshot,
    bool keepCurrentLock,
    std::chrono::steady_clock::time_point observationTime)
{
    auto now = (observationTime.time_since_epoch().count() != 0)
        ? observationTime
        : std::chrono::steady_clock::now();
    // 计算帧时间差，使用指数移动平均（EMA）平滑帧间隔波动
    if (lastTrackerFrameTime_.time_since_epoch().count() != 0)
    {
        double frameDt = std::chrono::duration<double>(now - lastTrackerFrameTime_).count();
        if (!std::isfinite(frameDt) || frameDt <= 0.0)
        {
            // dt 无效时回退使用平滑均值
            const double fallbackDt = std::clamp(frameDtMeanSec_, 1.0 / 500.0, 0.25);
            now = lastTrackerFrameTime_ +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(fallbackDt));
            frameDt = fallbackDt;
        }
        frameDt = std::clamp(frameDt, 1.0 / 500.0, 0.25);
        frameDtMeanSec_ = frameDtMeanSec_ * 0.88 + frameDt * 0.12;
    }
    lastTrackerFrameTime_ = now;

    int classPlayer = 0;
    int classHead = 1;
    float bodyOffset = 0.15f;
    float headOffset = 0.05f;
    {
        std::lock_guard<std::mutex> lock(configMutex);
        classPlayer = config.class_player;
        classHead = config.class_head;
        bodyOffset = config.body_y_offset;
        headOffset = config.head_y_offset;
    }

    // 重置所有轨迹的帧内观测标志
    for (auto& t : tracks_)
        t.observedThisFrame = false;

    const size_t inputCount = std::min(boxes.size(), classes.size());

    // 构建检测候选列表：过滤有效类别并计算支点坐标
    std::vector<DetectionCandidate> dets;
    dets.reserve(inputCount);
    for (size_t i = 0; i < inputCount; ++i)
    {
        const int cls = classes[i];
        if (disableHeadshot)
        {
            if (cls != classPlayer)
                continue;
        }
        else
        {
            if (cls != classPlayer && cls != classHead)
            {
                continue;
            }
        }

        const cv::Rect& b = boxes[i];
        if (b.width <= 0 || b.height <= 0)
            continue;

        const double yOffset = (cls == classHead) ? headOffset : bodyOffset;
        DetectionCandidate d;
        d.box = cv::Rect2f(static_cast<float>(b.x), static_cast<float>(b.y), static_cast<float>(b.width), static_cast<float>(b.height));
        d.classId = cls;
        d.pivotX = b.x + b.width * 0.5;
        d.pivotY = b.y + b.height * yOffset;
        dets.push_back(d);
    }

    // 头-身合并逻辑：将属于同一实体的头部检测合并到身体检测上
    // 对每个头部检测，检查其中心是否落在某个身体检测框的扩展范围内（水平外扩15%，垂直上扩20%+下扩35%）
    // 若匹配成功，保留身体框（更稳定）但将瞄准支点改为头部位置（实现精准爆头）
    if (!disableHeadshot && !dets.empty())
    {
        // 收集所有身体类别检测的索引
        std::vector<size_t> playerIdx;
        playerIdx.reserve(dets.size());
        for (size_t i = 0; i < dets.size(); ++i)
        {
            if (dets[i].classId == classPlayer)
                playerIdx.push_back(i);
        }

        if (!playerIdx.empty())
        {
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
    }

    // 关联结果存储：detAssigned[di] = ti 表示第 di 个检测匹配第 ti 条轨迹
    std::vector<int> detAssigned(dets.size(), -1);
    std::vector<int> trackAssigned(tracks_.size(), -1);
    constexpr double kUnassignedCost = 2.0;
    constexpr double kInvalidCost = 1000000.0;

    /**
     * computeMatchScore — 计算轨迹与检测候选之间的匹配代价
     *
     * 该函数是数据关联的核心，综合多个维度的信息产生一个标量代价，代价越小说明匹配越可信。
     *
     * 代价计算公式：
     *   score = (dist / maxDist) + (1 - overlap) * 0.30 + classPenalty + missPenalty - hitBonus - lockedBonus
     *
     * 各维度详解：
     *
     * 1. 类别检查（classPenalty）
     *    - 若轨迹与检测的类别必须一致；如果 disableHeadshot=false，允许头部/身体互换，
     *      此时会加一个较小的类别惩罚（0.18）。
     *    - 禁止类别互换且类别不匹配时，返回无穷大表示不可匹配。
     *
     * 2. 位置预测与距离计算（dist）
     *    - 使用轨迹已有的速度向量 × 时间差 dt，预测当前帧的期望位置。
     *    - 计算预测框与检测框之间的距离。类别互换时优先使用支点距离（pivotDist），
     *      其余情况取框距离和支点距离中的较小值。
     *
     * 3. 门控逻辑（Gating）
     *    门控阈值 maxDist 由三个分量叠加而成，只有距离在门内的匹配才被认为是有效的：
     *
     *    a) baseGate（基础门）
     *       - 公式：max(24.0, diag * 1.15 + 10.0)
     *       - 基于轨迹框的对角线长度动态缩放，适应不同尺寸的目标。
     *       - 保证即使极小目标也有至少 24 像素的匹配容忍。
     *
     *    b) speedGate（速度门）
     *       - 公式：speed * dt * (1.8 + missed * 0.35)
     *       - 根据目标运动速度动态扩大匹配范围，丢失帧越多扩展越大。
     *       - 防止快速移动目标因位置偏移过大而丢失匹配。
     *
     *    c) missGate（丢失门）
     *       - 公式：missed * max(14.0, diag * 0.18)
     *       - 丢失帧数越多，搜索范围越大，用于找回短暂消失后重新出现的目标。
     *       - 已锁定的目标（relaxedForLocked）总门限额外放大 1.6 倍。
     *
     *    若 dist > maxDist，返回无穷大，标记为不可匹配。
     *
     * 4. 重叠度（overlap）
     *    - 计算预测框与检测框的 IOU，鼓励位置高度重叠的匹配。
     *    - 代价贡献为 (1 - overlap) * 0.30。
     *
     * 5. 命中奖励（hitBonus）
     *    - 对累计命中次数较多的轨迹给予代价优惠，促使跟踪器保持已稳定跟踪的目标。
     *    - cap 为 6 次命中。
     *
     * 6. 锁定奖励（lockedBonus）
     *    - 已锁定的轨迹额外获得 0.10 的代价减免，增强锁定的惯性。
     *
     * @param t                轨迹状态
     * @param d                检测候选
     * @param relaxedForLocked 是否为已锁定目标（扩大门控阈值）
     * @return                 匹配代价，不可匹配时返回无穷大
     */
    auto computeMatchScore = [&](const TrackState& t, const DetectionCandidate& d, bool relaxedForLocked) -> double
        {
            const bool sameClass = (d.classId == t.classId);
            bool classSwappedWithinTarget = false;
            double classPenalty = 0.0;
            if (!sameClass)
            {
                const bool allowHeadBodySwap =
                    !disableHeadshot &&
                    ((t.classId == classPlayer && d.classId == classHead) ||
                     (t.classId == classHead && d.classId == classPlayer));
                if (!allowHeadBodySwap)
                    return std::numeric_limits<double>::infinity();

                classSwappedWithinTarget = true;
                classPenalty = 0.18;
            }

            // 计算时间差，限制在 [1e-4, 0.25] 秒范围内防止数值异常
            const double dt = std::clamp(
                std::chrono::duration<double>(now - t.lastUpdate).count(),
                1e-4, 0.25
            );

            // 速度外推预测：根据轨迹已有速度和 dt 估算当前帧的位置
            const float predCx = t.box.x + t.box.width * 0.5f + t.velocity.x * static_cast<float>(dt);
            const float predCy = t.box.y + t.box.height * 0.5f + t.velocity.y * static_cast<float>(dt);
            cv::Rect2f predBox(predCx - t.box.width * 0.5f, predCy - t.box.height * 0.5f, t.box.width, t.box.height);

            // 计算预测框与检测框的欧几里得距离
            const double detCx = d.box.x + d.box.width * 0.5;
            const double detCy = d.box.y + d.box.height * 0.5;
            const double boxDist = std::hypot(detCx - predCx, detCy - predCy);
            const double predPivotX = t.pivotX + t.velocity.x * dt;
            const double predPivotY = t.pivotY + t.velocity.y * dt;
            const double pivotDist = std::hypot(d.pivotX - predPivotX, d.pivotY - predPivotY);
            // 头身互换时优先使用支点距离（更敏感于瞄准点偏差），否则取框距离与支点距离的较小值
            const double dist = classSwappedWithinTarget ? pivotDist : std::min(boxDist, pivotDist);

            // 三部门控阈值计算
            const double diag = std::hypot(static_cast<double>(t.box.width), static_cast<double>(t.box.height));
            const double speed = std::hypot(t.velocity.x, t.velocity.y);
            const double baseGate = std::max(24.0, diag * 1.15 + 10.0);
            const double speedGate = speed * dt * (1.8 + t.missed * 0.35);
            const double missGate = t.missed * std::max(14.0, diag * 0.18);
            double maxDist = baseGate + speedGate + missGate;
            if (relaxedForLocked)
                maxDist *= 1.6;

            if (dist > maxDist)
                return std::numeric_limits<double>::infinity();

            // 综合代价计算：归一化距离 + 重叠惩罚 + 类别惩罚 + 丢失惩罚 - 命中奖励 - 锁定奖励
            const double overlap = iou(predBox, d.box);
            const double missPenalty = t.missed * 0.025;
            const double hitBonus = std::min(6, t.hits) * 0.01;
            const double lockedBonus = (t.id == lockedTrackId_) ? 0.10 : 0.0;
            return (dist / maxDist) + (1.0 - overlap) * 0.30 + classPenalty + missPenalty - hitBonus - lockedBonus;
        };

    // 构建代价矩阵并执行匈牙利算法匹配
    if (!tracks_.empty() && !dets.empty())
    {
        const size_t matrixSize = std::max(tracks_.size(), dets.size());
        std::vector<std::vector<double>> costs(
            matrixSize,
            std::vector<double>(matrixSize, kUnassignedCost));

        // 填充代价矩阵：每条轨迹对每个检测计算匹配代价
        for (size_t ti = 0; ti < tracks_.size(); ++ti)
        {
            const auto& t = tracks_[ti];
            for (size_t di = 0; di < dets.size(); ++di)
            {
                const bool relaxedForLocked = (t.id == lockedTrackId_);
                const double score = computeMatchScore(t, dets[di], relaxedForLocked);
                costs[ti][di] = std::isfinite(score) ? score : kInvalidCost;
            }
        }

        const auto assignment = hungarianMinimize(costs);
        // 将匈牙利算法结果转换为轨迹->检测和检测->轨迹的双向映射
        for (size_t ti = 0; ti < tracks_.size(); ++ti)
        {
            if (ti >= assignment.size())
                continue;

            const int di = assignment[ti];
            if (di < 0 || di >= static_cast<int>(dets.size()))
                continue;

            const double assignedCost = costs[ti][di];
            // 跳过无效匹配（代价为无穷大或超过未匹配代价阈值）
            if (!std::isfinite(assignedCost) ||
                assignedCost >= kInvalidCost ||
                assignedCost >= kUnassignedCost)
            {
                continue;
            }

            trackAssigned[ti] = di;
            detAssigned[di] = static_cast<int>(ti);
        }
    }

    // 更新匹配成功的轨迹：更新位置、计算速度（EMA平滑）、重置丢失计数
    for (size_t ti = 0; ti < tracks_.size(); ++ti)
    {
        auto& t = tracks_[ti];
        const int di = trackAssigned[ti];

        if (di >= 0)
        {
            const auto& d = dets[di];
            const double dt = std::clamp(
                std::chrono::duration<double>(now - t.lastUpdate).count(),
                1e-4, 0.2
            );

            const float oldX = static_cast<float>(t.pivotX);
            const float oldY = static_cast<float>(t.pivotY);
            const float newX = static_cast<float>(d.pivotX);
            const float newY = static_cast<float>(d.pivotY);
            // 计算原始速度（位移 / 时间差）
            const cv::Point2f rawVel(
                static_cast<float>((newX - oldX) / dt),
                static_cast<float>((newY - oldY) / dt)
            );

            cv::Point2f clampedRawVel = rawVel;
            const double rawSpeed = std::hypot(clampedRawVel.x, clampedRawVel.y);
            const double maxReasonableSpeed = std::max(screenWidth, screenHeight) * 3.5;
            // 限制最大速度，防止因检测抖动导致的速度异常跳变
            if (rawSpeed > maxReasonableSpeed && rawSpeed > 1e-4)
            {
                const float scale = static_cast<float>(maxReasonableSpeed / rawSpeed);
                clampedRawVel *= scale;
            }

            // 指数移动平均（EMA）平滑速度：已锁定目标平滑系数更大（更平滑）
            const float blend = (t.id == lockedTrackId_) ? 0.45f : 0.35f;
            t.velocity = t.velocity * (1.0f - blend) + clampedRawVel * blend;
            t.box = d.box;
            t.pivotX = d.pivotX;
            t.pivotY = d.pivotY;
            t.classId = d.classId;
            t.hits += 1;
            t.missed = 0;
            t.observedThisFrame = true;
            t.lastUpdate = now;
        }
        else
        {
            // 未匹配的轨迹：基于速度外推预测位置，速度衰减，递增丢失计数
            const double dt = std::clamp(
                std::chrono::duration<double>(now - t.lastUpdate).count(),
                0.0, 0.2
            );
            t.box.x += t.velocity.x * static_cast<float>(dt);
            t.box.y += t.velocity.y * static_cast<float>(dt);
            t.pivotX += t.velocity.x * dt;
            t.pivotY += t.velocity.y * dt;
            // 速度衰减因子：已锁定目标衰减更慢（0.90），保持运动趋势
            const float decay = (t.id == lockedTrackId_) ? 0.90f : 0.84f;
            t.velocity *= decay;
            t.missed += 1;
            t.observedThisFrame = false;
            t.lastUpdate = now;
        }
    }

    // 为未匹配的检测创建新轨迹
    for (size_t di = 0; di < dets.size(); ++di)
    {
        if (detAssigned[di] != -1)
            continue;

        const auto& d = dets[di];
        TrackState t;
        t.id = nextId_++;
        t.box = d.box;
        t.classId = d.classId;
        t.hits = 1;
        t.missed = 0;
        t.observedThisFrame = true;
        t.pivotX = d.pivotX;
        t.pivotY = d.pivotY;
        t.lastUpdate = now;
        tracks_.push_back(t);
    }

    // 清理死亡轨迹
    pruneDeadTracks();

    // 若锁定的目标已死亡，清除锁定状态
    if (findTrackIndexById(lockedTrackId_) < 0)
        lockedTrackId_ = -1;

    // 锁定选择逻辑：根据 keepCurrentLock 决定是否保持当前锁定
    if (!keepCurrentLock)
    {
        const int bestIdx = chooseBestTrack(screenWidth, screenHeight);
        lockedTrackId_ = (bestIdx >= 0) ? tracks_[bestIdx].id : -1;
        return;
    }

    // keepCurrentLock=true 但当前无锁定时，仍自动选一个最佳目标锁定
    if (lockedTrackId_ == -1)
    {
        const int bestIdx = chooseBestTrack(screenWidth, screenHeight);
        lockedTrackId_ = (bestIdx >= 0) ? tracks_[bestIdx].id : -1;
    }
}

/**
 * MultiTargetTracker::getLockedTarget — 获取当前锁定的目标信息
 *
 * 检查锁定目标是否存在且未超过丢失容忍帧数，若有效则将其信息填充到输出结构体中。
 *
 * @param [out] out  锁定目标信息输出结构体，包含轨迹ID、是否在当前帧被观测到、
 *                   丢失帧数和 AimbotTarget 对象
 * @return           true=锁定目标有效并已填充，false=无有效锁定目标
 */
bool MultiTargetTracker::getLockedTarget(LockedTargetInfo& out) const
{
    const int idx = findTrackIndexById(lockedTrackId_);
    if (idx < 0)
        return false;

    const auto& t = tracks_[idx];
    if (t.missed > allowedMissedFrames(t))
        return false;

    out.trackId = t.id;
    out.observedThisFrame = t.observedThisFrame;
    out.missedFrames = t.missed;
    out.target = AimbotTarget(
        static_cast<int>(std::lround(t.box.x)),
        static_cast<int>(std::lround(t.box.y)),
        static_cast<int>(std::lround(t.box.width)),
        static_cast<int>(std::lround(t.box.height)),
        t.classId,
        t.pivotX,
        t.pivotY
    );
    return true;
}

/**
 * MultiTargetTracker::getDebugTracks — 获取所有活跃轨迹的调试信息
 *
 * 遍历所有轨迹，筛选出未超过丢失容忍帧数的活跃轨迹，
 * 将轨迹的完整状态转换为 TrackDebugInfo 结构体返回，用于可视化调试。
 *
 * @return 活跃轨迹调试信息向量，每条包含ID、类别、框、支点、速度、
 *          时间戳、观测状态和锁定标记
 */
std::vector<TrackDebugInfo> MultiTargetTracker::getDebugTracks() const
{
    std::vector<TrackDebugInfo> out;
    out.reserve(tracks_.size());

    for (const auto& t : tracks_)
    {
        if (t.missed > allowedMissedFrames(t))
            continue;

        TrackDebugInfo d;
        d.trackId = t.id;
        d.classId = t.classId;
        d.box = cv::Rect(
            static_cast<int>(std::lround(t.box.x)),
            static_cast<int>(std::lround(t.box.y)),
            static_cast<int>(std::lround(t.box.width)),
            static_cast<int>(std::lround(t.box.height))
        );
        d.pivotX = t.pivotX;
        d.pivotY = t.pivotY;
        d.velocityX = t.velocity.x;
        d.velocityY = t.velocity.y;
        d.lastUpdate = t.lastUpdate;
        d.observedThisFrame = t.observedThisFrame;
        d.missedFrames = t.missed;
        d.isLocked = (t.id == lockedTrackId_);
        out.push_back(d);
    }

    return out;
}
