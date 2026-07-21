// keyboard_listener.cpp - 键盘/鼠标按键监听线程
// 提供键盘监听功能，实时检测用户按键状态并更新程序的全局状态。
// 支持多种按键输入源：Win32 API、外部输入设备（KMBOX/Makcu等）。
// 还通过方向键提供运行时调整配置参数的功能。

#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>
#include <mutex>

#include "config.h"
#include "keyboard_listener.h"
#include "mouse.h"
#include "keycodes.h"
#include "Xen.h"
#include "capture.h"
#include "runtime/thread_loops.h"
#include "runtime/application_shutdown.h"
#include "runtime/zoom_toggle.h"

extern std::atomic<bool> shouldExit;
extern std::atomic<bool> aiming;
extern std::atomic<bool> shooting;
extern std::atomic<bool> zooming;
extern std::atomic<bool> detectionPaused;
extern std::atomic<bool> detector_model_changed;
extern std::atomic<bool> remoteReloadRequested;

extern MouseThread* globalMouseThread;

// 方向键单次步进调整量
const float OFFSET_STEP = 0.01f;
// 无后座力强度单次步进调整量
const float NORECOIL_STEP = 5.0f;

// 方向键映射
const std::vector<std::string> upArrowKeys = { "UpArrow" };
const std::vector<std::string> downArrowKeys = { "DownArrow" };
const std::vector<std::string> leftArrowKeys = { "LeftArrow" };
const std::vector<std::string> rightArrowKeys = { "RightArrow" };
const std::vector<std::string> shiftKeys = { "LeftShift", "RightShift" };

// 前一次方向键状态（用于边缘检测，避免按住时重复触发）
bool prevUpArrow = false;
bool prevDownArrow = false;
bool prevLeftArrow = false;
bool prevRightArrow = false;

namespace
{
    /**
     * KeyboardConfigSnapshot - 按键配置快照结构体
     * 用于在监听循环开始时一次性读取所有按键配置，
     * 避免在循环中频繁加锁访问 Config 对象。
     */
    struct KeyboardConfigSnapshot
    {
        bool autoAim = false;
        bool enableArrowsSettings = false;
        bool zoomToggleMode = false;
        std::vector<std::string> buttonTargeting;
        std::vector<std::string> buttonShoot;
        std::vector<std::string> buttonZoom;
        std::vector<std::string> buttonExit;
        std::vector<std::string> buttonPause;
        std::vector<std::string> buttonReloadConfig;
    };

    /**
     * SnapshotKeyboardConfig - 拍摄按键配置快照
     * 通过 configMutex 加锁一次性读取所有配置，
     * 避免后续循环中重复加锁。
     * @return 当前按键配置的快照
     */
    KeyboardConfigSnapshot SnapshotKeyboardConfig()
    {
        std::lock_guard<std::mutex> lock(configMutex);
        KeyboardConfigSnapshot snapshot;
        snapshot.autoAim = config.auto_aim;
        snapshot.enableArrowsSettings = config.enable_arrows_settings;
        const auto& profile = config.currentProfile();
        snapshot.zoomToggleMode = profile.name == "CS" && profile.fovScaled;
        snapshot.buttonTargeting = config.button_targeting;
        snapshot.buttonShoot = config.button_shoot;
        snapshot.buttonZoom = config.button_zoom;
        snapshot.buttonExit = config.button_exit;
        snapshot.buttonPause = config.button_pause;
        snapshot.buttonReloadConfig = config.button_reload_config;
        return snapshot;
    }

    /**
     * isAimingActiveFromDevices - 检查外部输入设备的瞄准状态
     * @return 如果外部设备正在瞄准返回 true
     */
    bool isAimingActiveFromDevices()
    {
        std::lock_guard<std::mutex> lock(inputDevicesMutex);
        return activeMouseInputOwner && activeMouseInputOwner->isOpen() && activeMouseInputOwner->aimingActive();
    }

    /**
     * isShootingActiveFromDevices - 检查外部输入设备的射击状态
     * @return 如果外部设备正在射击返回 true
     */
    bool isShootingActiveFromDevices()
    {
        std::lock_guard<std::mutex> lock(inputDevicesMutex);
        return activeMouseInputOwner && activeMouseInputOwner->isOpen() && activeMouseInputOwner->shootingActive();
    }

    /**
     * isZoomingActiveFromDevices - 检查外部输入设备的缩放状态
     * @return 如果外部设备正在缩放返回 true
     */
    bool isZoomingActiveFromDevices()
    {
        std::lock_guard<std::mutex> lock(inputDevicesMutex);
        return activeMouseInputOwner && activeMouseInputOwner->isOpen() && activeMouseInputOwner->zoomingActive();
    }

    /**
     * isAnyKeyPressedInternal - 检查一组按键中是否有任意一个被按下
     *
     * 支持两种按键检测方式：
     * 1. 外部输入设备（如 KMBOX / Makcu） - 通过设备接口查询
     * 2. Win32 API GetAsyncKeyState - 通过系统 API 实时检测
     *
     * 对于鼠标按键，如果外部输入设备支持物理按键状态，则优先使用设备检测。
     *
     * @param keys 要检测的按键名称列表
     * @return 如果任意按键被按下返回 true
     */
    bool isAnyKeyPressedInternal(const std::vector<std::string>& keys)
    {
        bool usePhysicalDevice = false;

        std::lock_guard<std::mutex> lock(inputDevicesMutex);

        IMouseInput* input = activeMouseInputOwner.get();
        if (input && input->isOpen() && input->hasPhysicalButtonState())
            usePhysicalDevice = true;

        for (const auto& key_name : keys)
        {
            int key_code = KeyCodes::getKeyCode(key_name);
            bool pressed = false;

            // 从外部输入设备检测
            if (input && input->isOpen())
                pressed = input->keyPressed(key_name);

            // 从 Win32 API 检测（键盘键或非物理设备的鼠标键）
            if (!pressed && key_code != -1)
            {
                bool isMouse = (key_name == "LeftMouseButton" ||
                    key_name == "RightMouseButton" ||
                    key_name == "MiddleMouseButton" ||
                    key_name == "X1MouseButton" ||
                    key_name == "X2MouseButton");

                // 鼠标键只在没有物理设备时用 Win32 API
                if (!isMouse || !usePhysicalDevice)
                {
                    pressed = (GetAsyncKeyState(key_code) & 0x8000) != 0;
                }
            }

            if (pressed) return true;
        }
        return false;
    }
} // namespace

/**
 * isAnyKeyPressed - 公开接口，检查一组按键中是否有任意一个被按下
 * @param keys 按键名称列表
 * @return 按下返回 true
 */
bool isAnyKeyPressed(const std::vector<std::string>& keys)
{
    return isAnyKeyPressedInternal(keys);
}

/**
 * isAnyKeyPressedWin32Only - 仅使用 Win32 API 检查按键状态
 * @param keys 按键名称列表
 * @return 按下返回 true
 */
bool isAnyKeyPressedWin32Only(const std::vector<std::string>& keys)
{
    for (const auto& key_name : keys)
    {
        int key_code = KeyCodes::getKeyCode(key_name);
        if (key_code != -1 && (GetAsyncKeyState(key_code) & 0x8000))
            return true;
    }
    return false;
}

/**
 * keyboardListener - 键盘监听线程主循环
 *
 * 线程职责：
 * 1. 检测瞄准、射击、缩放按键，更新全局原子变量
 * 2. 检测退出、暂停、重载配置等系统功能键
 * 3. 检测方向键和 Shift 组合，用于运行时调整 body_y_offset、head_y_offset
 *    和 easynorecoilstrength 参数
 *
 * 循环频率：每 10ms 一次（约 100Hz）
 */
void keyboardListener()
{
    ZoomToggleState zoomToggleState;
    while (!shouldExit)
    {
        KeyboardConfigSnapshot cfg = SnapshotKeyboardConfig();

        // === 瞄准状态检测 ===
        // 如果 auto_aim 启用，保持瞄准状态为 true；
        // 否则检测按键或外部输入设备状态
        if (!cfg.autoAim)
        {
            aiming = isAnyKeyPressedInternal(cfg.buttonTargeting) ||
                isAimingActiveFromDevices();
        }
        else
        {
            aiming = true;
        }

        // === 射击状态检测 ===
        shooting = isAnyKeyPressedInternal(cfg.buttonShoot) ||
            isShootingActiveFromDevices();

        // === 缩放（开镜）状态检测 ===
        // CS单倍镜按右键上升沿切换，第二次点击退出；其他Profile保留按住语义。
        const bool zoomPressed = isAnyKeyPressedInternal(cfg.buttonZoom) ||
            isZoomingActiveFromDevices();
        zooming = zoomToggleState.update(zoomPressed, cfg.zoomToggleMode);

        // === 退出程序（仅 Win32） ===
        if (isAnyKeyPressedWin32Only(cfg.buttonExit))
        {
            RequestApplicationShutdown();
        }

        // === 暂停检测（仅 Win32，边缘触发） ===
        static bool pausePressed = false;
        if (isAnyKeyPressedWin32Only(cfg.buttonPause))
        {
            if (!pausePressed)
            {
                detectionPaused = !detectionPaused;
                pausePressed = true;
            }
        }
        else
        {
            pausePressed = false;
        }

        // === 重载配置（仅 Win32，边缘触发） ===
        static bool reloadPressed = false;
        const bool remoteReload = remoteReloadRequested.exchange(false);
        if (remoteReload || isAnyKeyPressedWin32Only(cfg.buttonReloadConfig))
        {
	            if (!reloadPressed)
	            {
	                try
	                {
	                    {
	                        std::lock_guard<std::mutex> lock(configMutex);
	                        // 保存旧配置用于对比，仅在有变化时通知相应模块
	                        const int oldDetectionResolution = config.detection_resolution;
	                        const int oldCaptureFps = config.capture_fps;
	                        const std::string oldCaptureMethod = config.capture_method;
	                        const std::string oldCaptureTarget = config.capture_target;
	                        const std::string oldCaptureWindowTitle = config.capture_window_title;
	                        const int oldMonitorIdx = config.monitor_idx;
	                        const bool oldCaptureBorders = config.capture_borders;
	                        const bool oldCaptureCursor = config.capture_cursor;
	                        const std::string oldVirtualCameraName = config.virtual_camera_name;
	                        const int oldVirtualCameraWidth = config.virtual_camera_width;
	                        const int oldVirtualCameraHeight = config.virtual_camera_height;
	                        const std::string oldUdpIp = config.udp_ip;
	                        const int oldUdpPort = config.udp_port;
	                        const int oldUdpSourceWidth = config.udp_source_width;
	                        const int oldUdpSourceHeight = config.udp_source_height;
	                        const std::string oldNdiSourceName = config.ndi_source_name;
	                        const int oldNdiSourceWidth = config.ndi_source_width;
	                        const int oldNdiSourceHeight = config.ndi_source_height;
	                        const std::string oldBackend = config.backend;
	                        const std::string oldAiModel = config.ai_model;
	                        const std::string oldInputMethod = config.input_method;

	                        if (!config.loadConfig())
	                        {
	                            std::cerr << "[Reload] Failed to parse config.ini, keeping previous configuration." << std::endl;
	                            reloadPressed = true;
	                            continue;
	                        }

	                        // 检测分辨率变化 -> 需要重载模型
	                        if (config.detection_resolution != oldDetectionResolution)
	                        {
	                            detection_resolution_changed.store(true);
	                            detector_model_changed.store(true);
	                        }

	                        // 帧率变化
	                        if (config.capture_fps != oldCaptureFps)
	                            capture_fps_changed.store(true);

	                        // 捕获方式或目标变化
	                        if (config.capture_method != oldCaptureMethod ||
	                            config.capture_target != oldCaptureTarget ||
	                            config.capture_window_title != oldCaptureWindowTitle ||
	                            config.monitor_idx != oldMonitorIdx ||
	                            config.virtual_camera_name != oldVirtualCameraName ||
	                            config.virtual_camera_width != oldVirtualCameraWidth ||
	                            config.virtual_camera_height != oldVirtualCameraHeight ||
	                            config.udp_ip != oldUdpIp ||
	                            config.udp_port != oldUdpPort ||
	                            config.udp_source_width != oldUdpSourceWidth ||
	                            config.udp_source_height != oldUdpSourceHeight ||
	                            config.ndi_source_name != oldNdiSourceName ||
	                            config.ndi_source_width != oldNdiSourceWidth ||
	                            config.ndi_source_height != oldNdiSourceHeight)
	                        {
	                            capture_method_changed.store(true);
	                            capture_window_changed.store(true);
	                        }

	                        // 窗口边框设置变化
	                        if (config.capture_borders != oldCaptureBorders)
	                            capture_borders_changed.store(true);

	                        // 鼠标光标设置变化
	                        if (config.capture_cursor != oldCaptureCursor)
	                            capture_cursor_changed.store(true);

	                        // AI 后端或模型变化
	                        if (config.backend != oldBackend || config.ai_model != oldAiModel)
	                            detector_model_changed.store(true);

	                        // 输入方法变化
	                        if (config.input_method != oldInputMethod)
	                            input_method_changed.store(true);

	                        // 通知鼠标线程更新配置
	                        if (globalMouseThread)
	                        {
	                            globalMouseThread->updateConfig(
	                                config.detection_resolution,
	                                config.fovX,
	                                config.fovY,
	                                config.auto_shoot,
	                                config.bScope_multiplier
	                            );
	                        }
	                    }
	                    std::cout << "[Reload] Configuration reloaded successfully." << std::endl;
	                }
	                catch (const std::exception& e)
	                {
	                    std::cerr << "[Reload] Error reloading config: " << e.what()
	                              << ". Keeping previous configuration." << std::endl;
	                }
	                catch (...)
	                {
	                    std::cerr << "[Reload] Unknown error reloading config. Keeping previous configuration." << std::endl;
	                }
	                reloadPressed = true;
	            }
        }
        else
        {
            reloadPressed = false;
        }

        // === 方向键调整配置（仅 Win32） ===
        bool upArrow = isAnyKeyPressedWin32Only(upArrowKeys);
        bool downArrow = isAnyKeyPressedWin32Only(downArrowKeys);
        bool leftArrow = isAnyKeyPressedWin32Only(leftArrowKeys);
        bool rightArrow = isAnyKeyPressedWin32Only(rightArrowKeys);
        bool shiftKey = isAnyKeyPressedWin32Only(shiftKeys);

        if (cfg.enableArrowsSettings)
        {
            // 上/下方向键调整瞄准偏移量（配合 Shift 调整头部偏移，否则调整身体偏移）
            if (upArrow && !prevUpArrow)
            {
                std::lock_guard<std::mutex> lock(configMutex);
                if (shiftKey)
                {
                    // Shift + 上箭头：减小头部 Y 偏移（瞄准点下移）
                    config.head_y_offset = std::max(0.0f, config.head_y_offset - OFFSET_STEP);
                }
                else
                {
                    // 上箭头：减小身体 Y 偏移（瞄准点下移）
                    config.body_y_offset = std::max(0.0f, config.body_y_offset - OFFSET_STEP);
                }
            }
            if (downArrow && !prevDownArrow)
            {
                std::lock_guard<std::mutex> lock(configMutex);
                if (shiftKey)
                {
                    // Shift + 下箭头：增大头部 Y 偏移（瞄准点上移）
                    config.head_y_offset = std::min(1.0f, config.head_y_offset + OFFSET_STEP);
                }
                else
                {
                    // 下箭头：增大身体 Y 偏移（瞄准点上移）
                    config.body_y_offset = std::min(1.0f, config.body_y_offset + OFFSET_STEP);
                }
            }

            // 左/右方向键调整无后座力强度
            if (leftArrow && !prevLeftArrow)
            {
                std::lock_guard<std::mutex> lock(configMutex);
                config.easynorecoilstrength = std::max(0.1f, config.easynorecoilstrength - NORECOIL_STEP);
            }

            if (rightArrow && !prevRightArrow)
            {
                std::lock_guard<std::mutex> lock(configMutex);
                config.easynorecoilstrength = std::min(500.0f, config.easynorecoilstrength + NORECOIL_STEP);
            }
        }

        // 保存当前方向键状态用于下一次边缘检测
        prevUpArrow = upArrow;
        prevDownArrow = downArrow;
        prevLeftArrow = leftArrow;
        prevRightArrow = rightArrow;

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
