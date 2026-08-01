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
