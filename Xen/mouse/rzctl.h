#ifndef RZCTL_H
#define RZCTL_H

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <filesystem>
#include <windows.h>

/**
 * @brief 雷蛇（Razer）鼠标控制类
 *
 * 通过加载雷蛇 SDK DLL（rzctl）实现鼠标移动和按键操作。
 * 支持鼠标绝对移动、按键按下/释放以及键盘输入。
 */
class RzctlMouse
{
public:
    RzctlMouse();
    ~RzctlMouse();

    /** @brief 设备是否已打开 */
    bool isOpen() const { return rzctlOk; }
    /** @brief 鼠标移动到指定绝对坐标 (x, y) */
    bool mouse_xy(int x, int y);
    /** @brief 按下鼠标按键（默认左键） */
    bool mouse_down(int key = 1);
    /** @brief 释放鼠标按键（默认左键） */
    bool mouse_up(int key = 1);
    /** @brief 关闭雷蛇连接 */
    bool mouse_close();

private:
    // 函数指针类型定义
    using InitFn = bool (*)();                    ///< 初始化函数
    using MouseMoveFn = void (*)(int, int, bool); ///< 鼠标移动函数
    using MouseMoveStatusFn = BOOL (*)(int, int, BOOL); ///< 鼠标移动（带返回值）
    using MouseClickFn = void (*)(int);           ///< 鼠标点击函数
    using MouseClickStatusFn = BOOL (*)(int);     ///< 鼠标点击（带返回值）
    using KeyboardInputFn = void (*)(short, int);  ///< 键盘输入函数

    std::filesystem::path dllPath;  ///< rzctl DLL 路径
    HMODULE rzctl = nullptr;        ///< 模块句柄
    bool rzctlOk = false;           ///< 初始化是否成功

    InitFn init = nullptr;                     ///< 初始化函数指针
    MouseMoveFn mouseMove = nullptr;            ///< 鼠标移动函数指针
    MouseMoveStatusFn mouseMoveStatus = nullptr;///< 鼠标移动（状态）函数指针
    MouseClickFn mouseClick = nullptr;          ///< 鼠标点击函数指针
    MouseClickStatusFn mouseClickStatus = nullptr; ///< 鼠标点击（状态）函数指针
    KeyboardInputFn keyboardInput = nullptr;     ///< 键盘输入函数指针

    /** @brief 解析 DLL 文件路径 */
    static std::filesystem::path resolveDllPath();
    /** @brief 获取按键按下标志 */
    static int downFlagForKey(int key);
    /** @brief 获取按键释放标志 */
    static int upFlagForKey(int key);
};

#endif // RZCTL_H
