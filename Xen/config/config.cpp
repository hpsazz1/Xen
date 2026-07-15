#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <string>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <algorithm>

#include "config.h"
#include "modules/SimpleIni.h"

extern std::mutex configMutex;

/**
 * splitString - 按分隔符分割字符串，并去除每个token的前后空白
 * @param str       待分割的字符串
 * @param delimiter 分隔符字符
 * @return 分割并清理后的字符串列表
 */
std::vector<std::string> Config::splitString(const std::string& str, char delimiter)
{
    std::vector<std::string> tokens;
    std::stringstream ss(str);
    std::string item;
    while (std::getline(ss, item, delimiter))
    {
        // 移除前导空白
        while (!item.empty() && (item.front() == ' ' || item.front() == '\t'))
            item.erase(item.begin());
        // 移除尾部空白
        while (!item.empty() && (item.back() == ' ' || item.back() == '\t'))
            item.pop_back();

        tokens.push_back(item);
    }
    return tokens;
}

/**
 * joinStrings - 将字符串列表用指定分隔符拼接
 * @param vec       待拼接的字符串列表
 * @param delimiter 分隔符
 * @return 拼接后的字符串
 */
std::string Config::joinStrings(const std::vector<std::string>& vec, const std::string& delimiter)
{
    std::ostringstream oss;
    for (size_t i = 0; i < vec.size(); ++i)
    {
        if (i != 0) oss << delimiter;
        oss << vec[i];
    }
    return oss.str();
}

bool Config::loadConfig(const std::string& filename)
{
    // 确定目标配置文件路径
    std::string target = filename.empty() ? "config.ini" : filename;
    std::error_code absEc;
    std::filesystem::path absPath = std::filesystem::absolute(target, absEc);
    config_path = absEc ? target : absPath.string();

    // 如果配置文件不存在，创建含有默认值的配置文件并保存
    if (!std::filesystem::exists(target))
    {
        std::cerr << "[Config] Config file does not exist, creating default config: " << target << std::endl;

        // === 画面捕获设置 ===
        capture_method = "duplication_api";              // 画面捕获方式：duplication_api (Windows桌面复制)
        capture_target = "monitor";                      // 捕获目标：monitor（显示器）或 window（窗口）
        capture_window_title = "";                       // 捕获窗口标题（空表示不指定）
        udp_ip = "192.168.3.10";                         // UDP 发送端 IP 过滤
        udp_port = 2333;                                 // UDP 接收端口
        udp_source_width = 2560;                         // UDP 320 ROI 对应的完整游戏 FOV 宽度
        udp_source_height = 1440;                        // UDP 320 ROI 对应的完整游戏 FOV 高度
        ndi_source_name = "HPSAZZ (main)";               // NDI 完整源名称
        ndi_source_width = 2560;                         // NDI 320 ROI 对应的完整游戏 FOV 宽度
        ndi_source_height = 1440;                        // NDI 320 ROI 对应的完整游戏 FOV 高度
        detection_resolution = 320;                      // AI 检测分辨率（160/320/640）
        capture_fps = 60;                                // 画面捕获帧率上限
        monitor_idx = 0;                                 // 显示器索引
        circle_fov_enabled = true;                       // 是否启用圆形视场限制
        circle_fov_radius_percent = 100;                 // 圆形 FOV 半径百分比
        circle_fov_show_preview = true;                  // 是否显示 FOV 预览
        capture_borders = true;                          // 是否捕获窗口边框
        capture_cursor = true;                           // 是否捕获鼠标光标
        virtual_camera_name = "None";                    // 虚拟摄像头名称
        virtual_camera_width = 1920;                     // 虚拟摄像头宽度
        virtual_camera_height = 1080;                    // 虚拟摄像头高度

        // === 瞄准目标设置 ===
        disable_headshot = false;                        // 是否禁用头部瞄准
        body_y_offset = 0.15f;                           // 身体瞄准点 Y 偏移
        head_y_offset = 0.05f;                           // 头部瞄准点 Y 偏移
        auto_aim = false;                                // 是否自动瞄准（无需按键）
        tracker_enabled = true;                          // 是否启用目标追踪器
        tracker_overlay_table_enabled = true;            // 是否显示追踪器表格

        // === 跟踪器参数 ===
        auto_derive_tracker_params = true;               // 自动推导开关（默认启用）
        ml_confirm_threshold = 2;                        // MOT 确认帧数
        ml_termination_frames = 8;                       // MOT 终止帧数
        ml_noise_vx = 1.0f;                              // 速度 X 过程噪声
        ml_noise_vy = 1.0f;                              // 速度 Y 过程噪声
        ml_noise_w = 0.01f;                              // 宽度过程噪声
        ml_noise_h = 0.01f;                              // 高度过程噪声
        ml_measurement_stddev = 5.0f;                    // 测量标准差
        ml_coast_frames = 15;                            // SOT 滑行帧数
        ml_selection_strategy = "nearest";               // SOT 选择策略
        ml_recapture_iou = 0.3f;                         // 重捕获 IoU 阈值
        ml_recapture_distance_mult = 2.5f;               // 重捕获距离乘数
        ml_coast_velocity_decay = 1.0f;                  // 滑行速度衰减

        // === 执行控制器 ===
        pure_pursuit_gain = 0.85f;
        pure_pursuit_dead_zone = 2.0f;
        pure_pursuit_smoothing = 0.8f;
        motion_change_protection = false;

        // === 鼠标移动设置 ===
        fovX = 106;                                      // 水平视野（度）
        fovY = 74;                                       // 垂直视野（度）
        minSpeedMultiplier = 0.1f;                       // 鼠标最小速度倍率
        maxSpeedMultiplier = 0.1f;                       // 鼠标最大速度倍率
        move_response_ms = 80.0f;                        // 基础控制响应时间（毫秒）
        move_max_speed_cps = 1440.0f;                    // 四链路九宫格复测值；1200 cps 下约八成远距帧仍受限
        move_integral_time_ms = 0.0f;                    // 默认关闭；320 ms 候选需先通过移动与静止复测
        aim_pipeline_mode = "legacy";                   // P0-0 默认保持 r30 正式输出
        aim_shadow_command_to_frame_delay_ms = 60.0f;    // 显式shadow候选，不自动采用被动标定结果
        aim_shadow_response_ms = 80.0f;                  // 第一子阶段仅启用P反馈
        aim_shadow_max_speed_cps = 1440.0f;
        aim_shadow_feedforward_gain = 0.0f;
        aim_shadow_settle_error_deg = 0.08f;
        aim_shadow_settle_rate_dps = 1.2f;
        aim_shadow_reverse_confirm_ms = 80.0f;
        aim_shadow_integral_time_ms = 0.0f;
        aim_shadow_integral_zone_deg = 1.0f;
        aim_shadow_lead_horizon_ms = 0.0f;
        aim_shadow_lead_strength = 0.0f;
        trajectory_shaper_mode = "off";                 // P0-4B默认先验证完全等价透传
        trajectory_output_hz = 240.0f;
        trajectory_max_velocity_cps = 1440.0f;
        trajectory_max_acceleration_cps2 = 60000.0f;
        trajectory_max_jerk_cps3 = 4000000.0f;

        prediction_enabled = true;                       // 连续真实观测预测总开关
        prediction_lead_ms = 50.0f;                      // 观测年龄之外的基础前瞻（毫秒）
        prediction_velocity_tau_ms = 50.0f;              // 稳健速度回归窗口（毫秒）
        prediction_strength = 1.0f;                      // 常速度提前总强度
        profile_calibration_enabled = false;             // 默认关闭，避免普通追踪数据污染标定

        snapRadius = 1.5f;                               // 瞄准吸附半径
        nearRadius = 25.0f;                              // "近距离"半径阈值
        speedCurveExponent = 3.0f;                       // 速度曲线指数（控制鼠标移动曲线）
        snapBoostFactor = 1.15f;                         // 吸附增强因子

        easynorecoil = false;                            // 是否启用简易无后座力
        easynorecoilstrength = 0.0f;                     // 无后座力强度
        input_method = "WIN32";                          // 输入方法

        // === 贝塞尔轨迹曲线 ===
        bezier_enabled = false;                          // 是否启用 Bezier 弧线
        bezier_strength = 0.35f;                         // 曲线弧度 (0=直线 1=大弧)

        // === 移动输出平滑 ===
        move_ema_enabled = false;                        // 是否启用 EMA 平滑
        move_ema_alpha = 0.60f;                          // 平滑系数 (越小越平滑)

        // === 轨迹模拟（模拟自然鼠标移动） ===
        wind_mouse_enabled = false;
        wind_G = 18.0f;                                  // 鼠标移速系数
        wind_W = 15.0f;                                  // 轨迹摆动幅度
        wind_M = 10.0f;                                  // 单步移动上限
        wind_D = 8.0f;                                   // 微调距离阈值

        // === kmbox_net 网络输入 ===
        kmbox_net_ip = "192.168.2.188";
        kmbox_net_port = "13384";
        kmbox_net_uuid = "7679E04E";

        // === kmbox_a 输入 ===
        kmbox_a_pidvid = "";

        // === MAKCU 输入 ===
        makcu_baudrate = 115200;
        makcu_port = "COM0";

        // === 鼠标自动射击 ===
        auto_shoot = false;
        bScope_multiplier = 1.2f;                        // 开镜倍率

        // === 开火拟人化 ===
        // 反应延迟和按键时长使用对数正态分布（正偏态/长尾），
        // 更接近真实人类反应时间分布（始终为正，右侧长尾）
        trigger_stable_frames = 3;                       // 连续确认帧数
        trigger_random_delay_ms = 45.0f;                 // 反应延迟中位数 (ms)
        trigger_delay_jitter_ms = 13.0f;                 // 延迟散布系数 (cv=jitter/delay, 越大越分散)
        trigger_hold_ms = 16.0f;                         // 按键时长中位数 (ms)
        trigger_hold_jitter_ms = 14.0f;                  // 按键时长散布系数
        trigger_shot_cooldown_ms = 54.0f;                // 两发最小间隔

        // === 自动急停 ===
        auto_stop_enabled = false;                       // 开火时释放 WASD
        auto_stop_hold_ms = 70.0f;                       // 急停最短保持时间

        // === 开火解锁 Y 轴 ===
        unlock_y_enabled = false;                        // 启用开火 Y 轴解锁
        unlock_y_threshold_ms = 200.0f;                  // 按住多久后解锁
        unlock_y_strength = 0.5f;                        // 解锁强度 (0=完全解锁 1=不锁)

        // === AI 射击修正 ===
        fire_correction_strength = 0.0f;                 // 射击修正强度 (0=关闭)

        // === AI 推理设置 ===
#ifdef USE_CUDA
        backend = "TRT";                                 // 使用 TensorRT 后端
#else
        backend = "DML";                                 // 使用 DirectML 后端
        dml_device_id = 0;                               // DirectML 设备 ID
#endif

#ifdef USE_CUDA
        ai_model = "sunxds_0.8.0.engine";                // TensorRT 引擎模型
#else
        ai_model = "sunxds_0.8.0.onnx";                  // ONNX 模型
#endif

        confidence_threshold = 0.15f;                    // 检测置信度阈值
        nms_threshold = 0.50f;                           // NMS IoU 阈值
        max_detections = 20;                             // 最大检测数
#ifdef USE_CUDA
        export_enable_fp8 = true;                        // 导出时启用 FP8
        export_enable_fp16 = true;                       // 导出时启用 FP16
#endif
        fixed_input_size = false;                        // 模型是否为固定输入尺寸

        // === CUDA 设置 ===
#ifdef USE_CUDA
        use_cuda_graph = false;                          // 是否使用 CUDA Graph
        use_pinned_memory = true;                        // 是否使用锁页内存
        gpuMemoryReserveMB = 2048;                       // GPU 预留显存（MB）
        enableGpuExclusiveMode = true;                   // 是否 GPU 独占模式
        capture_use_cuda = true;                         // 捕获是否使用 CUDA
#endif

        // === 系统资源设置 ===
        cpuCoreReserveCount = 4;                         // 预留 CPU 核心数
        systemMemoryReserveMB = 2048;                    // 系统预留内存（MB）

        // === 热键绑定 ===
        button_targeting = splitString("RightMouseButton"); // 瞄准键
        button_shoot = splitString("LeftMouseButton");      // 射击键
        button_zoom = splitString("RightMouseButton");      // 缩放键
        button_exit = splitString("F2");                    // 退出键
        button_pause = splitString("F3");                   // 暂停键
        button_reload_config = splitString("F4");           // 重载配置键
        button_open_overlay = splitString("Home");          // 打开覆盖层键
        enable_arrows_settings = false;                     // 是否启用方向键调整

        // === 程序覆盖层（UI） ===
        overlay_opacity = 225;                           // 覆盖层透明度
        overlay_ui_scale = 1.0f;                         // 覆盖层缩放比例
        overlay_exclude_from_capture = true;             // 排除覆盖层窗口
        overlay_x = 0;                                   // 覆盖层窗口 X 位置
        overlay_y = 0;                                   // 覆盖层窗口 Y 位置
        overlay_width = 860;                             // 覆盖层窗口宽度
        overlay_height = 526;                            // 覆盖层窗口高度

        // === 深度估计 ===
        depth_inference_enabled = true;                  // 启用深度推理
        depth_model_path = "depth_anything_v2.engine";   // 深度模型路径
        depth_fps = 100;                                 // 深度推理帧率
        depth_colormap = 18;                             // 深度伪彩色方案
        depth_mask_enabled = false;                      // 是否启用深度掩码
        depth_mask_fps = 5;                              // 掩码更新帧率
        depth_mask_near_percent = 20;                    // 近处百分比
        depth_mask_expand = 0;                           // 掩码扩展像素
        depth_mask_hold_frames = 0;                      // 掩码保持帧数
        depth_mask_alpha = 90;                           // 掩码透明度
        depth_mask_invert = false;                       // 反转掩码
        depth_debug_overlay_enabled = false;             // 深度调试覆盖层

        // === 游戏覆盖层（叠加在游戏画面上） ===
        game_overlay_enabled = false;                    // 启用游戏覆盖层
        game_overlay_max_fps = 0;                        // 覆盖层最大帧率（0=不限）
        game_overlay_draw_boxes = true;                  // 绘制检测框
        game_overlay_compensate_latency = true;          // 延迟补偿
        game_overlay_draw_wind_tail = true;              // 绘制轨迹模拟轨迹
        game_overlay_draw_frame = true;                  // 绘制边框
        game_overlay_draw_circle_fov = true;             // 绘制圆形 FOV
        game_overlay_show_target_correction = true;      // 显示目标修正
        game_overlay_box_a = 255;                        // 检测框透明度
        game_overlay_box_r = 0;                          // 检测框红色分量
        game_overlay_box_g = 255;                        // 检测框绿色分量
        game_overlay_box_b = 0;                          // 检测框蓝色分量
        game_overlay_frame_a = 180;                      // 边框透明度
        game_overlay_frame_r = 255;                      // 边框红色分量
        game_overlay_frame_g = 255;                      // 边框绿色分量
        game_overlay_frame_b = 255;                      // 边框蓝色分量
        game_overlay_box_thickness = 2.0f;               // 检测框线条粗细
        game_overlay_frame_thickness = 1.5f;             // 边框线条粗细

        // 覆盖层图标
        game_overlay_icon_enabled = false;
        game_overlay_icon_path = "icon.png";
        game_overlay_icon_width = 64;
        game_overlay_icon_height = 64;
        game_overlay_icon_offset_x = 0.0f;
        game_overlay_icon_offset_y = 0.0f;
        game_overlay_icon_anchor = "center";
        game_overlay_icon_class = -1;

        // === 数据采集 ===
        collect_data_while_playing = false;              // 游戏时采集数据
        collect_only_when_aimbot_running = false;        // 仅自瞄运行时采集
        collect_only_when_targets_present = true;        // 仅目标存在时采集
        collect_save_every_n_frames = 15;                // 每 N 帧保存一次
        collect_jpeg_quality = 95;                       // JPEG 压缩质量
        collect_output_dir.clear();                      // 输出目录
        auto_label_data = true;                          // 自动标注数据
        auto_label_min_conf = 0.30f;                     // 自动标注最小置信度
        auto_label_max_boxes = 20;                       // 自动标注最大框数
        auto_label_record_classes.clear();               // 自动标注记录类别

        // === 类 ID 映射 ===
        class_player = 0;                                // 玩家类 ID
        class_head = 1;                                  // 头部类 ID

        // === 调试 ===
        show_window = true;                              // 显示程序窗口
        show_fps = false;                                // 显示帧率
        screenshot_button = splitString("None");         // 截屏快捷键
        screenshot_delay = 500;                          // 截屏延迟（毫秒）
        verbose = false;                                 // 详细日志输出
        pipeline_tracer_enabled = false;                 // 流水线追踪开关
        pipeline_tracer_max_frames = 1000;               // 三次九点方位复测需要保留超过 300 帧

        // === 游戏配置文件 ===
        game_profiles.clear();
        GameProfile uni;
        uni.name = "UNIFIED";                            // 默认统一配置文件
        uni.sens = 1.0;                                   // 鼠标灵敏度
        uni.yaw = 0.022;                                  // 水平 yaw 值
        uni.pitch = uni.yaw;                              // 垂直 pitch 值
        uni.fovScaled = false;                            // 是否按 FOV 缩放
        uni.baseFOV = 0.0;                                // 基准 FOV
        game_profiles[uni.name] = uni;
        GameProfile cs;
        cs.name = "CS";
        cs.sens = 1.4;
        cs.yaw = 0.022;
        cs.pitch = 0.022;
        cs.fovScaled = false;
        cs.baseFOV = 0.0;
        game_profiles[cs.name] = cs;
        active_game = cs.name;

        saveConfig(target);                              // 保存默认配置到文件
        return true;
    }

    // === 使用 SimpleIni 库解析已存在的 INI 文件 ===
    CSimpleIniA ini;
    ini.SetUnicode();
    SI_Error rc = ini.LoadFile(target.c_str());
    if (rc < 0)
    {
        std::cerr << "[Config] Error parsing INI file: " << target << std::endl;
        return false;
    }

    // 辅助 lambda：从 INI 读取字符串值
    auto get_string = [&](const char* key, const std::string& defval)
    {
        const char* val = ini.GetValue("", key, defval.c_str());
        return val ? std::string(val) : defval;
    };

    // 辅助 lambda：从 INI 读取布尔值
    auto get_bool = [&](const char* key, bool defval)
        {
            return ini.GetBoolValue("", key, defval);
        };

    // 辅助 lambda：从 INI 读取整数值
    auto get_long = [&](const char* key, long defval)
        {
            return (int)ini.GetLongValue("", key, defval);
        };

    // 辅助 lambda：从 INI 读取浮点值
    auto get_double = [&](const char* key, double defval)
        {
            return ini.GetDoubleValue("", key, defval);
        };

    // === 游戏配置项读入 ===
    game_profiles.clear();

    CSimpleIniA::TNamesDepend keys;
    ini.GetAllKeys("Games", keys);

    // 遍历 [Games] 段的所有键，解析每条游戏配置
    // 格式: GameName = sens, yaw, pitch(可选), fovScaled(可选), baseFOV(可选)
    for (const auto& k : keys)
    {
        std::string name = k.pItem;
        std::string val = ini.GetValue("Games", k.pItem, "");
        auto parts = splitString(val, ',');

        try
        {
            if (parts.size() < 2)
                throw std::runtime_error("not enough values");

            GameProfile gp;
            gp.name = name;
            gp.sens = std::stod(parts[0]);
            gp.yaw = std::stod(parts[1]);
            gp.pitch = parts.size() > 2 ? std::stod(parts[2]) : gp.yaw;
            gp.fovScaled = parts.size() > 3 && (parts[3] == "true" || parts[3] == "1");
            gp.baseFOV = parts.size() > 4 ? std::stod(parts[4]) : 0.0;

            game_profiles[name] = gp;
        }
        catch (const std::exception& e)
        {
            std::cerr << "[Config] Failed to parse profile: " << name
                << " = " << val << " (" << e.what() << ")" << std::endl;
        }
    }

    // 确保 UNIFIED 配置文件始终存在
    if (!game_profiles.count("UNIFIED"))
    {
        GameProfile uni;
        uni.name = "UNIFIED";
        uni.sens = 1.0;
        uni.yaw = 0.022;
        uni.pitch = uni.yaw;
        uni.fovScaled = false;
        uni.baseFOV = 0.0;
        game_profiles[uni.name] = uni;
    }

    // 项目默认使用 CS 实测灵敏度；旧配置缺少该项时补齐，但不覆盖用户已保存的同名配置。
    if (!game_profiles.count("CS"))
    {
        GameProfile cs;
        cs.name = "CS";
        cs.sens = 1.4;
        cs.yaw = 0.022;
        cs.pitch = 0.022;
        cs.fovScaled = false;
        cs.baseFOV = 0.0;
        game_profiles[cs.name] = cs;
    }

    // 读取当前激活的游戏配置，如果不存在则选用第一个
    active_game = get_string("active_game", "CS");
    if (!game_profiles.count(active_game))
        active_game = game_profiles.count("CS") ? "CS" : "UNIFIED";

    // === 画面捕获配置 ===
    capture_method = get_string("capture_method", "duplication_api");
    capture_target = get_string("capture_target", "monitor");
    capture_window_title = get_string("capture_window_title", "");
    udp_ip = get_string("udp_ip", "192.168.3.10");
    udp_port = get_long("udp_port", 2333);
    if (udp_port < 1 || udp_port > 65535)
        udp_port = 2333;
    udp_source_width = get_long("udp_source_width", 2560);
    udp_source_height = get_long("udp_source_height", 1440);
    if (udp_source_width < 0 || udp_source_width > 16384)
        udp_source_width = 0;
    if (udp_source_height < 0 || udp_source_height > 16384)
        udp_source_height = 0;
    if ((udp_source_width == 0) != (udp_source_height == 0))
    {
        udp_source_width = 0;
        udp_source_height = 0;
    }
    ndi_source_name = get_string("ndi_source_name", "HPSAZZ (main)");
    ndi_source_width = get_long("ndi_source_width", 2560);
    ndi_source_height = get_long("ndi_source_height", 1440);
    if (ndi_source_width < 0 || ndi_source_width > 16384)
        ndi_source_width = 0;
    if (ndi_source_height < 0 || ndi_source_height > 16384)
        ndi_source_height = 0;
    // 宽高必须成对启用，避免只修正一个轴导致移动比例不一致。
    if ((ndi_source_width == 0) != (ndi_source_height == 0))
    {
        ndi_source_width = 0;
        ndi_source_height = 0;
    }
    detection_resolution = get_long("detection_resolution", 320);
    if (detection_resolution != 160 && detection_resolution != 320 && detection_resolution != 640)
        detection_resolution = 320;

    capture_fps = get_long("capture_fps", 60);
    monitor_idx = get_long("monitor_idx", 0);
    circle_fov_enabled = get_bool("circle_fov_enabled", true);
    circle_fov_radius_percent = get_long("circle_fov_radius_percent", 100);
    if (circle_fov_radius_percent < 1) circle_fov_radius_percent = 1;
    if (circle_fov_radius_percent > 100) circle_fov_radius_percent = 100;
    circle_fov_show_preview = get_bool("circle_fov_show_preview", true);
    capture_borders = get_bool("capture_borders", true);
    capture_cursor = get_bool("capture_cursor", true);
    virtual_camera_name = get_string("virtual_camera_name", "None");
    virtual_camera_width = get_long("virtual_camera_width", 1920);
    virtual_camera_height = get_long("virtual_camera_heigth", 1080);

    // === 瞄准目标配置 ===
    disable_headshot = get_bool("disable_headshot", false);
    body_y_offset = (float)get_double("body_y_offset", 0.15);
    head_y_offset = (float)get_double("head_y_offset", 0.05);
    auto_aim = get_bool("auto_aim", false);
    tracker_enabled = get_bool("tracker_enabled", true);
    tracker_overlay_table_enabled = get_bool("tracker_overlay_table_enabled", true);

    // === 跟踪器参数 ===
    auto_derive_tracker_params = get_bool("auto_derive_tracker_params", true);
    ml_confirm_threshold = get_long("ml_confirm_threshold", 2);
    ml_termination_frames = get_long("ml_termination_frames", 8);
    ml_noise_vx = (float)get_double("ml_noise_vx", 1.0);
    ml_noise_vy = (float)get_double("ml_noise_vy", 1.0);
    ml_noise_w = (float)get_double("ml_noise_w", 0.01);
    ml_noise_h = (float)get_double("ml_noise_h", 0.01);
    ml_measurement_stddev = (float)get_double("ml_measurement_stddev", 5.0);
    ml_coast_frames = get_long("ml_coast_frames", 15);
    ml_selection_strategy = get_string("ml_selection_strategy", "nearest");
    ml_recapture_iou = (float)get_double("ml_recapture_iou", 0.3);
    ml_recapture_distance_mult = (float)get_double("ml_recapture_distance_mult", 2.5);
    ml_coast_velocity_decay = (float)get_double("ml_coast_velocity_decay", 1.0);

    // === 执行控制器 ===
    pure_pursuit_gain = (float)get_double("pure_pursuit_gain", 0.85);
    pure_pursuit_dead_zone = (float)get_double("pure_pursuit_dead_zone", 2.0);
    pure_pursuit_smoothing = (float)get_double("pure_pursuit_smoothing", 0.8);
    motion_change_protection = get_bool("motion_change_protection", false);

    // === 鼠标控制配置 ===
    fovX = get_long("fovX", 106);
    fovY = get_long("fovY", 74);
    minSpeedMultiplier = (float)get_double("minSpeedMultiplier", 0.1);
    maxSpeedMultiplier = (float)get_double("maxSpeedMultiplier", 0.1);
    move_response_ms = (float)get_double("move_response_ms", 80.0);
    move_max_speed_cps = (float)get_double("move_max_speed_cps", 1440.0);
    move_integral_time_ms = (float)get_double("move_integral_time_ms", 0.0);
    aim_pipeline_mode = get_string("aim_pipeline_mode", "legacy");
    aim_shadow_command_to_frame_delay_ms = (float)get_double(
        "aim_shadow_command_to_frame_delay_ms", 60.0);
    aim_shadow_response_ms = (float)get_double("aim_shadow_response_ms", 80.0);
    aim_shadow_max_speed_cps = (float)get_double("aim_shadow_max_speed_cps", 1440.0);
    aim_shadow_feedforward_gain = (float)get_double("aim_shadow_feedforward_gain", 0.0);
    aim_shadow_settle_error_deg = (float)get_double("aim_shadow_settle_error_deg", 0.08);
    aim_shadow_settle_rate_dps = (float)get_double("aim_shadow_settle_rate_dps", 1.2);
    aim_shadow_reverse_confirm_ms = (float)get_double("aim_shadow_reverse_confirm_ms", 80.0);
    aim_shadow_integral_time_ms = (float)get_double("aim_shadow_integral_time_ms", 0.0);
    aim_shadow_integral_zone_deg = (float)get_double("aim_shadow_integral_zone_deg", 1.0);
    aim_shadow_lead_horizon_ms = (float)get_double("aim_shadow_lead_horizon_ms", 0.0);
    aim_shadow_lead_strength = (float)get_double("aim_shadow_lead_strength", 0.0);
    trajectory_shaper_mode = get_string("trajectory_shaper_mode", "off");
    trajectory_output_hz = (float)get_double("trajectory_output_hz", 240.0);
    trajectory_max_velocity_cps = (float)get_double("trajectory_max_velocity_cps", 1440.0);
    trajectory_max_acceleration_cps2 = (float)get_double(
        "trajectory_max_acceleration_cps2", 60000.0);
    trajectory_max_jerk_cps3 = (float)get_double("trajectory_max_jerk_cps3", 4000000.0);

    prediction_enabled = get_bool("prediction_enabled", true);
    const bool hasPredictionLeadMs = ini.GetValue("", "prediction_lead_ms", nullptr) != nullptr;
    prediction_lead_ms = hasPredictionLeadMs
        ? (float)get_double("prediction_lead_ms", 50.0)
        : (float)(get_double("predictionInterval", 0.020) * 1000.0);
    const bool hasPredictionVelocityTauMs =
        ini.GetValue("", "prediction_velocity_tau_ms", nullptr) != nullptr;
    prediction_velocity_tau_ms = hasPredictionVelocityTauMs
        ? (float)get_double("prediction_velocity_tau_ms", 50.0)
        : (float)(get_double("prediction_tau", 0.035) * 1000.0);
    prediction_strength = (float)get_double("prediction_strength", 1.0);
    profile_calibration_enabled = get_bool("profile_calibration_enabled", false);

    snapRadius = (float)get_double("snapRadius", 1.5);
    nearRadius = (float)get_double("nearRadius", 25.0);
    speedCurveExponent = (float)get_double("speedCurveExponent", 3.0);
    snapBoostFactor = (float)get_double("snapBoostFactor", 1.15);

    easynorecoil = get_bool("easynorecoil", false);
    easynorecoilstrength = (float)get_double("easynorecoilstrength", 0.0);
    input_method = get_string("input_method", "WIN32");

    // === 贝塞尔轨迹 + 输出平滑 ===
    bezier_enabled = get_bool("bezier_enabled", false);
    bezier_strength = (float)get_double("bezier_strength", 0.35);
    move_ema_enabled = get_bool("move_ema_enabled", false);
    move_ema_alpha = (float)get_double("move_ema_alpha", 0.60);

    // === 轨迹模拟配置 ===
    wind_mouse_enabled = get_bool("wind_mouse_enabled", false);
    wind_G = (float)get_double("wind_G", 18.0f);       // 鼠标移速系数
    wind_W = (float)get_double("wind_W", 15.0f);       // 轨迹摆动幅度
    wind_M = (float)get_double("wind_M", 10.0f);       // 单步移动上限
    wind_D = (float)get_double("wind_D", 8.0f);        // 微调距离阈值

    // === kmbox_net 输入 ===
    kmbox_net_ip = get_string("kmbox_net_ip", "192.168.2.188");
    kmbox_net_port = get_string("kmbox_net_port", "13384");
    kmbox_net_uuid = get_string("kmbox_net_uuid", "7679E04E");

    // === kmbox_a 输入 ===
    kmbox_a_pidvid = get_string("kmbox_a_pidvid", "");

    // === MAKCU 输入 ===
    makcu_baudrate = get_long("makcu_baudrate", 115200);
    makcu_port = get_string("makcu_port", "COM0");

    // === 鼠标自动射击 ===
    auto_shoot = get_bool("auto_shoot", false);
    bScope_multiplier = (float)get_double("bScope_multiplier", 1.2);

    // === 开火拟人化 ===
    trigger_stable_frames = get_long("trigger_stable_frames", 3);
    trigger_random_delay_ms = (float)get_double("trigger_random_delay_ms", 45.0);
    trigger_delay_jitter_ms = (float)get_double("trigger_delay_jitter_ms", 13.0);
    trigger_hold_ms = (float)get_double("trigger_hold_ms", 16.0);
    trigger_hold_jitter_ms = (float)get_double("trigger_hold_jitter_ms", 14.0);
    trigger_shot_cooldown_ms = (float)get_double("trigger_shot_cooldown_ms", 54.0);

    // === 自动急停 ===
    auto_stop_enabled = get_bool("auto_stop_enabled", false);
    auto_stop_hold_ms = (float)get_double("auto_stop_hold_ms", 70.0);

    // === 开火解锁 Y 轴 ===
    unlock_y_enabled = get_bool("unlock_y_enabled", false);
    unlock_y_threshold_ms = (float)get_double("unlock_y_threshold_ms", 200.0);
    unlock_y_strength = (float)get_double("unlock_y_strength", 0.5);

    // === AI 射击修正 ===
    fire_correction_strength = (float)get_double("fire_correction_strength", 0.0);

    // === AI 推理配置 ===
#ifdef USE_CUDA
    backend = "TRT";
#else
    backend = "DML";
    dml_device_id = get_long("dml_device_id", 0);
#endif

#ifdef USE_CUDA
    ai_model = get_string("ai_model", "sunxds_0.8.0.engine");
#else
    ai_model = get_string("ai_model", "sunxds_0.8.0.onnx");
#endif
    confidence_threshold = (float)get_double("confidence_threshold", 0.15);
    nms_threshold = (float)get_double("nms_threshold", 0.50);
    max_detections = get_long("max_detections", 20);
#ifdef USE_CUDA
    export_enable_fp8 = get_bool("export_enable_fp8", true);
    export_enable_fp16 = get_bool("export_enable_fp16", true);
#endif

    // === CUDA 配置 ===
#ifdef USE_CUDA
    use_cuda_graph = get_bool("use_cuda_graph", false);
    use_pinned_memory = get_bool("use_pinned_memory", true);
    gpuMemoryReserveMB = get_long("gpuMemoryReserveMB", 2048);
    enableGpuExclusiveMode = get_bool("enableGpuExclusiveMode", true);
    capture_use_cuda = get_bool("capture_use_cuda", true);
#endif

    // === 系统资源配置 ===
    cpuCoreReserveCount = get_long("cpuCoreReserveCount", 4);
    systemMemoryReserveMB = get_long("systemMemoryReserveMB", 2048);

    // === 热键绑定配置 ===
    button_targeting = splitString(get_string("button_targeting", "RightMouseButton"));
    button_shoot = splitString(get_string("button_shoot", "LeftMouseButton"));
    button_zoom = splitString(get_string("button_zoom", "RightMouseButton"));
    button_exit = splitString(get_string("button_exit", "F2"));
    button_pause = splitString(get_string("button_pause", "F3"));
    button_reload_config = splitString(get_string("button_reload_config", "F4"));
    button_open_overlay = splitString(get_string("button_open_overlay", "Home"));
    enable_arrows_settings = get_bool("enable_arrows_settings", false);

    // === 覆盖层配置 ===
    overlay_opacity = get_long("overlay_opacity", 225);
    overlay_ui_scale = (float)get_double("overlay_ui_scale", 1.0);
    overlay_exclude_from_capture = get_bool("overlay_exclude_from_capture", true);
    overlay_x = get_long("overlay_x", 0);
    overlay_y = get_long("overlay_y", 0);
    overlay_width = get_long("overlay_width", 860);
    overlay_height = get_long("overlay_height", 526);

    // === 深度估计配置 ===
    depth_inference_enabled = get_bool("depth_inference_enabled", true);
    depth_model_path = get_string("depth_model_path", "depth_anything_v2.engine");
    depth_fps = get_long("depth_fps", 100);
    if (depth_fps < 0) depth_fps = 0;
    depth_colormap = get_long("depth_colormap", 18);
    if (depth_colormap < 0 || depth_colormap > 21) depth_colormap = 18;
    depth_mask_enabled = get_bool("depth_mask_enabled", false);
    depth_mask_fps = get_long("depth_mask_fps", 5);
    if (depth_mask_fps < 0) depth_mask_fps = 0;
    depth_mask_near_percent = get_long("depth_mask_near_percent", 20);
    if (depth_mask_near_percent < 1) depth_mask_near_percent = 1;
    if (depth_mask_near_percent > 100) depth_mask_near_percent = 100;
    depth_mask_expand = get_long("depth_mask_expand", 0);
    if (depth_mask_expand < 0) depth_mask_expand = 0;
    if (depth_mask_expand > 128) depth_mask_expand = 128;
    depth_mask_hold_frames = get_long("depth_mask_hold_frames", 0);
    if (depth_mask_hold_frames < 0) depth_mask_hold_frames = 0;
    if (depth_mask_hold_frames > 120) depth_mask_hold_frames = 120;
    depth_mask_alpha = get_long("depth_mask_alpha", 90);
    if (depth_mask_alpha < 0) depth_mask_alpha = 0;
    if (depth_mask_alpha > 255) depth_mask_alpha = 255;
    depth_mask_invert = get_bool("depth_mask_invert", false);
    depth_debug_overlay_enabled = get_bool("depth_debug_overlay_enabled", false);

    // === 游戏覆盖层配置 ===
    game_overlay_enabled = get_bool("game_overlay_enabled", false);
    game_overlay_max_fps = get_long("game_overlay_max_fps", 0);
    game_overlay_draw_boxes = get_bool("game_overlay_draw_boxes", true);
    game_overlay_compensate_latency = get_bool("game_overlay_compensate_latency", true);
    game_overlay_draw_wind_tail = get_bool("game_overlay_draw_wind_tail", true);
    game_overlay_draw_frame = get_bool("game_overlay_draw_frame", true);
    game_overlay_draw_circle_fov = get_bool("game_overlay_draw_circle_fov", true);
    game_overlay_show_target_correction = get_bool("game_overlay_show_target_correction", true);
    game_overlay_box_a = get_long("game_overlay_box_a", 255);
    game_overlay_box_r = get_long("game_overlay_box_r", 0);
    game_overlay_box_g = get_long("game_overlay_box_g", 255);
    game_overlay_box_b = get_long("game_overlay_box_b", 0);
    game_overlay_frame_a = get_long("game_overlay_frame_a", 180);
    game_overlay_frame_r = get_long("game_overlay_frame_r", 255);
    game_overlay_frame_g = get_long("game_overlay_frame_g", 255);
    game_overlay_frame_b = get_long("game_overlay_frame_b", 255);
    game_overlay_box_thickness = (float)get_double("game_overlay_box_thickness", 2.0);
    game_overlay_frame_thickness = (float)get_double("game_overlay_frame_thickness", 1.5);
    clampGameOverlayColor();

    game_overlay_icon_enabled = get_bool("game_overlay_icon_enabled", false);
    game_overlay_icon_path = get_string("game_overlay_icon_path", "icon.png");
    game_overlay_icon_width = get_long("game_overlay_icon_width", 64);
    game_overlay_icon_height = get_long("game_overlay_icon_height", 64);
    game_overlay_icon_offset_x = (float)get_double("game_overlay_icon_offset_x", 0.0f);
    game_overlay_icon_offset_y = (float)get_double("game_overlay_icon_offset_y", 0.0f);
    game_overlay_icon_anchor = get_string("game_overlay_icon_anchor", "center");
    game_overlay_icon_class = get_long("game_overlay_icon_class", -1);

    // === 数据采集配置 ===
    collect_data_while_playing = get_bool("collect_data_while_playing", false);
    collect_only_when_aimbot_running = get_bool("collect_only_when_aimbot_running", false);
    collect_only_when_targets_present = get_bool("collect_only_when_targets_present", true);
    collect_save_every_n_frames = get_long("collect_save_every_n_frames", 15);
    collect_output_dir = get_string("collect_output_dir", "");
    collect_jpeg_quality = get_long("collect_jpeg_quality", 95);
    auto_label_data = get_bool("auto_label_data", true);
    auto_label_min_conf = (float)get_double("auto_label_min_conf", 0.30);
    auto_label_max_boxes = get_long("auto_label_max_boxes", 20);
    auto_label_record_classes = get_string("auto_label_record_classes", "");

    // === 基础移动参数范围校验 ===
    move_response_ms = std::clamp(move_response_ms, 20.0f, 300.0f);
    move_max_speed_cps = std::clamp(move_max_speed_cps, 30.0f, 4000.0f);
    move_integral_time_ms = std::clamp(move_integral_time_ms, 0.0f, 1000.0f);
    if (move_integral_time_ms > 0.0f && move_integral_time_ms < 50.0f)
        move_integral_time_ms = 50.0f;

    // === 连续观测预测参数范围校验 ===
    prediction_lead_ms = std::clamp(prediction_lead_ms, 0.0f, 100.0f);
    prediction_velocity_tau_ms = std::clamp(prediction_velocity_tau_ms, 40.0f, 120.0f);
    prediction_strength = std::clamp(prediction_strength, 0.0f, 4.0f);
    aim_shadow_command_to_frame_delay_ms = std::clamp(
        aim_shadow_command_to_frame_delay_ms, 0.0f, 250.0f);
    aim_shadow_response_ms = std::clamp(aim_shadow_response_ms, 10.0f, 500.0f);
    aim_shadow_max_speed_cps = std::clamp(aim_shadow_max_speed_cps, 30.0f, 4000.0f);
    aim_shadow_feedforward_gain = std::clamp(aim_shadow_feedforward_gain, 0.0f, 2.0f);
    aim_shadow_settle_error_deg = std::clamp(aim_shadow_settle_error_deg, 0.0f, 1.0f);
    aim_shadow_settle_rate_dps = std::clamp(aim_shadow_settle_rate_dps, 0.0f, 20.0f);
    aim_shadow_reverse_confirm_ms = std::clamp(aim_shadow_reverse_confirm_ms, 0.0f, 250.0f);
    aim_shadow_integral_time_ms = std::clamp(aim_shadow_integral_time_ms, 0.0f, 2000.0f);
    if (aim_shadow_integral_time_ms > 0.0f && aim_shadow_integral_time_ms < 50.0f)
        aim_shadow_integral_time_ms = 50.0f;
    aim_shadow_integral_zone_deg = std::clamp(aim_shadow_integral_zone_deg, 0.0f, 10.0f);
    aim_shadow_lead_horizon_ms = std::clamp(aim_shadow_lead_horizon_ms, 0.0f, 250.0f);
    aim_shadow_lead_strength = std::clamp(aim_shadow_lead_strength, 0.0f, 4.0f);
    if (trajectory_shaper_mode != "trapezoid")
        trajectory_shaper_mode = "off";
    trajectory_output_hz = std::clamp(trajectory_output_hz, 30.0f, 1000.0f);
    trajectory_max_velocity_cps = std::clamp(
        trajectory_max_velocity_cps, 30.0f, 4000.0f);
    trajectory_max_acceleration_cps2 = std::clamp(
        trajectory_max_acceleration_cps2, 1000.0f, 1000000.0f);
    trajectory_max_jerk_cps3 = std::clamp(
        trajectory_max_jerk_cps3, 10000.0f, 100000000.0f);

    // === 覆盖层尺寸范围校验 ===
    if (overlay_width < 560) overlay_width = 560;
    if (overlay_width > 3840) overlay_width = 3840;
    if (overlay_height < 340) overlay_height = 340;
    if (overlay_height > 2160) overlay_height = 2160;

    // === 数据采集参数范围校验 ===
    if (collect_save_every_n_frames < 1) collect_save_every_n_frames = 1;
    if (collect_save_every_n_frames > 600) collect_save_every_n_frames = 600;
    if (collect_jpeg_quality < 50) collect_jpeg_quality = 50;
    if (collect_jpeg_quality > 100) collect_jpeg_quality = 100;
    if (auto_label_min_conf < 0.01f) auto_label_min_conf = 0.01f;
    if (auto_label_min_conf > 0.99f) auto_label_min_conf = 0.99f;
    if (auto_label_max_boxes < 1) auto_label_max_boxes = 1;
    if (auto_label_max_boxes > 200) auto_label_max_boxes = 200;

    // === 类别 ID 映射 ===
    class_player = get_long("class_player", 0);
    class_head = get_long("class_head", 1);

    // === 调试窗口配置 ===
    show_window = get_bool("show_window", true);
    show_fps = get_bool("show_fps", false);
    screenshot_button = splitString(get_string("screenshot_button", "None"));
    screenshot_delay = get_long("screenshot_delay", 500);
    verbose = get_bool("verbose", false);
    pipeline_tracer_enabled = get_bool("pipeline_tracer_enabled", false);
    pipeline_tracer_max_frames = get_long("pipeline_tracer_max_frames", 1000);
    pipeline_tracer_max_frames = std::clamp(pipeline_tracer_max_frames, 100, 10000);

    return true;
}

/**
 * saveConfig - 将当前配置写入 INI 文件
 * @param filename 输出文件名，为空则默认使用 "config.ini"
 * @return 写入成功返回 true，否则返回 false
 *
 * 将 Config 结构体中的所有字段按分组写入 INI 格式文件，
 * 包括游戏配置表 [Games] 段。
 */
bool Config::saveConfig(const std::string& filename)
{
    std::string target = filename.empty() ? "config.ini" : filename;
    if (target == "config.ini" && !config_path.empty())
    {
        target = config_path;
    }

    std::ofstream file(target);
    if (!file.is_open())
    {
        std::cerr << "Error opening config for writing: " << target << std::endl;
        return false;
    }

    // 保存时输出的是 UTF-8 编码（MSVC /utf-8 标志使字符串常量为 UTF-8），
    // Windows 记事本 / VS 等编辑器能自动识别 UTF-8 并正常显示中文。
    // 如果终端使用 CP_UTF8（65001）或 Windows Terminal，控制台 cat/type 也能正常显示。

    // 文件头注释
    file << "# An explanation of the options can be found at:\n";
    file << "# https://github.com/hpsazz1/Xen/blob/main/docs/config.md\n\n";

    // ========== 画面捕获设置 ==========
    file << "# Capture\n"
        << "capture_method = " << capture_method << "\n"
        << "capture_target = " << capture_target << "\n"
        << "capture_window_title = " << capture_window_title << "\n"
        << "udp_ip = " << udp_ip << "\n"
        << "udp_port = " << udp_port << "\n"
        << "udp_source_width = " << udp_source_width << "\n"
        << "udp_source_height = " << udp_source_height << "\n"
        << "ndi_source_name = " << ndi_source_name << "\n"
        << "ndi_source_width = " << ndi_source_width << "\n"
        << "ndi_source_height = " << ndi_source_height << "\n"
        << "detection_resolution = " << detection_resolution << "\n"
        << "capture_fps = " << capture_fps << "\n"
        << "monitor_idx = " << monitor_idx << "\n"
        << "circle_fov_enabled = " << (circle_fov_enabled ? "true" : "false") << "\n"
        << "circle_fov_radius_percent = " << circle_fov_radius_percent << "\n"
        << "circle_fov_show_preview = " << (circle_fov_show_preview ? "true" : "false") << "\n"
        << "capture_borders = " << (capture_borders ? "true" : "false") << "\n"
        << "capture_cursor = " << (capture_cursor ? "true" : "false") << "\n"
        << "virtual_camera_name = " << virtual_camera_name << "\n"
        << "virtual_camera_width = " << virtual_camera_width << "\n"
        << "virtual_camera_heigth = " << virtual_camera_height << "\n\n";

    // === 瞄准 / 目标设置 ===
    file << "# Target\n"
        << "disable_headshot = " << (disable_headshot ? "true" : "false") << "\n"
        << std::fixed << std::setprecision(2)
        << "body_y_offset = " << body_y_offset << "\n"
        << "head_y_offset = " << head_y_offset << "\n"
        << "auto_aim = " << (auto_aim ? "true" : "false") << "\n"
        << "tracker_enabled = " << (tracker_enabled ? "true" : "false") << "\n"
        << "tracker_overlay_table_enabled = " << (tracker_overlay_table_enabled ? "true" : "false") << "\n"
        << "auto_derive_tracker_params = " << (auto_derive_tracker_params ? "true" : "false") << "\n"
        << "ml_confirm_threshold = " << ml_confirm_threshold << "\n"
        << "ml_termination_frames = " << ml_termination_frames << "\n"
        << "ml_noise_vx = " << ml_noise_vx << "\n"
        << "ml_noise_vy = " << ml_noise_vy << "\n"
        << "ml_noise_w = " << ml_noise_w << "\n"
        << "ml_noise_h = " << ml_noise_h << "\n"
        << "ml_measurement_stddev = " << ml_measurement_stddev << "\n"
        << "ml_coast_frames = " << ml_coast_frames << "\n"
        << "ml_selection_strategy = " << ml_selection_strategy << "\n"
        << "ml_recapture_iou = " << ml_recapture_iou << "\n"
        << "ml_recapture_distance_mult = " << ml_recapture_distance_mult << "\n"
        << "ml_coast_velocity_decay = " << ml_coast_velocity_decay << "\n\n";

    // === 鼠标移动设置 ===
    file << "# Mouse move\n"
        << "# WIN32, GHUB, RAZER, KMBOX_NET, KMBOX_A, MAKCU\n"
        << "fovX = " << fovX << "\n"
        << "fovY = " << fovY << "\n"
        << "move_response_ms = " << move_response_ms << "\n"
        << "move_max_speed_cps = " << move_max_speed_cps << "\n"
        << "move_integral_time_ms = " << move_integral_time_ms << "\n"
        << "aim_pipeline_mode = " << aim_pipeline_mode << "\n"
        << "aim_shadow_command_to_frame_delay_ms = " << aim_shadow_command_to_frame_delay_ms << "\n"
        << "aim_shadow_response_ms = " << aim_shadow_response_ms << "\n"
        << "aim_shadow_max_speed_cps = " << aim_shadow_max_speed_cps << "\n"
        << "aim_shadow_feedforward_gain = " << aim_shadow_feedforward_gain << "\n"
        << "aim_shadow_settle_error_deg = " << aim_shadow_settle_error_deg << "\n"
        << "aim_shadow_settle_rate_dps = " << aim_shadow_settle_rate_dps << "\n"
        << "aim_shadow_reverse_confirm_ms = " << aim_shadow_reverse_confirm_ms << "\n"
        << "aim_shadow_integral_time_ms = " << aim_shadow_integral_time_ms << "\n"
        << "aim_shadow_integral_zone_deg = " << aim_shadow_integral_zone_deg << "\n"
        << "aim_shadow_lead_horizon_ms = " << aim_shadow_lead_horizon_ms << "\n"
        << "aim_shadow_lead_strength = " << aim_shadow_lead_strength << "\n"
        << "trajectory_shaper_mode = " << trajectory_shaper_mode << "\n"
        << "trajectory_output_hz = " << trajectory_output_hz << "\n"
        << "trajectory_max_velocity_cps = " << trajectory_max_velocity_cps << "\n"
        << "trajectory_max_acceleration_cps2 = " << trajectory_max_acceleration_cps2 << "\n"
        << "trajectory_max_jerk_cps3 = " << trajectory_max_jerk_cps3 << "\n"
        << "prediction_enabled = " << (prediction_enabled ? "true" : "false") << "\n"
        << "prediction_lead_ms = " << prediction_lead_ms << "\n"
        << "prediction_velocity_tau_ms = " << prediction_velocity_tau_ms << "\n"
        << "prediction_strength = " << prediction_strength << "\n"
        << "profile_calibration_enabled = " << (profile_calibration_enabled ? "true" : "false") << "\n"
        << "easynorecoil = " << (easynorecoil ? "true" : "false") << "\n"
        << std::fixed << std::setprecision(1)
        << "easynorecoilstrength = " << easynorecoilstrength << "\n"
        << "input_method = " << input_method << "\n\n";

    // === kmbox_net 网络输入 ===
    file << "# Kmbox_net\n"
        << "kmbox_net_ip = " << kmbox_net_ip << "\n"
        << "kmbox_net_port = " << kmbox_net_port << "\n"
        << "kmbox_net_uuid = " << kmbox_net_uuid << "\n\n";

    // === kmbox_a 输入 ===
    file << "# Kmbox_a\n"
        << "kmbox_a_pidvid = " << kmbox_a_pidvid << "\n\n";

    // === MAKCU 输入 ===
    file << "# Makcu\n"
        << "makcu_baudrate = " << makcu_baudrate << "\n"
        << "makcu_port = " << makcu_port << "\n\n";

    // === 鼠标自动射击 ===
    file << "# Mouse shooting\n"
        << "auto_shoot = " << (auto_shoot ? "true" : "false") << "\n"
        << std::fixed << std::setprecision(1)
        << "bScope_multiplier = " << bScope_multiplier << "\n";

    // === 开火拟人化 ===
    file << "# Firing humanization\n"
        << "trigger_stable_frames = " << trigger_stable_frames << "\n"
        << std::fixed << std::setprecision(1)
        << "trigger_random_delay_ms = " << trigger_random_delay_ms << "\n"
        << "trigger_delay_jitter_ms = " << trigger_delay_jitter_ms << "\n"
        << "trigger_hold_ms = " << trigger_hold_ms << "\n"
        << "trigger_hold_jitter_ms = " << trigger_hold_jitter_ms << "\n"
        << "trigger_shot_cooldown_ms = " << trigger_shot_cooldown_ms << "\n";

    // === 自动急停 ===
    file << "# Auto stop\n"
        << "auto_stop_enabled = " << (auto_stop_enabled ? "true" : "false") << "\n"
        << "auto_stop_hold_ms = " << auto_stop_hold_ms << "\n";

    // === 开火解锁 Y 轴 ===
    file << "# Unlock Y\n"
        << "unlock_y_enabled = " << (unlock_y_enabled ? "true" : "false") << "\n"
        << "unlock_y_threshold_ms = " << unlock_y_threshold_ms << "\n"
        << std::fixed << std::setprecision(1)
        << "unlock_y_strength = " << unlock_y_strength << "\n";

    // === AI 射击修正 ===
    file << "# Fire correction\n"
        << std::fixed << std::setprecision(2)
        << "fire_correction_strength = " << fire_correction_strength << "\n\n";

    // === AI 推理设置 ===
    file << "# AI\n"
        << "backend = " << backend << "\n";
#ifndef USE_CUDA
    file << "dml_device_id = " << dml_device_id << "\n";
#endif
    file << "ai_model = " << ai_model << "\n"
        << std::fixed << std::setprecision(2)
        << "confidence_threshold = " << confidence_threshold << "\n"
        << "nms_threshold = " << nms_threshold << "\n"
        << std::setprecision(0)
        << "max_detections = " << max_detections << "\n"
#ifdef USE_CUDA
        << "export_enable_fp8 = " << (export_enable_fp8 ? "true" : "false") << "\n"
        << "export_enable_fp16 = " << (export_enable_fp16 ? "true" : "false") << "\n"
#endif
        ;

    // === CUDA 设置 ===
#ifdef USE_CUDA
    file << "# CUDA\n"
        << "use_cuda_graph = " << (use_cuda_graph ? "true" : "false") << "\n"
        << "use_pinned_memory = " << (use_pinned_memory ? "true" : "false") << "\n"
        << "gpuMemoryReserveMB = " << gpuMemoryReserveMB << "\n"
        << "enableGpuExclusiveMode = " << (enableGpuExclusiveMode ? "true" : "false") << "\n"
        << "capture_use_cuda = " << (capture_use_cuda ? "true" : "false") << "\n\n";
#endif

    // === 系统资源设置 ===
    file << "# System\n"
        << "cpuCoreReserveCount = " << cpuCoreReserveCount << "\n"
        << "systemMemoryReserveMB = " << systemMemoryReserveMB << "\n\n";

    // === 热键绑定 ===
    file << "# Buttons\n"
        << "button_targeting = " << joinStrings(button_targeting) << "\n"
        << "button_shoot = " << joinStrings(button_shoot) << "\n"
        << "button_zoom = " << joinStrings(button_zoom) << "\n"
        << "button_exit = " << joinStrings(button_exit) << "\n"
        << "button_pause = " << joinStrings(button_pause) << "\n"
        << "button_reload_config = " << joinStrings(button_reload_config) << "\n"
        << "button_open_overlay = " << joinStrings(button_open_overlay) << "\n"
        << "enable_arrows_settings = " << (enable_arrows_settings ? "true" : "false") << "\n\n";

    // === 覆盖层 UI 设置 ===
    file << "# Overlay\n"
        << "overlay_opacity = " << overlay_opacity << "\n"
        << std::fixed << std::setprecision(2)
        << "overlay_ui_scale = " << overlay_ui_scale << "\n"
        << "overlay_exclude_from_capture = " << (overlay_exclude_from_capture ? "true" : "false") << "\n"
        << std::setprecision(0)
        << "overlay_x = " << overlay_x << "\n"
        << "overlay_y = " << overlay_y << "\n"
        << "overlay_width = " << overlay_width << "\n"
        << "overlay_height = " << overlay_height << "\n\n";

    // === 深度估计设置 ===
    file << "# Depth\n"
        << "depth_inference_enabled = " << (depth_inference_enabled ? "true" : "false") << "\n"
        << "depth_model_path = " << depth_model_path << "\n"
        << "depth_fps = " << depth_fps << "\n"
        << "depth_colormap = " << depth_colormap << "\n"
        << "depth_mask_enabled = " << (depth_mask_enabled ? "true" : "false") << "\n"
        << "depth_mask_fps = " << depth_mask_fps << "\n"
        << "depth_mask_near_percent = " << depth_mask_near_percent << "\n"
        << "depth_mask_expand = " << depth_mask_expand << "\n"
        << "depth_mask_hold_frames = " << depth_mask_hold_frames << "\n"
        << "depth_mask_alpha = " << depth_mask_alpha << "\n"
        << "depth_mask_invert = " << (depth_mask_invert ? "true" : "false") << "\n"
        << "depth_debug_overlay_enabled = " << (depth_debug_overlay_enabled ? "true" : "false") << "\n\n";

    // === 游戏覆盖层（叠加在游戏画面上） ===
    file << "# Game Overlay\n"
        << "game_overlay_enabled = " << (game_overlay_enabled ? "true" : "false") << "\n"
        << "game_overlay_max_fps = " << game_overlay_max_fps << "\n"
        << "game_overlay_draw_boxes = " << (game_overlay_draw_boxes ? "true" : "false") << "\n"
        << "game_overlay_compensate_latency = " << (game_overlay_compensate_latency ? "true" : "false") << "\n"
        << "game_overlay_draw_wind_tail = " << (game_overlay_draw_wind_tail ? "true" : "false") << "\n"
        << "game_overlay_draw_frame = " << (game_overlay_draw_frame ? "true" : "false") << "\n"
        << "game_overlay_draw_circle_fov = " << (game_overlay_draw_circle_fov ? "true" : "false") << "\n"
        << "game_overlay_show_target_correction = " << (game_overlay_show_target_correction ? "true" : "false") << "\n"
        << "game_overlay_box_a = " << game_overlay_box_a << "\n"
        << "game_overlay_box_r = " << game_overlay_box_r << "\n"
        << "game_overlay_box_g = " << game_overlay_box_g << "\n"
        << "game_overlay_box_b = " << game_overlay_box_b << "\n"
        << "game_overlay_frame_a = " << game_overlay_frame_a << "\n"
        << "game_overlay_frame_r = " << game_overlay_frame_r << "\n"
        << "game_overlay_frame_g = " << game_overlay_frame_g << "\n"
        << "game_overlay_frame_b = " << game_overlay_frame_b << "\n"
        << std::fixed << std::setprecision(2)
        << "game_overlay_box_thickness = " << game_overlay_box_thickness << "\n"
        << "game_overlay_frame_thickness = " << game_overlay_frame_thickness << "\n\n";

    file << "game_overlay_icon_enabled = " << (game_overlay_icon_enabled ? "true" : "false") << "\n"
        << "game_overlay_icon_path = " << game_overlay_icon_path << "\n"
        << "game_overlay_icon_width = " << game_overlay_icon_width << "\n"
        << "game_overlay_icon_height = " << game_overlay_icon_height << "\n"
        << std::fixed << std::setprecision(2)
        << "game_overlay_icon_offset_x = " << game_overlay_icon_offset_x << "\n"
        << std::fixed << std::setprecision(2)
        << "game_overlay_icon_offset_y = " << game_overlay_icon_offset_y << "\n"
        << "game_overlay_icon_anchor = " << game_overlay_icon_anchor << "\n"
        << "game_overlay_icon_class = " << game_overlay_icon_class << "\n\n";

    // === 数据采集 ===
    file << "# Data Collection\n"
        << "collect_data_while_playing = " << (collect_data_while_playing ? "true" : "false") << "\n"
        << "collect_only_when_aimbot_running = " << (collect_only_when_aimbot_running ? "true" : "false") << "\n"
        << "collect_only_when_targets_present = " << (collect_only_when_targets_present ? "true" : "false") << "\n"
        << "collect_save_every_n_frames = " << collect_save_every_n_frames << "\n"
        << "collect_jpeg_quality = " << collect_jpeg_quality << "\n"
        << "collect_output_dir = " << collect_output_dir << "\n"
        << "auto_label_data = " << (auto_label_data ? "true" : "false") << "\n"
        << std::fixed << std::setprecision(2)
        << "auto_label_min_conf = " << auto_label_min_conf << "\n"
        << std::setprecision(0)
        << "auto_label_max_boxes = " << auto_label_max_boxes << "\n"
        << "auto_label_record_classes = " << auto_label_record_classes << "\n\n";

    // === 自定义类别映射 ===
    file << "# Custom Classes\n"
        << "class_player = " << class_player << "\n"
        << "class_head = " << class_head << "\n\n";

    // === 调试设置 ===
    file << "# Debug\n"
        << "show_window = " << (show_window ? "true" : "false") << "\n"
        << "show_fps = " << (show_fps ? "true" : "false") << "\n"
        << "screenshot_button = " << joinStrings(screenshot_button) << "\n"
        << "screenshot_delay = " << screenshot_delay << "\n"
        << "verbose = " << (verbose ? "true" : "false") << "\n"
        << "pipeline_tracer_enabled = " << (pipeline_tracer_enabled ? "true" : "false") << "\n"
        << "pipeline_tracer_max_frames = " << pipeline_tracer_max_frames << "\n\n";

    // === 当前激活的游戏配置 ===
    file << "# Active game profile\n";
    file << "active_game = " << active_game << "\n\n";
    file << std::defaultfloat << std::setprecision(6);
    file << "[Games]\n";
    for (auto& kv : game_profiles)
    {
        auto & gp = kv.second;
        file << gp.name << " = "
             << gp.sens << "," << gp.yaw;
        file << "," << gp.pitch;
        if (gp.fovScaled)
            file << ",true," << gp.baseFOV;
        file << "\n";
    }

    file.close();
    return true;
}

/**
 * currentProfile - 获取当前激活的游戏配置
 * @return 当前激活的 GameProfile 引用
 * @throws std::runtime_error 如果找不到当前激活的游戏配置
 */
const Config::GameProfile& Config::currentProfile() const
{
    auto it = game_profiles.find(active_game);
    if (it != game_profiles.end()) return it->second;
    throw std::runtime_error("Active game profile not found: " + active_game);
}

/**
 * degToCounts - 将角度转换为鼠标计数（DPI counts）
 *
 * 根据当前游戏配置的灵敏度（sens）和 yaw/pitch 值，将瞄准偏差角度
 * 转换为鼠标需要移动的计数（counts）。
 * 如果游戏配置启用了 FOV 缩放，还会根据当前 FOV 进行比例调整。
 *
 * @param degX    水平方向的角度偏差（度）
 * @param degY    垂直方向的角度偏差（度）
 * @param fovNow  当前 FOV（用于 FOV 缩放计算）
 * @return 鼠标移动计数 (countsX, countsY)
 *         计算公式: counts = deg / (sens * yaw/pitch * scale)
 */
std::pair<double, double> Config::degToCounts(double degX, double degY, double fovNow) const
{
    const auto& gp = currentProfile();
    // 如果启用了 FOV 缩放且有基准 FOV，计算缩放比例
    double scale = (gp.fovScaled && gp.baseFOV > 1.0) ? (fovNow / gp.baseFOV) : 1.0;

    if (gp.sens == 0.0 || gp.yaw == 0.0 || gp.pitch == 0.0)
        return { 0.0, 0.0 };

    double cx = degX / (gp.sens * gp.yaw * scale);
    double cy = degY / (gp.sens * gp.pitch * scale);
    return { cx, cy };
}

void Config::applyAutoDerivedTrackerParams(int detectionResolution, int captureFps)
{
    if (!auto_derive_tracker_params)
        return;

    std::lock_guard<std::mutex> cfgLock(configMutex);  // 保护对 config 字段的写入

    const double scale = static_cast<double>(detectionResolution) / 640.0;
    const int clampedFps = std::clamp(captureFps, 15, 500);

    ml_confirm_threshold   = 2;
    ml_termination_frames  = std::max(5, clampedFps / 8);
    ml_noise_vx            = static_cast<float>(1.0 * scale);
    ml_noise_vy            = static_cast<float>(1.0 * scale);
    ml_noise_w             = 0.01f;
    ml_noise_h             = 0.01f;
    ml_measurement_stddev  = static_cast<float>(5.0 * scale);
    ml_coast_frames        = std::max(8, clampedFps / 4);
    pure_pursuit_dead_zone = static_cast<float>(std::max(1.0, detectionResolution / 320.0));
    pure_pursuit_smoothing = 0.8f;
    pure_pursuit_gain      = 0.85f;  // 统一自动推导，与 mouse.cpp 中 autoGain 保持一致
    ml_recapture_iou       = 0.3f;
    ml_recapture_distance_mult = 2.5f;
    ml_coast_velocity_decay    = 1.0f;
    // 移动响应时间、最大设备速率和运动变化保护均属于用户/实测参数，不能由分辨率或 FPS
    // 自动覆盖。控制器会把每秒速率按真实观测 dt 换算为单帧预算，无需在此重新推导。
}
