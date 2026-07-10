#ifndef GHUB_H
#define GHUB_H

#include <filesystem>
#include <windows.h>

/**
 * @brief 罗技 G Hub 鼠标控制类
 *
 * 通过加载罗技 G Hub SDK DLL 实现鼠标移动和按键操作。
 * 绕过 Windows 默认鼠标驱动，直接与 G Hub 通信。
 */
class GhubMouse
{
public:
    GhubMouse();
    ~GhubMouse();
    /** @brief 鼠标移动到指定绝对坐标 (x, y) */
    bool mouse_xy(int x, int y);
    /** @brief 按下鼠标按键（默认左键） */
    bool mouse_down(int key = 1);
    /** @brief 释放鼠标按键（默认左键） */
    bool mouse_up(int key = 1);
    /** @brief 关闭 G Hub 连接 */
    bool mouse_close();

private:
    std::filesystem::path basedir;  ///< G Hub 基础目录
    std::filesystem::path dlldir;   ///< G Hub DLL 目录
    HMODULE gm;                     ///< G Hub DLL 模块句柄
    bool gmok;                      ///< DLL 是否加载成功

    /** @brief 封装 SendInput 调用，通过 G Hub 发送输入 */
    static UINT _ghub_SendInput(UINT nInputs, LPINPUT pInputs);
    /** @brief 将 MOUSEINPUT 转换为 INPUT 结构 */
    static INPUT _ghub_Input(MOUSEINPUT mi);
    /** @brief 创建 MOUSEINPUT 结构 */
    static MOUSEINPUT _ghub_MouseInput(DWORD flags, LONG x, LONG y, DWORD data);
    /** @brief 创建鼠标 INPUT 结构 */
    static INPUT _ghub_Mouse(DWORD flags, LONG x = 0, LONG y = 0, DWORD data = 0);
};

#endif // GHUB_H