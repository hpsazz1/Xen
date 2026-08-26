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
    // 产品默认配置集中在聚合层，独立模块仍保留适合算法与设备测试的通用安全默认值。
    DetectorConfig detector = [] {
        DetectorConfig value;
        value.model_path = "14wv11.onnx";
        value.backend = BackendType::TENSORRT;
        value.openvino_device = OpenVinoDevice::CPU;
        value.enable_fp16 = true;
        return value;
    }();
    CaptureConfig capture = [] {
        CaptureConfig value;
        value.backend = CaptureBackend::NDI;
        value.udp_frame_layout = UdpFrameLayout::CENTER_CROP_1_TO_1;
        value.udp_source_width = 2560;
        value.udp_source_height = 1440;
        value.ndi_source_name = "HPSAZZ (Xen-ROI-320)";
        value.ndi_discovery_timeout_ms = 10000;
        value.ndi_clock_sync_url = "udp://192.168.3.10:5011";
        value.ndi_frame_layout = NetworkFrameLayout::CENTER_CROP_1_TO_1;
        value.ndi_source_width = 2560;
        value.ndi_source_height = 1440;
        return value;
    }();
    AimConfig aim = [] {
        AimConfig value;
        value.person_class_ids = {0, 2};
        value.head_class_ids = {1, 3};
        value.smoothing = 0.475f;
        value.counts_per_pixel_x = 0.40f;
        value.counts_per_pixel_y = 0.40f;
        value.max_counts_per_frame = 12.0f;
        value.enable_delay_compensation = true;
        value.control_delay_ms = 40.0f;
        value.max_delay_compensation_ms = 44.0f;
        value.enable_prediction = true;
        return value;
    }();
    MouseConfig mouse = [] {
        MouseConfig value;
        value.backend = MouseBackend::KMBOX_NET;
        value.kmbox_ip = "192.168.2.188";
        value.kmbox_port = 13384;
        value.kmbox_uuid = "7679E04E";
        return value;
    }();
    KeyboardConfig keyboard;
    LogConfig log = [] {
        LogConfig value;
        value.global_level = LogLevel::INFO;
        return value;
    }();
    RuntimeConfig runtime;
    UiConfig ui = [] {
        UiConfig value;
        value.open_detached_preview_on_start = true;
        return value;
    }();
};

bool validate_app_config(const AppConfig& config,
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
