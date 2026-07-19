// ============================================================================
// MouseInput.cpp — 统一鼠标输入抽象层
//
// 本文件实现了针对所有受支持的鼠标硬件设备的 IMouseInput 接口，
// 涵盖以下设备类型：
//   - Win32      : Windows 原生 SendInput API（默认方案）
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
#include <unordered_map>

#include "KmboxAConnection.h"
#include "KmboxNetConnection.h"
#include "Makcu.h"
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
// SerialMouseInputBase — 串口/硬件设备包装器模板基类
//
// 统一处理 isOpen/move/leftDown/leftUp 转发，子类只需定义 name()、
// 构造函数和特定于设备的访问器方法。
// 适用于 KmboxA 等具有统一 press()/release()/move() 接口的设备。
// Makcu 使用 press(0)/release(0) 需覆写 leftDown/leftUp。
// ============================================================================
template <typename Device>
class SerialMouseInputBase : public IMouseInput
{
public:
    bool isOpen() const override { return device_ && device_->isOpen(); }
    bool move(int dx, int dy) override
    {
        if (!isOpen()) return false;
        device_->move(dx, dy);
        return true;
    }
    Device* devicePtr() { return device_.get(); }

protected:
    std::unique_ptr<Device> device_;
};

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
    bool isReadyForMotion() const override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->device && state_->device->isReadyForMotion();
    }
    bool move(int dx, int dy) override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->device || !state_->device->isOpen())
            return false;
        return state_->device->move(dx, dy);
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
    // NOTE: 返回的原始指针仅在 inputDevicesMutex 持锁期间有效。
    // 外部全局变量 (kmboxNetSerial 等) 在 Xen.cpp 中通过锁保护赋值，
    // 但调用者必须确保设备生命周期内指针有效。
    KmboxNetConnection* kmboxNet() override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->device.get();
    }
    bool keyDown(int vkey) override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->device) return false;
        state_->device->keyDown(vkey);
        return true;
    }
    bool keyUp(int vkey) override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->device) return false;
        state_->device->keyUp(vkey);
        return true;
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
// ============================================================================
class KmboxAMouseInput final : public SerialMouseInputBase<KmboxAConnection>
{
public:
    explicit KmboxAMouseInput(const std::string& pidvid)
    {
        device_ = std::make_unique<KmboxAConnection>(pidvid);
    }

    const char* name() const override { return "KMBOX_A"; }
    // KmboxA 使用 leftDown/leftUp 而非 press/release
    bool leftDown() override { if (!isOpen()) return false; device_->leftDown(); return true; }
    bool leftUp() override { if (!isOpen()) return false; device_->leftUp(); return true; }
    KmboxAConnection* kmboxA() override { return device_.get(); }
};

// ============================================================================
// MakcuMouseInput — Makcu 串口鼠标输入实现
// ============================================================================
class MakcuMouseInput final : public SerialMouseInputBase<MakcuConnection>
{
public:
    MakcuMouseInput(const std::string& port, unsigned int baudrate)
    {
        device_ = std::make_unique<MakcuConnection>(port, baudrate);
    }

    const char* name() const override { return "MAKCU"; }
    // Makcu 使用 press(index)/release(index)，索引 0 = 左键
    bool leftDown() override { if (!isOpen()) return false; device_->press(0); return true; }
    bool leftUp() override { if (!isOpen()) return false; device_->release(0); return true; }
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
//   "WIN32", "GHUB", "RAZER", "KMBOX_NET", "KMBOX_A", "MAKCU"
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
    static const std::unordered_map<std::string, MouseInputMethod> kMap = {
        {"WIN32",     MouseInputMethod::Win32},
        {"GHUB",      MouseInputMethod::GHub},
        {"RAZER",     MouseInputMethod::Razer},
        {"KMBOX_NET", MouseInputMethod::KmboxNet},
        {"KMBOX_A",   MouseInputMethod::KmboxA},
        {"MAKCU",     MouseInputMethod::Makcu},
    };
    auto it = kMap.find(method);
    return it != kMap.end() ? std::optional{it->second} : std::nullopt;
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
    case MouseInputMethod::KmboxNet: return "KMBOX_NET";
    case MouseInputMethod::KmboxA: return "KMBOX_A";
    case MouseInputMethod::Makcu: return "MAKCU";
    case MouseInputMethod::Win32: return "WIN32";
    }
    return "WIN32";  // 编译期可达性保障
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
