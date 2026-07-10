/*==============================================================================
 * MakcuConnection — Makcu 游戏鼠标通信连接
 *
 * 本文件实现了 MakcuConnection 类，负责与 Makcu（马库）游戏鼠标设备进行
 * 双向通信。主要功能包括：
 *   - 通过 SDK 连接鼠标硬件（高速串口通信，默认 4,000,000 波特率）
 *   - 将鼠标物理按键映射为射击、瞄准、缩放等游戏状态
 *   - 提供鼠标移动、点击、按下、释放等操作接口
 *   - 高性能模式支持，保证低延迟通信
 *
 * 按钮映射方案（核心设计）：
 *   左键   → 射击 (shooting_active)
 *   右键   → 缩放 (zooming_active)
 *   侧键2  → 瞄准 (aiming_active)
 *   中键/侧键1 → 暂未使用
 *==============================================================================*/

#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <windows.h>
#include <iostream>

#include "Makcu.h"
#include "Xen.h"

//=====================================================================
// 构造函数
//
// 功能：建立与 Makcu 鼠标设备的连接，完成初始化配置
//
// 参数：
//   port      —— 串口名称（如 "COM3"），标识鼠标硬件所在的通信端口
//   baud_rate —— 用户配置的波特率；实际由 SDK 接管并以 4M 高速运行，
//                此参数仅用于校验和提示
//
// 实现流程：
//   1. 注册按钮回调函数，使 SDK 在检测到物理按键事件时通知本对象
//   2. 启用 SDK 的按钮监控线程
//   3. 尝试连接指定端口
//   4. 连接成功后启用设备的高性能模式
//   5. 检查波特率：若用户配置了非 4M 的波特率，给出友好提示
//      （SDK 内部已锁定为 4,000,000 baud，忽略用户配置值）
//   6. 将 is_open_ 置为 true，标记连接可用
//   7. 任何异常均捕获并输出错误信息，不导致程序崩溃
//=====================================================================
MakcuConnection::MakcuConnection(const std::string& port, unsigned int baud_rate)
    : is_open_(false)
    , aiming_active(false)
    , shooting_active(false)
    , zooming_active(false)
{
    try
    {
        // 注册 SDK 按钮回调：当鼠标物理按键状态变化时，
        // onButtonCallback 会被自动调用
        device_.setMouseButtonCallback([this](makcu::MouseButton button, bool pressed) {
            onButtonCallback(button, pressed);
        });

        // 启动 SDK 内置的按钮状态监控线程
        device_.enableButtonMonitoring(true);

        // 尝试连接鼠标硬件
        if (device_.connect(port))
        {
            // 启用高性能模式，降低通信延迟
            device_.enableHighPerformanceMode(true);

            // SDK 内部已固定为 4,000,000 baud 高速运行
            constexpr unsigned int sdkHighSpeedBaud = 4000000;
            if (baud_rate > 0 && baud_rate != sdkHighSpeedBaud)
            {
                std::cout << "[Makcu] 忽略已配置的波特率 " << baud_rate
                    << "；MAKCU SDK 连接已在 "
                    << sdkHighSpeedBaud << " 波特率下运行。" << std::endl;
            }

            is_open_ = true;
            std::cout << "[Makcu] 连接成功！端口: " << port << std::endl;
        }
        else
        {
            std::cerr << "[Makcu] 无法连接到端口: " << port << std::endl;
        }
    }
    catch (const makcu::MakcuException& e)
    {
        std::cerr << "[Makcu] 错误: " << e.what() << std::endl;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Makcu] 错误: " << e.what() << std::endl;
    }
}

//=====================================================================
// 析构函数
//
// 功能：安全断开与 Makcu 鼠标设备的连接，释放资源
//
// 实现流程：
//   1. 调用 SDK 的 disconnect() 关闭串口连接
//   2. 若 disconect() 抛出异常则静默吞掉（析构函数不应传播异常）
//   3. 将 is_open_ 置为 false
//=====================================================================
MakcuConnection::~MakcuConnection()
{
    try
    {
        device_.disconnect();
    }
    catch (...)
    {
        // 析构函数中吞掉所有异常，保证不会因断开连接而崩溃
    }
    is_open_ = false;
}

//=====================================================================
// isOpen
//
// 功能：检查与 Makcu 鼠标设备的通信连接是否仍处于打开状态
//
// 返回值：
//   true  —— 连接已建立且 SDK 报告设备仍在线
//   false —— 连接已关闭或设备已断开
//=====================================================================
bool MakcuConnection::isOpen() const
{
    return is_open_ && device_.isConnected();
}

//=====================================================================
// move
//
// 功能：向 Makcu 鼠标设备发送相对移动指令，控制光标位移
//
// 参数：
//   x —— X 轴方向移动量（正数向右，负数向左）
//   y —— Y 轴方向移动量（正数向下，负数向上）
//
// 说明：
//   受 write_mutex_ 保护，避免多线程同时写入导致数据竞争。
//   若操作过程中设备断开，将自动标记 is_open_ = false。
//=====================================================================
void MakcuConnection::move(int x, int y)
{
    if (!is_open_)
        return;

    std::lock_guard<std::mutex> lock(write_mutex_);
    try
    {
        device_.mouseMove(x, y);
    }
    catch (...)
    {
        is_open_ = false;
    }
}

//=====================================================================
// click
//
// 功能：在 Makcu 鼠标设备上模拟一次鼠标左键单击事件
//
// 参数：
//   button —— 兼容上层接口保留的参数，目前固定发送左键单击
//
// 说明：
//   click 操作 = mouseDown + mouseUp 的快速组合。
//   受 write_mutex_ 保护，线程安全。
//=====================================================================
void MakcuConnection::click(int button)
{
    if (!is_open_)
        return;

    std::lock_guard<std::mutex> lock(write_mutex_);
    try
    {
        device_.click(makcu::MouseButton::LEFT);
    }
    catch (...)
    {
        is_open_ = false;
    }
}

//=====================================================================
// press
//
// 功能：在 Makcu 鼠标设备上按下左键并保持（不释放）
//
// 参数：
//   button —— 兼容上层接口保留的参数，目前固定发送左键按下
//
// 说明：
//   通常与 release() 配对使用，用于模拟长按操作。
//   受 write_mutex_ 保护，线程安全。
//=====================================================================
void MakcuConnection::press(int button)
{
    if (!is_open_)
        return;

    std::lock_guard<std::mutex> lock(write_mutex_);
    try
    {
        device_.mouseDown(makcu::MouseButton::LEFT);
    }
    catch (...)
    {
        is_open_ = false;
    }
}

//=====================================================================
// release
//
// 功能：在 Makcu 鼠标设备上释放左键（结束一次按下操作）
//
// 参数：
//   button —— 兼容上层接口保留的参数，目前固定发送左键释放
//
// 说明：
//   通常与 press() 配对使用，mouseUp 指令通知硬件弹起按键。
//   受 write_mutex_ 保护，线程安全。
//=====================================================================
void MakcuConnection::release(int button)
{
    if (!is_open_)
        return;

    std::lock_guard<std::mutex> lock(write_mutex_);
    try
    {
        device_.mouseUp(makcu::MouseButton::LEFT);
    }
    catch (...)
    {
        is_open_ = false;
    }
}

//=====================================================================
// onButtonCallback
//
// 功能：SDK 按钮回调函数，由 setMouseButtonCallback 注册的 lambda
//        间接调用。当 Makcu 鼠标上任何物理按键被按下或释放时触发。
//
// 参数：
//   button —— 触发事件的鼠标按钮（枚举类型 makcu::MouseButton）
//   pressed —— true 表示按下，false 表示释放
//
// 核心映射逻辑（射击/瞄准/缩放状态切换）：
//
//   LEFT（左键）→ shooting_active
//     按下时设置为 true，释放时设为 false。
//     同时更新 atomic<bool> shooting 供上层无锁读取。
//
//   RIGHT（右键）→ zooming_active
//     按下时设置为 true，释放时设为 false。
//     同时更新 atomic<bool> zooming 供上层无锁读取。
//
//   SIDE2（侧键2 / 前进键，通常对应鼠标第5键）→ aiming_active
//     按下时设置为 true，释放时设为 false。
//     同时更新 atomic<bool> aiming 供上层无锁读取。
//
//   MIDDLE（中键）—— 保留未用
//     为未来扩展预留，目前不触发任何状态变化。
//
//   SIDE1（侧键1 / 后退键，通常对应鼠标第4键）—— 保留未用
//     为未来扩展预留，目前不触发任何状态变化。
//
// 注意：
//   本函数在主线程以外的 SDK 监控线程中被调用，因此对共享状态的
//   写入使用普通赋值（bool）和原子操作（std::atomic<bool>.store()），
//   保证线程安全。
//=====================================================================
void MakcuConnection::onButtonCallback(makcu::MouseButton button, bool pressed)
{
    switch (button)
    {
    case makcu::MouseButton::LEFT:
        // 左键 → 射击状态
        // 物理左键被按下 → 角色开始射击
        // 物理左键被释放 → 角色停止射击
        shooting_active = pressed;
        shooting.store(pressed);
        break;

    case makcu::MouseButton::RIGHT:
        // 右键 → 缩放状态
        // 物理右键被按下 → 进入瞄准镜/缩放模式
        // 物理右键被释放 → 退出瞄准镜/缩放模式
        zooming_active = pressed;
        zooming.store(pressed);
        break;

    case makcu::MouseButton::MIDDLE:
        // 中键 — 暂未使用
        // 当前的按键方案不需要中键，保留给将来扩展
        break;

    case makcu::MouseButton::SIDE1:
        // 侧键1（鼠标第4键/后退键）— 暂未使用
        // 保留给将来扩展
        break;

    case makcu::MouseButton::SIDE2:
        // 侧键2（鼠标第5键/前进键）→ 瞄准状态
        // 物理侧键2被按下 → 角色进入机械瞄准/腰射状态
        // 物理侧键2被释放 → 角色退出瞄准状态
        aiming_active = pressed;
        aiming.store(pressed);
        break;
    }
}
