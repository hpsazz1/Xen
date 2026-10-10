#ifndef CONFIG_H
#define CONFIG_H

#include <string>
#include <vector>
#include "config/ui_theme.h"

#include "aim/aim.h"
#include "auto_stop/auto_stop.h"
#include "trigger/trigger.h"
#include "source_context/source_context.h"
#include "recoil/recoil_config.h"
#include "weapon/weapon.h"
#include "capture/capture.h"
#include "detector/detector.h"
#include "keyboard/keyboard.h"
#include "log/log.h"
#include "mouse/mouse.h"
#include "movement/movement.h"

struct RuntimeConfig {
    int profile_window = 256;
    // 自动逐帧报告、全程归档和压枪批次；显式采集、普通日志和崩溃诊断独立。
    bool diagnostics_enabled = false;
    // 仅由正式性能入口按轮次临时覆盖，不进入 INI 或 Overlay。正常应用默认
    // 关闭，避免新增时钟读取和两阶段诊断发布扰动生产热路径。
    bool enable_performance_probes = false;
};



// 六页紧凑布局在该尺寸下仍能保证安全按钮、表单和状态信息不互相遮挡。
inline constexpr int kMinimumUiWidth = 820;
inline constexpr int kMinimumUiHeight = 600;

struct UiConfig {
    int width = 900;
    int height = 640;
    bool enable_vsync = true;
    // 正式验收可在启动时自动打开不抢焦点的独立 TOPMOST 检测预览；普通用户默认关闭。
    bool open_detached_preview_on_start = false;
    UiTheme theme = UiTheme::LIGHT;
};

struct TeamFilterConfig {
    // 通用模型默认关闭；类别必须与实际模型的 CT/T 语义一致。
    bool enabled = false;
    std::vector<int> ct_class_ids{0, 1};
    std::vector<int> t_class_ids{2, 3};
};

struct AppConfig {
    // 产品默认配置集中在聚合层，独立模块仍保留适合算法与设备测试的通用安全默认值。
    DetectorConfig detector = [] {
        DetectorConfig value;
        value.model_path = "14wv11.onnx";
        value.backend = BackendType::CPU;
        value.openvino_device = OpenVinoDevice::CPU;
        value.enable_fp16 = false;
        return value;
    }();
    CaptureConfig capture = [] {
        CaptureConfig value;
        // 缺配置首次启动使用本机画面，不继承任何私人双机绑定。
        value.udp_url.clear();
        value.ndi_source_name.clear();
        return value;
    }();
    AimConfig aim = [] {
        AimConfig value;
        value.person_class_ids = {0, 2};
        value.head_class_ids = {1, 3};
        value.smoothing = 0.475f;
        value.counts_per_pixel_x = 0.425f;
        value.counts_per_pixel_y = 0.40f;
        value.body_aim_height_ratio = 0.16f;
        value.soft_zone_radius_percent = 30.0f;
        value.max_counts_per_frame = 14.0f;
        value.enable_delay_compensation = true;
        value.control_delay_ms = 15.0f;
        value.max_delay_compensation_ms = 44.0f;
        value.enable_prediction = false;
        return value;
    }();
    MouseConfig mouse;
    KeyboardConfig keyboard;
    movement::Config movement;
    // 日常入口采用HUD；显式false配置仍可选择H40对照。
    AutoStopConfig auto_stop{.experimental_hud_model = true};
    TriggerConfig trigger = [] {
        TriggerConfig value;
        value.fire_enabled = false;
        value.random_timing_enabled = true;
        value.require_stop = true;
        value.allow_estimated_stop = true;
        value.fire_delay_ms = 0;
        value.fire_mode = TriggerFireMode::SINGLE;
        value.head_width_percent = value.head_height_percent = 100.0f;
        value.body_width_percent = value.body_height_percent = 100.0f;
        return value;
    }();
    source_context::SourceContextConfig source_context;
    RecoilConfig recoil = [] {
        RecoilConfig value;
        value.sensitivity = 1.4;
        value.mixed_aim = true;
        return value;
    }();
    weapon::GsiConfig gsi;
    TeamFilterConfig team_filter;
    // 独立于弹道启用；启动时读取一次，运行中固定版本。
    std::string weapon_timing_file = "cache/recoil/weapon-timing.json";
    LogConfig log = [] {
        LogConfig value;
        value.global_level = LogLevel::INFO;
        return value;
    }();
    RuntimeConfig runtime;
    UiConfig ui;
};

bool validate_app_config(const AppConfig& config,
                         std::string& error) noexcept;
// Launcher 只需严格确定 Worker；其他配置错误交给 Worker 配置页修复。
// 此函数不验证或授权启动 Runtime，后者仍须使用完整配置校验。
bool load_app_backend(const std::string& path, BackendType& backend,
                      std::string& error) noexcept;
bool load_app_config(const std::string& path,
                     AppConfig& config,
                     std::string& error) noexcept;
// 仅当配置文件确实不存在时，把当前代码默认值完整写出；已有但无效的文件绝不覆盖。
bool load_or_create_app_config(const std::string& path,
                               AppConfig& config,
                               bool& created,
                               std::string& error) noexcept;
bool save_app_config(const std::string& path,
                     const AppConfig& config,
                     std::string& error) noexcept;

#endif // CONFIG_H
