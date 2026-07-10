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

namespace
{
/**
 * buildKalmanSettingsFromConfigUnlocked
 *
 * 从全局配置构建卡尔曼滤波器参数结构体（无锁版本）。
 * 调用者必须已持有 configMutex 锁。
 *
 * 配置项包括：
 *   - kalman_enabled: 卡尔曼滤波器启用开关
 *   - process_noise_position: 位置过程噪声，控制滤波器对目标位置变化的信任度
 *   - process_noise_velocity: 速度过程噪声，控制对速度变化的跟踪灵敏度
 *   - measurement_noise: 测量噪声，控制对检测结果的信任度（越大越平滑）
 *   - velocity_damping: 速度阻尼系数，抑制速度突变
 *   - max_velocity: 最大允许速度，防止异常跳变
 *   - warmup_frames: 预热帧数，在达到该帧数前滤波器输出未完全收敛
 */
aim::AimKalmanSettings buildKalmanSettingsFromConfigUnlocked()
{
    aim::AimKalmanSettings settings;
    settings.enabled = config.kalman_enabled;
    settings.process_noise_position = static_cast<double>(config.kalman_process_noise_position);
    settings.process_noise_velocity = static_cast<double>(config.kalman_process_noise_velocity);
    settings.measurement_noise = static_cast<double>(config.kalman_measurement_noise);
    settings.velocity_damping = static_cast<double>(config.kalman_velocity_damping);
    settings.max_velocity = static_cast<double>(config.kalman_max_velocity);
    settings.warmup_frames = config.kalman_warmup_frames;
    return settings;
}

/**
 * buildKalmanSettingsFromConfig
 *
 * 从全局配置构建卡尔曼滤波器参数（加锁版本）。
 * 内部持有 configMutex 后委托给 buildKalmanSettingsFromConfigUnlocked。
 */
aim::AimKalmanSettings buildKalmanSettingsFromConfig()
{
    std::lock_guard<std::mutex> lock(configMutex);
    return buildKalmanSettingsFromConfigUnlocked();
}

/**
 * currentFrameIntervalSec
 *
 * 计算当前每帧的时间间隔（秒）。
 * 优先使用 captureFps 原子变量中的实时 FPS；
 * 若 FPS 无效（<=0），则回退到配置中的 capture_fps；
 * 最终将 FPS 钳位在 [15, 500] 范围内以防止极端值。
 *
 * @return 帧间隔时间（秒），例如 60 FPS 时返回约 0.0167
 */
double currentFrameIntervalSec()
{
    double fps = static_cast<double>(captureFps.load());
    if (fps <= 0.0)
    {
        std::lock_guard<std::mutex> lock(configMutex);
        fps = (config.capture_fps > 0) ? static_cast<double>(config.capture_fps) : 60.0;
    }

    fps = std::clamp(fps, 15.0, 500.0);
    return 1.0 / fps;
}
}

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
    }
    resetWindState();
    clearWindDebugTrail();
    cachedKalmanSettings = buildKalmanSettingsFromConfig();
    targetKalman.setSettings(cachedKalmanSettings);
    targetKalman.reset();
    lastKalmanTelemetry = {};
    lastPredictionLookaheadSec = 0.0;
    lastDetectionDelaySec = 0.0;
    refreshGameProfileCache();

    // 启动后台工作线程，负责从队列中取出移动指令并发送到驱动
    moveWorker = std::thread(&MouseThread::moveWorkerLoop, this);
}

// ============================================================
// 参数预设定义
// ============================================================
namespace {

struct ParamPreset
{
    const char* name;
    float speedCurveExponent, snapRadius, nearRadius, minSpeed, maxSpeed, snapBoost;
    bool bezier, ema; float bezierStr, emaAlpha;
    bool wind; float windG, windW, windM, windD;
    float predInterval; int futurePos;
    float kalmanProcNoise, kalmanMeasNoise, kalmanVelDamping;
    int stableFrames; float trigDelay, trigJitter, trigHold, trigHoldJitter, fireCorr;
};

const ParamPreset kPresets[] = {
    // name        spdCurve snap near minSpd maxSpd snapBst  bez ema bezStr emaA  wind windG windW windM windD  predInt futP kalPN  kalMN  kalVD  stblF trigDel trigJit trigHld trigHldJ fireCor
    { "custom",     3.0f,  1.5f,25.0f,0.10f,0.10f,1.15f, 0,  0,  0.35f,0.60f,1,  18,   15,   10,   8,     0.020f,12,  40.0f, 35.0f, 0.08f, 3,    45.0f,  13.0f,  16.0f,   14.0f,    0.0f },
    { "stable",     2.5f,  2.0f,30.0f,0.05f,0.08f,1.00f, 1,  1,  0.45f,0.35f,1,  22,   10,   7,    12,     0.015f, 8,  20.0f, 55.0f, 0.12f, 5,    55.0f,  18.0f,  20.0f,   16.0f,    0.8f },
    { "balanced",   3.0f,  1.5f,25.0f,0.10f,0.10f,1.15f, 1,  1,  0.35f,0.60f,1,  18,   15,   10,   8,     0.025f,12,  35.0f, 40.0f, 0.08f, 3,    45.0f,  13.0f,  16.0f,   14.0f,    0.5f },
    { "aggressive", 4.0f,  1.0f,20.0f,0.15f,0.15f,1.50f, 1,  1,  0.25f,0.75f,1,  14,   18,   12,   5,     0.035f,15,  45.0f, 35.0f, 0.06f, 2,    35.0f,  10.0f,  12.0f,   10.0f,    0.3f },
    { "fast",       2.0f,  0.5f,15.0f,0.25f,0.30f,2.00f, 0,  0,  0.10f,0.90f,1,  8,    22,   16,   3,     0.045f,18,  55.0f, 30.0f, 0.04f, 1,    25.0f,  8.0f,   8.0f,    6.0f,     0.0f },
};

constexpr int kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);

} // namespace

void MouseThread::applyPreset(const char* presetName)
{
    const ParamPreset* p = nullptr;
    for (int i = 0; i < kPresetCount; ++i)
    {
        if (std::strcmp(kPresets[i].name, presetName) == 0)
        {
            p = &kPresets[i];
            break;
        }
    }
    if (!p || std::strcmp(p->name, "custom") == 0) return;

    // 写入 config
    config.speedCurveExponent   = p->speedCurveExponent;
    config.snapRadius           = p->snapRadius;
    config.nearRadius           = p->nearRadius;
    config.minSpeedMultiplier   = p->minSpeed;
    config.maxSpeedMultiplier   = p->maxSpeed;
    config.snapBoostFactor      = p->snapBoost;
    config.bezier_enabled       = p->bezier;
    config.bezier_strength      = p->bezierStr;
    config.move_ema_enabled     = p->ema;
    config.move_ema_alpha       = p->emaAlpha;
    config.wind_mouse_enabled   = p->wind;
    config.wind_G               = p->windG;
    config.wind_W               = p->windW;
    config.wind_M               = p->windM;
    config.wind_D               = p->windD;
    config.prediction_mode           = "linear";  // 所有预设统一用线性预测
    config.predictionInterval       = p->predInterval;
    config.prediction_futurePositions   = p->futurePos;
    config.kalman_process_noise_position = p->kalmanProcNoise;
    config.kalman_measurement_noise      = p->kalmanMeasNoise;
    config.kalman_velocity_damping       = p->kalmanVelDamping;
    config.trigger_stable_frames     = p->stableFrames;
    config.trigger_random_delay_ms   = p->trigDelay;
    config.trigger_delay_jitter_ms   = p->trigJitter;
    config.trigger_hold_ms           = p->trigHold;
    config.trigger_hold_jitter_ms    = p->trigHoldJitter;
    config.fire_correction_strength  = p->fireCorr;
    config.preset_style = presetName;

    // 同步到 mouseThread
    if (globalMouseThread)
    {
        globalMouseThread->updateConfig(
            config.detection_resolution, config.fovX, config.fovY,
            config.minSpeedMultiplier, config.maxSpeedMultiplier,
            config.predictionInterval, config.auto_shoot, config.bScope_multiplier);
    }
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
    predictionMode = config.prediction_mode;

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

    cachedKalmanSettings = buildKalmanSettingsFromConfigUnlocked();
    targetKalman.setSettings(cachedKalmanSettings);
    targetKalman.reset();
    lastKalmanTelemetry = {};
    lastPredictionLookaheadSec = 0.0;
    lastDetectionDelaySec = 0.0;
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
        std::cerr << "[Mouse] Move worker crashed: " << e.what() << std::endl;
    }
    catch (...)
    {
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

    // Perlin 风格平滑噪声：相邻帧的随机值连续，避免逐帧跳跃
    auto smoothNoise = [](int frame, double freq) -> double {
        double t = static_cast<double>(frame) * freq * 0.1;
        int i = static_cast<int>(std::floor(t));
        double f = t - i;
        f = f * f * (3.0 - 2.0 * f); // smoothstep
        auto hash = [](int n) -> double {
            n = (n << 13) ^ n;
            return (1.0 - static_cast<double>(
                (n * (n * n * 15731 + 789221) + 1376312589) & 0x7fffffff) / 1073741824.0);
        };
        return hash(i) * (1.0 - f) + hash(i + 1) * f;
    };

    double nx = -dy / dist;
    double ny =  dx / dist;
    double offset = dist * static_cast<double>(bezier_strength) * 0.65;

    // 两个独立控制点，使用不同频率的 Perlin 噪声
    double side1 = smoothNoise(frameCounter, 1.7);
    double side2 = smoothNoise(frameCounter, 2.3);

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

        int stepX = static_cast<int>(std::round(bx - pbx));
        int stepY = static_cast<int>(std::round(by - pby));
        if (stepX != 0 || stepY != 0)
            queueMove(stepX, stepY);
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

    // 随机分布生成器
    std::uniform_real_distribution<double> noiseDist(-1.0, 1.0);
    std::uniform_real_distribution<double> clipDist(0.55, 1.0);
    constexpr double twoPi = 6.28318530717958647692;

    // 根据累积位移幅度计算子步数，步数 = max(1, min(5, floor(mag * 0.24) + 1))
    const double carryMag = std::hypot(windCarryX, windCarryY);
    const int maxSubsteps = std::clamp(static_cast<int>(carryMag * 0.24) + 1, 1, 5);

    for (int i = 0; i < maxSubsteps; ++i)
    {
        const double dist = std::hypot(windCarryX, windCarryY);
        const double velMag = std::hypot(windVelX, windVelY);

        // 剩余距离和速度都很小时提前结束
        if (dist < 0.20 && velMag < 0.12)
            break;

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
        if (windPatternPhaseA > twoPi) windPatternPhaseA = std::fmod(windPatternPhaseA, twoPi);
        if (windPatternPhaseB > twoPi) windPatternPhaseB = std::fmod(windPatternPhaseB, twoPi);

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

    // 最终限幅：防止累积位移无限增长，上限 120
    const double carryCap = 120.0;
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
    constexpr double twoPi = 6.28318530717958647692;
    std::uniform_real_distribution<double> phaseDist(0.0, twoPi);
    std::uniform_real_distribution<double> rateDist(0.04, 0.16);

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
    motionCompensationTrail.push_back({ delta.first, delta.second, now });

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

    double x = 0.0;
    double y = 0.0;
    std::lock_guard<std::mutex> lock(motionCompensationMutex);
    for (const auto& sample : motionCompensationTrail)
    {
        if (sample.t >= since)
        {
            x += sample.x;
            y += sample.y;
        }
    }

    return { x, y };
}

/**
 * currentDetectionDelaySec
 *
 * 估算当前检测延迟（秒），即从画面捕获到目标检测完成的时间。
 *
 * 计算优先级：
 *   1. 如果 observationAgeSec 有效且 >= 0，直接使用它
 *   2. 否则根据编译配置（USE_CUDA / DML）读取检测器的最新推理时间
 *   3. 默认兜底值为 0.05 秒（50ms）
 *
 * 结果被钳位在 [0, 0.35] 秒范围内。
 *
 * @param observationAgeSec 观测数据的时间年龄（可选），若无效则使用推理时间
 * @return 检测延迟（秒）
 */
double MouseThread::currentDetectionDelaySec(double observationAgeSec) const
{
    double detectionDelaySec = 0.05;
    if (std::isfinite(observationAgeSec) && observationAgeSec >= 0.0)
    {
        detectionDelaySec = observationAgeSec;
    }
    else
    {
        // 使用检测器最新推理时间作为延迟估计
#ifdef USE_CUDA
        detectionDelaySec = trt_detector.lastInferenceTime.count() * 0.001;
#else
        if (dml_detector)
            detectionDelaySec = dml_detector->lastInferenceTimeDML.count() * 0.001;
#endif
    }
    if (!std::isfinite(detectionDelaySec))
        detectionDelaySec = 0.05;
    return std::clamp(detectionDelaySec, 0.0, 0.35);
}

/**
 * currentPredictionLookaheadSec
 *
 * 计算当前预测前瞻时间（秒）。卡尔曼滤波器将在这个时间点之后
 * 预测目标位置，以补偿检测延迟、网络延迟和运动滞后。
 *
 * 计算公式：
 *   前瞻时间 = max(0, prediction_interval)
 *              + (若补偿检测延迟开启) max(0, detectionDelaySec)
 *              + additionalPredictionMs 转换为秒
 *
 * 结果被钳位在 [0, 1.5] 秒范围内。
 *
 * @param detectionDelaySec 当前的检测延迟
 * @return 预测前瞻时间（秒）
 */
double MouseThread::currentPredictionLookaheadSec(double detectionDelaySec) const
{
    double lookahead = std::max(0.0, prediction_interval);
    bool compensateDetectionDelay = false;
    float additionalPredictionMs = 0.0f;
    {
        std::lock_guard<std::mutex> lock(configMutex);
        compensateDetectionDelay = config.kalman_compensate_detection_delay;
        additionalPredictionMs = config.kalman_additional_prediction_ms;
    }

    if (compensateDetectionDelay)
        lookahead += std::max(0.0, detectionDelaySec);
    lookahead += static_cast<double>(additionalPredictionMs) * 0.001;
    return std::clamp(lookahead, 0.0, 1.5);
}

/**
 * predict_target_position
 *
 * 使用卡尔曼滤波器预测目标在当前帧的未来位置。
 * 这是自瞄精度的核心 —— 通过历史轨迹预测目标在检测延迟之后的位置。
 *
 * 处理流程：
 *   1. 计算观测数据的年龄（自观测到当前的时间差）
 *   2. 首次检测或重置后，初始化状态并返回原始坐标
 *   3. 计算时间步长 dt（两次观测的时间差），钳位在 [1/500, 0.25] 秒
 *   4. 更新前存储当前速度和位置作为上一帧值
 *   5. 计算检测延迟和前瞻时间
 *   6. 调用卡尔曼滤波器 update，获得预测后的目标位置
 *   7. 对预测结果进行有限性检查，无效时回退到原始坐标
 *
 * @param target_x        检测到的目标中心 X 坐标
 * @param target_y        检测到的目标中心 Y 坐标
 * @param observationTime 观测时间点（检测完成的时间戳）
 * @return 预测后的目标位置 (predictedX, predictedY)
 */
std::pair<double, double> MouseThread::predict_target_position(
    double target_x,
    double target_y,
    std::chrono::steady_clock::time_point observationTime)
{
    auto current_time = std::chrono::steady_clock::now();
    if (observationTime.time_since_epoch().count() == 0)
        observationTime = current_time;

    double observationAgeSec = std::chrono::duration<double>(current_time - observationTime).count();
    if (!std::isfinite(observationAgeSec) || observationAgeSec < 0.0)
        observationAgeSec = 0.0;

    // ── 模式：关闭 ──
    if (predictionMode == "off")
    {
        prev_x = target_x; prev_y = target_y;
        prev_time = observationTime;
        return { target_x, target_y };
    }

    // ── 模式：仅延迟补偿 ──
    if (predictionMode == "delay")
    {
        // 只补偿推理延迟，不做未来预测
        double delaySec = currentDetectionDelaySec(observationAgeSec);
        double dx = (prev_time.time_since_epoch().count() != 0)
            ? (target_x - prev_x) : 0.0;
        double dy = (prev_time.time_since_epoch().count() != 0)
            ? (target_y - prev_y) : 0.0;
        prev_x = target_x; prev_y = target_y;
        prev_time = observationTime;
        double px = target_x + dx * delaySec / std::max(0.001, currentFrameIntervalSec());
        double py = target_y + dy * delaySec / std::max(0.001, currentFrameIntervalSec());
        return { px, py };
    }

    // ── 模式：线性预测（EMA 平滑速度 + 匀速外推）──
    if (predictionMode == "linear")
    {
        if (prev_time.time_since_epoch().count() == 0 || !target_detected.load())
        {
            prev_x = target_x; prev_y = target_y;
            prev_time = observationTime;
            smoothedVelX = 0.0; smoothedVelY = 0.0;
            return { target_x, target_y };
        }

        double dt = std::chrono::duration<double>(observationTime - prev_time).count();
        if (dt <= 0.0) dt = currentFrameIntervalSec();
        dt = std::clamp(dt, 1.0 / 500.0, 0.25);

        // EMA 平滑速度（alpha=0.25 平衡响应与抗抖）
        double rawVx = (target_x - prev_x) / dt;
        double rawVy = (target_y - prev_y) / dt;

        // 死区过滤：位移 < 2px 视为检测框抖动，不更新速度
        double rawDisp = std::hypot(target_x - prev_x, target_y - prev_y);
        if (rawDisp < 2.0)
        {
            rawVx = 0.0;
            rawVy = 0.0;
        }

        const double velAlpha = 0.25;
        smoothedVelX = smoothedVelX * velAlpha + rawVx * (1.0 - velAlpha);
        smoothedVelY = smoothedVelY * velAlpha + rawVy * (1.0 - velAlpha);

        prev_x = target_x; prev_y = target_y;
        prev_time = observationTime;

        double delaySec = currentDetectionDelaySec(observationAgeSec);
        double lookahead = std::max(0.0, static_cast<double>(prediction_interval));
        if (config.kalman_compensate_detection_delay)
            lookahead += std::max(0.0, delaySec);
        lookahead = std::clamp(lookahead, 0.0, 1.5);

        double px = target_x + smoothedVelX * lookahead;
        double py = target_y + smoothedVelY * lookahead;
        return { px, py };
    }

    // ── 模式：卡尔曼（原逻辑）──
    targetKalman.setSettings(cachedKalmanSettings);

    if (prev_time.time_since_epoch().count() == 0 || !target_detected.load())
    {
        prev_time = observationTime;
        prev_x = target_x;
        prev_y = target_y;
        prev_velocity_x = 0.0;
        prev_velocity_y = 0.0;
        targetKalman.reset();
        const double detectionDelaySec = currentDetectionDelaySec(observationAgeSec);
        const double lookaheadSec = currentPredictionLookaheadSec(detectionDelaySec);
        lastKalmanTelemetry = targetKalman.update(target_x, target_y, currentFrameIntervalSec(), lookaheadSec);
        lastDetectionDelaySec = detectionDelaySec;
        lastPredictionLookaheadSec = lookaheadSec;
        return { target_x, target_y };
    }

    double dt = std::chrono::duration<double>(observationTime - prev_time).count();
    if (!std::isfinite(dt) || dt <= 0.0) dt = currentFrameIntervalSec();
    dt = std::clamp(dt, 1.0 / 500.0, 0.25);

    prev_time = observationTime;
    prev_x = target_x;
    prev_y = target_y;

    const double detectionDelaySec = currentDetectionDelaySec(observationAgeSec);
    const double lookaheadSec = currentPredictionLookaheadSec(detectionDelaySec);
    lastDetectionDelaySec = detectionDelaySec;
    lastPredictionLookaheadSec = lookaheadSec;

    // 执行卡尔曼滤波器更新和预测
    lastKalmanTelemetry = targetKalman.update(target_x, target_y, dt, lookaheadSec);
    prev_velocity_x = lastKalmanTelemetry.velocity_x;
    prev_velocity_y = lastKalmanTelemetry.velocity_y;

    double predictedX = lastKalmanTelemetry.predicted_x;
    double predictedY = lastKalmanTelemetry.predicted_y;
    if (!std::isfinite(predictedX)) predictedX = target_x;  // 防 NaN 回退
    if (!std::isfinite(predictedY)) predictedY = target_y;

    return { predictedX, predictedY };
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
 * 简单的相对移动封装，直接将移动发送到底层驱动。
 * 不经过轨迹模拟算法或队列（直接发送），适用于不需要平滑处理的场景。
 *
 * @param dx 水平移动量
 * @param dy 垂直移动量
 */
void MouseThread::moveRelative(int dx, int dy)
{
    sendMovementToDriver(dx, dy);
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

    // 帧率补偿：以 30 FPS 为基准，高帧率时缩减移动量
    double corr = 1.0;
    double fps = static_cast<double>(captureFps.load());
    if (fps > 30.0) corr = 30.0 / fps;

    std::pair<double, double> counts_pair;
    {
        std::lock_guard<std::mutex> lock(configMutex);
        counts_pair = config.degToCounts(mmx, mmy, fov_x);  // 角度 -> 鼠标计数
    }
    double move_x = counts_pair.first * speed * corr;
    double move_y = counts_pair.second * speed * corr;

    return { move_x, move_y };
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
    float snapRadius = 0.0f;
    float nearRadius = 0.0f;
    float speedCurveExponent = 1.0f;
    float snapBoostFactor = 1.0f;
    {
        std::lock_guard<std::mutex> lock(configMutex);
        snapRadius = config.snapRadius;
        nearRadius = config.nearRadius;
        speedCurveExponent = config.speedCurveExponent;
        snapBoostFactor = config.snapBoostFactor;
    }

    // 吸附区域：目标很近时使用最小速度（精确微调）乘以加成系数
    if (distance < snapRadius)
        return min_speed_multiplier * snapBoostFactor;

    // 近端区域：指数曲线插值
    if (nearRadius > 0.0f && distance < nearRadius)
    {
        double t = distance / nearRadius;
        double curve = 1.0 - std::pow(1.0 - t, static_cast<double>(speedCurveExponent));
        return min_speed_multiplier +
            (max_speed_multiplier - min_speed_multiplier) * curve;
    }

    // 远端区域：线性映射到 [min, max]
    double norm = std::clamp(distance / max_distance, 0.0, 1.0);
    return min_speed_multiplier +
        (max_speed_multiplier - min_speed_multiplier) * norm;
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
 * 自瞄主入口：根据 AimbotTarget 执行一次鼠标移动。
 *
 * 流程：
 *   1. 锁定输入方法互斥锁
 *   2. 计算目标中心坐标
 *   3. 使用卡尔曼滤波预测目标位置
 *   4. 计算所需移动量
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
}

/**
 * moveMousePivot
 *
 * 基于旋转支点（pivot）的自瞄移动。相比 moveMouse，额外支持：
 *
 *   1. 运动补偿：如果提供了观测时间，减去自该时间以来相机移动造成的偏移，
 *      从而分离"目标自身移动"和"自瞄旋转导致的视差变化"。
 *   2. 轨迹模拟支持：如果启用了轨迹模拟，将移动指令交由
 *      windMouseMoveRelative 处理，产生类人曲线轨迹；
 *      否则直接投递到队列。
 *
 * @param pivotX        目标 pivot 点 X 坐标
 * @param pivotY        目标 pivot 点 Y 坐标
 * @param observationTime  检测观测时间戳（用于运动补偿）
 */
void MouseThread::moveMousePivot(
    double pivotX,
    double pivotY,
    std::chrono::steady_clock::time_point observationTime)
{
    std::lock_guard lg(input_method_mutex);
    if (observationTime.time_since_epoch().count() != 0)
    {
        // 运动补偿：减去自瞄自身旋转导致的屏幕偏移
        auto cameraDelta = getMotionCompensationSince(observationTime);
        pivotX -= cameraDelta.first;
        pivotY -= cameraDelta.second;
    }

    auto predicted = predict_target_position(pivotX, pivotY, observationTime);
    auto mv = calc_movement(predicted.first, predicted.second);
    double move_x = mv.first;
    double move_y = mv.second;

    // === EMA 输出平滑 ===
    if (move_ema_enabled)
    {
        double a = static_cast<double>(move_ema_alpha);
        move_x = prev_ema_move_x * a + move_x * (1.0 - a);
        move_y = prev_ema_move_y * a + move_y * (1.0 - a);
        prev_ema_move_x = move_x;
        prev_ema_move_y = move_y;
    }

    // === 开火解锁 Y 轴 ===
    if (unlock_y_enabled && mouse_pressed.load())
    {
        auto now = std::chrono::steady_clock::now();
        auto heldMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - leftPressStartTime).count();
        if (heldMs > static_cast<long long>(unlock_y_threshold_ms))
        {
            // ramp: 在 threshold 之后 gradual 释放 Y 轴
            double yFactor = 1.0 - static_cast<double>(unlock_y_strength);
            if (yFactor < 0.0) yFactor = 0.0;
            move_y *= yFactor;
        }
    }

    // === 惯性过冲模拟 (Overshoot) ===
    // 仅在长距离甩枪时偶尔触发（追踪时不触发），模拟真人惯性过头
    if (bezier_enabled || wind_mouse_enabled)
    {
        double totalDist = std::hypot(move_x, move_y);
        // 只在大幅度拉枪（> 50px）且非贴脸追踪时触发
        if (totalDist > 50.0)
        {
            static int osCounter = 0;
            static int osCooldown = 0;
            osCounter++;
            // 冷却：每次触发后跳过 8-15 帧，避免连续抖动
            if (osCooldown > 0) { osCooldown--; }
            else
            {
                double osRoll = [](int f) {
                    double t = f * 0.17;
                    int i = (int)std::floor(t);
                    double fr = t - i; fr = fr * fr * (3.0 - 2.0 * fr);
                    auto h = [](int n){ n=(n<<13)^n; return (1.0-((n*(n*n*15731+789221)+1376312589)&0x7fffffff)/1073741824.0); };
                    return h(i)*(1.0-fr)+h(i+1)*fr;
                }(osCounter);

                if (osRoll > 0.72)  // ~14% chance per eligible frame
                {
                    double osFactor = 1.02 + (osRoll - 0.72) * 0.28;  // 1.02 ~ 1.08
                    if (osFactor > 1.08) osFactor = 1.08;
                    move_x *= osFactor;
                    move_y *= osFactor;
                    osCooldown = 10 + static_cast<int>((osRoll + 1.0) * 10.0);  // 10-30 frame cooldown
                }
            }
        }
    }

    int mx = static_cast<int>(std::round(move_x));
    int my = static_cast<int>(std::round(move_y));

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
        // 使用轨迹模拟算法产生类人曲线轨迹
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
        // 生成随机延迟
        double delay = std::max(0.0, static_cast<double>(trigger_random_delay_ms)
            + (trigger_delay_jitter_ms > 0.0f
                ? std::normal_distribution<double>(0.0, static_cast<double>(trigger_delay_jitter_ms))(windRng)
                : 0.0));
        fireScheduleTime = now + std::chrono::microseconds(static_cast<long long>(delay * 1000.0));
        fireScheduled = true;

        // 生成随机保持时长
        double hold = std::max(1.0, static_cast<double>(trigger_hold_ms)
            + (trigger_hold_jitter_ms > 0.0f
                ? std::normal_distribution<double>(0.0, static_cast<double>(trigger_hold_jitter_ms))(windRng)
                : 0.0));
        holdReleaseTime = fireScheduleTime + std::chrono::microseconds(static_cast<long long>(hold * 1000.0));
        holdScheduled = true;
    }

    // === 执行开火 ===
    if (fireScheduled && !mouse_pressed && now >= fireScheduleTime)
    {
        std::lock_guard<std::mutex> lock(inputDevicesMutex);
        if (!mouseInput) return;

        // 自动急停：开火前释放 WASD
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

        // 恢复 WASD
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

        // 恢复 WASD（自动急停结束）
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
 * 完全重置卡尔曼预测状态。包括：
 *   - 清空移动指令队列
 *   - 重置上一帧时间和位置
 *   - 重置速度
 *   - 重置卡尔曼滤波器内部状态
 *   - 清空遥测数据
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
    targetKalman.reset();
    lastKalmanTelemetry = {};
    lastPredictionLookaheadSec = 0.0;
    lastDetectionDelaySec = 0.0;
    target_detected.store(false);
}

/**
 * checkAndResetPredictions
 *
 * 检查自上一次目标检测以来经过的时间，如果超过超时阈值，
 * 则自动重置预测状态。
 *
 * 超时时间从配置中读取（kalman_reset_timeout_sec），
 * 钳位在 [0.05, 3.0] 秒范围内。默认通常为 0.5 秒。
 *
 * 防止目标已离开但滤波器仍输出旧轨迹的问题。
 */
void MouseThread::checkAndResetPredictions()
{
    auto current_time = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(current_time - last_target_time).count();
    double resetTimeoutSec = 0.5;
    {
        std::lock_guard<std::mutex> lock(configMutex);
        resetTimeoutSec = static_cast<double>(config.kalman_reset_timeout_sec);
    }
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
 *
 * 首选方案（卡尔曼已初始化）：
 *   在 baseLookaheadSec 基础上依次累加帧间隔时间，
 *   调用卡尔曼滤波器的 predict 方法获取各帧预测位置。
 *
 * 回退方案（卡尔曼未初始化）：
 *   使用上一帧的速度进行简单的线性外推。
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

    const double frame_time = currentFrameIntervalSec();

    targetKalman.setSettings(cachedKalmanSettings);
    if (targetKalman.initialized())
    {
        const double detectionDelaySec = (lastDetectionDelaySec > 0.0)
            ? lastDetectionDelaySec
            : currentDetectionDelaySec();
        const double baseLookaheadSec = currentPredictionLookaheadSec(detectionDelaySec);
        for (int i = 1; i <= frames; ++i)
        {
            const double t = baseLookaheadSec + frame_time * i;
            auto predicted = targetKalman.predict(t);
            if (!std::isfinite(predicted.first) || !std::isfinite(predicted.second))
                continue;
            result.push_back(predicted);
        }

        if (!result.empty())
            return result;
    }

    // 回退方案：线性外推
    auto current_time = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(current_time - prev_time).count();

    if (prev_time.time_since_epoch().count() == 0 || dt > 0.5)
    {
        return result;
    }

    double vx = prev_velocity_x;
    double vy = prev_velocity_y;

    for (int i = 1; i <= frames; i++)
    {
        double t = frame_time * i;
        double px = pivotX + vx * t;
        double py = pivotY + vy * t;
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
