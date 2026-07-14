#ifndef CONFIG_H
#define CONFIG_H

#include <string>
#include <vector>
#include <unordered_map>
#include <utility>

// 项目配置类，存储所有可配置参数
// 通过 config.ini 文件加载/保存，涵盖捕获、目标、鼠标、AI 等全部模块的设置

class Config
{
public:
    // ========== 捕获设置 ==========
    std::string capture_method; // 捕获方式："duplication_api", "winrt", "virtual_camera", "udp_capture", "ndi"
    std::string capture_target;
    std::string capture_window_title;
    std::string udp_ip;
    int udp_port;
    int udp_source_width;  // UDP 预裁剪 ROI 对应的完整游戏 FOV 宽度；0 表示使用 JPEG 编码宽度
    int udp_source_height; // UDP 预裁剪 ROI 对应的完整游戏 FOV 高度；0 表示使用 JPEG 编码高度
    int detection_resolution;
    int capture_fps;
    int monitor_idx;
    bool circle_fov_enabled;
    int circle_fov_radius_percent;
    bool circle_fov_show_preview;
    bool capture_borders;
    bool capture_cursor;
    std::string virtual_camera_name;
    int virtual_camera_width;
    int virtual_camera_height;
    std::string ndi_source_name;
    int ndi_source_width;  // NDI 预裁剪 ROI 对应的完整游戏 FOV 宽度；0 表示使用帧尺寸/元数据
    int ndi_source_height; // NDI 预裁剪 ROI 对应的完整游戏 FOV 高度；0 表示使用帧尺寸/元数据

    // ========== 目标设置 ==========
    bool disable_headshot;
    float body_y_offset;
    float head_y_offset;
    bool auto_aim;
    bool tracker_enabled;
    bool tracker_overlay_table_enabled;

    // ========== 跟踪器参数（统一使用 motion_lib 引擎） ==========
    bool   auto_derive_tracker_params;     // 是否根据分辨率/帧率自动推导跟踪器参数 (默认 true)
    int    ml_confirm_threshold;         // MOT 确认帧数 (默认 2)
    int    ml_termination_frames;        // MOT 终止帧数 (默认 8)
    float  ml_noise_vx;                  // 速度 X 过程噪声 (默认 1.0)
    float  ml_noise_vy;                  // 速度 Y 过程噪声 (默认 1.0)
    float  ml_noise_w;                   // 宽度过程噪声 (默认 0.01)
    float  ml_noise_h;                   // 高度过程噪声 (默认 0.01)
    float  ml_measurement_stddev;        // 测量标准差 (默认 5.0)
    int    ml_coast_frames;              // SOT 滑行帧数限制 (默认 15)
    std::string ml_selection_strategy;   // SOT 选择策略（固定 "nearest"，历史保留）
    float  ml_recapture_iou;             // 重捕获 IoU 阈值 (默认 0.3)
    float  ml_recapture_distance_mult;   // 重捕获距离乘数 (默认 2.5)
    float  ml_coast_velocity_decay;      // 滑行速度衰减 (默认 1.0)

    // ========== 执行控制器参数（统一使用 Pure Pursuit） ==========
    float  pure_pursuit_gain;            // PurePursuit 比例增益 (默认 0.85)
    float  pure_pursuit_dead_zone;       // PurePursuit 死区 (默认 2.0)
    float  pure_pursuit_smoothing;       // PurePursuit 平滑系数 (默认 0.8)
    bool   motion_change_protection;     // 是否启用运动突变保护 (默认 false)

    // ========== FOV 与速度设置 ==========
    int fovX;
    int fovY;
    float minSpeedMultiplier;
    float maxSpeedMultiplier;
    // 基础移动控制（当前主链路）。旧速度倍率仅用于配置迁移，不再参与运行。
    float move_response_ms;            // 误差响应时间，毫秒
    float move_max_speed_cps;          // 设备最大速度，counts/sec
    float move_integral_time_ms;        // 移动目标积分时间，毫秒；0 表示关闭

    // ========== 目标预测 ==========
    bool  prediction_enabled = true;              // 连续真实观测预测总开关
    float prediction_lead_ms = 50.0f;             // 观测年龄之外的基础前瞻，毫秒
    float prediction_velocity_tau_ms = 50.0f;     // 兼容旧键名，实际为稳健速度回归窗口，毫秒
    float prediction_strength = 1.0f;             // 常速度提前总强度
    bool profile_calibration_enabled = false;     // 被动Profile标定；只估算并展示，不自动覆盖配置

    float snapRadius;
    float nearRadius;
    float speedCurveExponent;
    float snapBoostFactor;

    bool easynorecoil; // 简易压枪
    float easynorecoilstrength; // 压枪强度
    std::string input_method; // 输入方式："WIN32", "GHUB", "RAZER", "KMBOX_NET", "KMBOX_A", "MAKCU"

    // ========== 贝塞尔轨迹曲线 ==========
    bool bezier_enabled;       // 是否启用 Bezier 弧线轨迹
    float bezier_strength;     // Bezier 弧度大小 (0=直线, 1=大弧)

    // ========== 输出平滑 ==========
    bool move_ema_enabled;     // 是否启用移动输出 EMA 平滑
    float move_ema_alpha;      // EMA 新值权重 (0~1, 越小越平滑, 0.3=强平滑 0.6=中等)

    // ========== 轨迹模拟 ==========
    bool wind_mouse_enabled;  // 是否启用轨迹模拟
    float wind_G;             // 鼠标移速系数
    float wind_W;             // 轨迹摆动幅度
    float wind_M;             // 单步移动上限
    float wind_D;             // 微调距离阈值

    // ========== Kmbox Net 网络串口 ==========
    std::string kmbox_net_ip;
    std::string kmbox_net_port;
    std::string kmbox_net_uuid;

    // ========== Kmbox B 系列串口 ==========
    std::string kmbox_a_pidvid; // PIDVID 在同一字段中，格式：PPPPVVVV

    // ========== Makcu 串口鼠标 ==========
    int makcu_baudrate;
    std::string makcu_port;

    // ========== 开火与缩放设置 ==========
    bool auto_shoot;
    float bScope_multiplier;

    // ========== 开火拟人化 ==========
    int trigger_stable_frames;       // 连续确认帧数（防误射）
    float trigger_random_delay_ms;   // 反应延迟中位数 (ms, 对数正态分布)
    float trigger_delay_jitter_ms;   // 延迟散布系数 (cv, 越大延迟越分散)
    float trigger_hold_ms;           // 按键时长中位数 (ms, 对数正态分布)
    float trigger_hold_jitter_ms;    // 按键时长散布系数
    float trigger_shot_cooldown_ms;  // 两发最小冷却间隔

    // ========== 自动急停 (仅 KMBOX_NET) ==========
    bool auto_stop_enabled;          // 是否启用自动急停
    float auto_stop_hold_ms;         // 急停保持时长

    // ========== 开火解锁 Y 轴 ==========
    bool unlock_y_enabled;           // 是否启用开火解锁 Y 轴
    float unlock_y_threshold_ms;     // 按住多久后解锁 (ms)
    float unlock_y_strength;         // 解锁强度 (0=完全解锁, 1=不锁)

    // ========== AI 射击修正 ==========
    float fire_correction_strength;  // 射击修正强度 (0=关闭)

    // ========== AI 推理设置 ==========
    std::string backend;
    int dml_device_id;
    std::string ai_model;
    float confidence_threshold;
    float nms_threshold;
    int max_detections;
#ifdef USE_CUDA
    bool export_enable_fp8;
    bool export_enable_fp16;
#endif
    bool fixed_input_size; // 是否固定模型输入尺寸

    // ========== CUDA 设置 ==========
#ifdef USE_CUDA
    bool use_cuda_graph;
    bool use_pinned_memory;
    int gpuMemoryReserveMB;
    bool enableGpuExclusiveMode;
    bool capture_use_cuda;
#endif

    // ========== 系统资源预留 ==========
    int cpuCoreReserveCount;
    int systemMemoryReserveMB;

    // ========== 按键绑定 ==========
    std::vector<std::string> button_targeting;
    std::vector<std::string> button_shoot;
    std::vector<std::string> button_zoom;
    std::vector<std::string> button_exit;
    std::vector<std::string> button_pause;
    std::vector<std::string> button_reload_config;
    std::vector<std::string> button_open_overlay;
    bool enable_arrows_settings;

    // ========== Dear ImGui 叠加层 ==========
    int overlay_opacity;
    float overlay_ui_scale;
    bool overlay_exclude_from_capture;
    int overlay_x;
    int overlay_y;
    int overlay_width;
    int overlay_height;

    // ========== 深度估计设置 ==========
    bool depth_inference_enabled;
    std::string depth_model_path;
    int depth_fps;
    int depth_colormap;
    bool depth_mask_enabled;
    int depth_mask_fps;
    int depth_mask_near_percent;
    int depth_mask_expand;
    int depth_mask_hold_frames;
    int depth_mask_alpha;
    bool depth_mask_invert;
    bool depth_debug_overlay_enabled;

    // ========== 游戏内覆盖层 ==========
    bool game_overlay_enabled;
    int game_overlay_max_fps;
    bool game_overlay_draw_boxes;
    bool game_overlay_compensate_latency;
    bool game_overlay_draw_wind_tail;
    bool game_overlay_draw_frame;
    bool game_overlay_draw_circle_fov;
    bool game_overlay_show_target_correction;
    int game_overlay_box_a;
    int game_overlay_box_r;
    int game_overlay_box_g;
    int game_overlay_box_b;
    int game_overlay_frame_a;
    int game_overlay_frame_r;
    int game_overlay_frame_g;
    int game_overlay_frame_b;
    float game_overlay_box_thickness;
    float game_overlay_frame_thickness;

    bool game_overlay_icon_enabled;
    std::string game_overlay_icon_path;
    int game_overlay_icon_width;
    int game_overlay_icon_height;
    float game_overlay_icon_offset_x;
    float game_overlay_icon_offset_y;
    std::string game_overlay_icon_anchor; // 图标锚点："center", "top", "bottom", "head"
    int game_overlay_icon_class; // 图标类别（-1 表示所有类别）

    // ========== 数据采集 ==========
    bool collect_data_while_playing;
    bool collect_only_when_aimbot_running;
    bool collect_only_when_targets_present;
    int collect_save_every_n_frames;
    int collect_jpeg_quality;
    std::string collect_output_dir;
    bool auto_label_data;
    float auto_label_min_conf;
    int auto_label_max_boxes;
    std::string auto_label_record_classes;

    // 将游戏叠加层的颜色值裁剪到 0-255 范围
    void clampGameOverlayColor()
    {
        auto clamp255 = [](int& v) { if (v < 0) v = 0; if (v > 255) v = 255; };
        clamp255(game_overlay_box_a);
        clamp255(game_overlay_box_r);
        clamp255(game_overlay_box_g);
        clamp255(game_overlay_box_b);
        clamp255(game_overlay_frame_a);
        clamp255(game_overlay_frame_r);
        clamp255(game_overlay_frame_g);
        clamp255(game_overlay_frame_b);
    }

    // ========== 目标类别 ==========
    int class_player;
    int class_head;

    // ========== 调试设置 ==========
    bool show_window;
    bool show_fps;
    std::vector<std::string> screenshot_button;
    int screenshot_delay;
    bool verbose;

    // ========== 流水线追踪 ==========
    bool pipeline_tracer_enabled = false;  ///< 是否启用流水线追踪
    int  pipeline_tracer_max_frames = 1000; ///< 环形缓冲最大帧数；可完整保留三次九点方位复测

    // 游戏配置文件结构体
    struct GameProfile
    {
        std::string name;   // 游戏名称
        double sens;        // 鼠标灵敏度
        double yaw;         // 水平方向每计数对应的角度
        double pitch;       // 垂直方向每计数对应的角度
        bool fovScaled;     // 是否根据 FOV 缩放
        double baseFOV;     // 基础 FOV 值
    };

    std::unordered_map<std::string, GameProfile> game_profiles;
    std::string                                  active_game;

    const GameProfile & currentProfile() const;
    std::pair<double, double> degToCounts(double degX, double degY, double fovNow) const;

    bool loadConfig(const std::string& filename = "config.ini");
    bool saveConfig(const std::string& filename = "config.ini");

    /** @brief 根据检测分辨率和捕获帧率自动推导跟踪器参数；不覆盖用户配置的移动执行参数 */
    void applyAutoDerivedTrackerParams(int detectionResolution, int captureFps);

    std::string joinStrings(const std::vector<std::string>& vec, const std::string& delimiter = ",");
private:
    std::vector<std::string> splitString(const std::string& str, char delimiter = ',');
    std::string config_path;
};

#endif // CONFIG_H
