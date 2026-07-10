/**
 * @file ghub.cpp
 * @brief GhubMouse 类的实现文件
 *
 * 本文件实现了 GhubMouse 类，用于通过 Logitech G Hub 的 ghub_mouse.dll 动态链接库
 * 控制罗技游戏鼠标的移动、按键按下与释放等操作。
 * 该类同时提供了基于 Windows API (SendInput) 的回退方案，
 * 当无法加载 G Hub DLL 或获取相关函数地址失败时自动使用系统级输入模拟。
 */

#include <iostream>
#include <string>

#include "ghub.h"

/**
 * @brief 封装 Windows SendInput 函数，自动填充输入结构体大小
 *
 * 调用系统 SendInput API，将指定的输入事件插入到输入事件流中。
 * 此函数是对 SendInput 的简易封装，自动传入 sizeof(INPUT) 作为第三个参数。
 *
 * @param nInputs  输入事件的数量（pInputs 数组的元素个数）
 * @param pInputs  指向 INPUT 结构体数组的指针
 * @return UINT    成功发送的事件数量（应等于 nInputs）
 */
UINT GhubMouse::_ghub_SendInput(UINT nInputs, LPINPUT pInputs)
{
    return SendInput(nInputs, pInputs, sizeof(INPUT));
}

/**
 * @brief 根据 MOUSEINPUT 构造一个完整的 INPUT 结构体
 *
 * 将指定的鼠标输入数据（MOUSEINPUT）封装成类型为 INPUT_MOUSE 的 INPUT 结构体，
 * 供后续 SendInput 调用使用。
 *
 * @param mi  已填充好的 MOUSEINPUT 结构体，包含鼠标动作标志、坐标、滚轮数据等
 * @return INPUT  返回类型为 INPUT_MOUSE、mi 字段为给定参数的 INPUT 结构体
 */
INPUT GhubMouse::_ghub_Input(MOUSEINPUT mi)
{
    INPUT input = { 0 };
    input.type = INPUT_MOUSE;
    input.mi = mi;
    return input;
}

/**
 * @brief 构造 MOUSEINPUT 结构体，填充鼠标事件参数
 *
 * 根据传入的标志位、坐标和滚轮/按键数据初始化 MOUSEINPUT 结构体。
 * 此函数是构建鼠标输入的底层辅助方法。
 *
 * @param flags  MOUSEINPUT 标志位（如 MOUSEEVENTF_MOVE、MOUSEEVENTF_LEFTDOWN 等）
 * @param x      鼠标的 X 坐标偏移量（相对移动时的偏移量，或绝对坐标）
 * @param y      鼠标的 Y 坐标偏移量（相对移动时的偏移量，或绝对坐标）
 * @param data   鼠标滚轮滚动数据（WHEEL_DELTA）或 X 按键的附加信息
 * @return MOUSEINPUT  返回填充完毕的 MOUSEINPUT 结构体
 */
MOUSEINPUT GhubMouse::_ghub_MouseInput(DWORD flags, LONG x, LONG y, DWORD data)
{
    MOUSEINPUT mi = { 0 };
    mi.dx = x;
    mi.dy = y;
    mi.mouseData = data;
    mi.dwFlags = flags;
    return mi;
}

/**
 * @brief 便捷方法：由标志位、坐标和附加数据直接构造 INPUT 结构体
 *
 * 依次调用 _ghub_MouseInput 和 _ghub_Input，将原始鼠标参数一步转换为
 * 可供 SendInput 使用的 INPUT 结构体。
 *
 * @param flags  MOUSEINPUT 标志位
 * @param x      鼠标 X 坐标偏移量
 * @param y      鼠标 Y 坐标偏移量
 * @param data   滚轮或 X 按键数据
 * @return INPUT  返回可直接传入 _ghub_SendInput 的 INPUT 结构体
 */
INPUT GhubMouse::_ghub_Mouse(DWORD flags, LONG x, LONG y, DWORD data)
{
    return _ghub_Input(_ghub_MouseInput(flags, x, y, data));
}

/**
 * @brief 构造函数：加载 ghub_mouse.dll 并初始化鼠标连接
 *
 * 流程说明：
 * 1. 获取当前可执行文件所在目录作为 basedir；
 * 2. 拼接出 ghub_mouse.dll 的完整路径；
 * 3. 调用 LoadLibraryA 加载 DLL；
 *    - 若加载失败，打印错误信息并将 gmok 置为 false；
 * 4. 通过 GetProcAddress 获取 DLL 中 mouse_open 函数的地址；
 *    - 若获取失败，打印错误信息并将 gmok 置为 false；
 *    - 若获取成功，调用 mouse_open() 打开鼠标连接，结果存入 gmok。
 *
 * @note gmok 是类中各方法判断是否可使用 G Hub DLL 功能的关键标志。
 *       若 gmok 为 false，所有鼠标操作将回退到 Windows SendInput API。
 */
GhubMouse::GhubMouse()
{
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    basedir = std::filesystem::path(buffer).parent_path();
    dlldir = basedir / "ghub_mouse.dll";
    gm = LoadLibraryA(dlldir.string().c_str());
    if (gm == NULL)
    {
        std::cerr << "[Ghub] Failed to load DLL" << std::endl;
        gmok = false;
    }
    else
    {
        auto mouse_open = reinterpret_cast<bool(*)()>(GetProcAddress(gm, "mouse_open"));
        if (mouse_open == NULL)
        {
            std::cerr << "[Ghub] Failed to open mouse!" << std::endl;
            gmok = false;
        }
        else
        {
            gmok = mouse_open();
        }
    }
}

/**
 * @brief 析构函数：释放 ghub_mouse.dll
 *
 * 若 DLL 句柄（gm）不为空，调用 FreeLibrary 卸载已加载的 DLL，
 * 释放系统资源。
 */
GhubMouse::~GhubMouse()
{
    if (gm != NULL)
    {
        FreeLibrary(gm);
    }
}

/**
 * @brief 移动鼠标（相对偏移）
 *
 * 优先通过 G Hub DLL 的 moveR 函数实现鼠标相对移动。
 * 若 DLL 不可用或 moveR 函数地址获取失败，则回退到 SendInput API，
 * 使用 MOUSEEVENTF_MOVE 标志发送鼠标移动事件。
 *
 * @param x  X 方向的相对移动像素数（正数向右，负数向左）
 * @param y  Y 方向的相对移动像素数（正数向下，负数向上）
 * @return bool  操作成功返回 true，失败返回 false
 */
bool GhubMouse::mouse_xy(int x, int y)
{
    if (gmok)
    {
        auto moveR = reinterpret_cast<bool(*)(int, int)>(GetProcAddress(gm, "moveR"));
        if (moveR != NULL)
        {
            return moveR(x, y);
        }
    }
    INPUT input = _ghub_Mouse(MOUSEEVENTF_MOVE, x, y);
    return _ghub_SendInput(1, &input) == 1;
}

/**
 * @brief 按下鼠标按键（左键或右键）
 *
 * 优先通过 G Hub DLL 的 press 函数模拟按键按下。
 * 若 DLL 不可用或 press 函数地址获取失败，则回退到 SendInput API。
 * 根据 key 参数决定按下左键（MOUSEEVENTF_LEFTDOWN）还是右键（MOUSEEVENTF_RIGHTDOWN）。
 *
 * @param key  按键标识：1 表示左键，其他值表示右键
 * @return bool  操作成功返回 true，失败返回 false
 */
bool GhubMouse::mouse_down(int key)
{
    if (gmok)
    {
        auto press = reinterpret_cast<bool(*)(int)>(GetProcAddress(gm, "press"));
        if (press != NULL)
        {
            return press(key);
        }
    }
    DWORD flag = (key == 1) ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_RIGHTDOWN;
    INPUT input = _ghub_Mouse(flag);
    return _ghub_SendInput(1, &input) == 1;
}

/**
 * @brief 释放鼠标按键（左键或右键）
 *
 * 优先通过 G Hub DLL 的 release 函数模拟按键释放。
 * 若 DLL 不可用或 release 函数地址获取失败，则回退到 SendInput API。
 * 根据 key 参数决定释放左键（MOUSEEVENTF_LEFTUP）还是右键（MOUSEEVENTF_RIGHTUP）。
 *
 * @param key  按键标识：1 表示左键，其他值表示右键
 * @return bool  操作成功返回 true，失败返回 false
 */
bool GhubMouse::mouse_up(int key)
{
    if (gmok)
    {
        auto release = reinterpret_cast<bool(*)()>(GetProcAddress(gm, "release"));
        if (release != NULL)
        {
            return release();
        }
    }
    DWORD flag = (key == 1) ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_RIGHTUP;
    INPUT input = _ghub_Mouse(flag);
    return _ghub_SendInput(1, &input) == 1;
}

/**
 * @brief 关闭鼠标连接
 *
 * 优先通过 G Hub DLL 的 mouse_close 函数关闭与鼠标的连接。
 * 仅在 DLL 可用且 mouse_close 函数地址获取成功时执行。
 * 若 DLL 不可用或函数地址获取失败，直接返回 false。
 *
 * @return bool  关闭成功返回 true，失败或无法关闭返回 false
 */
bool GhubMouse::mouse_close()
{
    if (gmok)
    {
        auto mouse_close = reinterpret_cast<bool(*)()>(GetProcAddress(gm, "mouse_close"));
        if (mouse_close != NULL)
        {
            return mouse_close();
        }
    }
    return false;
}
