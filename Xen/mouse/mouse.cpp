// ============================================================================
// mouse.cpp — Xen 鼠标控制核心模块
//
// 本文件是 Xen 基础鼠标控制模块，当前主链路只负责：
//   - 检测观测的自适应滤波（不做未来位置预测）
//   - 帧率无关的误差响应与设备计数限速
//   - 运动补偿：记录已发送的鼠标移动，用于补偿因自瞄自身移动造成的视差变化
//   - 多鼠标设备支持：通过 IMouseInput 接口抽象，支持多种底层鼠标驱动
//     （物理鼠标、虚拟驱动、原始输入等），支持运行时热切换
//   - 线程安全：移动指令通过队列投递到后台工作线程，避免阻塞主循环
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <cmath>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <atomic>
#include <vector>
#include <iostream>
#include <random>

#include "mouse.h"
#include "capture.h"
#include "capture/ndi_capture.h"
#include "Xen.h"
#include "debug/pipeline_tracer.h"
#include "runtime/aim_coordinate_space.h"
#include "runtime/thread_loops.h"

/**
 * MouseThread 构造函数
 *
 * 初始化鼠标控制线程的核心参数：
 *
 *   - resolution / screen_width / screen_height: 检测裁剪分辨率，用于目标中心和稳定半径
 *   - fovX / fovY: 视场角（度），用于屏幕像素与游戏内角度的换算
 *   - min/maxSpeedMultiplier: 速度曲线的最小/最大倍率
 *   - auto_shoot: 自瞄时是否自动开火
 *   - bScope_multiplier: 瞄准范围判定系数，用于目标在准星内的检测
 *   - mouseInputDevice: 鼠标输入设备接口（支持运行时热切换）
 *
 * 初始化过程：
 *   1. 从配置读取轨迹模拟参数（wind_G=鼠标移速 / wind_W=轨迹摆动 / wind_M=单步上限 / wind_D=微调距离）
 *   2. 重置轨迹模拟运动状态
 *   3. 清空轨迹模拟调试轨迹
 *   4. 初始化卡尔曼滤波器
 *   5. 启动后台工作线程（moveWorkerLoop），用于异步处理鼠标移动指令
 */
MouseThread::MouseThread(
    int resolution,
    int fovX,
    int fovY,
    bool auto_shoot,
    float bScope_multiplier,
    IMouseInput* mouseInputDevice)
    : screen_width(resolution),
    screen_height(resolution),
    fov_x(fovX),
    fov_y(fovY),
    max_distance(std::hypot(resolution, resolution) / 2.0),
    center_x(resolution / 2.0),
    center_y(resolution / 2.0),
    auto_shoot(auto_shoot),
    bScope_multiplier(bScope_multiplier),
    mouseInput(mouseInputDevice)
{
    last_target_time = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(configMutex);
        move_response_seconds = static_cast<double>(config.move_response_ms) / 1000.0;
        move_max_speed_cps = static_cast<double>(config.move_max_speed_cps);
        move_catch_up_max_speed_cps = std::clamp(
            static_cast<double>(config.move_catch_up_max_speed_cps),
            move_max_speed_cps, 4000.0);
        predictionSettings.enabled = config.prediction_enabled;
        predictionSettings.additionalLeadSeconds =
            static_cast<double>(config.prediction_lead_ms) / 1000.0;
        predictionSettings.velocityTimeConstantSeconds =
            static_cast<double>(config.prediction_velocity_tau_ms) / 1000.0;
        predictionSettings.predictionStrength =
            static_cast<double>(config.prediction_strength);
        aimPipelineRuntime.configure(config.aim_pipeline_mode);
        LosAimController::Settings shadowControllerSettings;
        shadowControllerSettings.responseSeconds = config.aim_shadow_response_ms / 1000.0;
        shadowControllerSettings.maxCountsPerSecond = config.aim_shadow_max_speed_cps;
        shadowControllerSettings.feedforwardGain = config.aim_shadow_feedforward_gain;
        shadowControllerSettings.settleErrorDegrees = config.aim_shadow_settle_error_deg;
        shadowControllerSettings.settleRateDegreesPerSecond = config.aim_shadow_settle_rate_dps;
        shadowControllerSettings.reverseConfirmationSeconds =
            config.aim_shadow_reverse_confirm_ms / 1000.0;
        shadowControllerSettings.verticalCatchUpErrorDegrees =
            config.aim_shadow_vertical_catch_up_deg;
        shadowControllerSettings.integralTimeSeconds = config.aim_shadow_integral_time_ms / 1000.0;
        shadowControllerSettings.integralZoneDegrees = config.aim_shadow_integral_zone_deg;
        shadowControllerSettings.leadHorizonSeconds = config.aim_shadow_lead_horizon_ms / 1000.0;
        shadowControllerSettings.leadStrength = config.aim_shadow_lead_strength;
        aimPipelineRuntime.configureController(shadowControllerSettings);
        CommandTrajectoryShaper::Settings trajectorySettings;
        trajectorySettings.mode = parseTrajectoryShaperMode(config.trajectory_shaper_mode);
        trajectorySettings.maxVelocityCountsPerSecond = config.trajectory_max_velocity_cps;
        trajectorySettings.maxAccelerationCountsPerSecond2 = config.trajectory_max_acceleration_cps2;
        trajectorySettings.maxJerkCountsPerSecond3 = config.trajectory_max_jerk_cps3;
        OutputScheduler::Settings schedulerSettings;
        schedulerSettings.outputHz = config.trajectory_output_hz;
        aimPipelineRuntime.configureTrajectory(trajectorySettings, schedulerSettings);
        motionCompensationHistory.configure(
            config.aim_motion_compensation_delay_ms,
            config.aim_motion_compensation_response_ms);
        appliedViewMotionModel.configure(config.aim_shadow_command_to_frame_delay_ms);
        profileCalibrator.setEnabled(config.profile_calibration_enabled);
        refreshGameProfileCache();  // 必须在锁内调用（读取 config.game_profiles）
    }

    // 启动后台工作线程，负责从队列中取出移动指令并发送到驱动
    moveWorker = std::thread(&MouseThread::moveWorkerLoop, this);
}

/**
 * updateConfig
 *
 * 运行时更新鼠标控制参数。由外部配置变更时调用。
 * 重置所有运动相关的内部状态以确保参数变更立即生效。
 *
 * @param resolution  屏幕分辨率（宽=高，假设方形）
 * @param fovX        水平视场角
 * @param fovY        垂直视场角
 * @param auto_shoot          是否自动开火
 * @param bScope_multiplier   瞄准范围系数
 */
void MouseThread::updateConfig(
    int resolution,
    int fovX,
    int fovY,
    bool auto_shoot,
    float bScope_multiplier
)
{
    // 注意：调用者（keyboard_listener / mouse_thread_loop / 构造函数）必须已持有 configMutex
    // 此处不再重复加锁，避免 std::mutex 同一线程重入导致未定义行为

    screen_width = screen_height = resolution;
    fov_x = fovX;  fov_y = fovY;
    move_response_seconds = std::clamp(
        static_cast<double>(config.move_response_ms) / 1000.0, 0.020, 0.300);
    move_max_speed_cps = std::clamp(
        static_cast<double>(config.move_max_speed_cps), 30.0, 4000.0);
    move_catch_up_max_speed_cps = std::clamp(
        static_cast<double>(config.move_catch_up_max_speed_cps),
        move_max_speed_cps, 4000.0);
    move_integral_time_seconds = std::clamp(
        static_cast<double>(config.move_integral_time_ms) / 1000.0, 0.0, 1.0);
    predictionSettings.enabled = config.prediction_enabled;
    predictionSettings.additionalLeadSeconds = std::clamp(
        static_cast<double>(config.prediction_lead_ms) / 1000.0, 0.0, 0.100);
    predictionSettings.velocityTimeConstantSeconds = std::clamp(
        static_cast<double>(config.prediction_velocity_tau_ms) / 1000.0, 0.040, 0.120);
    predictionSettings.predictionStrength = std::clamp(
        static_cast<double>(config.prediction_strength), 0.0, 4.0);
    aimPipelineRuntime.configure(config.aim_pipeline_mode);
    LosAimController::Settings shadowControllerSettings;
    shadowControllerSettings.responseSeconds = config.aim_shadow_response_ms / 1000.0;
    shadowControllerSettings.maxCountsPerSecond = config.aim_shadow_max_speed_cps;
    shadowControllerSettings.feedforwardGain = config.aim_shadow_feedforward_gain;
    shadowControllerSettings.settleErrorDegrees = config.aim_shadow_settle_error_deg;
    shadowControllerSettings.settleRateDegreesPerSecond = config.aim_shadow_settle_rate_dps;
    shadowControllerSettings.reverseConfirmationSeconds =
        config.aim_shadow_reverse_confirm_ms / 1000.0;
    shadowControllerSettings.verticalCatchUpErrorDegrees =
        config.aim_shadow_vertical_catch_up_deg;
    shadowControllerSettings.integralTimeSeconds = config.aim_shadow_integral_time_ms / 1000.0;
    shadowControllerSettings.integralZoneDegrees = config.aim_shadow_integral_zone_deg;
    shadowControllerSettings.leadHorizonSeconds = config.aim_shadow_lead_horizon_ms / 1000.0;
    shadowControllerSettings.leadStrength = config.aim_shadow_lead_strength;
    aimPipelineRuntime.configureController(shadowControllerSettings);
    CommandTrajectoryShaper::Settings trajectorySettings;
    trajectorySettings.mode = parseTrajectoryShaperMode(config.trajectory_shaper_mode);
    trajectorySettings.maxVelocityCountsPerSecond = config.trajectory_max_velocity_cps;
    trajectorySettings.maxAccelerationCountsPerSecond2 = config.trajectory_max_acceleration_cps2;
    trajectorySettings.maxJerkCountsPerSecond3 = config.trajectory_max_jerk_cps3;
    OutputScheduler::Settings schedulerSettings;
    schedulerSettings.outputHz = config.trajectory_output_hz;
    aimPipelineRuntime.configureTrajectory(trajectorySettings, schedulerSettings);
    {
        std::lock_guard<std::mutex> lock(motionCompensationMutex);
        motionCompensationHistory.configure(
            config.aim_motion_compensation_delay_ms,
            config.aim_motion_compensation_response_ms);
    }
    appliedViewMotionModel.configure(config.aim_shadow_command_to_frame_delay_ms);
    profileCalibrator.setEnabled(config.profile_calibration_enabled);
    if (config.profile_calibration_enabled)
        profileCalibrator.reset();
    this->auto_shoot = auto_shoot;
    this->bScope_multiplier = bScope_multiplier;

    center_x = center_y = resolution / 2.0;
    max_distance = std::hypot(resolution, resolution) / 2.0;

    resetTracking();

    // 开火拟人化
    trigger_stable_frames = config.trigger_stable_frames;
    trigger_random_delay_ms = config.trigger_random_delay_ms;
    trigger_delay_jitter_ms = config.trigger_delay_jitter_ms;
    trigger_hold_ms = config.trigger_hold_ms;
    trigger_hold_jitter_ms = config.trigger_hold_jitter_ms;
    trigger_shot_cooldown_ms = config.trigger_shot_cooldown_ms;

    // 自动急停 / 解锁Y / 射击修正
    auto_stop_enabled = config.auto_stop_enabled;
    auto_stop_hold_ms = config.auto_stop_hold_ms;
    unlock_y_enabled = config.unlock_y_enabled;
    unlock_y_threshold_ms = config.unlock_y_threshold_ms;
    unlock_y_strength = config.unlock_y_strength;
    fire_correction_strength = config.fire_correction_strength;

    refreshGameProfileCache();
}

/**
 * ~MouseThread 析构函数
 *
 * 通知工作线程停止，并等待其安全退出。
 * 设置 workerStop 标志后 notify_all 唤醒可能阻塞在条件变量上的工作线程。
 */
MouseThread::~MouseThread()
{
    workerStop = true;
    queueCv.notify_all();
    if (moveWorker.joinable()) moveWorker.join();
}

/**
 * queueMove
 *
 * 线程安全地将鼠标移动指令投递到队列。
 * 由生产者（主循环/自瞄逻辑）调用，消费者是后台工作线程。
 *
 * 特性：
 *   - 空移动（dx=0, dy=0）直接丢弃
 *   - 队列满时丢弃最旧的指令（防止积压导致滞后）
 *   - 入队后通知工作线程
 *
 * @param dx 水平移动量（鼠标计数）
 * @param dy 垂直移动量（鼠标计数）
 */
ViewCommandSample MouseThread::queueMove(int dx, int dy)
{
    ViewCommandSample sample;
    sample.requestedCountsX = dx;
    sample.requestedCountsY = dy;
    if (dx == 0 && dy == 0)
    {
        return sample;
    }

    // Worker 线程已崩溃，丢弃移动指令避免队列无限积压
    if (!workerRunning.load())
        return sample;

    std::lock_guard lg(queueMtx);
    if (moveQueue.size() >= queueLimit)
    {
        g_pipelineTracer.recordCommandDropped(moveQueue.front().timing.sequence);
        moveQueue.pop();
    }
    sample.sequence = moveSequence.fetch_add(1, std::memory_order_relaxed) + 1;
    sample.enqueueTime = FrameTiming::Clock::now();
    sample.enqueueSucceeded = true;
    moveQueue.push({ dx, dy, moveCancellationEpoch.capture(), sample });
    queueCv.notify_one();
    return sample;
}

/**
 * moveWorkerLoop
 *
 * 后台工作线程主循环。负责：
 *   1. 等待条件变量通知（新指令入队或停止信号）
 *   2. 批量取出队列中的所有移动指令
 *   3. 通过 sendMovementToDriver 将移动发送到底层鼠标设备
 *   4. 记录轨迹模拟调试轨迹
 *
 * 使用 unique_lock 配合条件变量实现高效等待，避免忙轮询。
 * 异常时捕获并输出错误信息，线程不会因此崩溃退出。
 */
void MouseThread::moveWorkerLoop()
{
    try
    {
        while (!workerStop)
        {
            std::unique_lock ul(queueMtx);
            queueCv.wait(ul, [&] { return workerStop || !moveQueue.empty(); });

            while (!moveQueue.empty())
            {
                Move m = moveQueue.front();
                moveQueue.pop();
                ul.unlock();                      // 发送时释放锁，允许继续入队
                sendMovementToDriver(std::move(m));
                ul.lock();
            }
        }
    }
    catch (const std::exception& e)
    {
        workerRunning.store(false);
        std::cerr << "[Mouse] Move worker crashed: " << e.what() << std::endl;
    }
    catch (...)
    {
        workerRunning.store(false);
        std::cerr << "[Mouse] Move worker crashed: unknown exception." << std::endl;
    }
}

/**
 * windMouseMoveRelative
 *
 * 轨迹模拟相对移动算法 —— 核心函数。
 * 将目标鼠标位移分解为多个子步，每个子步叠加扰动，模拟人类操作的
 * 自然曲线轨迹而非机械的直线移动。
 *
 * 算法细节：
 *
 *   1. 轨迹模拟累积（Carry）：
 *      将原始位移 dx/dy 累加到 windCarryX/windCarryY 中，
 *      后续子步逐渐消耗这些累积量。
 *
 *   2. 子步分解（Substep Decomposition）：
 *      基于累积位移幅度计算子步数（最大 5 步），每一步独立模拟物理过程。
 *      当剩余距离 < 0.20 且速度 < 0.12 时提前终止。
 *
 *   3. 拉力（Pull Force）：
 *      将累积位移归一化后乘以拉力增益 baseG，形成朝向目标点的牵引力。
 *      距离越近归一化距离越大，拉力增益在 [0.25, 1.0] * baseG 范围内变化。
 *
 *   4. 随机噪声：
 *      使用均匀分布 [-1, 1] 的随机数生成噪声，经指数平滑（0.72 衰减因子）
 *      后叠加到移动力中，产生随机扰动感。
 *
 *   5. 模式振荡（Pattern Oscillation）：
 *      使用两个独立的正弦波，频率以黄金比例约数（1.61803398875）偏移，
 *      水平和垂直方向使用不同的相位偏移系数（0.79、1.17）。
 *      振荡幅度随归一化距离在 [0.05, 0.55] * baseW 范围内变化。
 *      模式与噪声按 42% 比例融合为最终合力。
 *
 *   6. 速度阻尼与上限（Velocity Damping & Capping）：
 *      每一步的速度乘以阻尼系数 drag = 0.82 + 0.10 * (1 - normDist)，
 *      距离越近阻尼越大。速度上限 vCap 在 [0.65, baseM] 之间随距离变化，
 *      超过上限时乘以随机裁剪系数 [0.55, 1.0]。
 *
 *   7. 分数累积（Fractional Accumulation）：
 *      使用 windFracX/windFracY 累积亚像素级精度，
 *      当累积值跨过整数阈值时才发出一个像素的移动指令。
 *
 *   8. 最终限幅：
 *      所有子步完成后，若累积剩余位移超过 120，则等比缩小。
 *
 * 参数 wind_G/W/M/D 在构造函数和 updateConfig 中从全局配置加载：
 *   wind_G: 鼠标移速系数（G越大鼠标移向目标越快越直）
 *   wind_W: 轨迹摆动幅度（W越大移动路径越弯曲）
 *   wind_M: 单步移动上限（M越大每帧能移动的像素越多）
 *   wind_D: 微调距离阈值（靠近目标后切换为精细微调的距离）
 */
#if 0 // Trajectory simulation is intentionally excluded from the basic stage.
void MouseThread::bezierMoveRelative(int dx, int dy)
{
    ++trajectoryFrameCounter;
    Trajectory::bezierMove(
        dx,
        dy,
        bezier_strength,
        trajectoryBezierState,
        trajectoryFrameCounter,
        [this](int stepX, int stepY) { queueMove(stepX, stepY); });
    return;

#if 0 // Legacy inline implementation retained temporarily for review; trajectory.h is authoritative.
    if (dx == 0 && dy == 0) return;

    static int frameCounter = 0;
    frameCounter++;

    const double dist = std::hypot(dx, dy);
    int steps = std::max(3, static_cast<int>(std::ceil(dist / 6.0)));
    steps = std::min(steps, 14);

    // 使用共享的 Perlin 风格平滑噪声
    double side1 = filters::perlinNoise(frameCounter, 1.7);
    double side2 = filters::perlinNoise(frameCounter, 2.3);

    double nx = -dy / dist;
    double ny =  dx / dist;
    double offset = dist * static_cast<double>(bezier_strength) * 0.65;

    // 三次贝塞尔: P0=(0,0), CP1, CP2, P3=(dx,dy)
    double cp1x = dx * 0.33 + nx * offset * side1;
    double cp1y = dy * 0.33 + ny * offset * side1;
    double cp2x = dx * 0.67 + nx * offset * side2;
    double cp2y = dy * 0.67 + ny * offset * side2;

    for (int i = 1; i <= steps; ++i)
    {
        double t = static_cast<double>(i) / steps;
        double t2 = t * t, t3 = t2 * t;
        double mt = 1.0 - t, mt2 = mt * mt, mt3 = mt2 * mt;
        double bx = 3.0 * mt2 * t * cp1x + 3.0 * mt * t2 * cp2x + t3 * dx;
        double by = 3.0 * mt2 * t * cp1y + 3.0 * mt * t2 * cp2y + t3 * dy;

        double pt = static_cast<double>(i - 1) / steps;
        double pt2 = pt * pt, pt3 = pt2 * pt;
        double pmt = 1.0 - pt, pmt2 = pmt * pmt, pmt3 = pmt2 * pmt;
        double pbx = 3.0 * pmt2 * pt * cp1x + 3.0 * pmt * pt2 * cp2x + pt3 * dx;
        double pby = 3.0 * pmt2 * pt * cp1y + 3.0 * pmt * pt2 * cp2y + pt3 * dy;

        // 亚像素累积：将浮点差值累加到余量缓冲区，跨整数阈值时发出
        bezierFracX += (bx - pbx);
        bezierFracY += (by - pby);
        int stepX = static_cast<int>(std::round(bezierFracX));
        int stepY = static_cast<int>(std::round(bezierFracY));
        if (stepX != 0 || stepY != 0)
        {
            bezierFracX -= static_cast<double>(stepX);
            bezierFracY -= static_cast<double>(stepY);
            queueMove(stepX, stepY);
        }
    }
#endif
}

void MouseThread::windMouseMoveRelative(int dx, int dy)
{
    Trajectory::windMouseMove(
        dx,
        dy,
        wind_G,
        wind_W,
        wind_M,
        wind_D,
        trajectoryWindState,
        [this](int stepX, int stepY) { queueMove(stepX, stepY); });
    return;

#if 0 // Legacy inline implementation retained temporarily for review; trajectory.h is authoritative.
    if (dx == 0 && dy == 0)
        return;

    // 将本次位移累加到轨迹模拟累积量中
    windCarryX += static_cast<double>(dx);
    windCarryY += static_cast<double>(dy);

    // 钳位参数到安全范围
    const double baseG = std::clamp(wind_G, 0.05, 50.0);
    const double baseW = std::clamp(wind_W, 0.0, 80.0);
    const double baseM = std::max(1.0, wind_M);
    const double baseD = std::max(1.0, wind_D);

    // ---- 算法常数（WindMouse 仿真模型经验参数）----
    // 拉力: pullGainMin/Max 控制子步向目标的牵引力度
    constexpr double kPullGainMin = 0.25, kPullGainMax = 0.75;
    // 噪声: 幅度范围 [0.15, 0.85]*baseW, 衰减因子控制随机漫步平滑度
    constexpr double kNoiseAmpMin = 0.15, kNoiseAmpMax = 0.85;
    constexpr double kNoiseSmooth = 0.72, kNoiseBlend = 0.28;
    // 缓动区间: 距目标 <3px 时零噪声防止终点死锁抖动; <50px 线性削弱
    constexpr double kNoiseZeroDist = 3.0, kNoiseRampDist = 50.0;
    // 模式振荡: blend 范围 [0.12, 0.20]; 模式融合比 0.42 (噪声+模式)
    constexpr double kPatternBlendMin = 0.12, kPatternBlendMax = 0.20;
    constexpr double kPatternMix = 0.42;
    // 振荡幅度: [0.05, 0.55]*baseW; 频率随机漂移 ±0.004; 频率范围 [0.025, 0.280]
    constexpr double kOscAmpMin = 0.05, kOscAmpMax = 0.55;
    constexpr double kRateDrift = 0.004, kRateMin = 0.025, kRateMax = 0.280;
    // 步调: [0.20, 0.95] 控制频率随距离变化
    constexpr double kTempoMin = 0.20, kTempoMax = 0.95;
    // 速度阻尼: [0.82, 0.92] 距离越近阻尼越大; 上限 [0.30, 0.70]*baseM, 最低 0.65
    constexpr double kDragMin = 0.82, kDragMax = 0.92, kDragExtra = 0.10;
    constexpr double kCapMin = 0.65, kCapScaleMin = 0.30, kCapScaleMax = 0.70;
    // 相位偏移: 0.79/1.17/0.35/0.48 调谐双正弦相位差
    constexpr double kPhaseAmpA = 0.79, kPhaseAmpB = 1.17;
    constexpr double kPhaseOffA = 0.35, kPhaseOffB = 0.48;
    constexpr double kOscBScale = 0.58;   // oscBX 和 oscBY 的叠加权重
    // 子步数映射: floor(mag*0.24)+1, 最多 5 步; 终止阈值 0.20/0.12
    constexpr double kSubstepScale = 0.24;
    constexpr int    kSubstepMax = 5;
    constexpr double kStopDist = 0.20, kStopVel = 0.12;
    // 累积安全帽: 500 counts ≈ 大角度拉枪上限
    constexpr double kCarryCap = 500.0;
    // 黄金比例: 1.618... 用于双正弦频率偏移; 2*pi
    constexpr double kGoldenRatio = 1.61803398875;
    constexpr double kTwoPi = 6.28318530717958647692;

    // 随机分布生成器
    static thread_local std::uniform_real_distribution<double> noiseDist(-1.0, 1.0);
    static thread_local std::uniform_real_distribution<double> clipDist(0.55, 1.0);

    // 根据累积位移幅度计算子步数，步数 = max(1, min(5, floor(mag * 0.24) + 1))
    const double carryMag = std::hypot(windCarryX, windCarryY);
    const int maxSubsteps = std::clamp(static_cast<int>(carryMag * 0.24) + 1, 1, 5);

    for (int i = 0; i < maxSubsteps; ++i)
    {
        const double dist = std::hypot(windCarryX, windCarryY);
        const double velMag = std::hypot(windVelX, windVelY);

        // 剩余距离和速度都很小时提前结束，但先刷新已累积的亚像素余量
        if (dist < 0.20 && velMag < 0.12)
        {
            int flushX = static_cast<int>(std::round(windFracX));
            int flushY = static_cast<int>(std::round(windFracY));
            if (flushX != 0 || flushY != 0)
            {
                windFracX -= static_cast<double>(flushX);
                windFracY -= static_cast<double>(flushY);
                windCarryX -= static_cast<double>(flushX);
                windCarryY -= static_cast<double>(flushY);
                queueMove(flushX, flushY);
            }
            break;
        }

        // 将距离归一化到 [0, 1]，用于参数插值
        const double normDist = std::clamp(dist / baseD, 0.0, 1.0);
        // 微扰尾段归零：贴近目标时自动削弱噪声，杜绝死锁颤抖
        const double zeroFactor = (dist < 3.0) ? 0.0 : std::min(dist / 50.0, 1.0);
        // 拉力增益：距离越近增益越大，范围 [0.25, 1.0] * baseG
        const double pullGain = baseG * (0.25 + 0.75 * normDist);
        // 噪声幅度：距离越近噪声越大，范围 [0.15, 0.85] * baseW
        const double noiseAmp = baseW * (0.15 + 0.85 * normDist) * zeroFactor;

        // 计算指向目标方向的拉力向量
        double pullX = 0.0;
        double pullY = 0.0;
        if (dist > 1e-8)
        {
            pullX = windCarryX / dist * pullGain;
            pullY = windCarryY / dist * pullGain;
        }

        // 使用随机漫步更新两个模式的频率
        windPatternRateA = std::clamp(windPatternRateA + noiseDist(windRng) * 0.004, 0.025, 0.280);
        windPatternRateB = std::clamp(windPatternRateB + noiseDist(windRng) * 0.004, 0.025, 0.280);

        // 步调系数随距离变化，使振荡频率与距离相关
        const double stepTempo = 0.20 + 0.95 * normDist;
        windPatternPhaseA += windPatternRateA * stepTempo;
        windPatternPhaseB += windPatternRateB * stepTempo;
        if (windPatternPhaseA > kTwoPi) windPatternPhaseA = std::fmod(windPatternPhaseA, kTwoPi);
        if (windPatternPhaseB > kTwoPi) windPatternPhaseB = std::fmod(windPatternPhaseB, kTwoPi);

        // 双正弦波模式振荡，使用黄金比例（1.618...）偏移产生非重复波形
        const double oscAX = std::sin(windPatternPhaseA);
        const double oscBX = std::sin(windPatternPhaseB + 1.61803398875);
        const double oscAY = std::cos(windPatternPhaseA * 0.79 + 0.35);
        const double oscBY = std::cos(windPatternPhaseB * 1.17 - 0.48);

        // 模式振荡幅度与距离相关
        const double patternAmp = baseW * (0.05 + 0.55 * normDist);
        const double patternTargetX = (oscAX + 0.58 * oscBX) * patternAmp;
        const double patternTargetY = (oscAY + 0.58 * oscBY) * patternAmp;
        const double patternBlend = 0.12 + 0.20 * normDist;
        // 指数平滑更新模式状态
        windPatternX = windPatternX * (1.0 - patternBlend) + patternTargetX * patternBlend;
        windPatternY = windPatternY * (1.0 - patternBlend) + patternTargetY * patternBlend;

        // 随机噪声：指数平滑（0.72 衰减）的随机扰动
        windNoiseX = windNoiseX * 0.72 + noiseDist(windRng) * noiseAmp * 0.28;
        windNoiseY = windNoiseY * 0.72 + noiseDist(windRng) * noiseAmp * 0.28;

        // 合力 = 噪声 + 模式振荡 * 0.42
        const double windForceX = windNoiseX + windPatternX * 0.42;
        const double windForceY = windNoiseY + windPatternY * 0.42;

        // 阻尼系数：距离越近阻尼越大，范围 [0.82, 0.92]
        const double drag = 0.82 + (1.0 - normDist) * 0.10;
        // 更新速度：旧速度衰减 + 拉力 + 合力
        windVelX = windVelX * drag + pullX + windForceX;
        windVelY = windVelY * drag + pullY + windForceY;

        // 速度上限裁剪：根据距离动态调整，附加随机裁剪系数
        const double vCap = std::max(0.65, baseM * (0.30 + 0.70 * normDist));
        const double newVelMag = std::hypot(windVelX, windVelY);
        if (newVelMag > vCap)
        {
            const double clip = vCap * clipDist(windRng);
            windVelX = (windVelX / newVelMag) * clip;
            windVelY = (windVelY / newVelMag) * clip;
        }

        // 分数累积：将浮点速度累积到分数缓冲区
        windFracX += windVelX;
        windFracY += windVelY;

        // 四舍五入取出整数步
        int stepX = static_cast<int>(std::round(windFracX));
        int stepY = static_cast<int>(std::round(windFracY));
        if (stepX == 0 && stepY == 0)
            continue;  // 未达到一个像素，继续累积

        // 从分数缓冲区减去已取出的整数部分
        windFracX -= static_cast<double>(stepX);
        windFracY -= static_cast<double>(stepY);
        // 从累积位移中减去已发送的部分
        windCarryX -= static_cast<double>(stepX);
        windCarryY -= static_cast<double>(stepY);
        queueMove(stepX, stepY);  // 投递到工作队列
    }

    // 最终限幅：防止极端情况下累积位移无限增长。
    // 上限 500 counts ≈ 一次大角度拉枪的合理上限，超出部分等比缩小。
    // 注意：正常帧间位移远低于此值，此处仅作安全网。
    const double carryCap = 500.0;
    const double finalCarryMag = std::hypot(windCarryX, windCarryY);
    if (finalCarryMag > carryCap)
    {
        const double s = carryCap / finalCarryMag;
        windCarryX *= s;
        windCarryY *= s;
    }
#endif
}

/**
 * resetWindState
 *
 * 重置轨迹模拟算法的所有内部状态变量。
 * 在参数变更、队列清空或切换场景时调用，确保轨迹模拟运动从干净状态重新开始。
 *
 * 重置内容包括：
 *   - 累积位移（windCarry）、当前速度（windVel）归零
 *   - 噪声缓冲（windNoise）、分数累积（windFrac）归零
 *   - 振荡模式状态（windPattern）归零
 *   - 两个振荡模式的相位在 [0, 2pi] 内随机初始化
 *   - 两个振荡模式的频率在 [0.04, 0.16] 内随机初始化
 */
void MouseThread::resetWindState()
{
    constexpr double kTwoPi = 6.28318530717958647692;
    constexpr double kRateMin = 0.025;
    std::uniform_real_distribution<double> phaseDist(0.0, kTwoPi);
    std::uniform_real_distribution<double> rateDist(kRateMin * 1.6, kRateMin * 4.0);

    trajectoryWindState = Trajectory::WindState{};
    trajectoryWindState.patternPhaseA = phaseDist(trajectoryWindState.rng);
    trajectoryWindState.patternPhaseB = phaseDist(trajectoryWindState.rng);
    trajectoryWindState.patternRateA = rateDist(trajectoryWindState.rng);
    trajectoryWindState.patternRateB = rateDist(trajectoryWindState.rng);
    trajectoryBezierState = Trajectory::BezierState{};
    trajectoryFrameCounter = 0;

    windCarryX = 0.0;
    windCarryY = 0.0;
    windVelX = 0.0;
    windVelY = 0.0;
    windNoiseX = 0.0;
    windNoiseY = 0.0;
    windFracX = 0.0;
    windFracY = 0.0;
    windPatternX = 0.0;
    windPatternY = 0.0;
    windPatternPhaseA = phaseDist(windRng);  // 随机初始化相位 A
    windPatternPhaseB = phaseDist(windRng);  // 随机初始化相位 B
    windPatternRateA = rateDist(windRng);    // 随机初始化频率 A
    windPatternRateB = rateDist(windRng);    // 随机初始化频率 B
    bezierFracX = 0.0;                       // 重置贝塞尔亚像素余量
    bezierFracY = 0.0;
}

/**
 * appendWindDebugStep
 *
 * 将一次鼠标移动记录到轨迹模拟调试轨迹中，用于可视化展示鼠标路径。
 * 将移动计数（counts）转换为屏幕像素后追加到轨迹队列。
 *
 * 如果轨迹为空，则以屏幕中心作为起点。
 * 轨迹总点数上限为 220，超出时自动移除最早的点。
 *
 * @param dx 水平移动量（鼠标计数）
 * @param dy 垂直移动量（鼠标计数）
 */
void MouseThread::appendWindDebugStep(int dx, int dy)
{
    if (dx == 0 && dy == 0)
        return;

    auto delta = mouseCountsToScreenPixels(dx, dy);
    double deltaPxX = delta.first;
    double deltaPxY = delta.second;
    if (std::abs(deltaPxX) < 1e-8 && std::abs(deltaPxY) < 1e-8)
        return;
    std::lock_guard<std::mutex> lock(windDebugTrailMutex);
    const auto now = std::chrono::steady_clock::now();
    pruneWindDebugTrailLocked(now);  // 清理过期点

    if (windDebugTrail.empty())
    {
        windDebugCursorX = center_x;
        windDebugCursorY = center_y;
        windDebugTrail.push_back({ windDebugCursorX, windDebugCursorY, now });
    }

    windDebugCursorX += deltaPxX;
    windDebugCursorY += deltaPxY;
    windDebugTrail.push_back({ windDebugCursorX, windDebugCursorY, now });

    constexpr size_t maxTrailPoints = 220;
    while (windDebugTrail.size() > maxTrailPoints)
        windDebugTrail.pop_front();
}

/**
 * pruneWindDebugTrailLocked
 *
 * 清理轨迹模拟调试轨迹中超过生命周期（900 毫秒）的旧点。
 * 调用者必须已持有 windDebugTrailMutex 锁。
 *
 * @param now 当前时间点，用于比较判断是否过期
 */
void MouseThread::pruneWindDebugTrailLocked(const std::chrono::steady_clock::time_point& now)
{
    constexpr auto windTrailLifetime = std::chrono::milliseconds(900);
    while (!windDebugTrail.empty() && (now - windDebugTrail.front().t) > windTrailLifetime)
        windDebugTrail.pop_front();
}
#endif

/**
 * refreshGameProfileCache - 刷新缓存的游戏配置值
 *
 * 从 config 中读取当前活跃游戏的灵敏度、yaw/pitch、FOV 缩放参数，
 * 缓存到成员变量中，避免 mouseCountsToScreenPixels 每次移动都加锁查询。
 * 调用者必须已持有 configMutex。
 */
void MouseThread::refreshGameProfileCache()
{
    // 注意：调用者（构造函数/updateConfig）必须已持有 configMutex
    const Config::GameProfile* gpPtr = nullptr;
    auto activeIt = config.game_profiles.find(config.active_game);
    if (activeIt != config.game_profiles.end())
        gpPtr = &activeIt->second;
    else
    {
        auto unifiedIt = config.game_profiles.find("UNIFIED");
        if (unifiedIt != config.game_profiles.end())
            gpPtr = &unifiedIt->second;
    }

    if (gpPtr)
    {
        cachedGameSens = gpPtr->sens;
        cachedGameYaw = gpPtr->yaw;
        cachedGamePitch = gpPtr->pitch;
        cachedGameFovScaled = gpPtr->fovScaled;
        cachedGameBaseFOV = gpPtr->baseFOV;
    }

    move_response_seconds = std::clamp(
        static_cast<double>(config.move_response_ms) / 1000.0, 0.020, 0.300);
    move_max_speed_cps = std::clamp(
        static_cast<double>(config.move_max_speed_cps), 30.0, 4000.0);
    move_catch_up_max_speed_cps = std::clamp(
        static_cast<double>(config.move_catch_up_max_speed_cps),
        move_max_speed_cps, 4000.0);
    move_integral_time_seconds = std::clamp(
        static_cast<double>(config.move_integral_time_ms) / 1000.0, 0.0, 1.0);
}

/**
 * mouseCountsToScreenPixels
 *
 * 将鼠标计数（counts）转换为屏幕像素坐标。
 * 转换过程涉及多个游戏相关的缩放因子：
 *
 *   1. 查找当前活跃游戏配置（或 UNIFIED 回退配置）
 *   2. 使用游戏的 sensitivity（灵敏度）、yaw（水平旋转系数）、
 *      pitch（垂直旋转系数）将 counts 转换为角度偏移（度）
 *   3. 如果配置了 FOV 缩放，根据当前 FOV 与基准 FOV 的比例缩放
 *   4. 使用 screen_width/screen_height 与 fov_x/fov_y 计算每像素对应角度，
 *      将角度偏移转换为像素偏移
 *
 * 这个转换对于运动补偿和调试可视化至关重要，因为它建立了
 * "鼠标物理移动"与"屏幕上像素变化"之间的桥梁。
 *
 * @param dx 水平鼠标计数
 * @param dy 垂直鼠标计数
 * @return 对应的屏幕像素偏移量 (pixelX, pixelY)
 */
std::pair<double, double> MouseThread::mouseCountsToScreenPixels(int dx, int dy) const
{
    double deltaPxX = static_cast<double>(dx);
    double deltaPxY = static_cast<double>(dy);

    if (cachedGameSens != 0.0 && cachedGameYaw != 0.0 && cachedGamePitch != 0.0)
    {
        const double fovNow = std::max(1.0, fov_x);
        const double fovScale = (cachedGameFovScaled && cachedGameBaseFOV > 1.0) ? (fovNow / cachedGameBaseFOV) : 1.0;
        const double degX = static_cast<double>(dx) * cachedGameSens * cachedGameYaw * fovScale;
        const double degY = static_cast<double>(dy) * cachedGameSens * cachedGamePitch * fovScale;

        // 检测画面是原始源画面的中心裁剪，单个检测像素仍对应一个源像素。
        // FOV 覆盖的是完整源画面，不能用 320 等检测裁剪宽度换算，否则会把 counts/px 高估数倍。
        const double sourcePixelWidth = AimCoordinateSpace::resolveFovPixelSpan(
            ::screenWidth.load(std::memory_order_relaxed), screen_width);
        const double sourcePixelHeight = AimCoordinateSpace::resolveFovPixelSpan(
            ::screenHeight.load(std::memory_order_relaxed), screen_height);
        deltaPxX = AimCoordinateSpace::sourcePixelDeltaForAngleDegrees(
            degX, fov_x, sourcePixelWidth);
        deltaPxY = AimCoordinateSpace::sourcePixelDeltaForAngleDegrees(
            degY, fov_y, sourcePixelHeight);
    }

    return { deltaPxX, deltaPxY };
}

/**
 * recordMotionCompensationStep
 *
 * 记录一次鼠标移动，用于运动补偿。
 * 运动补偿的目的是消除自瞄自身移动对目标位置的干扰：
 * 当自瞄系统移动鼠标旋转视角时，目标的屏幕坐标会发生变化，
 * 这种变化应该被追踪并从目标预测中减去。
 *
 * 记录的移动量首先转换为屏幕像素，然后存储到环形缓冲中。
 * 缓冲上限为 512 个采样点。
 *
 * @param dx 水平鼠标计数
 * @param dy 垂直鼠标计数
 */
void MouseThread::recordMotionCompensationStep(
    int dx, int dy,
    std::chrono::steady_clock::time_point sendTime)
{
    if (dx == 0 && dy == 0)
        return;

    const auto delta = mouseCountsToScreenPixels(dx, dy);
    if (std::abs(delta.first) < 1e-8 && std::abs(delta.second) < 1e-8)
        return;

    std::lock_guard<std::mutex> lock(motionCompensationMutex);
    motionCompensationHistory.add(delta.first, delta.second, sendTime);
}

std::pair<double, double> MouseThread::currentDegreesPerCount() const
{
    const double fovScale = (cachedGameFovScaled && cachedGameBaseFOV > 1.0)
        ? (fov_x / cachedGameBaseFOV) : 1.0;
    return {
        cachedGameSens * cachedGameYaw * fovScale,
        cachedGameSens * cachedGamePitch * fovScale
    };
}

/**
 * getMotionCompensationSince
 *
 * 获取从指定时间点以来累积的运动补偿偏移量。
 * 用于在 pivot 模式中减去自瞄自身旋转导致的视差变化。
 *
 * @param since 起始时间点（若为零时间则返回 {0, 0}）
 * @return 从该时间点起的累积屏幕像素偏移 (x, y)
 */
std::pair<double, double> MouseThread::getMotionCompensationSince(
    std::chrono::steady_clock::time_point since) const
{
    if (since.time_since_epoch().count() == 0)
        return { 0.0, 0.0 };

    std::lock_guard<std::mutex> lock(motionCompensationMutex);
    return motionCompensationHistory.since(since);
}

std::pair<double, double> MouseThread::getMotionCompensationAt(
    std::chrono::steady_clock::time_point time) const
{
    std::lock_guard<std::mutex> lock(motionCompensationMutex);
    return motionCompensationHistory.at(time);
}

std::pair<double, double> MouseThread::filter_target_position(
    double target_x, double target_y,
    std::chrono::steady_clock::time_point observationTime,
    bool useMotionTrend,
    double maxObservationGapSeconds)
{
    lastFilterResult = targetFilter.update(
        target_x, target_y, observationTime,
        frameIntervalSec(captureFps.load()), screen_width, useMotionTrend,
        maxObservationGapSeconds);
    return { lastFilterResult.x, lastFilterResult.y };
}

/**
 * sendMovementToDriver
 *
 * 将鼠标移动指令发送到底层鼠标设备驱动。
 * 通过 IMouseInput 接口进行抽象，支持多种实现：
 *   - 物理鼠标硬件移动
 *   - 虚拟鼠标驱动
 *   - Windows 原始输入 API
 *
 * 每次成功发送后自动记录运动补偿步骤。
 *
 * @param dx 水平移动量（鼠标计数）
 * @param dy 垂直移动量（鼠标计数）
 */
void MouseThread::sendMovementToDriver(Move move)
{
    // clearQueuedMoves()不仅清空尚在队列内的命令，还会切换取消代次。
    // Worker可能已经把旧命令弹出并释放queueMtx；设备调用前必须再次核对，
    // 否则松开瞄准后仍可能多发送最后1~3 counts，形成每轮末尾的轻微拖尾。
    if (!moveCancellationEpoch.isCurrent(move.cancellationToken))
    {
        g_pipelineTracer.recordCommandDropped(move.timing.sequence);
        return;
    }

    move.timing.sendAttempted = true;
    if (move.dx == 0 && move.dy == 0)
    {
        g_pipelineTracer.recordCommandResult(move.timing);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(inputDevicesMutex);

        if (!mouseInput)
        {
            g_pipelineTracer.recordCommandResult(move.timing);
            return;
        }

        // 等待设备锁期间也可能发生目标重置；在不可撤销的驱动调用前做最终核对。
        if (!moveCancellationEpoch.isCurrent(move.cancellationToken))
        {
            move.timing.sendAttempted = false;
            move.timing.droppedBeforeSend = true;
            g_pipelineTracer.recordCommandResult(move.timing);
            return;
        }

        if (!mouseInput->move(move.dx, move.dy))
        {
            g_pipelineTracer.recordCommandResult(move.timing);
            return;
        }
    }

    const auto sendTime = std::chrono::steady_clock::now();
    move.timing.deviceSendTime = sendTime;
    move.timing.appliedCountsX = move.dx;
    move.timing.appliedCountsY = move.dy;
    move.timing.sendSucceeded = true;
    profileCalibrator.recordCommand(move.dx, move.dy, sendTime);
    recordMotionCompensationStep(move.dx, move.dy, sendTime);
    const auto degreesPerCount = currentDegreesPerCount();
    appliedViewMotionModel.addCommand(
        move.dx, move.dy,
        degreesPerCount.first, degreesPerCount.second,
        sendTime);
    g_pipelineTracer.recordCommandResult(move.timing);
}

PassiveProfileCalibrator::Snapshot MouseThread::getProfileCalibrationSnapshot() const
{
    return profileCalibrator.snapshot();
}

void MouseThread::resetProfileCalibration()
{
    profileCalibrator.reset();
}

/**
 * moveRelative
 *
 * 简单的相对移动封装，通过队列投递到工作线程。
 * 与 moveMousePivot 使用同一条队列，保证移动指令全局有序。
 *
 * @param dx 水平移动量
 * @param dy 垂直移动量
 */
void MouseThread::moveRelative(int dx, int dy)
{
    queueMove(dx, dy);
}

#if 0 // Legacy speed-multiplier conversion removed from the active base controller.
/**
 * calc_movement
 *
 * 根据目标在屏幕上的偏移量计算所需的鼠标移动计数（counts）。
 * 这是自瞄核心的坐标转换链路：屏幕像素 -> 角度 -> 鼠标计数。
 *
 * 转换步骤：
 *   1. 计算目标中心到屏幕中心的像素偏移
 *   2. 通过 calculate_speed_multiplier 获取速度倍率（基于距离的速度曲线）
 *   3. 根据 FOV 和分辨率将像素偏移转换为角度偏移（度）
 *   4. 使用 30 FPS 基准进行帧率补偿（corr = 30/fps），
 *      较高帧率时减小移动量以保持一致性
 *   5. 通过 config.degToCounts 将角度转换为鼠标计数，应用速度倍率和帧率修正
 *
 * @param tx 目标预测位置的 X 坐标
 * @param ty 目标预测位置的 Y 坐标
 * @return 需要移动的鼠标计数 (move_x, move_y)，可能为负值
 */
std::pair<double, double> MouseThread::calc_movement(double tx, double ty)
{
    double offx = tx - center_x;
    double offy = ty - center_y;
    double dist = std::hypot(offx, offy);
    double speed = calculate_speed_multiplier(dist);

    // 每像素对应的角度（度）
    double degPerPxX = fov_x / screen_width;
    double degPerPxY = fov_y / screen_height;

    // 像素偏移转换为角度偏移
    double mmx = offx * degPerPxX;
    double mmy = offy * degPerPxY;

    // The controller runs once per captured frame. Normalize the legacy
    // absolute-position path to a 30 FPS reference so higher capture rates do
    // not multiply the total movement per second.
    const double fps = std::clamp(static_cast<double>(captureFps.load()), 1.0, 500.0);
    const double frameScale = 30.0 / fps;

    // 使用缓存的游戏配置计算 counts（避免每帧加锁 configMutex）
    double scale = (cachedGameFovScaled && cachedGameBaseFOV > 1.0) ? (fov_x / cachedGameBaseFOV) : 1.0;
    double cx = mmx / (cachedGameSens * cachedGameYaw * scale);
    double cy = mmy / (cachedGameSens * cachedGamePitch * scale);
    double move_x = cx * speed * frameScale;
    double move_y = cy * speed * frameScale;

    return { move_x, move_y };
}

/**
 * pixelDeltaToCounts
 *
 * 将像素空间的位移增量转换为鼠标计数（counts）。
 * 这是 calc_movement 中坐标转换链的复用：像素 → 角度 → 计数。
 * 与 calc_movement 的区别：输入是增量而非绝对坐标，且速度曲线由调用者传入。
 *
 * 转换步骤：
 *   1. 像素增量 × 每像素角度 = 角度偏移（度）
 *   2. 角度偏移 / (灵敏度 × yaw/pitch × FOV缩放) = 鼠标计数
 *   3. 乘以速度曲线倍率和帧率修正
 *
 * @param dpx                水平像素增量
 * @param dpy                垂直像素增量
 * @param speedCurveMultiplier 速度曲线倍率（由 calculate_speed_multiplier 预计算）
 * @return 对应的鼠标计数 (countX, countY)
 */
std::pair<double, double> MouseThread::pixelDeltaToCounts(
    double dpx, double dpy, double speedCurveMultiplier)
{
    // 每像素对应的角度（度）
    double degPerPxX = fov_x / std::max(1.0, screen_width);
    double degPerPxY = fov_y / std::max(1.0, screen_height);

    // 像素增量 → 角度增量
    double degX = dpx * degPerPxX;
    double degY = dpy * degPerPxY;

    // Keep output rate stable when the control loop runs at different FPS.
    const double fps = std::clamp(static_cast<double>(captureFps.load()), 1.0, 500.0);
    const double frameScale = 30.0 / fps;

    // FOV 缩放修正
    double scale = (cachedGameFovScaled && cachedGameBaseFOV > 1.0)
        ? (fov_x / cachedGameBaseFOV) : 1.0;

    // 角度 → 鼠标计数
    double cx = degX / (cachedGameSens * cachedGameYaw * scale);
    double cy = degY / (cachedGameSens * cachedGamePitch * scale);

    cx *= speedCurveMultiplier * frameScale;
    cy *= speedCurveMultiplier * frameScale;

    return { cx, cy };
}

/**
 * calculate_speed_multiplier
 *
 * 根据目标与屏幕中心的距离计算速度倍率。
 * 实现三级速度曲线：
 *
 *   1. Snap 半径（snapRadius）：
 *      距离小于 snapRadius 时使用最小速度倍率乘以 snap 加成系数，
 *      实现"吸附"效果——目标很近时精确定位。
 *
 *   2. 近端半径（nearRadius）：
 *      距离在 snapRadius 和 nearRadius 之间时，使用指数曲线插值：
 *        t = distance / nearRadius
 *        curve = 1 - (1 - t)^exponent
 *        speed = min + (max - min) * curve
 *      指数越大，曲线越偏向"在远端保持高速，近端才减速"。
 *
 *   3. 远端区域：
 *      距离超过 nearRadius 时，线性映射到 [min, max] 范围。
 *
 * @param distance 目标到屏幕中心的距离（像素）
 * @return 速度倍率 [min_speed_multiplier, max_speed_multiplier]
 */
double MouseThread::calculate_speed_multiplier(double distance)
{
    return computeSpeedMultiplier(
        distance, max_distance,
        cachedSnapRadius, cachedNearRadius, cachedSpeedCurveExponent, cachedSnapBoostFactor,
        min_speed_multiplier, max_speed_multiplier);
}
#endif

/**
 * check_target_in_scope
 *
 * 检查目标是否在瞄准范围（scope）内。
 * 以目标中心为基准，按 reduction_factor 缩小目标包围盒，
 * 判断屏幕中心（准星）是否落在这个缩小的包围盒内。
 *
 * 用于自动开火的触发判定：仅当准星真正对准目标时才允许开火。
 *
 * @param target_x          目标包围盒左上角 X
 * @param target_y          目标包围盒左上角 Y
 * @param target_w          目标包围盒宽度
 * @param target_h          目标包围盒高度
 * @param reduction_factor  缩小系数（值越小要求准星越精确对准）
 * @return 准星是否在目标瞄准范围内
 */
bool MouseThread::check_target_in_scope(double target_x, double target_y, double target_w, double target_h, double reduction_factor)
{
    double center_target_x = target_x + target_w / 2.0;
    double center_target_y = target_y + target_h / 2.0;

    // 按缩小系数计算缩小后的半宽/半高
    double reduced_w = target_w * (reduction_factor / 2.0);
    double reduced_h = target_h * (reduction_factor / 2.0);

    double x1 = center_target_x - reduced_w;
    double x2 = center_target_x + reduced_w;
    double y1 = center_target_y - reduced_h;
    double y2 = center_target_y + reduced_h;

    return (center_x > x1 && center_x < x2 && center_y > y1 && center_y < y2);
}

/**
 * moveMouse
 *
 * 自瞄主入口（非 pivot 路径）：根据 AimbotTarget 执行一次鼠标移动。
 * 注意：此路径不经过 PurePursuit 控制器，也不支持运动补偿。
 * 当前主循环使用 moveMousePivot（完整管线），此函数为简化备用路径。
 *
 * 流程：
 *   1. 锁定输入方法互斥锁
 *   2. 计算目标中心坐标
 *   3. 使用 EMA 滤波预测目标位置
 *   4. 计算所需移动量（像素→counts，含灵敏度/FOV/帧率修正）
 *   5. 投递到工作队列
 *
 * @param target 目标检测结果（包含包围盒和位置信息）
 */
void MouseThread::moveMouse(const AimbotTarget& target)
{
    moveMousePivot(target);
}

/**
 * moveMousePivot
 *
 * 基于旋转支点（pivot）的自瞄移动。数据流：
 *
 *   1. 运动补偿：减去自观测时间以来自瞄自身旋转造成的屏幕偏移
 *   2. 目标预测：EMA + 常速外推，补偿检测延迟
 *   3. 速度曲线：基于目标距离预计算三段式速度倍率
 *   4. PurePursuit 控制器：像素空间误差 → 像素空间增量（含增益/死区/平滑/前馈）
 *   5. 坐标转换（pixelDeltaToCounts）：像素增量 → 鼠标计数
 *      （应用灵敏度/FOV缩放/帧率补偿/速度曲线）
 *   6. EMA 输出平滑（可选）
 *   7. 开火解锁 Y 轴（可选）
 *   8. 惯性过冲模拟（可选）
 *   9. 轨迹分发：Bezier / WindMouse / 直通
 *
 * @param target        目标框、类别和 pivot 坐标
 * @param observationTime  检测观测时间戳（用于运动补偿）
 */
void MouseThread::moveMousePivot(
    const AimbotTarget& target,
    FrameTiming frameTiming,
    int targetTrackId)
{
    std::lock_guard lg(input_method_mutex);

    const double pivotX = target.pivotX;
    const double pivotY = target.pivotY;
    const auto observationTime = frameTiming.backendReceiveTime;

    PipelineFrame traceFrame;
    PipelineFrame* pf = nullptr;
    if (g_pipelineTracer.isEnabled())
    {
        traceFrame = g_pipelineTracer.beginFrame(static_cast<int>(screen_width));
        pf = &traceFrame;
        pf->rawPivotX = pivotX;
        pf->rawPivotY = pivotY;
        pf->targetBoxX = target.x;
        pf->targetBoxY = target.y;
        pf->targetBoxWidth = target.w;
        pf->targetBoxHeight = target.h;
        pf->targetClassId = target.classId;
        pf->targetDetected = true;
        pf->fpsValue = static_cast<double>(captureFps.load());
        pf->inferenceFps = detectionBuffer.getPublishFps();
#ifndef USE_CUDA
        if (dml_detector)
        {
            const DirectMLDetector::TimingSnapshot timing = dml_detector->getTimingSnapshot();
            pf->dmlPreprocessMs = timing.preprocessMs;
            pf->dmlTensorSetupMs = timing.tensorSetupMs;
            pf->dmlInferenceMs = timing.inferenceMs;
            pf->dmlCopyMs = timing.copyMs;
            pf->dmlPostprocessMs = timing.postprocessMs;
            pf->dmlNmsMs = timing.nmsMs;
            pf->dmlTotalMs = timing.totalMs;
            pf->dmlModel = timing.modelPath;
            pf->dmlInputWidth = timing.inputWidth;
            pf->dmlInputHeight = timing.inputHeight;
        }
#endif
        const CaptureSourceDiagnostics sourceDiagnostics = GetCaptureSourceDiagnostics();
        pf->sourceDeclaredFps = sourceDiagnostics.declaredFps;
        pf->sourceReceiveFps = sourceDiagnostics.receiveFps;
        pf->sourceReceivedFrames = sourceDiagnostics.receivedFrames;
        pf->sourceDroppedFrames = sourceDiagnostics.droppedFrames;
        const NdiCaptureDiagnostics ndiDiagnostics = NDICapture::GetDiagnostics();
        pf->ndiDeclaredFps = ndiDiagnostics.declaredFps;
        pf->ndiReceiveFps = ndiDiagnostics.receiveFps;
        pf->ndiReceivedFrames = ndiDiagnostics.receivedFrames;
        pf->ndiDroppedFrames = ndiDiagnostics.droppedFrames;
        pf->sourceWidth = ::screenWidth.load(std::memory_order_relaxed);
        pf->sourceHeight = ::screenHeight.load(std::memory_order_relaxed);
        if (observationTime.time_since_epoch().count() != 0)
        {
            const double age = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - observationTime).count();
            if (std::isfinite(age) && age >= 0.0)
                pf->observationAgeSec = age;
        }
    }
    auto commitTrace = [&]() {
        if (pf)
        {
            g_pipelineTracer.commitFrame(std::move(traceFrame));
            pf = nullptr;
        }
    };

    const auto controlTime = std::chrono::steady_clock::now();
    // 短暂松开期间跟踪器仍持续确认同一目标，但控制输出已暂停。若在350 ms内
    // 重新按住，允许滤波和预测用跨暂停区间的真实观测继续估速；更长暂停仍按
    // 冷启动处理，避免复用已经过时或可能被玩家手动转向污染的运动状态。
    constexpr double kQuickResumeMaximumGapSeconds = 0.35;
    double maximumObservationGapSeconds = 0.10;
    bool quickResumeCandidate = false;
    if (predictionResumePending)
    {
        const double suspendedSeconds = std::chrono::duration<double>(
            controlTime - aimingOutputSuspendedAt).count();
        if (std::isfinite(suspendedSeconds) && suspendedSeconds >= 0.0 &&
            suspendedSeconds <= kQuickResumeMaximumGapSeconds)
        {
            maximumObservationGapSeconds = kQuickResumeMaximumGapSeconds;
            quickResumeCandidate = true;
        }
        else
        {
            targetFilter.reset();
            targetPredictor.reset();
            lastFilterResult = {};
            lastPredictionResult = {};
            quickResumeCatchUpUntil = {};
        }
        predictionResumePending = false;
    }
    const auto effectiveObservationTime = observationTime.time_since_epoch().count() == 0
        ? controlTime : observationTime;
    frameTiming.observationTime = effectiveObservationTime;
    frameTiming.controlTime = controlTime;
    if (pf)
    {
        pf->frameTiming = frameTiming;
        if (frameTiming.backendReceiveTime.time_since_epoch().count() != 0)
            pf->observationAgeSec = std::chrono::duration<double>(
                controlTime - frameTiming.backendReceiveTime).count();
    }
    AimObservation shadowObservation;
    shadowObservation.timing = frameTiming;
    if (shadowObservation.timing.sourceWidth <= 0)
        shadowObservation.timing.sourceWidth = ::screenWidth.load(std::memory_order_relaxed);
    if (shadowObservation.timing.sourceHeight <= 0)
        shadowObservation.timing.sourceHeight = ::screenHeight.load(std::memory_order_relaxed);
    shadowObservation.trackId = targetTrackId;
    shadowObservation.classId = target.classId;
    shadowObservation.confidence = target.confidence;
    shadowObservation.pivotX = pivotX;
    shadowObservation.pivotY = pivotY;
    shadowObservation.boxX = target.x;
    shadowObservation.boxY = target.y;
    shadowObservation.boxWidth = target.w;
    shadowObservation.boxHeight = target.h;
    shadowObservation.valid = true;
    aimPipelineRuntime.observe(shadowObservation);
    if (aimPipelineRuntime.effectiveMode() == AimPipelineMode::Shadow)
    {
        const double sourceWidth = AimCoordinateSpace::resolveFovPixelSpan(
            shadowObservation.timing.sourceWidth, screen_width);
        const double sourceHeight = AimCoordinateSpace::resolveFovPixelSpan(
            shadowObservation.timing.sourceHeight, screen_height);
        const auto measuredLos = AimCoordinateSpace::pixelOffsetToLosAngles(
            pivotX - center_x,
            pivotY - center_y,
            fov_x,
            fov_y,
            sourceWidth,
            sourceHeight);
        const auto cameraAtObservation = appliedViewMotionModel.at(effectiveObservationTime);
        const auto cameraAtControl = appliedViewMotionModel.at(controlTime);
        const auto degreesPerCount = currentDegreesPerCount();

        ViewMotionShadowDiagnostics diagnostics;
        diagnostics.valid = true;
        diagnostics.commandToFrameDelayMs = appliedViewMotionModel.commandToFrameDelayMs();
        diagnostics.degreesPerCountX = degreesPerCount.first;
        diagnostics.degreesPerCountY = degreesPerCount.second;
        diagnostics.measuredLosYawDegrees = measuredLos.yawDegrees;
        diagnostics.measuredLosPitchDownDegrees = measuredLos.pitchDownDegrees;
        diagnostics.appliedCameraYawAtObservationDegrees = cameraAtObservation.first;
        diagnostics.appliedCameraPitchAtObservationDegrees = cameraAtObservation.second;
        diagnostics.appliedCameraYawAtControlDegrees = cameraAtControl.first;
        diagnostics.appliedCameraPitchAtControlDegrees = cameraAtControl.second;
        diagnostics.stabilizedLosYawDegrees = measuredLos.yawDegrees + cameraAtObservation.first;
        diagnostics.stabilizedLosPitchDownDegrees = measuredLos.pitchDownDegrees + cameraAtObservation.second;
        diagnostics.relativeErrorYawDegrees =
            diagnostics.stabilizedLosYawDegrees - cameraAtControl.first;
        diagnostics.relativeErrorPitchDownDegrees =
            diagnostics.stabilizedLosPitchDownDegrees - cameraAtControl.second;
        aimPipelineRuntime.setViewMotionDiagnostics(diagnostics);
    }
    const AimPipelineFrameState shadowFrame = aimPipelineRuntime.snapshot();
    if (pf)
        pf->aimPipeline = shadowFrame;
    const double calibrationSourceWidth = AimCoordinateSpace::resolveFovPixelSpan(
        ::screenWidth.load(std::memory_order_relaxed), screen_width);
    const double calibrationSourceHeight = AimCoordinateSpace::resolveFovPixelSpan(
        ::screenHeight.load(std::memory_order_relaxed), screen_height);
    profileCalibrator.addObservation(
        pivotX, pivotY, effectiveObservationTime,
        calibrationSourceWidth, calibrationSourceHeight, fov_x, fov_y);
    const PassiveProfileCalibrator::Snapshot calibration =
        profileCalibrator.snapshot();
    const auto viewAtObservation = getMotionCompensationAt(effectiveObservationTime);
    const auto viewAtControl = getMotionCompensationAt(controlTime);
    const double stabilizedPivotX = pivotX + viewAtObservation.first;
    const double stabilizedPivotY = pivotY + viewAtObservation.second;
    const bool useMotionTrend =
        std::abs(lastPredictionResult.offsetX) +
        std::abs(lastPredictionResult.offsetY) > 1e-6;
    const auto filtered = filter_target_position(
        stabilizedPivotX, stabilizedPivotY, effectiveObservationTime, useMotionTrend,
        maximumObservationGapSeconds);
    // 100 ms快速追赶结束后，相机命令还需约一个命令到画面延迟才能完全反映在
    // 检测坐标中。额外覆盖50 ms尾迹，避免把已知控制响应误判为目标高速或反向。
    constexpr auto kQuickResumePredictionTail = std::chrono::milliseconds(50);
    const bool preserveMatureDirectionDuringCatchUp = quickResumeCandidate ||
        (quickResumeCatchUpUntil.time_since_epoch().count() != 0 &&
         controlTime < quickResumeCatchUpUntil + kQuickResumePredictionTail);
    lastPredictionResult = targetPredictor.update(
        filtered.first, filtered.second, effectiveObservationTime,
        controlTime, screen_width, predictionSettings,
        maximumObservationGapSeconds, preserveMatureDirectionDuringCatchUp);
    const double observedScreenAtObservationX = filtered.first - viewAtObservation.first;
    const double observedScreenAtObservationY = filtered.second - viewAtObservation.second;
    bool selfMotionArtifactDetected = false;
    if (predictionObservationHistory.size() >= 3 && lastPredictionResult.directionLocked)
    {
        const auto& historyStart = predictionObservationHistory.front();
        const double screenDeltaX =
            observedScreenAtObservationX - historyStart.screenX;
        const double screenDeltaY =
            observedScreenAtObservationY - historyStart.screenY;
        const double viewDeltaX = viewAtObservation.first - historyStart.viewX;
        const double viewDeltaY = viewAtObservation.second - historyStart.viewY;
        const bool rawSelfMotionArtifact = TargetPredictor::isSelfMotionArtifact(
            observedScreenAtObservationX - center_x,
            observedScreenAtObservationY - center_y,
            screenDeltaX, screenDeltaY,
            viewDeltaX, viewDeltaY,
            lastPredictionResult.velocityX,
            lastPredictionResult.velocityY);
        // 单帧伪迹也可能是真实 jump/反转的高速观测。连续两帧才确认，
        // 避免一次误判撤销前瞻并让高速目标脱离控制。
        selfMotionArtifactDetected = rawSelfMotionArtifact && previousSelfMotionArtifact;
        previousSelfMotionArtifact = rawSelfMotionArtifact;
    }
    else
    {
        previousSelfMotionArtifact = false;
    }
    targetPredictor.applySelfMotionSuppression(
        lastPredictionResult, selfMotionArtifactDetected);
    // 只有预测器已经确认沿用松开前的成熟方向，才允许进入短恢复追赶。窗口只覆盖
    // 暂停积累误差的首个100 ms；目标停止、反向或安全门控撤销提前量时不会启动。
    if (quickResumeCandidate)
    {
        constexpr auto kQuickResumeCatchUpDuration = std::chrono::milliseconds(100);
        if (lastPredictionResult.directionLocked &&
            !lastPredictionResult.selfMotionSuppressed &&
            std::abs(lastPredictionResult.offsetX) > 1e-6)
        {
            quickResumeCatchUpUntil = controlTime + kQuickResumeCatchUpDuration;
        }
        else
        {
            quickResumeCatchUpUntil = {};
        }
    }
    predictionObservationHistory.push_back({
        observedScreenAtObservationX, observedScreenAtObservationY,
        viewAtObservation.first, viewAtObservation.second
    });
    while (predictionObservationHistory.size() > 3)
        predictionObservationHistory.pop_front();
    const double filteredScreenX = filtered.first - viewAtControl.first;
    const double filteredScreenY = filtered.second - viewAtControl.second;
    lastPredictionResult.x -= viewAtControl.first;
    lastPredictionResult.y -= viewAtControl.second;
    if (lastPredictionResult.oscillationSuppressed)
    {
        const double viewDeltaX = viewAtObservation.first - viewAtControl.first;
        const double viewDeltaY = viewAtObservation.second - viewAtControl.second;
        // reverse实测中央50%仍有69.9%帧不输出，目标中心误差在抑制后扩大近一倍。
        // 水平只保留中央20%走廊以持续跟住框体，同时不恢复逐帧追逐检测pivot。
        lastPredictionResult.x = TargetPredictor::boxHoldCoordinate(
            center_x, target.x + viewDeltaX, target.w,
            std::max(2.0, target.w * 0.40));
        lastPredictionResult.y = TargetPredictor::boxHoldCoordinate(
            center_y, target.y + viewDeltaY, target.h);
    }
    const double errorX = lastPredictionResult.x - center_x;
    const double errorY = lastPredictionResult.y - center_y;

    const double fovScale = (cachedGameFovScaled && cachedGameBaseFOV > 1.0)
        ? (fov_x / cachedGameBaseFOV) : 1.0;
    // FOV 属于捕获源完整画面；检测坐标只是该画面的中心裁剪且保持 1:1 像素映射。
    // 使用捕获后端动态发布的源尺寸，可避免把 2560 等实际源宽误按 320 宽检测区域换算。
    const double sourcePixelWidth = AimCoordinateSpace::resolveFovPixelSpan(
        ::screenWidth.load(std::memory_order_relaxed), screen_width);
    const double sourcePixelHeight = AimCoordinateSpace::resolveFovPixelSpan(
        ::screenHeight.load(std::memory_order_relaxed), screen_height);
    const double horizontalDenominator = cachedGameSens * cachedGameYaw * fovScale;
    const double verticalDenominator = cachedGameSens * cachedGamePitch * fovScale;
    const double countsPerPixelX = AimCoordinateSpace::countsPerSourcePixel(
        fov_x, sourcePixelWidth, horizontalDenominator);
    const double countsPerPixelY = AimCoordinateSpace::countsPerSourcePixel(
        fov_y, sourcePixelHeight, verticalDenominator);

    BasicAimController::Settings settings;
    settings.responseSeconds = move_response_seconds;
    // reverse 的高频方向抑制只撤销预测前瞻，不能同时保留 120 ms 的慢响应，
    // 否则目标会在抑制窗口内持续漂移。仅对已确认运动且速度可靠的该模式加快
    // 基础反馈；left/right、static 和 jump 不进入此分支。
    if (lastPredictionResult.oscillationSuppressed &&
        !lastPredictionResult.selfMotionSuppressed &&
        std::hypot(lastPredictionResult.velocityX, lastPredictionResult.velocityY) >= 80.0)
    {
        settings.responseSeconds = std::min(settings.responseSeconds, 0.080);
    }
    settings.integralTimeSeconds = move_integral_time_seconds;
    settings.settleRadiusPixels = std::max(6.0, screen_width / 64.0);
    settings.releaseRadiusPixels = settings.settleRadiusPixels * 1.6;

    // r56 的 3200/4000 全局 A/B 只在 3000~5000 px/s 区间达到 15.7% 收益，
    // 同时放大了普通区间的输出步进。额外预算因此只允许由预测器已经判定的
    // 高速高加速度瞬态触发，并要求目标继续沿当前水平误差方向远离准星。
    // 阈值按 320 裁剪标定并随检测分辨率等比例缩放；退出阈值减半形成滞回，
    // 120 ms 最长窗口与 200 ms 冷却避免检测尖峰反复开关高预算。
    ConditionalSpeedBudget::Settings speedBudgetSettings;
    speedBudgetSettings.baseMaxCountsPerSecond = move_max_speed_cps;
    speedBudgetSettings.catchUpMaxCountsPerSecond = move_catch_up_max_speed_cps;
    speedBudgetSettings.entryErrorPixels = std::max(60.0, screen_width * 0.1875);
    speedBudgetSettings.exitErrorPixels = std::max(32.0, screen_width * 0.1000);
    speedBudgetSettings.entryVelocityPixelsPerSecond =
        std::max(1280.0, screen_width * 4.0);
    speedBudgetSettings.exitVelocityPixelsPerSecond =
        std::max(640.0, screen_width * 2.0);
    const bool speedBudgetSafetySuppressed =
        lastPredictionResult.selfMotionSuppressed ||
        lastPredictionResult.oscillationSuppressed ||
        lastPredictionResult.stationarySuppressed;
    const auto speedBudgetOutput = conditionalSpeedBudget.update(
        errorX, lastPredictionResult.velocityX,
        lastPredictionResult.highSpeedSuppressed,
        lastPredictionResult.directionLocked,
        speedBudgetSafetySuppressed,
        controlTime, speedBudgetSettings);
    settings.maxCountsPerSecond = speedBudgetOutput.maxCountsPerSecond;
    // r51短恢复数据中，19~60 px原始积累误差没有触发设备限速，追到16 px内仍需
    // 62~95 ms；79~89 px样本已限速，继续全局提速只会增加饱和。仅在同向短恢复
    // 的100 ms窗口且预测误差仍大于两倍释放半径时，把基础响应从120 ms收紧到
    // 100 ms。低于大误差阈值立即恢复原参数，设备限速继续作为最终硬边界。
    const double quickResumeCatchUpThreshold =
        std::max(32.0, settings.releaseRadiusPixels * 2.0);
    const bool quickResumeCatchUpActive =
        controlTime < quickResumeCatchUpUntil &&
        std::abs(errorX) >= quickResumeCatchUpThreshold &&
        lastPredictionResult.offsetX * errorX > 0.0;
    if (quickResumeCatchUpActive)
        settings.responseSeconds = std::min(settings.responseSeconds, 0.100);
    // 移动积分和稳定锁存必须按轴判断。水平目标的可靠X运动不能阻止Y轴
    // 独立停发，否则Y会持续把约1~2 px检测噪声量化为±1 count。
    // 水平A/B中Y提前量P95仅0.061 px，不能让这种数值残差重新打开Y轴。
    // 小于稳定半径10%的提前由可靠速度分量兜底，单独不构成轴向运动证据。
    const double meaningfulPredictionLead = std::max(0.5, settings.settleRadiusPixels * 0.10);
    const bool predictionLeadActiveX =
        std::abs(lastPredictionResult.offsetX) >= meaningfulPredictionLead;
    const bool predictionLeadActiveY =
        std::abs(lastPredictionResult.offsetY) >= meaningfulPredictionLead;
    const double predictionSpeed = std::hypot(
        lastPredictionResult.velocityX, lastPredictionResult.velocityY);
    const bool reliableMovingTarget =
        lastPredictionResult.directionLocked &&
        predictionSpeed >= 80.0 &&
        !lastPredictionResult.selfMotionSuppressed;
    const double reliableAxisSpeed = predictionSpeed * 0.35;
    const bool reliableMovingTargetX = reliableMovingTarget &&
        std::abs(lastPredictionResult.velocityX) >= reliableAxisSpeed;
    const bool reliableMovingTargetY = reliableMovingTarget &&
        std::abs(lastPredictionResult.velocityY) >= reliableAxisSpeed;
    settings.preserveMovingIntegralX = predictionLeadActiveX || reliableMovingTargetX;
    settings.preserveMovingIntegralY = predictionLeadActiveY || reliableMovingTargetY;
    settings.allowMovingInsideSettleX = settings.preserveMovingIntegralX;
    settings.allowMovingInsideSettleY = settings.preserveMovingIntegralY;
    // 观测时间用于滤波和预测；设备命令按控制消费节奏发送，因此 PI 输出 counts
    // 必须覆盖相邻 controlTime，而不能复用 NDI 成批到达的观测时间差。
    const double dt = controlIntervalTracker.update(
        controlTime, frameIntervalSec(captureFps.load()));
    const auto output = aimController.update(
        errorX, errorY, dt, countsPerPixelX, countsPerPixelY, settings);

    // 先累计分数 counts 再整数化。直接逐帧 round 会在 10/20 ms 交替的
    // NDI 时间基下产生 1、6、1、6 这类脉冲，视觉上就是左右晃动；余量
    // 只改变发送时刻，不改变长期平均位移。
    if (output.settledX)
        legacyCountRemainderX = 0.0;
    if (output.settledY)
        legacyCountRemainderY = 0.0;
    const double quantizedX = output.countsX + legacyCountRemainderX;
    const double quantizedY = output.countsY + legacyCountRemainderY;
    int mx = static_cast<int>(std::round(quantizedX));
    int my = static_cast<int>(std::round(quantizedY));
    legacyCountRemainderX = quantizedX - static_cast<double>(mx);
    legacyCountRemainderY = quantizedY - static_cast<double>(my);
    if (output.settled)
    {
        mx = 0;
        my = 0;
        legacyCountRemainderX = 0.0;
        legacyCountRemainderY = 0.0;
        clearQueuedMoves();
    }

    if (pf)
    {
        pf->filteredX = filteredScreenX;
        pf->filteredY = filteredScreenY;
        pf->observedVelocityX = lastFilterResult.observedVelocityX;
        pf->observedVelocityY = lastFilterResult.observedVelocityY;
        pf->observedSpeed = lastFilterResult.observedSpeed;
        pf->filterTrendSpeed = lastFilterResult.motionTrendSpeed;
        pf->filterTrendActive = lastFilterResult.motionTrendActive;
        pf->filterResidual = lastFilterResult.residual;
        pf->predictionApplied = lastPredictionResult.applied;
        pf->predictionEnabled = predictionSettings.enabled;
        pf->predictionAdditionalLeadMs = predictionSettings.additionalLeadSeconds * 1000.0;
        pf->predictionVelocityTauMs = predictionSettings.velocityTimeConstantSeconds * 1000.0;
        pf->predictionStrength = predictionSettings.predictionStrength;
        pf->predictionVelocityX = lastPredictionResult.velocityX;
        pf->predictionVelocityY = lastPredictionResult.velocityY;
        pf->predictionAccelerationX = lastPredictionResult.accelerationX;
        pf->predictionAccelerationY = lastPredictionResult.accelerationY;
        pf->predictionLeadMs = lastPredictionResult.leadSeconds * 1000.0;
        pf->predictionOffsetX = lastPredictionResult.offsetX;
        pf->predictionOffsetY = lastPredictionResult.offsetY;
        pf->viewMotionX = viewAtControl.first - viewAtObservation.first;
        pf->viewMotionY = viewAtControl.second - viewAtObservation.second;
        {
            std::lock_guard<std::mutex> lock(motionCompensationMutex);
            pf->viewMotionCompensationDelayMs =
                motionCompensationHistory.commandToFrameDelayMs();
            pf->viewMotionCompensationResponseMs =
                motionCompensationHistory.commandResponseMs();
        }
        pf->predictionDirectionLocked = lastPredictionResult.directionLocked;
        pf->predictionSelfMotionSuppressed = lastPredictionResult.selfMotionSuppressed;
        pf->predictionOscillationSuppressed =
            lastPredictionResult.oscillationSuppressed;
        pf->predictionHighSpeedSuppressed =
            lastPredictionResult.highSpeedSuppressed;
        pf->predictionStationarySuppressed =
            lastPredictionResult.stationarySuppressed;
        pf->predictionMotionEvidenceSuppressed =
            lastPredictionResult.motionEvidenceSuppressed;
        pf->predictedX = lastPredictionResult.x;
        pf->predictedY = lastPredictionResult.y;
        pf->errorX = errorX;
        pf->errorY = errorY;
        pf->errorDistance = output.errorDistance;
        pf->requestedPixelX = output.requestedPixelX;
        pf->requestedPixelY = output.requestedPixelY;
        pf->requestedCountsX = output.countsX;
        pf->requestedCountsY = output.countsY;
        pf->integralCountsX = output.integralCountsX;
        pf->integralCountsY = output.integralCountsY;
        pf->finalMx = mx;
        pf->finalMy = my;
        pf->profileCalibrationEnabled = calibration.enabled;
        pf->profileCalibrationValidX = calibration.x.valid;
        pf->profileCalibrationValidY = calibration.y.valid;
        pf->profileCalibrationPixelsPerCountX = calibration.x.pixelsPerCount;
        pf->profileCalibrationPixelsPerCountY = calibration.y.pixelsPerCount;
        pf->profileCalibrationDegreesPerCountX = calibration.x.degreesPerCount;
        pf->profileCalibrationDegreesPerCountY = calibration.y.degreesPerCount;
        pf->profileCalibrationDelayMsX = calibration.x.delayMs;
        pf->profileCalibrationDelayMsY = calibration.y.delayMs;
        pf->profileCalibrationDriftX = calibration.x.driftPixelsPerSecond;
        pf->profileCalibrationDriftY = calibration.y.driftPixelsPerSecond;
        pf->profileCalibrationRmseX = calibration.x.rmsePixels;
        pf->profileCalibrationRmseY = calibration.y.rmsePixels;
        pf->profileCalibrationCorrelationX = calibration.x.correlation;
        pf->profileCalibrationCorrelationY = calibration.y.correlation;
        pf->profileCalibrationConfidenceX = calibration.x.confidence;
        pf->profileCalibrationConfidenceY = calibration.y.confidence;
        pf->profileCalibrationSamplesX = calibration.x.sampleCount;
        pf->profileCalibrationSamplesY = calibration.y.sampleCount;
        pf->profileCalibrationActiveSamplesX = calibration.x.activeSampleCount;
        pf->profileCalibrationActiveSamplesY = calibration.y.activeSampleCount;
        pf->profileCalibrationOverallConfidence = calibration.overallConfidence;
        pf->responseSeconds = settings.responseSeconds;
        pf->effectiveResponseSecondsX = output.effectiveResponseSecondsX;
        pf->effectiveResponseSecondsY = output.effectiveResponseSecondsY;
        pf->integralTimeSeconds = settings.integralTimeSeconds;
        pf->maxCountsPerSecond = settings.maxCountsPerSecond;
        pf->conditionalSpeedBudgetActive = speedBudgetOutput.active;
        pf->frameCountLimit = output.frameCountLimit;
        pf->controllerUpdateIntervalMs =
            output.controllerUpdateIntervalSeconds * 1000.0;
        pf->errorMotion = output.errorMotion;
        pf->errorMotionX = output.errorMotionX;
        pf->errorMotionY = output.errorMotionY;
        pf->settleMotionThreshold = output.settleMotionThreshold;
        pf->movingInsideSettle = output.movingInsideSettle;
        pf->movingInsideSettleX = output.movingInsideSettleX;
        pf->movingInsideSettleY = output.movingInsideSettleY;
        pf->horizontalCatchUp = output.horizontalCatchUp;
        pf->verticalCatchUp = output.verticalCatchUp;
        pf->speedLimited = output.speedLimited;
        pf->settled = output.settled;
        pf->settledX = output.settledX;
        pf->settledY = output.settledY;
    }

    ViewCommandSample commandSample;
    if (mx != 0 || my != 0)
        commandSample = queueMove(mx, my);
    if (pf)
        pf->commandSample = commandSample;
    if (pf)
        pf->queuedMoveCount = pendingMoveCount();
    commitTrace();
    return;

#if 0 // Removed base-pipeline stages: prediction, speed curve, PurePursuit and trajectory shaping.

    // ===== 流水线追踪：Stage 1 原始目标 =====
    PipelineFrame traceFrame;
    PipelineFrame* pf = nullptr;
    if (g_pipelineTracer.isEnabled())
    {
        traceFrame = g_pipelineTracer.beginFrame(static_cast<int>(screen_width));
        pf = &traceFrame;
        pf->rawPivotX = pivotX;
        pf->rawPivotY = pivotY;
        pf->targetClassId = targetClassId;
        pf->targetDetected = true;
        pf->fpsValue = static_cast<double>(captureFps.load());
    }
    auto commitTrace = [&]() {
        if (pf)
        {
            g_pipelineTracer.commitFrame(std::move(traceFrame));
            pf = nullptr;
        }
    };

    // ===== 运动补偿 =====
    double mcDeltaX = 0.0, mcDeltaY = 0.0;
    if (observationTime.time_since_epoch().count() != 0)
    {
        // 运动补偿：减去自瞄自身旋转导致的屏幕偏移
        auto cameraDelta = getMotionCompensationSince(observationTime);
        mcDeltaX = cameraDelta.first;
        mcDeltaY = cameraDelta.second;
        pivotX -= mcDeltaX;
        pivotY -= mcDeltaY;
    }

    // ===== 流水线追踪：Stage 2 运动补偿后 =====
    if (pf)
    {
        pf->mcPivotX = pivotX;
        pf->mcPivotY = pivotY;
        pf->mcDeltaX = mcDeltaX;
        pf->mcDeltaY = mcDeltaY;
        auto now = std::chrono::steady_clock::now();
        if (observationTime.time_since_epoch().count() != 0)
        {
            double age = std::chrono::duration<double>(now - observationTime).count();
            if (std::isfinite(age) && age >= 0.0)
                pf->observationAgeSec = age;
        }
    }

    auto predicted = predict_target_position(pivotX, pivotY, observationTime);

    // ===== 流水线追踪：Stage 3 预测后 =====
    if (pf)
    {
        pf->predX = predicted.first;
        pf->predY = predicted.second;
        pf->velocityX = motionEstimator.velocityX();
        pf->velocityY = motionEstimator.velocityY();
        pf->hasExternalVel = motionEstimator.lastUsedExternalVelocity();
    }

    // 预计算速度曲线倍率（基于目标到屏幕中心的像素距离）
    double offx = predicted.first - center_x;
    double offy = predicted.second - center_y;
    double distToTarget = std::hypot(offx, offy);
    double speedCurve = calculate_speed_multiplier(distToTarget);

    // A stationary target inside the settle radius should not be chased by
    // detector jitter or residual controller state. Moving targets bypass this
    // gate so they remain responsive near the crosshair.
    const double targetSpeed = std::hypot(motionEstimator.velocityX(), motionEstimator.velocityY());
    const double settleRadius = std::max(1.5, screen_width / 320.0);
    // Detector pivot quantization can leave a static target reporting a small
    // apparent speed near the crosshair. Treat that low-speed range as
    // precision mode so one-count corrections cannot alternate forever.
    // The detector/SOT pair can still report roughly 55 px/sec while a
    // stationary target is settling because the camera is responding to our
    // own prior one-count commands. Use a resolution-scaled 64 px/sec band
    // only inside the near-center precision path; this removes that feedback
    // loop without limiting normal target pursuit at meaningful distances.
    const double staticSpeedThreshold = std::max(18.0, screen_width * 0.20);
    const bool precisionMode = targetSpeed <= staticSpeedThreshold &&
        distToTarget <= std::max(static_cast<double>(cachedNearRadius), settleRadius);
    if (pf)
        pf->precisionMode = precisionMode;
    if (distToTarget <= settleRadius && targetSpeed <= staticSpeedThreshold)
    {
        purePursuitController.reset();
        clearQueuedMoves();
        prev_ema_move_x = 0.0;
        prev_ema_move_y = 0.0;
        if (pf)
        {
            pf->controllerLimitPx = 0.0;
            pf->finalMx = 0;
            pf->finalMy = 0;
            pf->queuedMoveCount = pendingMoveCount();
        }
        commitTrace();
        return;
    }

    // ===== 流水线追踪：Stage 4 速度曲线 =====
    if (pf)
    {
        pf->distToTarget = distToTarget;
        pf->speedMultiplier = speedCurve;
    }

    // ==================== 统一执行控制器（Pure Pursuit，像素空间） ====================
    double move_x = 0.0, move_y = 0.0;
    double ppDxOut = 0.0, ppDyOut = 0.0;
    {
        // PurePursuit 模式：基于屏幕像素坐标计算 2D 移动（输出为像素空间增量）
        double screenCx = screen_width / 2.0;
        double screenCy = screen_height / 2.0;
        const double controlDt = frameIntervalSec(captureFps.load());
        const double baseLimit = std::clamp(screen_width * 2.25 * controlDt, 2.0, 14.0);
        // 速度只用于给移动目标增加有限的单帧余量。不能让单次检测速度
        // 突刺把静止目标的输出上限抬到 18 px，导致越过目标后反向振荡。
        const double speedForLimit = std::min(targetSpeed, screen_width * 0.60);
        const double dynamicLimit = std::clamp(
            baseLimit + speedForLimit * controlDt * 0.75, 2.0, 12.0);
        purePursuitController.setOutputLimit(dynamicLimit);
        if (pf)
            pf->controllerLimitPx = dynamicLimit;
        double dxOut = 0.0, dyOut = 0.0;
        purePursuitController.computeMovement2D(
            predicted.first, predicted.second,
            screenCx, screenCy,
            dxOut, dyOut,
            false);

        ppDxOut = dxOut;
        ppDyOut = dyOut;

        // ===== 像素空间增量 → 鼠标计数（含灵敏度/FOV/帧率修正+速度曲线） =====
        auto counts = pixelDeltaToCounts(dxOut, dyOut, speedCurve);
        move_x = counts.first;
        move_y = counts.second;

        // External velocity has been consumed by the single prediction stage.
        motionEstimator.clearExternalVelocity();
    }

    // ===== 流水线追踪：Stage 5 Pure Pursuit + Stage 6 Counts =====
    if (pf)
    {
        pf->ppDx = ppDxOut;
        pf->ppDy = ppDyOut;
        pf->countsX = move_x;
        pf->countsY = move_y;
    }

    // === EMA 输出平滑 ===
    if (move_ema_enabled)
    {
        double a = static_cast<double>(move_ema_alpha);
        move_x = move_x * a + prev_ema_move_x * (1.0 - a);
        move_y = move_y * a + prev_ema_move_y * (1.0 - a);
        prev_ema_move_x = move_x;
        prev_ema_move_y = move_y;
    }

    // ===== 流水线追踪：Stage 7 EMA 平滑后 =====
    if (pf)
    {
        pf->emaCountsX = move_x;
        pf->emaCountsY = move_y;
    }

    // === 开火解锁 Y 轴 ===
    if (unlock_y_enabled && mouse_pressed.load())
    {
        auto now = std::chrono::steady_clock::now();
        auto heldMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - leftPressStartTime).count();
        if (heldMs > static_cast<long long>(unlock_y_threshold_ms))
        {
            double yFactor = 1.0 - static_cast<double>(unlock_y_strength);
            if (yFactor < 0.0) yFactor = 0.0;
            move_y *= yFactor;
        }
    }

    // === 惯性过冲模拟 (Overshoot) ===
    if (!precisionMode && (bezier_enabled || wind_mouse_enabled))
    {
        double totalDist = std::hypot(move_x, move_y);
        if (totalDist > 50.0)
        {
            if (osCooldown > 0) { osCooldown--; }
            else
            {
                std::uniform_real_distribution<double> osDist(0.0, 1.0);
                double osRoll = osDist(windRng);

                if (osRoll > 0.86)
                {
                    double osFactor = 1.02 + osRoll * 0.06;
                    if (osFactor > 1.08) osFactor = 1.08;
                    move_x *= osFactor;
                    move_y *= osFactor;
                    std::uniform_int_distribution<int> cooldownDist(10, 30);
                    osCooldown = cooldownDist(windRng);
                }
            }
        }
    }

    int mx = static_cast<int>(std::round(move_x));
    int my = static_cast<int>(std::round(move_y));

    // At 320 px detection resolution, a static target near the center can
    // alternate between adjacent detector pixels. Suppress only the final
    // one-count correction inside a resolution-scaled micro-settle zone;
    // meaningful movement and faster targets still use the normal path.
    const double microSettleRadius = std::max(3.0, screen_width / 53.3333333333);
    if (precisionMode && distToTarget <= microSettleRadius &&
        std::abs(mx) <= 1 && std::abs(my) <= 1)
    {
        purePursuitController.reset();
        clearQueuedMoves();
        prev_ema_move_x = 0.0;
        prev_ema_move_y = 0.0;
        mx = 0;
        my = 0;
    }

    // ===== 流水线追踪：Stage 8 最终输出 =====
    if (pf)
    {
        pf->finalMx = mx;
        pf->finalMy = my;
    }

    if (mx == 0 && my == 0)
    {
        if (pf)
            pf->queuedMoveCount = pendingMoveCount();
        commitTrace();
        return;
    }

    // === 轨迹分发 ===
    if (precisionMode)
    {
        queueMove(mx, my);
    }
    else if (bezier_enabled)
    {
        bezierMoveRelative(mx, my);
    }
    else if (wind_mouse_enabled)
    {
        windMouseMoveRelative(mx, my);
    }
    else
    {
        queueMove(mx, my);
    }

    if (pf)
        pf->queuedMoveCount = pendingMoveCount();

    commitTrace();
#endif
}

/**
 * clearQueuedMoves
 *
 * 清空所有待处理的鼠标移动指令队列。
 * 在基础跟踪重置、目标丢失或参数变更时调用。
 */
void MouseThread::clearQueuedMoves()
{
    // 先切换代次，再处理队列；这样已经被Worker弹出的命令也会在设备调用前失效。
    moveCancellationEpoch.cancel();
    {
        std::lock_guard<std::mutex> lock(queueMtx);
        while (!moveQueue.empty())
        {
            g_pipelineTracer.recordCommandDropped(moveQueue.front().timing.sequence);
            moveQueue.pop();
        }
        std::queue<Move> empty;
        moveQueue.swap(empty);
    }

    // 与可能已经进入设备调用的旧命令建立完成屏障。返回后，不会再有取消前的
    // 命令晚到设备；等待锁期间的新旧命令仍由上方代次核对决定是否可发送。
    {
        std::lock_guard<std::mutex> inputLock(inputDevicesMutex);
    }
}

size_t MouseThread::pendingMoveCount()
{
    std::lock_guard<std::mutex> lock(queueMtx);
    return moveQueue.size();
}

/**
 * pressMouse
 *
 * 控制鼠标左键按下/释放，实现自瞄开火功能。
 *
 * 逻辑：
 *   - 如果目标在瞄准范围内且鼠标尚未按下：执行左键按下
 *   - 如果目标不在瞄准范围内但鼠标已按下：执行左键释放
 *   - 使用 bScope_multiplier 控制瞄准判定的严格程度
 *
 * 这实现了"准星对准目标时自动开火，离开目标时自动松开"的行为。
 *
 * @param target 目标信息（用于判断是否在瞄准范围内）
 */
void MouseThread::pressMouse(const AimbotTarget& target)
{
    auto now = std::chrono::steady_clock::now();
    bool bScope = check_target_in_scope(target.x, target.y, target.w, target.h, bScope_multiplier);

    // === 开火拟人化：确认帧数 ===
    if (bScope && trigger_stable_frames > 0)
    {
        stableFrameCount++;
        if (stableFrameCount < trigger_stable_frames)
            return; // 等待更多帧确认
    }
    else if (!bScope)
    {
        stableFrameCount = 0;
        // 目标脱锁，取消所有待执行的开火计划
        fireScheduled = false;
        holdScheduled = false;
    }

    // === 冷却检查 ===
    if (bScope && trigger_shot_cooldown_ms > 0.0f && lastShotTime.time_since_epoch().count() != 0)
    {
        auto sinceLast = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastShotTime).count();
        if (sinceLast < static_cast<long long>(trigger_shot_cooldown_ms))
            return;
    }

    // === 计划延时开火 ===
    if (bScope && !mouse_pressed && !fireScheduled)
    {
        // 对数正态分布生成反应延迟（人类反应时间服从正偏态/长尾分布）
        // delay = baseDelay * exp(N(0, cv)), 其中 cv = jitter / baseDelay
        // 始终为正、右偏，更贴近真实反应时间分布
        double baseDelay = static_cast<double>(trigger_random_delay_ms);
        double delay = baseDelay;
        if (trigger_delay_jitter_ms > 0.0f && baseDelay > 0.0)
        {
            double cv = static_cast<double>(trigger_delay_jitter_ms) / baseDelay;
            std::lognormal_distribution<double> delayDist(0.0, std::clamp(cv, 0.01, 1.5));
            delay = baseDelay * delayDist(behaviorRng);
            delay = std::clamp(delay, 5.0, 500.0);  // 硬钳位防极端值
        }
        fireScheduleTime = now + std::chrono::microseconds(static_cast<long long>(delay * 1000.0));
        fireScheduled = true;

        // 对数正态分布生成随机保持时长
        double baseHold = static_cast<double>(trigger_hold_ms);
        double hold = baseHold;
        if (trigger_hold_jitter_ms > 0.0f && baseHold > 0.0)
        {
            double cv = static_cast<double>(trigger_hold_jitter_ms) / baseHold;
            std::lognormal_distribution<double> holdDist(0.0, std::clamp(cv, 0.01, 1.5));
            hold = baseHold * holdDist(behaviorRng);
            hold = std::clamp(hold, 2.0, 300.0);
        }
        holdReleaseTime = fireScheduleTime + std::chrono::microseconds(static_cast<long long>(hold * 1000.0));
        holdScheduled = true;
    }

    // === 执行开火 ===
    if (fireScheduled && !mouse_pressed && now >= fireScheduleTime)
    {
        std::lock_guard<std::mutex> lock(inputDevicesMutex);
        if (!mouseInput) return;

        // 自动急停：开火前同时按下 WASD 四键，利用方向键互相抵消实现急停
        // （KMBOX NET 发送虚拟键盘 HID 报告，与物理按键叠加后各方向抵消）
        if (auto_stop_enabled && !wasdReleased)
        {
            mouseInput->keyDown(0x1A); // W
            mouseInput->keyDown(0x04); // A
            mouseInput->keyDown(0x16); // S
            mouseInput->keyDown(0x07); // D
            wasdReleased = true;
        }

        if (mouseInput->leftDown())
        {
            mouse_pressed = true;
            leftPressStartTime = now;
            fireScheduled = false;
        }
    }

    // === 自动松开 (单发模式) ===
    if (holdScheduled && mouse_pressed && now >= holdReleaseTime)
    {
        std::lock_guard<std::mutex> lock(inputDevicesMutex);
        if (!mouseInput) { mouse_pressed = false; return; }

        if (mouseInput->leftUp())
        {
            mouse_pressed = false;
            holdScheduled = false;
            lastShotTime = now;
            stableFrameCount = 0;
        }
    }

    // === 脱锁时释放 ===
    if (!bScope && mouse_pressed)
    {
        std::lock_guard<std::mutex> lock(inputDevicesMutex);
        if (!mouseInput) { mouse_pressed = false; return; }

        if (mouseInput->leftUp())
            mouse_pressed = false;

        // 恢复 WASD：释放虚拟按键，物理按键继续生效
        if (wasdReleased)
        {
            mouseInput->keyUp(0x1A); mouseInput->keyUp(0x04);
            mouseInput->keyUp(0x16); mouseInput->keyUp(0x07);
            wasdReleased = false;
        }

        fireScheduled = false;
        holdScheduled = false;
        stableFrameCount = 0;
    }
}

/**
 * releaseMouse
 *
 * 强制释放鼠标左键。
 * 当自瞄停止、目标丢失或用户取消操作时调用，
 * 确保鼠标不会处于卡键状态。
 */
void MouseThread::releaseMouse()
{
    // 取消所有待执行的开火计划，防止再次开镜时立即无条件开火
    fireScheduled = false;
    holdScheduled = false;
    stableFrameCount = 0;

    if (mouse_pressed)
    {
        std::lock_guard<std::mutex> lock(inputDevicesMutex);
        if (!mouseInput)
        {
            mouse_pressed = false;
            return;
        }

        if (mouseInput->leftUp())
            mouse_pressed = false;

        // 恢复 WASD：释放虚拟按键，物理按键继续生效（自动急停结束）
        if (wasdReleased)
        {
            mouseInput->keyUp(0x1A); // W
            mouseInput->keyUp(0x04); // A
            mouseInput->keyUp(0x16); // S
            mouseInput->keyUp(0x07); // D
            wasdReleased = false;
        }
    }
}

/**
 * resetTracking
 *
 * 目标丢失、身份切换或瞄准状态变化时统一清空队列、滤波、预测和控制器状态。
 * 预测器不保留滑行状态，因此重置后必须重新收到两个连续真实观测才会产生前瞻。
 */
void MouseThread::resetTracking()
{
    clearQueuedMoves();
    targetFilter.reset();
    targetPredictor.reset();
    aimController.reset();
    conditionalSpeedBudget.reset();
    aimPipelineRuntime.reset();
    lastFilterResult = {};
    lastPredictionResult = {};
    controlIntervalTracker.reset();
    legacyCountRemainderX = 0.0;
    legacyCountRemainderY = 0.0;
    previousSelfMotionArtifact = false;
    predictionResumePending = false;
    aimingOutputSuspendedAt = {};
    quickResumeCatchUpUntil = {};
    predictionObservationHistory.clear();
    target_detected.store(false);
    // 目标切换时重置确认帧计数，避免新目标跳过确认延迟
    stableFrameCount = 0;
    fireScheduled = false;
    holdScheduled = false;
}

/**
 * suspendAimingOutput
 *
 * 松开瞄准时立即撤销设备输出和控制器余量，但保留同一跟踪ID的滤波、预测方向
 * 与成熟提前量。检测线程在未瞄准期间仍会维护目标身份；若目标切换、丢失或暂停
 * 超过350 ms，完整resetTracking路径或恢复超时会清除此状态。
 */
void MouseThread::suspendAimingOutput()
{
    clearQueuedMoves();
    aimController.reset();
    conditionalSpeedBudget.reset();
    aimPipelineRuntime.reset();
    controlIntervalTracker.reset();
    legacyCountRemainderX = 0.0;
    legacyCountRemainderY = 0.0;
    previousSelfMotionArtifact = false;
    predictionObservationHistory.clear();
    predictionResumePending = true;
    aimingOutputSuspendedAt = std::chrono::steady_clock::now();
    quickResumeCatchUpUntil = {};
    stableFrameCount = 0;
    fireScheduled = false;
    holdScheduled = false;
}

/**
 * clearWindDebugTrail
 *
 * 清空轨迹模拟调试轨迹，并将光标位置重置到屏幕中心。
 */
void MouseThread::clearWindDebugTrail()
{
}

/**
 * getWindDebugTrail
 *
 * 获取轨迹模拟的调试轨迹点列表，用于可视化叠加显示。
 * 返回前先清理过期点（超过 900ms）。
 * 返回坐标的副本，不保留时间戳。
 *
 * @return 轨迹点列表，每项为 (x, y) 屏幕像素坐标
 */
std::vector<std::pair<double, double>> MouseThread::getWindDebugTrail()
{
    return {};
}

/**
 * setMouseInput
 *
 * 运行时热切换鼠标输入设备。
 * 允许在不重启自瞄的情况下更换底层鼠标驱动实现。
 * 线程安全，内部持有 inputDevicesMutex。
 *
 * @param newMouseInput 新的 IMouseInput 接口指针（可为 nullptr）
 */
void MouseThread::setMouseInput(IMouseInput* newMouseInput)
{
    std::lock_guard<std::mutex> lock(inputDevicesMutex);
    mouseInput = newMouseInput;
}
