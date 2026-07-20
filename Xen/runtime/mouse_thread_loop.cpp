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
    FrameTiming detectionTiming;
    // 基础观测锁定器：不做速度预测或丢失滑行
    BasicTargetTracker motionLibTracker;
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
     * 同时重置基础目标滤波和控制状态
     */
    auto resetActiveTarget = [&](bool targetLost = true) {
        activeTarget.reset();
        activeTrackId = -1;
        activeTargetObserved = false;
        mouseThread.resetTracking(targetLost);
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
        bool autoShoot = false;
        bool autoDeriveTrackerParams = true;
        std::vector<float> confidences;

        {
            std::lock_guard<std::mutex> cfgLock(configMutex);
            detectionResolution = config.detection_resolution;
            disableHeadshot = config.disable_headshot;
            trackerEnabled = config.tracker_enabled;
            autoShoot = config.auto_shoot;
            autoDeriveTrackerParams = config.auto_derive_tracker_params;

        }

        // ---- 自动推导目标跟踪参数；不得覆盖用户标定的移动执行参数 ----
        {
            static int lastDerivedResolution = -1;
            static int lastDerivedFps = -1;
            static bool lastAutoDerive = false;
            // 跟踪器每次检测结果发布只推进一帧，生命周期参数必须按检测发布率
            // 换算真实时间。捕获处理率通常高于DML推理率，用它会让同样的帧数
            // 在运行时对应过长或反复变化的滑行窗口。
            int fps = detectionBuffer.getPublishFps();
            if (fps <= 0)
                fps = captureFps.load();
            if (fps <= 0) fps = 60;
            const int stableFps = std::clamp(((fps + 5) / 10) * 10, 20, 500);
            if (autoDeriveTrackerParams &&
                (detectionResolution != lastDerivedResolution ||
                 stableFps != lastDerivedFps || !lastAutoDerive))
            {
                float previousResponseMs = 0.0f;
                float previousMaxCps = 0.0f;
                {
                    std::lock_guard<std::mutex> cfgLock(configMutex);
                    previousResponseMs = config.move_response_ms;
                    previousMaxCps = config.move_max_speed_cps;
                }
                config.applyAutoDerivedTrackerParams(detectionResolution, stableFps);
                {
                    std::lock_guard<std::mutex> cfgLock(configMutex);
                    if (previousResponseMs != config.move_response_ms ||
                        previousMaxCps != config.move_max_speed_cps)
                    {
                        mouseThread.updateConfig(
                            config.detection_resolution,
                            config.fovX,
                            config.fovY,
                            config.auto_shoot,
                            config.bScope_multiplier);
                    }
                }
                lastDerivedResolution = detectionResolution;
                lastDerivedFps = stableFps;
            }
            lastAutoDerive = autoDeriveTrackerParams;

        }

        // ---- 检测瞄准状态变化 ----
        const bool aimingNow = aiming.load();
        if (aimingNow != wasAiming)
        {
            // 松开只暂停输出并保留短时同目标运动状态；重新按住时直接复用。
            // 未瞄准期间跟踪器仍更新身份，目标丢失/切换会走完整resetTracking。
            if (!aimingNow)
                mouseThread.suspendAimingOutput();
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
                detectionBuffer.swapLocked(boxes, classes, confidences, lastVersion, detectionTiming);
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
                config.auto_shoot,
                config.bScope_multiplier
            );
            motionLibTracker.reset();
            {
                std::lock_guard<std::mutex> lk(g_trackerDebugMutex);
                g_trackerDebugTracks.clear();
                g_trackerLockedId = -1;
            }
            resetActiveTarget(false);
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
            resetActiveTarget(false);
        }

        // ========== 新检测帧处理 ==========
        if (hasNewDetection)
        {
            // ---- 跟踪器路径：使用统一跟踪器更新和选择目标 ----
            if (trackerEnabled)
            {
                // === 统一跟踪器（motion_lib）路径 ===
                motionLibTracker.update(
                    boxes, classes, confidences,
                    detectionResolution, detectionResolution,
                    disableHeadshot, detectionTiming.backendReceiveTime);

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
                    // 跟踪ID切换时重置基础滤波与控制状态
                    if (activeTrackId != -1 && activeTrackId != lockInfo.trackId)
                    {
                        mouseThread.resetTracking();
                    }

                    activeTarget = lockInfo.target;
                    activeTrackId = lockInfo.trackId;
                    activeTargetObserved = lockInfo.observedThisFrame;
                    mouseThread.setTargetDetected(true);

                    // 基础阶段只接受真实检测观测；SOT 滑行帧仅维持锁定，
                    // 不生成鼠标移动，避免把预测数据混入控制测试。
                    const bool observed = lockInfo.observedThisFrame;
                    if (observed)
                    {
                        hasAimObservation = true;
                        mouseThread.setLastTargetTime(std::chrono::steady_clock::now());
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
            const int staleMs = trackerStaleTimeoutMs(
                detectionBuffer.getPublishFps());
            if (std::chrono::steady_clock::now() - lastTrackerUpdate > std::chrono::milliseconds(staleMs))
            {
                resetActiveTarget();
            }
        }

        // P0-6C：松开只暂停正式输出。只要同一跟踪目标仍有真实检测，独立影子链
        // 就继续消费观测和原始时间戳；该入口不运行legacy滤波/控制，也不访问
        // 设备发送路径。目标丢失或ID切换仍通过resetTracking完整重置两条链。
        if (!aimingNow && activeTarget && hasAimObservation)
        {
            mouseThread.observeAimPipelineOnly(
                *activeTarget, detectionTiming, activeTrackId);
        }

        // ========== 鼠标移动和自动射击逻辑 ==========
        if (aimingNow)
        {
            // 有有效目标且本帧有观测：执行瞄准移动和自动射击
            if (activeTarget && hasAimObservation)
            {
                mouseThread.moveMousePivot(*activeTarget, detectionTiming, activeTrackId);

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

    }
}
