#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <vector>

#include "capture.h"
#include "mouse.h"
#include "Xen.h"
#include "runtime/thread_loops.h"

namespace
{
// 纯预测移动的宽容帧数：允许目标短暂丢失时继续预测
constexpr int kPredictedOnlyMoveGraceFrames = 3;
// 宽容帧数对应的秒数（基于60fps计算）
constexpr double kPredictedOnlyMoveGraceSec =
    static_cast<double>(kPredictedOnlyMoveGraceFrames) / 60.0;
// 预测移动超时附加填充时间（毫秒），补偿检测延迟
constexpr int kPredictedOnlyMoveStalePadMs = 16;

/**
 * 计算帧间隔时间（秒）
 * @param captureFpsValue 当前捕获帧率
 * @return 每帧的时间间隔（秒），限制在 15~500fps 范围内
 */
double trackerFrameIntervalSec(int captureFpsValue)
{
    const double fps = std::clamp(
        static_cast<double>((captureFpsValue > 0) ? captureFpsValue : 60),
        15.0,
        500.0);
    return 1.0 / fps;
}

/**
 * 判断是否允许基于预测的纯移动（目标被短暂遮挡时继续追踪）
 * 当目标跟踪ID与锁定ID一致、存在有效目标且错过帧数大于0时，
 * 若错过时间在宽容范围内则允许预测移动
 *
 * @param activeTrackId 当前激活的跟踪ID
 * @param hasActiveTarget 是否存在激活目标
 * @param lockInfo 锁定目标信息（包含错过帧数）
 * @param captureFpsValue 捕获帧率
 * @return true 表示允许基于预测的纯移动
 */
bool allowPredictedOnlyMove(
    int activeTrackId,
    bool hasActiveTarget,
    const LockedTargetInfo& lockInfo,
    int captureFpsValue)
{
    if (activeTrackId != lockInfo.trackId ||
        !hasActiveTarget ||
        lockInfo.missedFrames <= 0)
    {
        return false;
    }

    const double frameDtSec = trackerFrameIntervalSec(captureFpsValue);
    const double missedSec = static_cast<double>(lockInfo.missedFrames) * frameDtSec;
    return missedSec <= kPredictedOnlyMoveGraceSec + frameDtSec * 0.51;
}

/**
 * 计算目标跟踪的过期间隔（毫秒）
 * 基于帧率和宽容时间综合判断目标何时被视为过期
 * @param captureFpsValue 捕获帧率
 * @return 超时时间（毫秒），限制在 25~180ms 之间
 */
int trackerStaleTimeoutMs(int captureFpsValue)
{
    const int fps = std::max(1, captureFpsValue);
    const int frameBasedMs = 2000 / fps;
    const int graceBasedMs =
        static_cast<int>(kPredictedOnlyMoveGraceSec * 1000.0 + 0.5) +
        kPredictedOnlyMoveStalePadMs;
    return std::clamp(std::max(frameBasedMs, graceBasedMs), 25, 180);
}
}

void createInputDevices();
void assignInputDevices();
/**
 * 简易无后坐力补偿处理
 * 当"简易无后坐力"功能启用、玩家正在射击且正在开镜时，
 * 自动向下方移动鼠标以抵消后坐力
 *
 * @param mouseThread 鼠标线程对象引用
 */
void handleEasyNoRecoil(MouseThread& mouseThread)
{
    bool easyNoRecoil = false;
    int recoil_compensation = 0;
    {
        std::lock_guard<std::mutex> cfgLock(configMutex);
        easyNoRecoil = config.easynorecoil;
        recoil_compensation = static_cast<int>(config.easynorecoilstrength);
    }

    if (easyNoRecoil && shooting.load() && zooming.load())
    {
        mouseThread.moveRelative(0, recoil_compensation);
    }
}

/**
 * 鼠标主线程循环——核心瞄准逻辑
 * 负责从检测缓冲区获取目标数据、更新多目标跟踪器、
 * 处理检测分辨率与输入设备变化、执行鼠标移动和自动射击、
 * 以及简易无后坐力补偿
 *
 * @param mouseThread 鼠标线程对象引用
 */
void mouseThreadFunction(MouseThread& mouseThread)
{
    // 上一次处理的检测版本号，用于判断是否有新帧
    int lastVersion = -1;
    // 当前帧的检测框列表
    std::vector<cv::Rect> boxes;
    // 当前帧的检测类别列表
    std::vector<int> classes;
    // 当前帧检测的时间戳
    std::chrono::steady_clock::time_point detectionTimestamp{};
    // 多目标跟踪器实例，用于跨帧稳定追踪目标
    MultiTargetTracker targetTracker;
    // 当前选中的瞄准目标（可选）
    std::optional<AimbotTarget> activeTarget;
    // 当前激活的跟踪ID（非"无跟踪器"模式下使用）
    int activeTrackId = -1;
    // 当前帧是否观测到激活目标（用于自动射击判定）
    bool activeTargetObserved = false;
    // 上一帧是否处于开镜状态，用于检测状态切换
    bool wasAiming = false;
    // 已应用的检测分辨率，用于检测变化
    int appliedDetectionResolution = -1;
    // 已应用的跟踪器启用状态，用于检测变化
    bool appliedTrackerEnabled = true;
    // 上次跟踪器更新的时间点，用于超时判定
    auto lastTrackerUpdate = std::chrono::steady_clock::time_point::min();

    /**
     * 重置激活目标——清除当前瞄准目标、跟踪ID和观测状态，
     * 同时清空鼠标线程中的未来位置缓存和预测状态
     */
    auto resetActiveTarget = [&]() {
        activeTarget.reset();
        activeTrackId = -1;
        activeTargetObserved = false;
        mouseThread.clearFuturePositions();
        mouseThread.resetPrediction();
    };

    // ========== 主循环：持续处理检测结果和执行瞄准 ==========
    while (!shouldExit)
    {
        // ---- 从配置读取当前帧的各类参数 ----
        bool hasNewDetection = false;
        bool hasAimObservation = false;
        int detectionResolution = 0;
        bool disableHeadshot = false;
        bool trackerEnabled = true;
        int predictionFuturePositions = 0;
        bool autoShoot = false;

        {
            std::lock_guard<std::mutex> cfgLock(configMutex);
            detectionResolution = config.detection_resolution;
            disableHeadshot = config.disable_headshot;
            trackerEnabled = config.tracker_enabled;
            predictionFuturePositions = config.prediction_futurePositions;
            autoShoot = config.auto_shoot;
        }

        // ---- 检测开镜状态变化，切换瞄准状态时重置目标 ----
        const bool aimingNow = aiming.load();
        if (aimingNow != wasAiming)
        {
            resetActiveTarget();
            wasAiming = aimingNow;
        }

        // ---- 从检测缓冲区轮询新帧（带 1ms 等待避免忙等） ----
        {
            std::unique_lock<std::mutex> lock(detectionBuffer.mutex);
            detectionBuffer.cv.wait_for(lock, std::chrono::milliseconds(1), [&] {
                return detectionBuffer.version > lastVersion || shouldExit;
                }
            );

            if (shouldExit) break;

            if (detectionBuffer.version > lastVersion)
            {
                boxes = detectionBuffer.boxes;
                classes = detectionBuffer.classes;
                detectionTimestamp = detectionBuffer.frameTimestamp;
                lastVersion = detectionBuffer.version;
                hasNewDetection = true;
            }
        }

        // ---- 输入设备变化处理：重新创建并分配输入设备 ----
        if (input_method_changed.exchange(false))
        {
            createInputDevices();
            assignInputDevices();
        }

        // ---- 检测分辨率变化处理：重新配置鼠标线程并重置跟踪器 ----
        if (detection_resolution_changed.load() || detectionResolution != appliedDetectionResolution)
        {
            {
                std::lock_guard<std::mutex> cfgLock(configMutex);
                appliedDetectionResolution = config.detection_resolution;
                mouseThread.updateConfig(
                    config.detection_resolution,
                    config.fovX,
                    config.fovY,
                    config.minSpeedMultiplier,
                    config.maxSpeedMultiplier,
                    config.predictionInterval,
                    config.auto_shoot,
                    config.bScope_multiplier
                );
            }
            targetTracker.reset();
            {
                std::lock_guard<std::mutex> lk(g_trackerDebugMutex);
                g_trackerDebugTracks.clear();
                g_trackerLockedId = -1;
            }
            resetActiveTarget();
        }

        // ---- 跟踪器启用状态变化处理：重置跟踪器和调试信息 ----
        if (trackerEnabled != appliedTrackerEnabled)
        {
            appliedTrackerEnabled = trackerEnabled;
            targetTracker.reset();
            {
                std::lock_guard<std::mutex> lk(g_trackerDebugMutex);
                g_trackerDebugTracks.clear();
                g_trackerLockedId = -1;
            }
            resetActiveTarget();
        }

        // ========== 新检测帧处理 ==========
        if (hasNewDetection)
        {
            // ---- 跟踪器路径：使用多目标跟踪器更新和选择目标 ----
            if (trackerEnabled)
            {
                targetTracker.update(
                    boxes,
                    classes,
                    detectionResolution,
                    detectionResolution,
                    disableHeadshot,
                    aimingNow,
                    detectionTimestamp
                );
                lastTrackerUpdate = std::chrono::steady_clock::now();
                {
                    std::lock_guard<std::mutex> lk(g_trackerDebugMutex);
                    g_trackerDebugTracks = targetTracker.getDebugTracks();
                    g_trackerLockedId = targetTracker.getLockedTrackId();
                }

                LockedTargetInfo lockInfo;
                if (targetTracker.getLockedTarget(lockInfo))
                {
                    const int previousActiveTrackId = activeTrackId;
                    const bool hadActiveTarget = activeTarget.has_value();
                    // 跟踪ID切换时重置预测状态
                    if (activeTrackId != -1 && activeTrackId != lockInfo.trackId)
                    {
                        mouseThread.resetPrediction();
                        mouseThread.clearFuturePositions();
                    }

                    activeTarget = lockInfo.target;
                    activeTrackId = lockInfo.trackId;
                    activeTargetObserved = lockInfo.observedThisFrame;
                    mouseThread.setTargetDetected(true);

                    // 当前帧观测到目标：更新时间和预测未来位置
                    if (lockInfo.observedThisFrame)
                    {
                        hasAimObservation = true;
                        mouseThread.setLastTargetTime(std::chrono::steady_clock::now());

                        auto futurePositions = mouseThread.predictFuturePositions(
                            activeTarget->pivotX,
                            activeTarget->pivotY,
                            predictionFuturePositions
                        );
                        mouseThread.storeFuturePositions(futurePositions);
                    }
                    // 目标短暂丢失：若在宽容期内则仍用预测值继续追踪
                    else if (allowPredictedOnlyMove(
                        previousActiveTrackId,
                        hadActiveTarget,
                        lockInfo,
                        captureFps.load()))
                    {
                        hasAimObservation = true;

                        auto futurePositions = mouseThread.predictFuturePositions(
                            activeTarget->pivotX,
                            activeTarget->pivotY,
                            predictionFuturePositions
                        );
                        mouseThread.storeFuturePositions(futurePositions);
                    }
                }
                else
                {
                    resetActiveTarget();
                }
            }
            // ---- 非跟踪器路径（最近目标模式）：直接排序选择最近目标 ----
            else
            {
                targetTracker.reset();
                {
                    std::lock_guard<std::mutex> lk(g_trackerDebugMutex);
                    g_trackerDebugTracks.clear();
                    g_trackerLockedId = -1;
                }

                std::unique_ptr<AimbotTarget> selected(
                    sortTargets(
                        boxes,
                        classes,
                        detectionResolution,
                        detectionResolution,
                        disableHeadshot));
                lastTrackerUpdate = std::chrono::steady_clock::now();

                if (selected)
                {
                    activeTarget = *selected;
                    activeTrackId = -1;
                    activeTargetObserved = true;
                    hasAimObservation = true;
                    mouseThread.setTargetDetected(true);
                    mouseThread.setLastTargetTime(std::chrono::steady_clock::now());

                    auto futurePositions = mouseThread.predictFuturePositions(
                        activeTarget->pivotX,
                        activeTarget->pivotY,
                        predictionFuturePositions
                    );
                    mouseThread.storeFuturePositions(futurePositions);
                }
                else
                {
                    resetActiveTarget();
                }
            }
        }

        // ---- 目标过期间隔检查：超过阈值则重置目标 ----
        if (activeTarget)
        {
            const int staleMs = trackerStaleTimeoutMs(captureFps.load());
            if (std::chrono::steady_clock::now() - lastTrackerUpdate > std::chrono::milliseconds(staleMs))
            {
                resetActiveTarget();
            }
        }

        // ========== 鼠标移动和自动射击逻辑 ==========
        if (aimingNow)
        {
            // 有有效目标且本帧有观测：执行瞄准移动和自动射击
            if (activeTarget && hasAimObservation)
            {
                mouseThread.moveMousePivot(activeTarget->pivotX, activeTarget->pivotY, detectionTimestamp);

                if (autoShoot)
                {
                    // 目标被直接观测到时按下鼠标射击
                    if (activeTargetObserved)
                    {
                        mouseThread.pressMouse(*activeTarget);
                    }
                    // 目标未被观测到时释放鼠标（基于预测的纯移动）
                    else
                    {
                        mouseThread.releaseMouse();
                    }
                }
            }
            else
            {
                // 没有目标或目标不可靠：清除排队的移动
                if (!activeTarget || !activeTargetObserved)
                {
                    mouseThread.clearQueuedMoves();
                }

                if (autoShoot)
                {
                    mouseThread.releaseMouse();
                }
            }
        }
        else
        {
            // 未开镜：清除所有排队的移动并释放鼠标
            mouseThread.clearQueuedMoves();
            if (autoShoot)
            {
                mouseThread.releaseMouse();
            }
        }

        // ---- 简易无后坐力补偿（射击+开镜时自动下移鼠标） ----
        handleEasyNoRecoil(mouseThread);

        // ---- 检查并重置过期的预测状态 ----
        mouseThread.checkAndResetPredictions();
    }
}


