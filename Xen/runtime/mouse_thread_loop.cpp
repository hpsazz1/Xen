#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
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

    const double frameDtSec = frameIntervalSec(captureFpsValue);
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
    static double emaRecoil = 0.0;
    static std::mt19937 rng(std::random_device{}());

    bool easyNoRecoil = false;
    float recoilStrength = 0.0f;
    float fireCorrection = 0.0f;
    {
        std::lock_guard<std::mutex> cfgLock(configMutex);
        easyNoRecoil = config.easynorecoil;
        recoilStrength = config.easynorecoilstrength;
        fireCorrection = config.fire_correction_strength;
    }

    if (easyNoRecoil && shooting.load() && zooming.load())
    {
        // 优先使用新的 AI 射击修正（EMA 平滑 + 随机微扰）
        if (fireCorrection > 0.0f)
        {
            double raw = static_cast<double>(recoilStrength) * static_cast<double>(fireCorrection);

            // 随机微扰（±10% 高斯噪声，模拟人手压枪不均匀）
            std::normal_distribution<double> jitter(0.0, raw * 0.03);
            raw += jitter(rng);

            // EMA 平滑
            const double alpha = 0.35;
            emaRecoil = emaRecoil * alpha + raw * (1.0 - alpha);

            int compensated = static_cast<int>(std::round(emaRecoil));
            if (compensated > 0)
                mouseThread.moveRelative(0, compensated);
        }
        else
        {
            // 回退到旧版：固定强度垂直下移
            int recoil = static_cast<int>(recoilStrength);
            mouseThread.moveRelative(0, recoil);
        }
    }
    else
    {
        // 不在射击状态时重置 EMA
        emaRecoil = 0.0;
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
    // 移动控制库跟踪器实例（统一跟踪层）
    MotionLibTargetTracker motionLibTracker;
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
    // 初始化为当前时间，避免 time_point::min() 导致的算术溢出风险
    auto lastTrackerUpdate = std::chrono::steady_clock::now();

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
        // ---- 从配置读取当前帧的各类参数（一次加锁读取全部） ----
        bool hasNewDetection = false;
        bool hasAimObservation = false;
        int detectionResolution = 0;
        bool disableHeadshot = false;
        bool trackerEnabled = true;
        int predictionFuturePositions = 0;
        bool autoShoot = false;

        // 当前 ML 参数指纹（与下方跟踪器参数检测共用锁）
        struct MlFingerprint {
            int confirm_threshold, termination_frames, coast_frames;
            float noise_vx, noise_vy, noise_w, noise_h, measurement_stddev;
            float recapture_iou, recapture_distance_mult, coast_velocity_decay;
            bool operator==(const MlFingerprint& o) const {
                return confirm_threshold == o.confirm_threshold
                    && termination_frames == o.termination_frames
                    && coast_frames == o.coast_frames
                    && noise_vx == o.noise_vx
                    && noise_vy == o.noise_vy
                    && noise_w == o.noise_w
                    && noise_h == o.noise_h
                    && measurement_stddev == o.measurement_stddev
                    && recapture_iou == o.recapture_iou
                    && recapture_distance_mult == o.recapture_distance_mult
                    && coast_velocity_decay == o.coast_velocity_decay;
            }
        };
        MlFingerprint mlFingerprint{};

        {
            std::lock_guard<std::mutex> cfgLock(configMutex);
            detectionResolution = config.detection_resolution;
            disableHeadshot = config.disable_headshot;
            trackerEnabled = config.tracker_enabled;
            predictionFuturePositions = config.prediction_futurePositions;
            autoShoot = config.auto_shoot;

            mlFingerprint.confirm_threshold    = config.ml_confirm_threshold;
            mlFingerprint.termination_frames   = config.ml_termination_frames;
            mlFingerprint.noise_vx             = config.ml_noise_vx;
            mlFingerprint.noise_vy             = config.ml_noise_vy;
            mlFingerprint.noise_w              = config.ml_noise_w;
            mlFingerprint.noise_h              = config.ml_noise_h;
            mlFingerprint.measurement_stddev   = config.ml_measurement_stddev;
            mlFingerprint.coast_frames         = config.ml_coast_frames;
            mlFingerprint.recapture_iou        = config.ml_recapture_iou;
            mlFingerprint.recapture_distance_mult = config.ml_recapture_distance_mult;
            mlFingerprint.coast_velocity_decay = config.ml_coast_velocity_decay;
        }

        // ---- 检测跟踪器参数变化，更新时重新配置并重置 ----
        // 首次运行时应用自动推导参数（由 Config::applyAutoDerivedTrackerParams 在加载时处理）
        {
            static bool autoDerived = false;
            if (!autoDerived || detectionResolution != appliedDetectionResolution)
            {
                autoDerived = true;
                int fps = captureFps.load();
                if (fps <= 0) fps = 60;
                config.applyAutoDerivedTrackerParams(detectionResolution, fps);
            }
            
            static MlFingerprint lastMlFingerprint{};
            
            if (!(mlFingerprint == lastMlFingerprint) || !motionLibTracker.isConfigured())
            {
                lastMlFingerprint = mlFingerprint;
                
                {
                    std::lock_guard<std::mutex> cfgLock(configMutex);
                    motionLibTracker.configure(
                        config.ml_confirm_threshold,
                        config.ml_termination_frames,
                        config.ml_noise_vx, config.ml_noise_vy,
                        config.ml_noise_w, config.ml_noise_h,
                        config.ml_measurement_stddev,
                        config.ml_coast_frames,
                        "nearest",
                        config.ml_recapture_iou,
                        config.ml_recapture_distance_mult,
                        config.ml_coast_velocity_decay);
                }
                {
                    std::lock_guard<std::mutex> lk(g_trackerDebugMutex);
                    g_trackerDebugTracks.clear();
                    g_trackerLockedId = -1;
                }
                resetActiveTarget();
            }
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
                std::vector<float> dummyConf;
                detectionBuffer.swapLocked(boxes, classes, dummyConf, lastVersion, detectionTimestamp);
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
            }
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
            motionLibTracker.reset();
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
            motionLibTracker.reset();
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
            // ---- 跟踪器路径：使用统一跟踪器更新和选择目标 ----
            if (trackerEnabled)
            {
                // === 统一跟踪器（motion_lib）路径 ===
                motionLibTracker.update(
                    boxes, classes,
                    detectionResolution, detectionResolution,
                    disableHeadshot, aimingNow, detectionTimestamp);

                lastTrackerUpdate = std::chrono::steady_clock::now();
                {
                    std::lock_guard<std::mutex> lk(g_trackerDebugMutex);
                    g_trackerDebugTracks = motionLibTracker.getDebugTracks();
                    g_trackerLockedId = motionLibTracker.getLockedTrackId();
                }

                LockedTargetInfo lockInfo;
                bool hasLock = motionLibTracker.getLockedTarget(lockInfo);

                if (hasLock)
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

                    // 向 MouseThread 注入外部速度估计（来自 SOT Kalman）
                    // 注意: SOT Kalman 速度为 px/frame，此处转换为 px/sec
                    // 以便 predict_target_position 做时间外推和 PurePursuit 做速度前馈
                    // 即使目标未被直接观测（滑行中），SOT 仍提供速度估计
                    {
                        auto [vx, vy] = motionLibTracker.getLockedVelocity();
                        double fps = static_cast<double>(std::max(1, captureFps.load()));
                        mouseThread.setExternalTargetVelocity(
                            static_cast<double>(vx) * fps,   // px/frame → px/sec
                            static_cast<double>(vy) * fps);
                    }

                    // 当前帧观测到目标或短暂丢失在宽容期内：更新预测
                    const bool observed = lockInfo.observedThisFrame;
                    const bool predictedOk = !observed && allowPredictedOnlyMove(
                        previousActiveTrackId, hadActiveTarget, lockInfo, captureFps.load());

                    if (observed || predictedOk)
                    {
                        hasAimObservation = true;
                        // 观测到目标或预测移动有效时都更新时间戳，
                        // 防止 SOT 滑行期间预测超时误重置
                        if (observed || predictedOk)
                            mouseThread.setLastTargetTime(std::chrono::steady_clock::now());

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
                motionLibTracker.reset();
                {
                    std::lock_guard<std::mutex> lk(g_trackerDebugMutex);
                    g_trackerDebugTracks.clear();
                    g_trackerLockedId = -1;
                }

                auto selected = sortTargets(
                        boxes,
                        classes,
                        detectionResolution,
                        detectionResolution,
                        disableHeadshot);
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
                mouseThread.moveMousePivot(activeTarget->pivotX, activeTarget->pivotY, detectionTimestamp, activeTarget->classId);

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


