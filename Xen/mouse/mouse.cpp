// ============================================================================
// mouse.cpp — Xen 鼠标控制核心模块
//
// 本文件是 Xen 自瞄辅助功能的鼠标运动控制中枢，负责：
//   - 轨迹模拟算法：模拟人类鼠标操作的类自然位移算法，包含累积、
//     子步分解、拉力趋向目标、随机扰动、双正弦波黄金比例模式振荡
//   - 卡尔曼滤波预测：基于目标历史位置进行卡尔曼滤波，预测目标未来位置，
//     并补偿检测延迟和网络延迟
//   - 运动补偿：记录已发送的鼠标移动，用于补偿因自瞄自身移动造成的视差变化
//   - 多鼠标设备支持：通过 IMouseInput 接口抽象，支持多种底层鼠标驱动
//     （物理鼠标、虚拟驱动、原始输入等），支持运行时热切换
//   - 速度曲线：根据目标距离动态调整移动速度，支持 snap 半径、近端半径和曲线指数
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
#include "Xen.h"
#include "debug/pipeline_tracer.h"
#include "runtime/speed_curve.h"
#include "runtime/thread_loops.h"

/**
 * MouseThread 构造函数
 *
 * 初始化鼠标控制线程的核心参数：
 *
 *   - resolution / screen_width / screen_height: 屏幕分辨率，用于坐标归一化
 *   - fovX / fovY: 视场角（度），用于屏幕像素与游戏内角度的换算
 *   - min/maxSpeedMultiplier: 速度曲线的最小/最大倍率
 *   - predictionInterval: 预测前瞻时间（秒），卡尔曼滤波预测目标未来的偏移量
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
    double minSpeedMultiplier,
    double maxSpeedMultiplier,
    double predictionInterval,
    bool auto_shoot,
    float bScope_multiplier,
    IMouseInput* mouseInputDevice)
    : screen_width(resolution),
    screen_height(resolution),
    prediction_interval(predictionInterval),
    fov_x(fovX),
    fov_y(fovY),
    max_distance(std::hypot(resolution, resolution) / 2.0),
    min_speed_multiplier(minSpeedMultiplier),
    max_speed_multiplier(maxSpeedMultiplier),
    center_x(resolution / 2.0),
    center_y(resolution / 2.0),
    auto_shoot(auto_shoot),
    bScope_multiplier(bScope_multiplier),
    mouseInput(mouseInputDevice),

    // 卡尔曼预测的上一次速度（用于速度平滑和趋势推算）
    prev_velocity_x(0.0),
    prev_velocity_y(0.0),
    // 卡尔曼预测的上一次目标位置
    prev_x(0.0),
    prev_y(0.0)
{
    prev_time = std::chrono::steady_clock::time_point();
    last_target_time = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(configMutex);
        // 从配置加载轨迹模拟参数
        wind_mouse_enabled = config.wind_mouse_enabled;
        wind_G = config.wind_G;   // 鼠标移速系数
        wind_W = config.wind_W;   // 轨迹摆动幅度
        wind_M = config.wind_M;   // 单步移动上限
        wind_D = config.wind_D;   // 微调距离阈值

        refreshGameProfileCache();  // 必须在锁内调用（读取 config.game_profiles）
    }
    resetWindState();
    clearWindDebugTrail();

    // 初始化 PurePursuit 控制器（默认参数，updateConfig 会覆盖）
    purePursuitController = execution::PurePursuitController(0.85, 60.0, 25.0, 0.6, 2.0, 0.8);
    purePursuitController.setFeedforwardCoeff(prediction_interval);

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
 * @param minSpeedMultiplier  最小速度倍率
 * @param maxSpeedMultiplier  最大速度倍率
 * @param predictionInterval  预测前瞻时间
 * @param auto_shoot          是否自动开火
 * @param bScope_multiplier   瞄准范围系数
 */
void MouseThread::updateConfig(
    int resolution,
    int fovX,
    int fovY,
    double minSpeedMultiplier,
    double maxSpeedMultiplier,
    double predictionInterval,
    bool auto_shoot,
    float bScope_multiplier
)
{
    // 注意：调用者（keyboard_listener / mouse_thread_loop / 构造函数）必须已持有 configMutex
    // 此处不再重复加锁，避免 std::mutex 同一线程重入导致未定义行为

    screen_width = screen_height = resolution;
    fov_x = fovX;  fov_y = fovY;
    min_speed_multiplier = minSpeedMultiplier;
    max_speed_multiplier = maxSpeedMultiplier;
    prediction_interval = predictionInterval;
    this->auto_shoot = auto_shoot;
    this->bScope_multiplier = bScope_multiplier;

    center_x = center_y = resolution / 2.0;
    max_distance = std::hypot(resolution, resolution) / 2.0;

    // 重新加载轨迹模拟参数并重置状态
    wind_mouse_enabled = config.wind_mouse_enabled;
    wind_G = config.wind_G; wind_W = config.wind_W;
    wind_M = config.wind_M; wind_D = config.wind_D;
    resetWindState();
    clearWindDebugTrail();

    // 贝塞尔轨迹 + EMA 平滑
    bezier_enabled = config.bezier_enabled;
    bezier_strength = config.bezier_strength;
    move_ema_enabled = config.move_ema_enabled;
    move_ema_alpha = config.move_ema_alpha;

    // 移动控制库执行控制器（统一使用 Pure Pursuit，参数自动推导）
    motionChangeProtection = config.motion_change_protection;
    {
        // 自动推导执行器参数（基于分辨率）
        double res = static_cast<double>(resolution);
        double autoGain = 0.85;
        double autoDeadZone = std::max(1.0, res / 320.0);
        double autoSmoothing = 0.8;
        // 用户可在高级参数中覆盖，若配置值与自动值偏差很小则使用自动值
        bool useAuto = (std::abs(config.pure_pursuit_gain - autoGain) < 0.01f &&
                        std::abs(config.pure_pursuit_dead_zone - autoDeadZone) < 0.1f &&
                        std::abs(config.pure_pursuit_smoothing - autoSmoothing) < 0.01f);
        double gain = useAuto ? autoGain : static_cast<double>(config.pure_pursuit_gain);
        double dz   = useAuto ? autoDeadZone : static_cast<double>(config.pure_pursuit_dead_zone);
        double sm   = useAuto ? autoSmoothing : static_cast<double>(config.pure_pursuit_smoothing);
        purePursuitController = execution::PurePursuitController(
            gain, 60.0, 25.0, 0.6, dz, sm);
        // 启用速度前馈：基于预测时间的前馈系数，补偿目标自身运动
        purePursuitController.setFeedforwardCoeff(
            static_cast<double>(config.predictionInterval));
    }
    purePursuitController.reset();

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
void MouseThread::queueMove(int dx, int dy)
{
    if (dx == 0 && dy == 0)
    {
        return;
    }

    // Worker 线程已崩溃，丢弃移动指令避免队列无限积压
    if (!workerRunning.load())
        return;

    std::lock_guard lg(queueMtx);
    if (moveQueue.size() >= queueLimit) moveQueue.pop();  // 队列满时丢弃最旧指令
    moveQueue.push({ dx,dy });
    queueCv.notify_one();
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
                sendMovementToDriver(m.dx, m.dy);
                appendWindDebugStep(m.dx, m.dy);  // 记录调试轨迹
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
void MouseThread::bezierMoveRelative(int dx, int dy)
{
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
}

void MouseThread::windMouseMoveRelative(int dx, int dy)
{
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

    // 同时缓存预测配置（避免 predict_target_position 每次加锁）
    cachedPredictionEnabled     = config.prediction_enabled;
    cachedPredictionInterval    = static_cast<double>(config.predictionInterval);
    cachedPredictionTau         = static_cast<double>(config.prediction_tau);
    cachedPredictionCompensateDelay = config.prediction_compensate_delay;
    cachedPredictionResetTimeout    = static_cast<double>(config.prediction_reset_timeout_sec);

    // 缓存速度曲线参数（避免 calculate_speed_multiplier 每帧加锁）
    cachedSnapRadius        = config.snapRadius;
    cachedNearRadius        = config.nearRadius;
    cachedSpeedCurveExponent = config.speedCurveExponent;
    cachedSnapBoostFactor   = config.snapBoostFactor;
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

        const double degPerPxX = fov_x / std::max(1.0, screen_width);
        const double degPerPxY = fov_y / std::max(1.0, screen_height);

        if (std::abs(degPerPxX) > 1e-8 && std::abs(degPerPxY) > 1e-8)
        {
            deltaPxX = degX / degPerPxX;
            deltaPxY = degY / degPerPxY;
        }
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
void MouseThread::recordMotionCompensationStep(int dx, int dy)
{
    if (dx == 0 && dy == 0)
        return;

    const auto delta = mouseCountsToScreenPixels(dx, dy);
    if (std::abs(delta.first) < 1e-8 && std::abs(delta.second) < 1e-8)
        return;

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(motionCompensationMutex);
    pruneMotionCompensationTrailLocked(now);

    double cumX = motionCompensationTrail.empty() ? 0.0
        : motionCompensationTrail.back().cumX;
    double cumY = motionCompensationTrail.empty() ? 0.0
        : motionCompensationTrail.back().cumY;
    motionCompensationTrail.push_back(
        { delta.first, delta.second, cumX + delta.first, cumY + delta.second, now });

    constexpr size_t maxSamples = 512;
    while (motionCompensationTrail.size() > maxSamples)
        motionCompensationTrail.pop_front();
}

/**
 * pruneMotionCompensationTrailLocked
 *
 * 清理运动补偿轨迹中超过生命周期（2 秒）的旧采样。
 * 调用者必须已持有 motionCompensationMutex 锁。
 *
 * @param now 当前时间点
 */
void MouseThread::pruneMotionCompensationTrailLocked(const std::chrono::steady_clock::time_point& now)
{
    constexpr auto motionTrailLifetime = std::chrono::seconds(2);
    while (!motionCompensationTrail.empty() && (now - motionCompensationTrail.front().t) > motionTrailLifetime)
        motionCompensationTrail.pop_front();
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
    if (motionCompensationTrail.empty())
        return { 0.0, 0.0 };

    // 二分查找第一个 t >= since 的采样（前缀和 O(log n) + O(1)）
    auto it = std::lower_bound(
        motionCompensationTrail.begin(), motionCompensationTrail.end(), since,
        [](const MotionCompensationSample& s, const std::chrono::steady_clock::time_point& t) {
            return s.t < t;
        });
    if (it == motionCompensationTrail.end())
        return { 0.0, 0.0 };  // 无采样 >= since

    const double totalX = motionCompensationTrail.back().cumX;
    const double totalY = motionCompensationTrail.back().cumY;
    const double prevX = (it != motionCompensationTrail.begin()) ? (it - 1)->cumX : 0.0;
    const double prevY = (it != motionCompensationTrail.begin()) ? (it - 1)->cumY : 0.0;
    return { totalX - prevX, totalY - prevY };
}

/**
 * predict_target_position
 *
 * 统一预测流水线。自适应选择最优速度源，单一路径完成：
 *   速度估计 (外部SOT > 帧间差分) → 时间校正EMA滤波 → 常速外推 → 自适应钳位
 *
 * 四项优化：
 *   1. 时间校正 EMA — α = 1-exp(-dt/τ)，跨帧率一致响应
 *   2. 自适应钳位 — 基于目标速度动态计算预测上限
 *   3. 按轴死区 — 逐轴判定，省 hypot + 更精确
 *   4. 热路径内联 — 无函数调用 + 零锁读取
 *
 * @param target_x,y      检测到的目标坐标
 * @param observationTime 观测时间戳
 * @return                预测后的目标位置
 */
std::pair<double, double> MouseThread::predict_target_position(
    double target_x, double target_y,
    std::chrono::steady_clock::time_point observationTime)
{
    auto current_time = std::chrono::steady_clock::now();
    if (observationTime.time_since_epoch().count() == 0)
        observationTime = current_time;

    double observationAgeSec = std::chrono::duration<double>(current_time - observationTime).count();
    if (!std::isfinite(observationAgeSec) || observationAgeSec < 0.0)
        observationAgeSec = 0.0;

    // ===== 预测关闭 =====
    if (!cachedPredictionEnabled)
    {
        prev_x = target_x; prev_y = target_y;
        prev_time = observationTime;
        return { target_x, target_y };
    }

    // ===== 首帧 / 目标丢失后初始化 =====
    if (prev_time.time_since_epoch().count() == 0 || !target_detected.load())
    {
        prev_time = observationTime;
        prev_x = target_x; prev_y = target_y;
        prev_velocity_x = 0.0; prev_velocity_y = 0.0;
        emaVelX.reset(0.0); emaVelY.reset(0.0);
        emaPosX.reset(target_x); emaPosY.reset(target_y);
        hasExternalVelocity = false;
        return { target_x, target_y };
    }

    // ===== 帧间隔 dt =====
    double dt = std::chrono::duration<double>(observationTime - prev_time).count();
    if (!std::isfinite(dt) || dt <= 0.0) dt = frameIntervalSec(captureFps.load());
    dt = std::clamp(dt, 1.0 / 500.0, 0.25);

    // ===== 优化1: 时间校正 EMA 系数（dt 缓存: 仅变化 >10% 时重算） =====
    double alphaVel, alphaPos;
    if (std::abs(dt - cachedDtExp) / std::max(dt, 1e-9) > 0.1)
    {
        const double tauVel = cachedPredictionTau;
        const double tauPos = cachedPredictionTau * 2.0;
        cachedAlphaVel = 1.0 - std::exp(-dt / tauVel);
        cachedAlphaPos = 1.0 - std::exp(-dt / tauPos);
        cachedDtExp = dt;
    }
    alphaVel = cachedAlphaVel;
    alphaPos = cachedAlphaPos;

    // ===== 速度估计 (自动选最优源) =====
    double velX = 0.0, velY = 0.0;  // 默认零初始化，防止 NaN 分支未赋值
    if (hasExternalVelocity)
    {
        // 优先: SOT Kalman 外部速度（已是 px/sec，由 mouse_thread_loop 转换）
        // NaN/Inf 防护：异常外部速度清零并回退到帧间差分估计
        if (std::isfinite(externalTargetVelX) && std::isfinite(externalTargetVelY))
        {
            velX = externalTargetVelX;
            velY = externalTargetVelY;
            // 同步更新 EMA 滤波器，保持状态最新；回退时无需重置
            emaVelX.setAlpha(alphaVel);
            emaVelY.setAlpha(alphaVel);
            emaVelX.update(velX);
            emaVelY.update(velY);
        }
        else
        {
            hasExternalVelocity = false; // 异常数据，放弃本次外部速度
            // velX/velY 保持 0.0，下一帧通过帧间差分恢复
        }
    }
    else
    {
        double dispX = target_x - prev_x;
        double dispY = target_y - prev_y;

        // ===== 优化3: 径向死区 — 位移<2px时整帧速度清零，保留对角线方向 =====
        double rawVx, rawVy;
        double disp = std::hypot(dispX, dispY);
        if (disp < 2.0)
        {
            rawVx = 0.0;
            rawVy = 0.0;
        }
        else
        {
            rawVx = dispX / dt;
            rawVy = dispY / dt;
        }

        // 跳变保护: 位移异常大 → 重置
        if (disp > 800.0)
        {
            prev_time = observationTime;
            prev_x = target_x; prev_y = target_y;
            emaVelX.reset(0.0); emaVelY.reset(0.0);
            emaPosX.reset(target_x); emaPosY.reset(target_y);
            hasExternalVelocity = false;
            return { target_x, target_y };
        }

        emaVelX.setAlpha(alphaVel);
        emaVelY.setAlpha(alphaVel);
        velX = emaVelX.update(rawVx);
        velY = emaVelY.update(rawVy);
    }

    // ===== 位置滤波 =====
    emaPosX.setAlpha(alphaPos);
    emaPosY.setAlpha(alphaPos);
    double filteredX = emaPosX.update(target_x);
    double filteredY = emaPosY.update(target_y);

    // ===== 存储状态 =====
    prev_velocity_x = velX;
    prev_velocity_y = velY;
    prev_x = target_x;
    prev_y = target_y;
    prev_time = observationTime;

    // ===== 优化4: 热路径内联延迟计算 =====
    double totalDelay = cachedPredictionInterval;
    if (cachedPredictionCompensateDelay && observationAgeSec > 0.0)
        totalDelay += std::clamp(observationAgeSec, 0.0, 0.35);
    totalDelay = std::clamp(totalDelay, 0.0, 1.5);

    // ===== 常速外推 =====
    double predX = filteredX + velX * totalDelay;
    double predY = filteredY + velY * totalDelay;

    // ===== 优化2: 自适应钳位 — 基于速度动态计算 =====
    double speed = std::hypot(velX, velY);
    double maxOffset = speed * totalDelay * 1.5;
    maxOffset = std::clamp(maxOffset, 50.0, 800.0);

    predX = std::clamp(predX, target_x - maxOffset, target_x + maxOffset);
    predY = std::clamp(predY, target_y - maxOffset, target_y + maxOffset);

    // ===== NaN 回退 =====
    if (!std::isfinite(predX)) predX = target_x;
    if (!std::isfinite(predY)) predY = target_y;

    return { predX, predY };
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
void MouseThread::sendMovementToDriver(int dx, int dy)
{
    if (dx == 0 && dy == 0)
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(inputDevicesMutex);

        if (!mouseInput)
        {
            return;
        }

        if (!mouseInput->move(dx, dy))
        {
            return;
        }
    }

    recordMotionCompensationStep(dx, dy);  // 记录用于运动补偿
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

    // 帧率补偿：以 30 FPS 为基准，高帧率时缩减，低帧率时放大
    double corr = 1.0;
    double fps = static_cast<double>(captureFps.load());
    fps = std::clamp(fps, 1.0, 500.0);  // 与 frameIntervalSec 一致的钳位
    if (fps > 30.0) corr = 30.0 / fps;
    else if (fps > 0.0 && fps < 30.0) corr = 30.0 / fps;

    // 使用缓存的游戏配置计算 counts（避免每帧加锁 configMutex）
    double scale = (cachedGameFovScaled && cachedGameBaseFOV > 1.0) ? (fov_x / cachedGameBaseFOV) : 1.0;
    double cx = mmx / (cachedGameSens * cachedGameYaw * scale);
    double cy = mmy / (cachedGameSens * cachedGamePitch * scale);
    double move_x = cx * speed * corr;
    double move_y = cy * speed * corr;

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

    // 帧率补偿：以 30 FPS 为基准，高帧率时缩减，低帧率时放大
    double corr = 1.0;
    double fps = static_cast<double>(captureFps.load());
    fps = std::clamp(fps, 1.0, 500.0);  // 与 frameIntervalSec 一致的钳位
    if (fps > 30.0) corr = 30.0 / fps;
    else if (fps > 0.0 && fps < 30.0) corr = 30.0 / fps;  // 低帧率时放大补偿

    // FOV 缩放修正
    double scale = (cachedGameFovScaled && cachedGameBaseFOV > 1.0)
        ? (fov_x / cachedGameBaseFOV) : 1.0;

    // 角度 → 鼠标计数
    double cx = degX / (cachedGameSens * cachedGameYaw * scale);
    double cy = degY / (cachedGameSens * cachedGamePitch * scale);

    cx *= speedCurveMultiplier * corr;
    cy *= speedCurveMultiplier * corr;

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
    std::lock_guard lg(input_method_mutex);

    auto predicted = predict_target_position(
        target.x + target.w / 2.0,
        target.y + target.h / 2.0);

    auto mv = calc_movement(predicted.first, predicted.second);
    queueMove(static_cast<int>(mv.first), static_cast<int>(mv.second));

    // 消费外部速度标志（moveMouse 不使用 PurePursuit 前馈）
    hasExternalVelocity = false;
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
 * @param pivotX        目标 pivot 点 X 坐标
 * @param pivotY        目标 pivot 点 Y 坐标
 * @param observationTime  检测观测时间戳（用于运动补偿）
 */
void MouseThread::moveMousePivot(
    double pivotX,
    double pivotY,
    std::chrono::steady_clock::time_point observationTime,
    int targetClassId)
{
    std::lock_guard lg(input_method_mutex);

    // ===== 流水线追踪：Stage 1 原始目标 =====
    PipelineFrame* pf = nullptr;
    if (g_pipelineTracer.isEnabled())
    {
        pf = &g_pipelineTracer.beginFrame(static_cast<int>(screen_width));
        pf->rawPivotX = pivotX;
        pf->rawPivotY = pivotY;
        pf->targetClassId = targetClassId;
        pf->targetDetected = true;
        pf->fpsValue = static_cast<double>(captureFps.load());
    }

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
        pf->velocityX = prev_velocity_x;
        pf->velocityY = prev_velocity_y;
        pf->hasExternalVel = hasExternalVelocity;
    }

    // 预计算速度曲线倍率（基于目标到屏幕中心的像素距离）
    double offx = predicted.first - center_x;
    double offy = predicted.second - center_y;
    double distToTarget = std::hypot(offx, offy);
    double speedCurve = calculate_speed_multiplier(distToTarget);

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
        double targetVx = 0.0, targetVy = 0.0;

        if (hasExternalVelocity)
        {
            // SOT Kalman 速度 (px/sec)，用于 PurePursuit 前馈补偿目标自身运动
            targetVx = externalTargetVelX;
            targetVy = externalTargetVelY;
        }

        double dxOut = 0.0, dyOut = 0.0;
        purePursuitController.computeMovement2D(
            predicted.first, predicted.second,
            screenCx, screenCy,
            dxOut, dyOut,
            true, targetVx, targetVy);

        ppDxOut = dxOut;
        ppDyOut = dyOut;

        // ===== 像素空间增量 → 鼠标计数（含灵敏度/FOV/帧率修正+速度曲线） =====
        auto counts = pixelDeltaToCounts(dxOut, dyOut, speedCurve);
        move_x = counts.first;
        move_y = counts.second;

        // 外部速度已同时用于预测和 PurePursuit 前馈，在此统一消费
        hasExternalVelocity = false;
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
    if (bezier_enabled || wind_mouse_enabled)
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

    // ===== 流水线追踪：Stage 8 最终输出 =====
    if (pf)
    {
        pf->finalMx = mx;
        pf->finalMy = my;
    }

    if (mx == 0 && my == 0)
    {
        return;
    }

    // === 轨迹分发 ===
    if (bezier_enabled)
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
}

/**
 * clearQueuedMoves
 *
 * 清空所有待处理的鼠标移动指令队列。
 * 同时重置轨迹模拟状态，确保后续运动从干净状态开始。
 *
 * 在预测重置、目标丢失或参数变更时调用。
 */
void MouseThread::clearQueuedMoves()
{
    std::lock_guard<std::mutex> lock(queueMtx);
    std::queue<Move> empty;
    moveQueue.swap(empty);  // 高效清空队列
    resetWindState();
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
            delay = baseDelay * delayDist(windRng);
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
            hold = baseHold * holdDist(windRng);
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
 * resetPrediction
 *
 * 完全重置预测状态。包括：
 *   - 清空移动指令队列
 *   - 重置上一帧时间和位置
 *   - 重置速度
 *   - 重置 EMA 滤波器状态
 *   - 清除目标检测标志
 *
 * 通常由 checkAndResetPredictions 在目标长时间丢失时自动触发。
 */
void MouseThread::resetPrediction()
{
    clearQueuedMoves();
    prev_time = std::chrono::steady_clock::time_point();
    prev_x = 0;
    prev_y = 0;
    prev_velocity_x = 0;
    prev_velocity_y = 0;
    emaVelX.reset(0.0);
    emaVelY.reset(0.0);
    emaPosX.reset(0.0);
    emaPosY.reset(0.0);
    hasExternalVelocity = false;
    target_detected.store(false);
    // 目标切换时重置确认帧计数，避免新目标跳过确认延迟
    stableFrameCount = 0;
    fireScheduled = false;
    holdScheduled = false;
}

/**
 * checkAndResetPredictions
 *
 * 检查自上一次目标检测以来经过的时间，如果超过超时阈值，
 * 则自动重置预测状态。
 *
 * 超时时间从配置中读取，钳位在 [0.05, 3.0] 秒范围内。默认通常为 0.5 秒。
 * 防止目标已离开但预测器仍输出旧轨迹的问题。
 */
void MouseThread::checkAndResetPredictions()
{
    auto current_time = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(current_time - last_target_time).count();
    double resetTimeoutSec = cachedPredictionResetTimeout;
    const double timeoutSec = std::clamp(resetTimeoutSec, 0.05, 3.0);

    if (elapsed > timeoutSec && target_detected.load())
    {
        resetPrediction();
    }
}

/**
 * predictFuturePositions
 *
 * 预测未来多帧的目标位置，用于可视化显示。
 * 基于当前速度和位置进行线性外推。
 *
 * @param pivotX 当前目标 X 坐标
 * @param pivotY 当前目标 Y 坐标
 * @param frames 需要预测的帧数
 * @return 未来各帧的预测位置列表，每项为 (x, y)
 */
std::vector<std::pair<double, double>> MouseThread::predictFuturePositions(double pivotX, double pivotY, int frames)
{
    std::vector<std::pair<double, double>> result;
    if (frames <= 0)
        return result;

    result.reserve(frames);

    const double frame_time = frameIntervalSec(captureFps.load());

    double vx = prev_velocity_x;
    double vy = prev_velocity_y;

    // 检查速度是否有效 (零速度意味着目标静止，仍输出同位置)
    auto current_time = std::chrono::steady_clock::now();
    double dtSinceLast = std::chrono::duration<double>(current_time - prev_time).count();
    if (prev_time.time_since_epoch().count() == 0 || dtSinceLast > 0.5)
    {
        return result;
    }

    for (int i = 1; i <= frames; i++)
    {
        double t = frame_time * i;
        double px = pivotX + vx * t;
        double py = pivotY + vy * t;
        if (!std::isfinite(px) || !std::isfinite(py))
            continue;
        result.push_back({ px, py });
    }

    return result;
}

/**
 * storeFuturePositions
 *
 * 存储预测的未来帧位置，供可视化模块（叠加显示）使用。
 * 线程安全，内部持有 futurePositionsMutex。
 *
 * @param positions 未来位置列表
 */
void MouseThread::storeFuturePositions(const std::vector<std::pair<double, double>>& positions)
{
    std::lock_guard<std::mutex> lock(futurePositionsMutex);
    futurePositions = positions;
}

/**
 * clearFuturePositions
 *
 * 清空未来位置预测数据。
 */
void MouseThread::clearFuturePositions()
{
    std::lock_guard<std::mutex> lock(futurePositionsMutex);
    futurePositions.clear();
}

/**
 * getFuturePositions
 *
 * 获取存储的未来帧位置预测数据。
 * 返回副本而非引用，确保线程安全。
 *
 * @return 未来位置列表
 */
std::vector<std::pair<double, double>> MouseThread::getFuturePositions()
{
    std::lock_guard<std::mutex> lock(futurePositionsMutex);
    return futurePositions;
}

/**
 * clearWindDebugTrail
 *
 * 清空轨迹模拟调试轨迹，并将光标位置重置到屏幕中心。
 */
void MouseThread::clearWindDebugTrail()
{
    std::lock_guard<std::mutex> lock(windDebugTrailMutex);
    windDebugTrail.clear();
    windDebugCursorX = center_x;
    windDebugCursorY = center_y;
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
    std::lock_guard<std::mutex> lock(windDebugTrailMutex);
    const auto now = std::chrono::steady_clock::now();
    pruneWindDebugTrailLocked(now);

    std::vector<std::pair<double, double>> out;
    out.reserve(windDebugTrail.size());
    for (const auto& p : windDebugTrail)
        out.emplace_back(p.x, p.y);
    return out;
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
