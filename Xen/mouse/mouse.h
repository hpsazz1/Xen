#ifndef MOUSE_H
#define MOUSE_H

#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <Windows.h>

#include <mutex>
#include <atomic>
#include <chrono>
#include <vector>
#include <utility>
#include <queue>
#include <thread>
#include <condition_variable>
#include <deque>
#include <random>

#include "AimbotTarget.h"
#include "MouseInput.h"
#include "runtime/basic_aim_controller.h"
#include "runtime/basic_target_filter.h"
#include "runtime/target_predictor.h"
#include "runtime/view_motion_history.h"

/**
 * @brief 鼠标控制主线程类
 *
 * 负责管理基础目标滤波、误差控制和鼠标设备输出。
 * 通过独立的线程处理鼠标指令队列，支持多种鼠标输入设备。
 */
class MouseThread
{
private:
    // ==================== 配置参数 ====================
    double screen_width;             ///< 检测裁剪宽度（像素），用于目标中心和检测空间阈值
    double screen_height;            ///< 检测裁剪高度（像素），用于目标中心和检测空间阈值
    double fov_x;                    ///< 水平完整视场角（度）
    double fov_y;                    ///< 垂直完整视场角（度）
    double max_distance;             ///< 最大瞄准距离（像素）
    double move_response_seconds = 0.080; ///< 基础控制响应时间
    double move_max_speed_cps = 1440.0;   ///< 设备最大速度（counts/sec）；四链路九宫格复测后按真实观测 dt 动态换算单帧预算
    double move_integral_time_seconds = 0.0; ///< 匀速移动目标积分时间；0 表示关闭实验性 PI 补偿
    double center_x;                 ///< 屏幕中心 X
    double center_y;                 ///< 屏幕中心 Y
    bool   auto_shoot;               ///< 是否启用自动射击
    float  bScope_multiplier;        ///< 瞄准镜倍率补偿
    bool   auto_stop_enabled;        ///< 开火时释放 WASD（仅 KMBOX_NET）
    float  auto_stop_hold_ms;        ///< 急停最短保持时间
    bool   unlock_y_enabled;         ///< 开火解锁 Y 轴
    float  unlock_y_threshold_ms;    ///< 按住多久后解锁
    float  unlock_y_strength;        ///< 解锁强度
    float  fire_correction_strength; ///< 射击修正强度

    // ==================== 开火拟人化状态 ====================
    int    trigger_stable_frames;       ///< 连续确认帧数
    float  trigger_random_delay_ms;     ///< 反应延迟均值
    float  trigger_delay_jitter_ms;     ///< 反应延迟抖动
    float  trigger_hold_ms;             ///< 按键时长均值
    float  trigger_hold_jitter_ms;      ///< 按键时长抖动
    float  trigger_shot_cooldown_ms;    ///< 两发最小间隔
    int    stableFrameCount = 0;        ///< 当前连续确认计数
    std::chrono::steady_clock::time_point lastShotTime{};     ///< 上次开火时间
    std::chrono::steady_clock::time_point leftPressStartTime{}; ///< 左键按下时间 (用于急停/解锁Y)
    bool   fireScheduled = false;       ///< 是否有待执行的开火
    std::chrono::steady_clock::time_point fireScheduleTime{};  ///< 计划开火时间
    bool   holdScheduled = false;       ///< 是否有待执行的松开
    std::chrono::steady_clock::time_point holdReleaseTime{};   ///< 计划松开时间
    bool   wasdReleased = false;        ///< 是否已激活 WASD 互相抵消（开火急停）

    // ==================== 运动状态 ====================
    BasicTargetFilter targetFilter;                                ///< 基础观测滤波（不做未来预测）
    TargetPredictor targetPredictor;                               ///< 仅连续真实观测生效的前瞻预测器
    BasicAimController aimController;                              ///< 帧率无关的基础误差控制器
    BasicTargetFilter::Result lastFilterResult{};                  ///< 流水线诊断快照
    TargetPredictor::Result lastPredictionResult{};                ///< 流水线预测诊断快照
    TargetPredictor::Settings predictionSettings{};                ///< 运行时预测配置缓存
    std::chrono::steady_clock::time_point lastControlObservationTime{}; ///< 上一有效观测时间，用于网络抖动下逐帧计算 dt
    struct PredictionObservationContext
    {
        double screenX = 0.0;
        double screenY = 0.0;
        double viewX = 0.0;
        double viewY = 0.0;
    };
    std::deque<PredictionObservationContext> predictionObservationHistory; ///< 最近三帧屏幕与自身视角观测
    std::chrono::steady_clock::time_point last_target_time;        ///< 最后检测到目标的时间
    std::atomic<bool> target_detected{ false };                   ///< 是否检测到目标（atomic 为未来多线程访问预留）
    std::atomic<bool> mouse_pressed{ false };                     ///< 鼠标是否按下（atomic，与 leftPressStartTime 同线程访问）

    IMouseInput* mouseInput;  ///< 鼠标输入设备接口指针

    /** @brief 向驱动程序发送鼠标移动数据 */
    void sendMovementToDriver(int dx, int dy);

    /** @brief 鼠标移动指令结构 */
    struct Move { int dx; int dy; };

    // ==================== 移动指令队列 ====================
    std::queue<Move>              moveQueue;     ///< 移动指令队列
    std::mutex                    queueMtx;      ///< 队列互斥锁
    std::condition_variable       queueCv;       ///< 队列条件变量
    std::mutex                    input_method_mutex;  ///< 输入方法互斥锁（保护 moveMouse/moveMousePivot 并发调用）
    const size_t                  queueLimit = 20;///< 队列最大长度（需容纳 Bezier 14步 + Wind 5步）
    std::thread                   moveWorker;    ///< 移动工作线程
    std::atomic<bool>             workerStop{ false };  ///< 工作线程停止标志
    std::atomic<bool>             workerRunning{ true }; ///< 工作线程健康标志（异常退出时置false）

    // ==================== 配置缓存 ====================
    // 缓存活跃游戏配置的静态值，避免每次鼠标移动都加锁查询 map
    double                        cachedGameSens = 1.0;
    double                        cachedGameYaw = 0.022;
    double                        cachedGamePitch = 0.022;
    bool                          cachedGameFovScaled = false;
    double                        cachedGameBaseFOV = 0.0;
    // 缓存速度曲线参数（避免 calculate_speed_multiplier 每帧加锁）
    float                         cachedSnapRadius = 1.5f;
    float                         cachedNearRadius = 25.0f;
    float                         cachedSpeedCurveExponent = 3.0f;
    float                         cachedSnapBoostFactor = 1.15f;


    /** @brief 移动工作线程主循环 */
    void moveWorkerLoop();
    /** @brief 向队列添加移动指令 */
    void queueMove(int dx, int dy);

#if 0 // Trajectory simulation will be reintroduced only after the base stage is validated.
    // ==================== 轨迹模拟 ====================
    bool   wind_mouse_enabled = true;   ///< 是否启用轨迹模拟
    double wind_G, wind_W, wind_M, wind_D;  ///< 鼠标移速、轨迹摆动、单步上限、微调距离
    bool   bezier_enabled = false;      ///< 是否启用贝塞尔弧线轨迹
    float  bezier_strength = 0.35f;     ///< 贝塞尔弧度
    double bezierFracX = 0.0;           ///< 贝塞尔亚像素余量 X
    double bezierFracY = 0.0;           ///< 贝塞尔亚像素余量 Y
    Trajectory::BezierState trajectoryBezierState;
    int trajectoryFrameCounter = 0;
    bool   move_ema_enabled = false;    ///< 是否启用 EMA 平滑
    float  move_ema_alpha = 0.60f;      ///< EMA 平滑系数
    double prev_ema_move_x = 0.0;       ///< 上帧 EMA 平滑后 X
    double prev_ema_move_y = 0.0;

    // ==================== 过冲模拟状态 ====================
    int    osCooldown = 0;              ///< 过冲冷却帧计数

    // ==================== 移动控制库执行控制器 ====================
    execution::PurePursuitController purePursuitController;           ///< Pure Pursuit 控制器（统一执行层）
    filters::MotionChangeDetector motionChangeDetector;               ///< 运动突变检测器
    bool motionChangeProtection = false;                              ///< 是否启用运动突变保护

    void   windMouseMoveRelative(int dx, int dy);  ///< 轨迹模拟相对移动
    void   bezierMoveRelative(int dx, int dy);     ///< 贝塞尔弧线轨迹移动
    void   resetWindState();              ///< 重置轨迹模拟状态
    void   appendWindDebugStep(int dx, int dy);  ///< 追加轨迹模拟调试步进
    void   pruneWindDebugTrailLocked(const std::chrono::steady_clock::time_point& now);  ///< 修剪轨迹模拟调试轨迹
    std::pair<double, double> mouseCountsToScreenPixels(int dx, int dy) const;  ///< 鼠标计数转屏幕像素

    /** @brief 轨迹模拟调试点结构 */
    struct WindDebugPoint
    {
        double x = 0.0;                                   ///< X 坐标
        double y = 0.0;                                   ///< Y 坐标
        std::chrono::steady_clock::time_point t{};         ///< 时间戳
    };

    // 持久轨迹模拟状态，避免每帧出现"重置"感
    double windCarryX = 0.0;      ///< 轨迹模拟携带量 X
    double windCarryY = 0.0;      ///< 轨迹模拟携带量 Y
    double windVelX = 0.0;        ///< 轨迹模拟速度 X
    double windVelY = 0.0;        ///< 轨迹模拟速度 Y
    double windNoiseX = 0.0;      ///< 轨迹模拟噪声 X
    double windNoiseY = 0.0;      ///< 轨迹模拟噪声 Y
    double windFracX = 0.0;       ///< 轨迹模拟分数 X
    double windFracY = 0.0;       ///< 轨迹模拟分数 Y
    double windPatternX = 0.0;    ///< 轨迹模拟模式 X
    double windPatternY = 0.0;    ///< 轨迹模拟模式 Y
    double windPatternPhaseA = 0.0;  ///< 轨迹模拟模式相位 A
    double windPatternPhaseB = 0.0;  ///< 轨迹模拟模式相位 B
    double windPatternRateA = 0.0;   ///< 轨迹模拟模式速率 A
    double windPatternRateB = 0.0;   ///< 轨迹模拟模式速率 B
    Trajectory::WindState trajectoryWindState;
    std::mt19937 windRng{ std::random_device{}() };  ///< 轨迹模拟随机数生成器

    std::deque<WindDebugPoint> windDebugTrail;       ///< 轨迹模拟调试轨迹
    std::mutex                             windDebugTrailMutex;  ///< 轨迹模拟调试轨迹互斥锁
    double                                 windDebugCursorX = 0.0;  ///< 轨迹模拟调试光标 X
    double                                 windDebugCursorY = 0.0;  ///< 轨迹模拟调试光标 Y
#endif

    std::mt19937 behaviorRng{ std::random_device{}() }; ///< 射击时序随机源，与移动轨迹无关
    std::pair<double, double> mouseCountsToScreenPixels(int dx, int dy) const;

    // ==================== 运动补偿 ====================
    mutable std::mutex motionCompensationMutex;                   ///< 运动补偿互斥锁
    ViewMotionHistory motionCompensationHistory;                  ///< 自身视角累计位移时间线
    void recordMotionCompensationStep(int dx, int dy);            ///< 记录运动补偿步进
    std::pair<double, double> getMotionCompensationAt(
        std::chrono::steady_clock::time_point time) const;        ///< 查询指定时刻累计自身视角位移

    /** @brief 计算从当前位置到目标位置的移动量（像素→counts，含速度曲线+FPS修正） */
    std::pair<double, double> calc_movement(double target_x, double target_y);
    /** @brief 像素位移增量 → 鼠标计数（含灵敏度/FOV/帧率修正，速度曲线由调用者传入） */
    std::pair<double, double> pixelDeltaToCounts(double dpx, double dpy, double speedCurveMultiplier);
    /** @brief 根据距离计算速度倍率 */
    double calculate_speed_multiplier(double distance);

    /** @brief 刷新缓存的游戏配置值（在 updateConfig 和构造函数中调用） */
    void refreshGameProfileCache();

public:
    /**
     * @brief 构造函数
     * @param resolution 屏幕分辨率
     * @param fovX 水平视野
     * @param fovY 垂直视野
     * @param auto_shoot 自动射击
     * @param bScope_multiplier 瞄准镜倍率
     * @param mouseInputDevice 鼠标输入设备
     */
    MouseThread(
        int  resolution,
        int  fovX,
        int  fovY,
        bool auto_shoot,
        float bScope_multiplier,
        IMouseInput* mouseInputDevice = nullptr
    );
    ~MouseThread();

    /**
     * @brief 更新配置参数
     */
    void updateConfig(
        int resolution,
        int fovX,
        int fovY,
        bool auto_shoot,
        float bScope_multiplier
    );

    /**
     * @brief 根据目标框和枢轴点执行追踪瞄准
     * @param target 包含目标框、类别和枢轴点的真实检测目标
     * @param observationTime 观测时间戳
     */
    void moveMousePivot(
        const AimbotTarget& target,
        std::chrono::steady_clock::time_point observationTime = {});
    /** @brief 相对移动鼠标 */
    void moveRelative(int dx, int dy);
    /** @brief 清除所有排队中的移动指令 */
    void clearQueuedMoves();
    size_t pendingMoveCount();
    /** @brief 对检测观测进行基础滤波（不做未来外推） */
    std::pair<double, double> filter_target_position(
        double target_x,
        double target_y,
        std::chrono::steady_clock::time_point observationTime = {},
        bool useMotionTrend = false);
    /** @brief 执行鼠标移动（对目标） */
    void moveMouse(const AimbotTarget& target);
    /** @brief 按下鼠标（射击） */
    void pressMouse(const AimbotTarget& target);
    /** @brief 释放鼠标 */
    void releaseMouse();
    /** @brief 重置基础跟踪与控制状态 */
    void resetTracking();
    /** @brief 检查目标是否在准星范围内 */
    bool check_target_in_scope(double target_x, double target_y,
        double target_w, double target_h, double reduction_factor);

    /** @brief 清除轨迹模拟调试轨迹 */
    void clearWindDebugTrail();
    /** @brief 获取轨迹模拟调试轨迹 */
    std::vector<std::pair<double, double>> getWindDebugTrail();
    /** @brief 获取自指定时间起的运动补偿量 */
    std::pair<double, double> getMotionCompensationSince(
        std::chrono::steady_clock::time_point since) const;

    /** @brief 设置鼠标输入设备 */
    void setMouseInput(IMouseInput* newMouseInput);

    /** @brief 设置目标检测状态 */
    void setTargetDetected(bool detected) { target_detected.store(detected); }
    /** @brief 设置最后检测到目标的时间 */
    void setLastTargetTime(const std::chrono::steady_clock::time_point& t) { last_target_time = t; }
};

#endif // MOUSE_H
