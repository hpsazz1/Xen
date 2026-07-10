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
    int virtual_camera_heigth;
    std::string ndi_source_name;

    // ========== 目标设置 ==========
    bool disable_headshot;
    float body_y_offset;
    float head_y_offset;
    bool auto_aim;
    bool tracker_enabled;
    bool tracker_overlay_table_enabled;

    // ========== FOV 与速度设置 ==========
    int fovX;
    int fovY;
    float minSpeedMultiplier;
    float maxSpeedMultiplier;

    // ========== 目标预测（含卡尔曼滤波）==========
    float predictionInterval;
    int prediction_futurePositions;
    bool draw_futurePositions;
    bool kalman_enabled;
    float kalman_process_noise_position;
    float kalman_process_noise_velocity;
    float kalman_measurement_noise;
    float kalman_velocity_damping;
    float kalman_max_velocity;
    int kalman_warmup_frames;
    bool kalman_compensate_detection_delay;
    float kalman_additional_prediction_ms;
    float kalman_reset_timeout_sec;

    float snapRadius;
    float nearRadius;
    float speedCurveExponent;
    float snapBoostFactor;

    bool easynorecoil; // 简易压枪
    float easynorecoilstrength; // 压枪强度
    std::string input_method; // 输入方式："WIN32", "GHUB", "RAZER", "ARDUINO", "RP2350", "TEENSY41", "TEENSY41_HID", "KMBOX_NET", "KMBOX_A", "MAKCU"

    // ========== 轨迹模拟 ==========
    bool wind_mouse_enabled;  // 是否启用轨迹模拟
    float wind_G;             // 鼠标移速系数
    float wind_W;             // 轨迹摆动幅度
    float wind_M;             // 单步移动上限
    float wind_D;             // 微调距离阈值

    // ========== Arduino 串口鼠标 ==========
    int arduino_baudrate;
    std::string arduino_port;
    bool arduino_16_bit_mouse;
    bool arduino_enable_keys;

    // ========== RP2350 串口鼠标 ==========
    int rp2350_baudrate;
    std::string rp2350_port;
    bool rp2350_16_bit_mouse;
    bool rp2350_enable_keys;

    // ========== Teensy 4.1 RawHID 通用鼠标桥接 ==========
    std::string teensy_hid_serial;
    std::string teensy_hid_vid_filter;
    std::string teensy_hid_pid_filter;
    int teensy_hid_usage_page;
    int teensy_hid_usage_id;
    int teensy_hid_open_index;
    int teensy_hid_packet_timeout_ms;
    int teensy_hid_reconnect_interval_ms;

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
    bool game_overlay_draw_future;
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
    float game_overlay_future_point_radius;
    float game_overlay_future_alpha_falloff;

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

    std::string joinStrings(const std::vector<std::string>& vec, const std::string& delimiter = ",");
private:
    std::vector<std::string> splitString(const std::string& str, char delimiter = ',');
    std::string config_path;
};

#endif // CONFIG_H
