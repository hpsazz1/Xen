#ifndef MOUSE_INPUT_H
#define MOUSE_INPUT_H

#include <memory>
#include <optional>
#include <string>

class Config;
class GhubMouse;
class KmboxAConnection;
class KmboxNetConnection;
class MakcuConnection;
class RzctlMouse;

/**
 * @brief 鼠标输入方法枚举
 *
 * 定义程序支持的所有鼠标输入方式，包括：
 * Win32 API、罗技 G Hub、雷蛇、Kmbox Net/A 以及 Makcu。
 */
enum class MouseInputMethod
{
    Win32,    ///< Win32 API 鼠标输入
    GHub,     ///< 罗技 G Hub 鼠标输入
    Razer,    ///< 雷蛇鼠标输入
    KmboxNet, ///< Kmbox Net 网络鼠标控制
    KmboxA,   ///< Kmbox A 型鼠标控制
    Makcu     ///< Makcu 设备鼠标控制
};

/** @brief 将字符串解析为 MouseInputMethod 枚举值 */
std::optional<MouseInputMethod> ParseMouseInputMethod(const std::string& method);
/** @brief 将 MouseInputMethod 枚举值转换为字符串名称 */
std::string MouseInputMethodName(MouseInputMethod method);

/**
 * @brief 鼠标输入抽象接口
 *
 * 定义所有鼠标输入设备必须实现的纯虚接口。
 * 支持鼠标移动、按键操作以及物理按钮状态查询。
 */
class IMouseInput
{
public:
    virtual ~IMouseInput() = default;

    /** @brief 返回输入设备名称 */
    virtual const char* name() const = 0;
    /** @brief 设备是否已打开 */
    virtual bool isOpen() const = 0;
    virtual bool isReadyForMotion() const { return isOpen(); }
    /** @brief 相对移动鼠标 (dx, dy) */
    virtual bool move(int dx, int dy) = 0;
    /** @brief 按下左键 */
    virtual bool leftDown() = 0;
    /** @brief 释放左键 */
    virtual bool leftUp() = 0;
    /** @brief 是否支持物理按钮状态查询 */
    virtual bool hasPhysicalButtonState() const { return false; }
    /** @brief 指定按键是否被按下 */
    virtual bool keyPressed(const std::string& keyName) { (void)keyName; return false; }
    /** @brief 模拟按下按键（仅硬件设备支持，如 KMBOX） */
    virtual bool keyDown(int /*vkey*/) { return false; }
    /** @brief 模拟释放按键（仅硬件设备支持，如 KMBOX） */
    virtual bool keyUp(int /*vkey*/) { return false; }
    /** @brief 是否正在瞄准 */
    virtual bool aimingActive() const { return false; }
    /** @brief 是否正在射击 */
    virtual bool shootingActive() const { return false; }
    /** @brief 是否正在缩放（开镜） */
    virtual bool zoomingActive() const { return false; }

    /** @brief 获取 G Hub 鼠标指针（如适用） */
    virtual GhubMouse* ghub() { return nullptr; }
    /** @brief 获取雷蛇鼠标指针（如适用） */
    virtual RzctlMouse* razer() { return nullptr; }
    /** @brief 获取 Kmbox Net 连接指针（如适用） */
    virtual KmboxNetConnection* kmboxNet() { return nullptr; }
    /** @brief 获取 Kmbox A 连接指针（如适用） */
    virtual KmboxAConnection* kmboxA() { return nullptr; }
    /** @brief 获取 Makcu 连接指针（如适用） */
    virtual MakcuConnection* makcu() { return nullptr; }
};

/**
 * @brief 根据配置创建鼠标输入设备实例
 * @param config 程序配置对象
 * @return 鼠标输入设备唯一指针
 */
std::unique_ptr<IMouseInput> CreateMouseInputDevice(const Config& config);

#endif // MOUSE_INPUT_H
