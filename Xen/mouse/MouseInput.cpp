// ============================================================================
// MouseInput.cpp — 统一鼠标输入抽象层
//
// 本文件实现了针对所有受支持的鼠标硬件设备的 IMouseInput 接口，
// 涵盖以下设备类型：
//   - Win32      : Windows 原生 SendInput API（默认方案）
//   - Arduino    : 串口连接的 Arduino 设备（传统 / Teensy 4.1 协议）
//   - RP2350     : 串口连接的 RP2350 微控制器设备
//   - Teensy41   : 串口连接的 Teensy 4.1 设备（Arduino 协议的变体）
//   - Teensy41_HID : 通过 RawHID 接口通信的 Teensy 4.1 设备
//   - GHub       : Logitech G Hub 游戏鼠标（ghub_mouse.dll）
//   - Razer      : Razer 游戏鼠标（rzctl.dll）
//   - Kmbox Net  : 基于网络（TCP）连接的 Kmbox 设备
//   - Kmbox A    : 通过 USB HID 连接的 Kmbox A 设备
//   - Makcu      : 串口连接的 Makcu 设备
//
// 本文件同时提供以下工具函数：
//   - ParseMouseInputMethod()  : 将字符串解析为 MouseInputMethod 枚举
//   - MouseInputMethodName()   : 将枚举转换回对应的字符串名称
//   - CreateMouseInputDevice() : 工厂函数，根据配置创建合适的设备实例
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <Windows.h>

#include "MouseInput.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include "Arduino.h"
#include "KmboxAConnection.h"
#include "KmboxNetConnection.h"
#include "Makcu.h"
#include "RP2350.h"
#include "Teensy41RawHid.h"
#include "config.h"
#include "ghub.h"
#include "rzctl.h"

namespace
{

// ============================================================================
// logicalButtonPressed — 将逻辑按键名称映射为对应的按键状态
//
// 根据游戏中常见的鼠标按键映射规则，将字符串形式的按键名称
// 转换为对应的布尔状态值：
//   - "LeftMouseButton"  →  shootingActive（开火/射击）
//   - "RightMouseButton" →  zoomingActive（瞄准/缩放）
//   - "X2MouseButton"    →  aimingActive（腰射/侧键）
//   其他按键名称均返回 false。
//
// 参数:
//   keyName        - 待查询的按键名称字符串
//   shootingActive - 当前开火状态
//   zoomingActive  - 当前缩放/瞄准状态
//   aimingActive   - 当前腰射/侧键状态
//
// 返回: 按键名称匹配且对应状态为 true 时返回 true，否则返回 false
// ============================================================================
bool logicalButtonPressed(
    const std::string& keyName,
    bool shootingActive,
    bool zoomingActive,
    bool aimingActive)
{
    if (keyName == "LeftMouseButton")
        return shootingActive;
    if (keyName == "RightMouseButton")
        return zoomingActive;
    if (keyName == "X2MouseButton")
        return aimingActive;
    return false;
}

// ============================================================================
// sendWin32Move — 通过 Win32 SendInput API 发送相对鼠标移动
//
// 构造一个类型为 INPUT_MOUSE 的 INPUT 结构体，设置相对位移量
// (dx, dy) 以及 MOUSEEVENTF_MOVE | MOUSEEVENTF_VIRTUALDESK 标志，
// 然后调用 SendInput 执行移动。适用于虚拟桌面环境下的相对移动。
//
// 参数:
//   dx - X 轴方向的相对移动像素数（正值向右）
//   dy - Y 轴方向的相对移动像素数（正值向下）
//
// 返回: SendInput 调用成功（返回值为 1）时返回 true，否则返回 false
// ============================================================================
bool sendWin32Move(int dx, int dy)
{
    INPUT input{ 0 };
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_VIRTUALDESK;
    return SendInput(1, &input, sizeof(INPUT)) == 1;
}

// ============================================================================
// sendWin32Click — 通过 Win32 SendInput API 发送鼠标点击事件
//
// 构造一个 INPUT_MOUSE 类型的 INPUT 结构体，设置指定的鼠标事件标志，
// 然后调用 SendInput 执行。典型的用法是分别调用
// sendWin32Click(MOUSEEVENTF_LEFTDOWN) 和
// sendWin32Click(MOUSEEVENTF_LEFTUP) 来模拟一次完整的左键点击。
//
// 参数:
//   flag - 鼠标事件标志（如 MOUSEEVENTF_LEFTDOWN、MOUSEEVENTF_LEFTUP）
//
// 返回: SendInput 调用成功（返回值为 1）时返回 true，否则返回 false
// ============================================================================
bool sendWin32Click(DWORD flag)
{
    INPUT input{ 0 };
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flag;
    return SendInput(1, &input, sizeof(INPUT)) == 1;
}

// ============================================================================
// Win32MouseInput — Windows 默认鼠标输入实现
//
// 封装 Win32 的 SendInput API 实现最基本的鼠标移动与点击操作。
// 该类是最简单也是最终的保底方案：不依赖任何外部硬件或第三方库。
//
// 特性说明:
//   - isOpen() 始终返回 true —— 不存在"连接"的概念
//   - 移动操作通过 sendWin32Move() 实现相对移动
//   - 点击操作通过 sendWin32Click() 分别发送按下/释放事件
//   - 不支持物理按键状态查询（hasPhysicalButtonState 未重写，默认 false）
// ============================================================================
class Win32MouseInput final : public IMouseInput
{
public:
    const char* name() const override { return "WIN32"; }
    bool isOpen() const override { return true; }
    bool move(int dx, int dy) override { return sendWin32Move(dx, dy); }
    bool leftDown() override { return sendWin32Click(MOUSEEVENTF_LEFTDOWN); }
    bool leftUp() override { return sendWin32Click(MOUSEEVENTF_LEFTUP); }
};

// ============================================================================
// ArduinoMouseInput — Arduino 串口鼠标输入实现（传统协议）
//
// 通过串口连接 Arduino 微控制器，使用指定的 ArduinoProtocol 协议
// （通常为 Legacy 协议）进行通信。Arduino 作为 USB HID 桥接器，
// 接收串口指令并模拟鼠标操作。
//
// 特性说明:
//   - 初始化时需指定串口名称（如 "COM3"）、波特率及协议版本
//   - 支持可选的物理按键状态上报（useButtonState_ 为 true 时启用）
//   - 按键状态通过逻辑映射函数 logicalButtonPressed() 将硬件按钮
//     状态（shooting_active / zooming_active / aiming_active）映射为按键名
//   - 提供 arduino() 方法允许外部直接访问底层 Arduino 对象
// ============================================================================
class ArduinoMouseInput final : public IMouseInput
{
public:
    // 构造函数：创建 Arduino 串口连接
    // 参数:
    //   port           - 串口名称（如 "COM3"）
    //   baudrate       - 串口波特率（如 115200）
    //   useButtonState - 是否启用物理按键状态上报功能
    //   protocol       - Arduino 通信协议版本（默认 Legacy）
    ArduinoMouseInput(
        const std::string& port,
        unsigned int baudrate,
        bool useButtonState,
        ArduinoProtocol protocol = ArduinoProtocol::Legacy)
        : device_(std::make_unique<Arduino>(port, baudrate, protocol)),
          useButtonState_(useButtonState)
    {
    }

    const char* name() const override { return "ARDUINO"; }
    bool isOpen() const override { return device_ && device_->isOpen(); }
    bool move(int dx, int dy) override
    {
        if (!isOpen())
            return false;
        device_->move(dx, dy);
        return true;
    }
    bool leftDown() override
    {
        if (!isOpen())
            return false;
        device_->press();
        return true;
    }
    bool leftUp() override
    {
        if (!isOpen())
            return false;
        device_->release();
        return true;
    }
    bool hasPhysicalButtonState() const override { return useButtonState_; }
    bool keyPressed(const std::string& keyName) override
    {
        return isOpen() && useButtonState_ &&
            logicalButtonPressed(keyName, shootingActive(), zoomingActive(), aimingActive());
    }
    bool aimingActive() const override { return useButtonState_ && device_ && device_->aiming_active; }
    bool shootingActive() const override { return useButtonState_ && device_ && device_->shooting_active; }
    bool zoomingActive() const override { return useButtonState_ && device_ && device_->zooming_active; }
    Arduino* arduino() override { return device_.get(); }

private:
    std::unique_ptr<Arduino> device_;       // 底层 Arduino 串口设备实例
    bool useButtonState_ = false;           // 是否启用物理按键状态上报
};

// ============================================================================
// RP2350MouseInput — RP2350 微控制器串口鼠标输入实现
//
// 通过串口连接 RP2350（树莓派 Pico 2 等）微控制器设备。
// 与 ArduinoMouseInput 结构类似，但内部使用 RP2350 驱动类，
// 且原子状态变量（std::atomic<bool>）适用于多线程环境。
//
// 特性说明:
//   - 初始化时需指定串口名称与波特率
//   - 支持可选的物理按键状态上报（useButtonState_ 控制开关）
//   - 状态变量（shooting_active 等）使用 std::atomic 保证线程安全
//   - 提供 rp2350() 方法允许外部直接访问底层 RP2350 对象
// ============================================================================
class RP2350MouseInput final : public IMouseInput
{
public:
    // 构造函数：创建 RP2350 串口连接
    // 参数:
    //   port           - 串口名称（如 "COM5"）
    //   baudrate       - 串口波特率
    //   useButtonState - 是否启用物理按键状态上报
    RP2350MouseInput(const std::string& port, unsigned int baudrate, bool useButtonState)
        : device_(std::make_unique<RP2350>(port, baudrate)),
          useButtonState_(useButtonState)
    {
    }

    const char* name() const override { return "RP2350"; }
    bool isOpen() const override { return device_ && device_->isOpen(); }
    bool move(int dx, int dy) override
    {
        if (!isOpen())
            return false;
        device_->move(dx, dy);
        return true;
    }
    bool leftDown() override
    {
        if (!isOpen())
            return false;
        device_->press();
        return true;
    }
    bool leftUp() override
    {
        if (!isOpen())
            return false;
        device_->release();
        return true;
    }
    bool hasPhysicalButtonState() const override { return useButtonState_; }
    bool keyPressed(const std::string& keyName) override
    {
        return isOpen() && useButtonState_ &&
            logicalButtonPressed(keyName, shootingActive(), zoomingActive(), aimingActive());
    }
    bool aimingActive() const override { return useButtonState_ && device_ && device_->aiming_active.load(); }
    bool shootingActive() const override { return useButtonState_ && device_ && device_->shooting_active.load(); }
    bool zoomingActive() const override { return useButtonState_ && device_ && device_->zooming_active.load(); }
    RP2350* rp2350() override { return device_.get(); }

private:
    std::unique_ptr<RP2350> device_;        // 底层 RP2350 串口设备实例
    bool useButtonState_ = false;           // 是否启用物理按键状态上报
};

// ============================================================================
// Teensy41MouseInput — Teensy 4.1 串口鼠标输入实现
//
// 使用 Arduino 协议但指定协议版本为 ArduinoProtocol::Teensy41，
// 通过串口连接 PJRC Teensy 4.1 开发板。Teensy 4.1 作为 USB HID
// 设备接收串口指令模拟鼠标操作。
//
// 特性说明:
//   - 底层复用了 Arduino 驱动类，但使用 Teensy41 专属协议
//   - 始终启用物理按键状态上报（hasPhysicalButtonState 返回 true）
//   - 串口端口与波特率通过构造函数参数传入
//   - 提供 arduino() 方法允许外部访问底层的 Arduino 对象
// ============================================================================
class Teensy41MouseInput final : public IMouseInput
{
public:
    // 构造函数：创建 Teensy 4.1 串口连接（复用 Arduino 驱动，使用 Teensy41 协议）
    // 参数:
    //   port     - 串口名称
    //   baudrate - 串口波特率
    Teensy41MouseInput(const std::string& port, unsigned int baudrate)
        : device_(std::make_unique<Arduino>(port, baudrate, ArduinoProtocol::Teensy41))
    {
    }

    const char* name() const override { return "TEENSY41"; }
    bool isOpen() const override { return device_ && device_->isOpen(); }
    bool move(int dx, int dy) override
    {
        if (!isOpen())
            return false;
        device_->move(dx, dy);
        return true;
    }
    bool leftDown() override
    {
        if (!isOpen())
            return false;
        device_->press();
        return true;
    }
    bool leftUp() override
    {
        if (!isOpen())
            return false;
        device_->release();
        return true;
    }
    bool hasPhysicalButtonState() const override { return true; }
    bool keyPressed(const std::string& keyName) override
    {
        return isOpen() &&
            logicalButtonPressed(keyName, shootingActive(), zoomingActive(), aimingActive());
    }
    bool aimingActive() const override { return device_ && device_->aiming_active; }
    bool shootingActive() const override { return device_ && device_->shooting_active; }
    bool zoomingActive() const override { return device_ && device_->zooming_active; }
    Arduino* arduino() override { return device_.get(); }

private:
    std::unique_ptr<Arduino> device_;       // 底层 Arduino 串口设备实例（Teensy41 协议模式）
};

// ============================================================================
// Teensy41RawHidMouseInput — Teensy 4.1 RawHID 鼠标输入实现
//
// 通过 RawHID（原始 HID）协议与 Teensy 4.1 设备通信，区别于传统的串口方式。
// RawHID 使用 USB HID 原生通道，可能具有更低的延迟和更高的可靠性。
//
// 特性说明:
//   - 初始化时接收完整的 Config 结构体，从中提取 RawHID 所需参数
//   - 始终启用物理按键状态上报（hasPhysicalButtonState 返回 true）
//   - 所有操作（move / press / release）均返回 bool 表示成功与否
//   - 提供 teensy41RawHid() 方法允许外部访问底层 RawHID 对象
// ============================================================================
class Teensy41RawHidMouseInput final : public IMouseInput
{
public:
    // 构造函数：创建 Teensy 4.1 RawHID 连接
    // 参数:
    //   config - 应用配置对象，包含 RawHID 通信所需的所有参数
    explicit Teensy41RawHidMouseInput(const Config& config)
        : device_(std::make_unique<Teensy41RawHid>(config))
    {
    }

    const char* name() const override { return "TEENSY41_HID"; }
    bool isOpen() const override { return device_ && device_->isOpen(); }
    bool move(int dx, int dy) override
    {
        if (!isOpen())
            return false;
        return device_->move(dx, dy);
    }
    bool leftDown() override
    {
        if (!isOpen())
            return false;
        return device_->press();
    }
    bool leftUp() override
    {
        if (!isOpen())
            return false;
        return device_->release();
    }
    bool hasPhysicalButtonState() const override { return true; }
    bool keyPressed(const std::string& keyName) override
    {
        return isOpen() &&
            logicalButtonPressed(keyName, shootingActive(), zoomingActive(), aimingActive());
    }
    bool aimingActive() const override { return device_ && device_->aimingActive(); }
    bool shootingActive() const override { return device_ && device_->shootingActive(); }
    bool zoomingActive() const override { return device_ && device_->zoomingActive(); }
    Teensy41RawHid* teensy41RawHid() override { return device_.get(); }

private:
    std::unique_ptr<Teensy41RawHid> device_;    // 底层 Teensy 4.1 RawHID 设备实例
};

// ============================================================================
// GHubMouseInput — Logitech G Hub 鼠标输入实现
//
// 通过 Logitech G Hub 提供的动态链接库（ghub_mouse.dll）与罗技游戏鼠标
// 进行通信。G Hub 的 DLL 接口基于 Windows 消息或共享内存实现，
// 无需直接操作 USB HID 即可控制鼠标光标的移动与点击。
//
// 特性说明:
//   - 构造函数内通过 device_->mouse_xy(0, 0) 发送测试指令以验证连接
//   - 使用 open_ 标志记录连接状态，而非依赖 isOpen() 的查询
//   - 不支持物理按键状态查询（未重写相关方法）
//   - 析构函数中调用 device_->mouse_close() 清理资源
//   - 提供 ghub() 方法允许外部直接访问底层 GhubMouse 对象
// ============================================================================
class GHubMouseInput final : public IMouseInput
{
public:
    // 构造函数：初始化 G Hub 连接并通过零位移移动测试连接有效性
    GHubMouseInput()
        : device_(std::make_unique<GhubMouse>())
    {
        open_ = device_ && device_->mouse_xy(0, 0);
    }

    // 析构函数：关闭 G Hub 连接并释放底层资源
    ~GHubMouseInput() override
    {
        if (device_)
            device_->mouse_close();
    }

    const char* name() const override { return "GHUB"; }
    bool isOpen() const override { return device_ && open_; }
    bool move(int dx, int dy) override { return isOpen() && device_->mouse_xy(dx, dy); }
    bool leftDown() override { return isOpen() && device_->mouse_down(); }
    bool leftUp() override { return isOpen() && device_->mouse_up(); }
    GhubMouse* ghub() override { return device_.get(); }

private:
    std::unique_ptr<GhubMouse> device_;     // 底层 G Hub 鼠标设备实例
    bool open_ = false;                     // 连接状态标志（由构造函数中的测试指令确认）
};

// ============================================================================
// RazerMouseInput — Razer 鼠标输入实现
//
// 通过 Razer 提供的动态链接库（rzctl.dll）与雷蛇游戏鼠标进行通信。
// rzctl.dll 封装了 Razer 设备的私有 HID 协议，使上层应用能够
// 以标准接口控制鼠标光标的移动与按键。
//
// 特性说明:
//   - 底层设备在构造时创建，连接状态由 isOpen() 动态查询
//   - 不支持物理按键状态查询（未重写相关方法）
//   - 析构函数中调用 device_->mouse_close() 清理资源
//   - 提供 razer() 方法允许外部直接访问底层 RzctlMouse 对象
// ============================================================================
class RazerMouseInput final : public IMouseInput
{
public:
    // 构造函数：创建 Razer 鼠标控制接口实例
    RazerMouseInput()
        : device_(std::make_unique<RzctlMouse>())
    {
    }

    // 析构函数：关闭 Razer 连接并释放底层资源
    ~RazerMouseInput() override
    {
        if (device_)
            device_->mouse_close();
    }

    const char* name() const override { return "RAZER"; }
    bool isOpen() const override { return device_ && device_->isOpen(); }
    bool move(int dx, int dy) override { return isOpen() && device_->mouse_xy(dx, dy); }
    bool leftDown() override { return isOpen() && device_->mouse_down(); }
    bool leftUp() override { return isOpen() && device_->mouse_up(); }
    RzctlMouse* razer() override { return device_.get(); }

private:
    std::unique_ptr<RzctlMouse> device_;    // 底层 Razer 鼠标设备实例
};

// ============================================================================
// KmboxNetMouseInput — Kmbox Net（网络）鼠标输入实现
//
// 通过 TCP 网络连接与 Kmbox Net 设备进行通信，该设备通常作为
// 局域网中的独立硬件工作。连接过程异步执行，以避免阻塞主线程。
//
// 核心设计：
//   1. 构造时创建一个 std::thread 后台线程执行实际的 TCP 连接，
//      使用分离（detach）模式使线程在后台独立运行
//   2. 通过 std::shared_ptr<State> 在异步线程与主对象间共享状态，
//      利用 std::mutex 保证线程安全的数据访问
//   3. std::atomic<bool> connecting 标志指示设备是否正在连接中
//   4. 所有公开操作（move / leftDown / leftUp / keyPressed 等）
//      均在加锁后检查设备就绪状态再执行
//   5. 支持物理按键状态查询，直接从 KmboxNetConnection 的原子变量读取
//
// 按键查询优先级：先检查 KmboxNetConnection 的 monitor 系列方法
// （直接读取硬件按钮状态），降级到 logicalButtonPressed 逻辑映射。
// ============================================================================
class KmboxNetMouseInput final : public IMouseInput
{
public:
    // 构造函数：启动异步连接线程
    //
    // 参数:
    //   ip   - Kmbox Net 设备的 IP 地址
    //   port - 目标端口号
    //   uuid - 设备 UUID（用于身份验证或设备标识）
    //
    // 实现说明:
    //   后台线程创建 KmboxNetConnection 实例，完成 TCP 握手后
    //   通过共享状态结构体 State 将设备实例传递给主对象。
    //   构造函数立即返回，不等待连接完成。
    KmboxNetMouseInput(const std::string& ip, const std::string& port, const std::string& uuid)
        : state_(std::make_shared<State>())
    {
        state_->connecting.store(true);
        std::thread([state = state_, ip, port, uuid] {
            auto device = std::make_unique<KmboxNetConnection>(ip, port, uuid);
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->device = std::move(device);
            }
            state->connecting.store(false);
            }).detach();
    }

    // 析构函数：安全断开网络连接
    //
    // 在互斥锁保护下销毁设备对象并将 connecting 标志置为 false，
    // 确保异步线程不会在对象销毁后继续访问已释放的内存。
    ~KmboxNetMouseInput() override
    {
        if (!state_)
            return;

        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->device.reset();
        state_->connecting.store(false);
    }

    const char* name() const override { return "KMBOX_NET"; }
    bool isOpen() const override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->device && state_->device->isOpen();
    }
    bool move(int dx, int dy) override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->device || !state_->device->isOpen())
            return false;
        state_->device->move(dx, dy);
        return true;
    }
    bool leftDown() override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->device || !state_->device->isOpen())
            return false;
        state_->device->leftDown();
        return true;
    }
    bool leftUp() override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->device || !state_->device->isOpen())
            return false;
        state_->device->leftUp();
        return true;
    }
    bool hasPhysicalButtonState() const override { return true; }
    bool keyPressed(const std::string& keyName) override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        KmboxNetConnection* device = state_->device.get();
        if (!device || !device->isOpen())
            return false;

        if (keyName == "LeftMouseButton" && device->monitorMouseLeft() == 1)
            return true;
        if (keyName == "RightMouseButton" && device->monitorMouseRight() == 1)
            return true;
        if (keyName == "MiddleMouseButton" && device->monitorMouseMiddle() == 1)
            return true;
        if (keyName == "X1MouseButton" && device->monitorMouseSide1() == 1)
            return true;
        if (keyName == "X2MouseButton" && device->monitorMouseSide2() == 1)
            return true;

        return logicalButtonPressed(
            keyName,
            device->shooting_active.load(),
            device->zooming_active.load(),
            device->aiming_active.load());
    }
    bool aimingActive() const override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->device && state_->device->aiming_active.load();
    }
    bool shootingActive() const override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->device && state_->device->shooting_active.load();
    }
    bool zoomingActive() const override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->device && state_->device->zooming_active.load();
    }
    KmboxNetConnection* kmboxNet() override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->device.get();
    }

private:
    // State — 异步连接共享状态结构体
    //
    // 包含线程安全的互斥锁、设备实例指针以及连接中标志。
    // 使用 std::shared_ptr 在异步线程和主对象间共享生命周期。
    struct State
    {
        mutable std::mutex mutex;                       // 保护 device 访问的互斥锁
        std::unique_ptr<KmboxNetConnection> device;     // Kmbox 网络连接实例
        std::atomic<bool> connecting{ false };          // 是否正在连接中的标志
    };

    std::shared_ptr<State> state_;                      // 共享状态指针
};

// ============================================================================
// KmboxAMouseInput — Kmbox A（USB HID）鼠标输入实现
//
// 通过 USB HID 直连方式与 Kmbox A 设备通信。Kmbox A 以 USB HID
// 设备的形式连接到主机，通过 HID 报告（report）收发鼠标指令，
// 无需网络协议栈参与。
//
// 特性说明:
//   - 初始化时需指定 USB 设备的 VID:PID 标识符（如 "1234:5678"）
//   - 不支持物理按键状态查询（未重写相关方法）
//   - 提供 kmboxA() 方法允许外部直接访问底层 KmboxAConnection 对象
// ============================================================================
class KmboxAMouseInput final : public IMouseInput
{
public:
    // 构造函数：通过 USB VID:PID 创建 Kmbox A HID 连接
    // 参数:
    //   pidvid - USB 设备的 VID:PID 字符串（如 "046D:C077"）
    explicit KmboxAMouseInput(const std::string& pidvid)
        : device_(std::make_unique<KmboxAConnection>(pidvid))
    {
    }

    const char* name() const override { return "KMBOX_A"; }
    bool isOpen() const override { return device_ && device_->isOpen(); }
    bool move(int dx, int dy) override
    {
        if (!isOpen())
            return false;
        device_->move(dx, dy);
        return true;
    }
    bool leftDown() override
    {
        if (!isOpen())
            return false;
        device_->leftDown();
        return true;
    }
    bool leftUp() override
    {
        if (!isOpen())
            return false;
        device_->leftUp();
        return true;
    }
    KmboxAConnection* kmboxA() override { return device_.get(); }

private:
    std::unique_ptr<KmboxAConnection> device_;      // 底层 Kmbox A HID 连接实例
};

// ============================================================================
// MakcuMouseInput — Makcu 串口鼠标输入实现
//
// 通过串口连接 Makcu 品牌设备。与 Arduino 类似，Makcu 也是
// 基于串行通信的鼠标模拟硬件，但使用专有的 MakcuConnection 驱动类。
//
// 特性说明:
//   - 初始化时需指定串口名称与波特率
//   - 始终启用物理按键状态上报（hasPhysicalButtonState 返回 true）
//   - 左键点击通过 press(index) / release(index) 方法，索引 0 表示左键
//   - 提供 makcu() 方法允许外部直接访问底层 MakcuConnection 对象
// ============================================================================
class MakcuMouseInput final : public IMouseInput
{
public:
    // 构造函数：创建 Makcu 串口连接
    // 参数:
    //   port     - 串口名称（如 "COM7"）
    //   baudrate - 串口波特率
    MakcuMouseInput(const std::string& port, unsigned int baudrate)
        : device_(std::make_unique<MakcuConnection>(port, baudrate))
    {
    }

    const char* name() const override { return "MAKCU"; }
    bool isOpen() const override { return device_ && device_->isOpen(); }
    bool move(int dx, int dy) override
    {
        if (!isOpen())
            return false;
        device_->move(dx, dy);
        return true;
    }
    bool leftDown() override
    {
        if (!isOpen())
            return false;
        device_->press(0);
        return true;
    }
    bool leftUp() override
    {
        if (!isOpen())
            return false;
        device_->release(0);
        return true;
    }
    bool hasPhysicalButtonState() const override { return true; }
    bool keyPressed(const std::string& keyName) override
    {
        return isOpen() &&
            logicalButtonPressed(keyName, shootingActive(), zoomingActive(), aimingActive());
    }
    bool aimingActive() const override { return device_ && device_->aiming_active; }
    bool shootingActive() const override { return device_ && device_->shooting_active; }
    bool zoomingActive() const override { return device_ && device_->zooming_active; }
    MakcuConnection* makcu() override { return device_.get(); }

private:
    std::unique_ptr<MakcuConnection> device_;       // 底层 Makcu 串口连接实例
};

}   // namespace

// ============================================================================
// ParseMouseInputMethod — 将字符串解析为鼠标输入方法枚举值
//
// 将配置文件或用户输入的字符串形式的鼠标输入方法名称
// 转换为 MouseInputMethod 枚举值。支持的大小写形式与
// MouseInputMethodName() 的输出完全一致。
//
// 支持的输入字符串：
//   "WIN32", "GHUB", "RAZER", "ARDUINO", "RP2350",
//   "TEENSY41", "TEENSY41_HID", "KMBOX_NET", "KMBOX_A", "MAKCU"
//
// 参数:
//   method - 鼠标输入方法名称字符串（全大写，不含空格）
//
// 返回:
//   匹配成功时返回对应的 MouseInputMethod 枚举值，
//   无匹配项时返回 std::nullopt
// ============================================================================
std::optional<MouseInputMethod> ParseMouseInputMethod(const std::string& method)
{
    if (method == "WIN32")
        return MouseInputMethod::Win32;
    if (method == "GHUB")
        return MouseInputMethod::GHub;
    if (method == "RAZER")
        return MouseInputMethod::Razer;
    if (method == "ARDUINO")
        return MouseInputMethod::Arduino;
    if (method == "RP2350")
        return MouseInputMethod::RP2350;
    if (method == "TEENSY41")
        return MouseInputMethod::Teensy41;
    if (method == "TEENSY41_HID")
        return MouseInputMethod::Teensy41Hid;
    if (method == "KMBOX_NET")
        return MouseInputMethod::KmboxNet;
    if (method == "KMBOX_A")
        return MouseInputMethod::KmboxA;
    if (method == "MAKCU")
        return MouseInputMethod::Makcu;
    return std::nullopt;
}

// ============================================================================
// MouseInputMethodName — 将鼠标输入方法枚举值转换为字符串名称
//
// 与 ParseMouseInputMethod() 互为逆操作，将枚举值转换回
// 全大写的字符串名称。主要用于日志输出、调试信息及配置序列化。
//
// 参数:
//   method - 鼠标输入方法枚举值
//
// 返回:
//   枚举值对应的字符串名称；Win32 作为默认返回值（switch 的 default 分支）
// ============================================================================
std::string MouseInputMethodName(MouseInputMethod method)
{
    switch (method)
    {
    case MouseInputMethod::GHub: return "GHUB";
    case MouseInputMethod::Razer: return "RAZER";
    case MouseInputMethod::Arduino: return "ARDUINO";
    case MouseInputMethod::RP2350: return "RP2350";
    case MouseInputMethod::Teensy41: return "TEENSY41";
    case MouseInputMethod::Teensy41Hid: return "TEENSY41_HID";
    case MouseInputMethod::KmboxNet: return "KMBOX_NET";
    case MouseInputMethod::KmboxA: return "KMBOX_A";
    case MouseInputMethod::Makcu: return "MAKCU";
    case MouseInputMethod::Win32:
    default:
        return "WIN32";
    }
}

// ============================================================================
// CreateMouseInputDevice — 鼠标输入设备工厂函数
//
// 根据 Config 对象中配置的输入方法（input_method），创建并返回
// 对应的 IMouseInput 子类实例。如果配置的字符串无法解析或未配置，
// 则默认创建 Win32MouseInput 作为保底方案。
//
// 调度逻辑:
//   1. 调用 ParseMouseInputMethod() 将字符串解析为枚举值
//   2. 根据枚举值在 switch 中创建对应的设备实例
//   3. 每种设备从 Config 中提取所需的特定参数
//      （串口、波特率、IP、UUID、PID:VID 等）
//   4. 未识别或默认分支回退到 Win32MouseInput
//
// 参数:
//   config - 应用配置对象，包含 input_method 字段及各类设备参数
//
// 返回:
//   新创建的 IMouseInput 子类实例的 unique_ptr
// ============================================================================
std::unique_ptr<IMouseInput> CreateMouseInputDevice(const Config& config)
{
    const MouseInputMethod method = ParseMouseInputMethod(config.input_method).value_or(MouseInputMethod::Win32);
    switch (method)
    {
    case MouseInputMethod::Arduino:
        return std::make_unique<ArduinoMouseInput>(
            config.arduino_port,
            static_cast<unsigned int>(config.arduino_baudrate),
            config.arduino_enable_keys);
    case MouseInputMethod::RP2350:
        return std::make_unique<RP2350MouseInput>(
            config.rp2350_port,
            static_cast<unsigned int>(config.rp2350_baudrate),
            config.rp2350_enable_keys);
    case MouseInputMethod::Teensy41:
        return std::make_unique<Teensy41MouseInput>(config.arduino_port, static_cast<unsigned int>(config.arduino_baudrate));
    case MouseInputMethod::Teensy41Hid:
        return std::make_unique<Teensy41RawHidMouseInput>(config);
    case MouseInputMethod::GHub:
        return std::make_unique<GHubMouseInput>();
    case MouseInputMethod::Razer:
        return std::make_unique<RazerMouseInput>();
    case MouseInputMethod::KmboxNet:
        return std::make_unique<KmboxNetMouseInput>(config.kmbox_net_ip, config.kmbox_net_port, config.kmbox_net_uuid);
    case MouseInputMethod::KmboxA:
        return std::make_unique<KmboxAMouseInput>(config.kmbox_a_pidvid);
    case MouseInputMethod::Makcu:
        return std::make_unique<MakcuMouseInput>(config.makcu_port, static_cast<unsigned int>(config.makcu_baudrate));
    case MouseInputMethod::Win32:
    default:
        return std::make_unique<Win32MouseInput>();
    }
}
