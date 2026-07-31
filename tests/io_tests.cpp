#include "capture/capture.h"
#include "keyboard/keyboard.h"
#include "log/log.h"
#include "mouse/mouse.h"

#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << message << '\n';
}

void test_mouse_disabled_by_default() {
    MouseConfig config;
    auto mouse = MouseDeviceFactory::create(config);
    expect(mouse && mouse->open(), "Win32 Mouse 后端应可初始化");
    expect(mouse && mouse->status() == MouseStatus::DISABLED,
           "物理输出默认必须为 DISABLED");
    expect(mouse && !mouse->move({1, 1}),
           "未显式允许 SendInput 时不得提交命令");
}

void test_invalid_keyboard_config() {
    KeyboardConfig config;
    config.aim_hold_virtual_key = 0;
    KeyboardListener keyboard(config);
    expect(!keyboard.open() && keyboard.status() == KeyboardStatus::FAILURE,
           "非法虚拟键配置必须失败关闭");
}

void test_invalid_capture_config() {
    CaptureConfig config;
    config.roi_width = 0;
    auto capture = create_capture(config);
    expect(capture && !capture->open(),
           "非法 ROI 必须在访问 DXGI 设备前被拒绝");
    expect(capture && capture->status() == CaptureStatus::INVALID_CONFIG,
           "非法 Capture 配置应返回明确状态");
}

} // namespace

int main() {
    LogConfig log_config;
    log_config.enable_console = false;
    log_config.enable_file = false;
    log_config.enable_ringbuf = false;
    Log::init(log_config);
    test_mouse_disabled_by_default();
    test_invalid_keyboard_config();
    test_invalid_capture_config();
    Log::shutdown();

    if (failures != 0) {
        std::cerr << "输入输出测试失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "输入输出测试全部通过。\n";
    return 0;
}
