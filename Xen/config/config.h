#ifndef CONFIG_H
#define CONFIG_H

#include <string>

#include "aim/aim.h"
#include "capture/capture.h"
#include "detector/detector.h"
#include "keyboard/keyboard.h"
#include "log/log.h"
#include "mouse/mouse.h"

struct RuntimeConfig {
    int profile_window = 256;
    // 仅由正式性能入口按轮次临时覆盖，不进入 INI 或 Overlay。正常应用默认
    // 关闭，避免新增时钟读取和两阶段诊断发布扰动生产热路径。
    bool enable_performance_probes = false;
};

enum class UiTheme {
    LIGHT,
    DARK,
};

// 五页紧凑布局在该尺寸下仍能保证安全按钮、表单和状态信息不互相遮挡。
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

struct AppConfig {
    DetectorConfig detector;
    CaptureConfig capture;
    AimConfig aim;
    MouseConfig mouse;
    KeyboardConfig keyboard;
    LogConfig log;
    RuntimeConfig runtime;
    UiConfig ui;
};

bool validate_app_config(const AppConfig& config,
                         std::string& error) noexcept;
bool load_app_config(const std::string& path,
                     AppConfig& config,
                     std::string& error) noexcept;
bool save_app_config(const std::string& path,
                     const AppConfig& config,
                     std::string& error) noexcept;

#endif // CONFIG_H
