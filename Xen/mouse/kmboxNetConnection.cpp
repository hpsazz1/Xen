#include <iostream>
#include <thread>
#include <chrono>

#include "kmbox_net/kmboxNet.h"
#include "KmboxNetConnection.h"

/**
 * @brief KmboxNetConnection 构造函数
 *
 * KmboxNetConnection 是用于与 Kmbox Net 硬件设备进行网络通信的封装类。
 * 构造函数负责初始化网络连接，建立与 Kmbox Net 设备的 TCP 连接，
 * 并启动后台监听线程以捕获物理鼠标/键盘的按键事件。
 *
 * @param ip   Kmbox Net 设备的 IP 地址（字符串形式）
 * @param port Kmbox Net 设备的端口号（字符串形式）
 * @param uuid Kmbox Net 设备的 UUID 标识符（用于设备鉴权）
 *
 * 实现逻辑：
 * 1. 将传入的 ip、port、uuid 保存到成员变量中
 * 2. 调用 kmNet_init() 建立与设备的网络连接
 * 3. 根据返回值判断连接是否成功（ret == 0 表示成功）
 * 4. 若连接失败，则输出错误信息并提前返回
 * 5. 初始化鼠标状态标志（瞄准、射击、缩放均为未激活）
 * 6. 停止旧的监听线程（如果存在）
 * 7. 创建并启动新的后台监听线程，用于持续接收设备事件
 */
KmboxNetConnection::KmboxNetConnection(const std::string& ip, const std::string& port, const std::string& uuid)
    : ip_(ip), port_(port), uuid_(uuid)
{
    int ret = kmNet_init((char*)ip.c_str(), (char*)port.c_str(), (char*)uuid.c_str());
    is_open_ = (ret == 0);
    if (!is_open_)
    {
        std::cerr << "[KmboxNet] 连接失败，返回值 ret=" << ret << std::endl;
        return;
    }

    aiming_active = false;   // 瞄准状态：初始为未激活
    shooting_active = false; // 射击状态：初始为未激活
    zooming_active = false;  // 缩放状态：初始为未激活

    monitor_ = false;           // 监听标志：初始为关闭
    if (monitor_thread_.joinable())
        monitor_thread_.join(); // 确保旧线程已结束

    monitor_running_ = true;                                        // 设置监听线程运行标志
    monitor_thread_ = std::thread(&KmboxNetConnection::monitorThread, this); // 启动监听线程
}

/**
 * @brief 后台监听线程函数
 *
 * 该函数在独立的线程中运行，负责调用 kmNet_monitor() 启动设备的监听模式。
 * 监听模式允许设备捕获用户物理操作（如物理鼠标点击、物理键盘按键），
 * 并将这些事件上报给上层逻辑，以便程序能够感知用户的真实输入。
 *
 * 实现逻辑：
 * 1. 调用 kmNet_monitor(10000) 启动监听，超时时间为 10000 毫秒
 * 2. 若启动失败（返回值非 0），输出错误信息并退出线程
 * 3. 若启动成功，进入循环，每次休眠 1 毫秒以降低 CPU 占用
 * 4. 当 monitor_running_ 标志被置为 false 时退出循环
 * 5. 捕获所有可能抛出的异常，避免线程因未处理异常而崩溃
 */
void KmboxNetConnection::monitorThread()
{
    try
    {
        int ret = kmNet_monitor(10000); // 启动监听，超时时间 10 秒
        if (ret != 0)
        {
            std::cerr << "[KmboxNet] 监听启动失败，返回值 ret=" << ret << std::endl;
            return;
        }

        while (monitor_running_)
            std::this_thread::sleep_for(std::chrono::milliseconds(1)); // 每秒轮询 1000 次，保持线程响应
    }
    catch (const std::exception& e)
    {
        std::cerr << "[KmboxNet] 监听线程崩溃，异常信息: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[KmboxNet] 监听线程崩溃，未知异常。" << std::endl;
    }
}

/**
 * @brief 析构函数
 *
 * 负责清理资源，包括：停止监听线程、关闭设备监听模式、断开与设备的网络连接。
 *
 * 实现逻辑：
 * 1. 将 monitor_running_ 置为 false，通知监听线程退出
 * 2. 等待监听线程结束（join）
 * 3. 如果连接仍处于打开状态：
 *    a. 调用 kmNet_monitor(0) 停止设备监听模式（传入 0 表示关闭）
 *    b. 调用 kmNet_close() 关闭与设备的网络连接
 */
KmboxNetConnection::~KmboxNetConnection()
{
    monitor_running_ = false;             // 通知监听线程退出
    if (monitor_thread_.joinable())
        monitor_thread_.join();           // 等待监听线程结束
    if (is_open_)
    {
        kmNet_monitor(0);  // 停止设备监听（参数 0 表示关闭监听）
        kmNet_close();      // 关闭网络连接
    }
}

/**
 * @brief 鼠标绝对移动（对应 kmNet_mouse_move）
 *
 * 将鼠标光标移动到屏幕上的指定坐标位置。
 * 该方法是绝对移动，坐标值对应屏幕的绝对坐标系。
 *
 * @param x 目标位置的 X 坐标（short 类型，范围取决于设备）
 * @param y 目标位置的 Y 坐标（short 类型，范围取决于设备）
 *
 * 内部调用 kmNet_mouse_move()，将 int 参数强制转换为 short 后传递给底层 SDK。
 */
void KmboxNetConnection::move(int x, int y)
{
    if (!is_open_) return;
    kmNet_mouse_move((short)x, (short)y);
}

/**
 * @brief 鼠标自动移动（对应 kmNet_mouse_move_auto）
 *
 * 将鼠标光标在指定的时间内平滑移动到目标位置。
 * 与 move() 不同，该方法的移动过程是渐变的，适合模拟人类操作。
 *
 * @param x  目标位置的 X 坐标
 * @param y  目标位置的 Y 坐标
 * @param ms 移动耗时，单位毫秒（ms）
 *
 * 内部调用 kmNet_mouse_move_auto()，由设备固件控制移动的插值计算。
 */
void KmboxNetConnection::moveAuto(int x, int y, int ms)
{
    if (!is_open_) return;
    kmNet_mouse_move_auto(x, y, ms);
}

/**
 * @brief 鼠标贝塞尔曲线移动（对应 kmNet_mouse_move_beizer）
 *
 * 沿贝塞尔曲线路径将鼠标光标从当前位置平滑移动到目标位置。
 * 通过两个控制点（x1,y1）和（x2,y2）定义曲线的形状，
 * 实现更自然的鼠标移动轨迹，常用于规避检测。
 *
 * @param x  目标位置的 X 坐标
 * @param y  目标位置的 Y 坐标
 * @param ms 移动耗时，单位毫秒（ms）
 * @param x1 贝塞尔曲线第一个控制点的 X 坐标
 * @param y1 贝塞尔曲线第一个控制点的 Y 坐标
 * @param x2 贝塞尔曲线第二个控制点的 X 坐标
 * @param y2 贝塞尔曲线第二个控制点的 Y 坐标
 *
 * 内部调用 kmNet_mouse_move_beizer()，设备固件负责路径插值计算。
 * 注意：函数名 "beizer" 是底层 SDK 的拼写，应为 "bezier" 的变体。
 */
void KmboxNetConnection::moveBezier(int x, int y, int ms, int x1, int y1, int x2, int y2)
{
    if (!is_open_) return;
    kmNet_mouse_move_beizer(x, y, ms, x1, y1, x2, y2);
}

/**
 * @brief 鼠标左键按下（对应 kmNet_mouse_left(1)）
 *
 * 模拟按下鼠标左键。参数传 1 表示按下操作。
 */
void KmboxNetConnection::leftDown()
{
    if (!is_open_) return;
    kmNet_mouse_left(1);
}

/**
 * @brief 鼠标左键弹起（对应 kmNet_mouse_left(0)）
 *
 * 模拟释放鼠标左键。参数传 0 表示弹起操作。
 */
void KmboxNetConnection::leftUp()
{
    if (!is_open_) return;
    kmNet_mouse_left(0);
}

/**
 * @brief 鼠标右键按下（对应 kmNet_mouse_right(1)）
 *
 * 模拟按下鼠标右键。参数传 1 表示按下操作。
 */
void KmboxNetConnection::rightDown()
{
    if (!is_open_) return;
    kmNet_mouse_right(1);
}

/**
 * @brief 鼠标右键弹起（对应 kmNet_mouse_right(0)）
 *
 * 模拟释放鼠标右键。参数传 0 表示弹起操作。
 */
void KmboxNetConnection::rightUp()
{
    if (!is_open_) return;
    kmNet_mouse_right(0);
}

/**
 * @brief 鼠标中键按下（对应 kmNet_mouse_middle(1)）
 *
 * 模拟按下鼠标中键（滚轮键）。参数传 1 表示按下操作。
 */
void KmboxNetConnection::middleDown()
{
    if (!is_open_) return;
    kmNet_mouse_middle(1);
}

/**
 * @brief 鼠标中键弹起（对应 kmNet_mouse_middle(0)）
 *
 * 模拟释放鼠标中键（滚轮键）。参数传 0 表示弹起操作。
 */
void KmboxNetConnection::middleUp()
{
    if (!is_open_) return;
    kmNet_mouse_middle(0);
}

/**
 * @brief 鼠标滚轮滚动（对应 kmNet_mouse_wheel）
 *
 * 模拟滚动鼠标滚轮。正数表示向前滚动（向上），负数表示向后滚动（向下）。
 *
 * @param wheel 滚轮滚动量。正值为向上滚动，负值为向下滚动。
 *              具体步进值由设备固件决定。
 */
void KmboxNetConnection::wheel(int wheel)
{
    if (!is_open_) return;
    kmNet_mouse_wheel(wheel);
}

/**
 * @brief 鼠标综合操作（对应 kmNet_mouse_all）
 *
 * 在一次调用中同时设置鼠标的按键状态、坐标位置和滚轮值。
 * 此方法可以原子化地执行多个鼠标操作，减少网络通信次数。
 *
 * @param button 鼠标按键状态掩码（不同位表示不同按键的按下/弹起状态）
 * @param x      鼠标 X 坐标
 * @param y      鼠标 Y 坐标
 * @param wheel  滚轮滚动量
 *
 * 内部调用 kmNet_mouse_all()，将多个操作合并为一个数据包发送给设备。
 */
void KmboxNetConnection::mouseAll(int button, int x, int y, int wheel)
{
    if (!is_open_) return;
    kmNet_mouse_all(button, x, y, wheel);
}

/**
 * @brief 键盘按键按下（对应 kmNet_keydown）
 *
 * 模拟按下指定的虚拟键码（Virtual Key Code）对应的键盘按键。
 *
 * @param vkey 虚拟键码（如 VK_SPACE、VK_RETURN 等 Windows 虚拟键码）
 *
 * 内部调用 kmNet_keydown()，将按键事件发送至设备执行。
 */
void KmboxNetConnection::keyDown(int vkey)
{
    if (!is_open_) return;
    kmNet_keydown(vkey);
}

/**
 * @brief 键盘按键弹起（对应 kmNet_keyup）
 *
 * 模拟释放指定的虚拟键码（Virtual Key Code）对应的键盘按键。
 *
 * @param vkey 虚拟键码（与 keyDown 中的值对应）
 *
 * 内部调用 kmNet_keyup()，将释放事件发送至设备执行。
 */
void KmboxNetConnection::keyUp(int vkey)
{
    if (!is_open_) return;
    kmNet_keyup(vkey);
}

/**
 * @brief 设置监听端口（对应 kmNet_monitor）
 *
 * 启动或停止对指定端口的物理设备事件监听。
 * 此方法可独立设置监听端口号，与构造函数中启动的默认监听不同。
 *
 * @param port 端口号。传 0 表示停止监听，非零值表示在该端口上启动监听。
 *
 * 内部调用 kmNet_monitor()。注意：此方法名与 monitorThread 中的 kmNet_monitor(10000) 功能相同，
 * 但在此处端口号由调用者动态指定。
 */
void KmboxNetConnection::monitor(short port)
{
    if (!is_open_) return;
    kmNet_monitor(port);
}

/**
 * @brief 监听鼠标左键状态（对应 kmNet_monitor_mouse_left）
 *
 * 查询物理鼠标左键当前是否处于按下状态。
 * 此方法仅在监听模式启动后有效。
 *
 * @return int 返回 1 表示左键被按下，返回 0 表示左键已释放，
 *             返回 -1 表示连接未打开或查询失败。
 *
 * 内部调用 kmNet_monitor_mouse_left() 获取设备上报的状态。
 */
int KmboxNetConnection::monitorMouseLeft()
{
    if (!is_open_) return -1;
    return kmNet_monitor_mouse_left();
}

/**
 * @brief 监听鼠标右键状态（对应 kmNet_monitor_mouse_right）
 *
 * 查询物理鼠标右键当前是否处于按下状态。
 *
 * @return int 返回 1 表示右键被按下，返回 0 表示右键已释放，
 *             返回 -1 表示连接未打开或查询失败。
 */
int KmboxNetConnection::monitorMouseRight()
{
    if (!is_open_) return -1;
    return kmNet_monitor_mouse_right();
}

/**
 * @brief 监听鼠标中键状态（对应 kmNet_monitor_mouse_middle）
 *
 * 查询物理鼠标中键当前是否处于按下状态。
 *
 * @return int 返回 1 表示中键被按下，返回 0 表示中键已释放，
 *             返回 -1 表示连接未打开或查询失败。
 */
int KmboxNetConnection::monitorMouseMiddle()
{
    if (!is_open_) return -1;
    return kmNet_monitor_mouse_middle();
}

/**
 * @brief 监听鼠标侧键1状态（对应 kmNet_monitor_mouse_side1）
 *
 * 查询物理鼠标侧面第一个按键（通常为前进键）当前是否处于按下状态。
 *
 * @return int 返回 1 表示侧键1被按下，返回 0 表示侧键1已释放，
 *             返回 -1 表示连接未打开或查询失败。
 */
int KmboxNetConnection::monitorMouseSide1()
{
    if (!is_open_) return -1;
    return kmNet_monitor_mouse_side1();
}

/**
 * @brief 监听鼠标侧键2状态（对应 kmNet_monitor_mouse_side2）
 *
 * 查询物理鼠标侧面第二个按键（通常为后退键）当前是否处于按下状态。
 *
 * @return int 返回 1 表示侧键2被按下，返回 0 表示侧键2已释放，
 *             返回 -1 表示连接未打开或查询失败。
 */
int KmboxNetConnection::monitorMouseSide2()
{
    if (!is_open_) return -1;
    return kmNet_monitor_mouse_side2();
}

/**
 * @brief 监听指定虚拟键码的键盘按键状态（对应 kmNet_monitor_keyboard）
 *
 * 查询物理键盘上指定虚拟键码对应的按键当前是否处于按下状态。
 *
 * @param vkey 要查询的虚拟键码（如 VK_SHIFT、VK_CONTROL 等）
 *
 * @return int 返回 1 表示按键被按下，返回 0 表示按键已释放，
 *             返回 -1 表示连接未打开或查询失败。
 *
 * 内部调用 kmNet_monitor_keyboard()，由设备固件捕获真实的物理按键状态。
 */
int KmboxNetConnection::monitorKeyboard(short vkey)
{
    if (!is_open_) return -1;
    return kmNet_monitor_keyboard(vkey);
}

/**
 * @brief 屏蔽鼠标左键输入（对应 kmNet_mask_mouse_left）
 *
 * 启用或禁用对物理鼠标左键事件的屏蔽。
 * 屏蔽后，物理鼠标左键的操作不会被系统处理，但设备仍能监听其状态。
 * 常用于防止用户的物理操作干扰自动化脚本。
 *
 * @param enable true 表示屏蔽左键，false 表示取消屏蔽
 */
void KmboxNetConnection::maskMouseLeft(bool enable)
{
    if (!is_open_) return;
    kmNet_mask_mouse_left(enable ? 1 : 0);
}

/**
 * @brief 屏蔽鼠标右键输入（对应 kmNet_mask_mouse_right）
 *
 * 启用或禁用对物理鼠标右键事件的屏蔽。
 *
 * @param enable true 表示屏蔽右键，false 表示取消屏蔽
 */
void KmboxNetConnection::maskMouseRight(bool enable)
{
    if (!is_open_) return;
    kmNet_mask_mouse_right(enable ? 1 : 0);
}

/**
 * @brief 屏蔽鼠标中键输入（对应 kmNet_mask_mouse_middle）
 *
 * 启用或禁用对物理鼠标中键事件的屏蔽。
 *
 * @param enable true 表示屏蔽中键，false 表示取消屏蔽
 */
void KmboxNetConnection::maskMouseMiddle(bool enable)
{
    if (!is_open_) return;
    kmNet_mask_mouse_middle(enable ? 1 : 0);
}

/**
 * @brief 屏蔽鼠标侧键1输入（对应 kmNet_mask_mouse_side1）
 *
 * 启用或禁用对物理鼠标侧键1事件的屏蔽。
 *
 * @param enable true 表示屏蔽侧键1，false 表示取消屏蔽
 */
void KmboxNetConnection::maskMouseSide1(bool enable)
{
    if (!is_open_) return;
    kmNet_mask_mouse_side1(enable ? 1 : 0);
}

/**
 * @brief 屏蔽鼠标侧键2输入（对应 kmNet_mask_mouse_side2）
 *
 * 启用或禁用对物理鼠标侧键2事件的屏蔽。
 *
 * @param enable true 表示屏蔽侧键2，false 表示取消屏蔽
 */
void KmboxNetConnection::maskMouseSide2(bool enable)
{
    if (!is_open_) return;
    kmNet_mask_mouse_side2(enable ? 1 : 0);
}

/**
 * @brief 屏蔽鼠标 X 轴移动（对应 kmNet_mask_mouse_x）
 *
 * 启用或禁用物理鼠标在 X 轴方向上的移动。
 * 屏蔽后，用户左右移动物理鼠标不会影响光标位置。
 *
 * @param enable true 表示屏蔽 X 轴移动，false 表示取消屏蔽
 */
void KmboxNetConnection::maskMouseX(bool enable)
{
    if (!is_open_) return;
    kmNet_mask_mouse_x(enable ? 1 : 0);
}

/**
 * @brief 屏蔽鼠标 Y 轴移动（对应 kmNet_mask_mouse_y）
 *
 * 启用或禁用物理鼠标在 Y 轴方向上的移动。
 * 屏蔽后，用户上下移动物理鼠标不会影响光标位置。
 *
 * @param enable true 表示屏蔽 Y 轴移动，false 表示取消屏蔽
 */
void KmboxNetConnection::maskMouseY(bool enable)
{
    if (!is_open_) return;
    kmNet_mask_mouse_y(enable ? 1 : 0);
}

/**
 * @brief 屏蔽鼠标滚轮输入（对应 kmNet_mask_mouse_wheel）
 *
 * 启用或禁用对物理鼠标滚轮事件的屏蔽。
 * 屏蔽后，用户滚动滚轮不会产生滚动效果。
 *
 * @param enable true 表示屏蔽滚轮，false 表示取消屏蔽
 */
void KmboxNetConnection::maskMouseWheel(bool enable)
{
    if (!is_open_) return;
    kmNet_mask_mouse_wheel(enable ? 1 : 0);
}

/**
 * @brief 屏蔽指定键盘按键（对应 kmNet_mask_keyboard）
 *
 * 屏蔽物理键盘上指定虚拟键码对应的按键。
 * 屏蔽后，用户按下该按键不会产生输入效果。
 *
 * @param vkey 要屏蔽的虚拟键码
 */
void KmboxNetConnection::maskKeyboard(short vkey)
{
    if (!is_open_) return;
    kmNet_mask_keyboard(vkey);
}

/**
 * @brief 取消屏蔽指定键盘按键（对应 kmNet_unmask_keyboard）
 *
 * 恢复被 maskKeyboard() 屏蔽的键盘按键，使其能够正常输入。
 *
 * @param vkey 要取消屏蔽的虚拟键码
 */
void KmboxNetConnection::unmaskKeyboard(short vkey)
{
    if (!is_open_) return;
    kmNet_unmask_keyboard(vkey);
}

/**
 * @brief 取消所有屏蔽（对应 kmNet_unmask_all）
 *
 * 恢复所有被屏蔽的鼠标和键盘输入，包括：
 * - 鼠标各按键（左键、右键、中键、侧键1、侧键2）
 * - 鼠标移动（X轴、Y轴）
 * - 鼠标滚轮
 * - 所有被屏蔽的键盘按键
 */
void KmboxNetConnection::unmaskAll()
{
    if (!is_open_) return;
    kmNet_unmask_all();
}

/**
 * @brief 重启 Kmbox Net 设备（对应 kmNet_reboot）
 *
 * 向 Kmbox Net 设备发送重启命令。
 * 重启后网络连接会断开，需要重新调用构造函数或重新建立连接。
 */
void KmboxNetConnection::reboot()
{
    if (!is_open_) return;
    kmNet_reboot();
}

/**
 * @brief 配置设备网络参数（对应 kmNet_setconfig）
 *
 * 动态修改 Kmbox Net 设备的 IP 地址和端口配置。
 * 注意：此方法修改的是设备的网络参数，并非当前连接对象的连接参数。
 *
 * @param ip   新的 IP 地址字符串
 * @param port 新的端口号（unsigned short）
 */
void KmboxNetConnection::setConfig(const std::string& ip, unsigned short port)
{
    if (!is_open_) return;
    kmNet_setconfig((char*)ip.c_str(), port);
}

/**
 * @brief 调试模式控制（对应 kmNet_debug）
 *
 * 启用或禁用 Kmbox Net 设备的调试模式。
 * 调试模式下设备可能输出更多的日志信息，用于故障排查。
 *
 * @param port   调试端口号
 * @param enable 调试模式开关：1 表示启用，0 表示禁用
 */
void KmboxNetConnection::debug(short port, char enable)
{
    if (!is_open_) return;
    kmNet_debug(port, enable);
}

/**
 * @brief 设置 LCD 屏幕背景颜色（对应 kmNet_lcd_color）
 *
 * 设置 Kmbox Net 设备上的 LCD 显示屏的背景颜色。
 * 颜色格式为 RGB565（16位颜色空间）。
 *
 * @param rgb565 RGB565 格式的颜色值。
 *               RGB565 中，红色占高 5 位，绿色占中间 6 位，蓝色占低 5 位。
 *               例如：0xF800 表示纯红色，0x07E0 表示纯绿色，0x001F 表示纯蓝色。
 */
void KmboxNetConnection::lcdColor(unsigned short rgb565)
{
    if (!is_open_) return;
    kmNet_lcd_color(rgb565);
}

/**
 * @brief 在 LCD 屏幕底部区域显示图片（对应 kmNet_lcd_picture_bottom）
 *
 * 在 Kmbox Net 设备 LCD 屏幕的下半部分（底部）显示一张自定义图片。
 *
 * @param buff_128_80 图片数据缓冲区指针。图片分辨率为 128x80 像素。
 *                    数据格式为原始像素数据，具体格式依赖设备固件定义。
 *
 * 内部调用 kmNet_lcd_picture_bottom()，将缓冲区数据发送至设备显示。
 */
void KmboxNetConnection::lcdPictureBottom(unsigned char* buff_128_80)
{
    if (!is_open_) return;
    kmNet_lcd_picture_bottom(buff_128_80);
}

/**
 * @brief 在 LCD 屏幕全屏显示图片（对应 kmNet_lcd_picture）
 *
 * 在 Kmbox Net 设备的 LCD 屏幕上全屏显示一张自定义图片。
 *
 * @param buff_128_160 图片数据缓冲区指针。图片分辨率为 128x160 像素（全屏尺寸）。
 *                     数据格式为原始像素数据，具体格式依赖设备固件定义。
 *
 * 内部调用 kmNet_lcd_picture()，将缓冲区数据发送至设备全屏显示。
 */
void KmboxNetConnection::lcdPicture(unsigned char* buff_128_160)
{
    if (!is_open_) return;
    kmNet_lcd_picture(buff_128_160);
}
