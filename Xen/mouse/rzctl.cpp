/**
 * rzctl.cpp — Razer 鼠标控制接口实现
 *
 * 本文件实现了 RzctlMouse 类，通过动态加载 rzctl.dll 来模拟 Razer 鼠标的
 * 移动和点击操作。支持左键、右键、中键的按下与释放，以及鼠标的绝对/相对移动。
 *
 * 设计要点：
 * - 动态加载 rzctl.dll，避免编译时依赖
 * - 自动搜索 DLL 可能的位置（程序目录、controls 子目录、当前工作目录）
 * - 兼容两套 API 接口（旧版 mouseMove/mouseClick 与新版 mouseMoveStatus/mouseClickStatus）
 */
#include "rzctl.h"

#include <iostream>
#include <string>
#include <vector>

/**
 * 匿名命名空间：Razer 鼠标按钮标志常量
 *
 * rzctl.dll 底层使用位标志来表示鼠标按钮事件。
 * 每个按钮有独立的按下（DOWN）和释放（UP）标志，
 * 组合使用时可以通过按位或（|）来同时表达多个按钮状态。
 *
 * 数值设计采用 2 的幂次（1, 2, 4, 8, 16, 32），
 * 确保各标志在位运算中互不干扰。
 */
namespace
{
    /** 左键按下标志 */
    constexpr int RZ_LEFT_DOWN = 1;
    /** 左键释放标志 */
    constexpr int RZ_LEFT_UP = 2;
    /** 右键按下标志 */
    constexpr int RZ_RIGHT_DOWN = 4;
    /** 右键释放标志 */
    constexpr int RZ_RIGHT_UP = 8;
    /** 中键按下标志 */
    constexpr int RZ_MIDDLE_DOWN = 16;
    /** 中键释放标志 */
    constexpr int RZ_MIDDLE_UP = 32;
}

/**
 * 解析 rzctl.dll 的完整路径
 *
 * 在运行时动态查找 DLL，搜索顺序如下：
 *   1. 可执行文件所在目录 / rzctl.dll
 *   2. 可执行文件所在目录 / controls / rzctl.dll
 *   3. 当前工作目录 / rzctl.dll
 *   4. 当前工作目录 / controls / rzctl.dll
 *
 * 如果以上均不存在，则返回可执行文件目录下的 rzctl.dll（作为默认路径）；
 * 若可执行文件目录信息获取失败，则直接返回 "rzctl.dll"。
 *
 * @return DLL 的完整路径（std::filesystem::path）
 */
std::filesystem::path RzctlMouse::resolveDllPath()
{
    wchar_t buffer[MAX_PATH]{};

    // exeDir：当前可执行文件所在的目录，用于作为 DLL 搜索起始点
    std::filesystem::path exeDir;
    if (GetModuleFileNameW(nullptr, buffer, MAX_PATH) > 0)
        exeDir = std::filesystem::path(buffer).parent_path();

    // cwd：进程当前工作目录，作为备选搜索路径
    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);

    // candidates：按优先级顺序存放所有待尝试的 DLL 路径
    std::vector<std::filesystem::path> candidates;
    if (!exeDir.empty())
    {
        candidates.push_back(exeDir / L"rzctl.dll");
        candidates.push_back(exeDir / L"controls" / L"rzctl.dll");
    }
    if (!ec)
    {
        candidates.push_back(cwd / L"rzctl.dll");
        candidates.push_back(cwd / L"controls" / L"rzctl.dll");
    }

    // 遍历候选路径，返回第一个实际存在的文件
    for (const auto& candidate : candidates)
    {
        if (std::filesystem::exists(candidate))
            return candidate;
    }

    // 所有候选路径均不存在时，返回默认路径（至少保证 LoadLibrary 行为可预测）
    return exeDir.empty() ? std::filesystem::path(L"rzctl.dll") : exeDir / L"rzctl.dll";
}

/**
 * 根据按钮编号获取对应的按下标志
 *
 * 将外部统一的按钮编号（1 = 左键, 2 = 右键, 3 = 中键）
 * 映射到 rzctl.dll 内部使用的按下标志值。
 *
 * @param  key  按钮编号（1=左键, 2=右键, 3=中键）
 * @return      对应的按下标志（RZ_LEFT_DOWN / RZ_RIGHT_DOWN / RZ_MIDDLE_DOWN）
 */
int RzctlMouse::downFlagForKey(int key)
{
    if (key == 2)
        return RZ_RIGHT_DOWN;
    if (key == 3)
        return RZ_MIDDLE_DOWN;
    return RZ_LEFT_DOWN;
}

/**
 * 根据按钮编号获取对应的释放标志
 *
 * 将外部统一的按钮编号（1 = 左键, 2 = 右键, 3 = 中键）
 * 映射到 rzctl.dll 内部使用的释放标志值。
 *
 * @param  key  按钮编号（1=左键, 2=右键, 3=中键）
 * @return      对应的释放标志（RZ_LEFT_UP / RZ_RIGHT_UP / RZ_MIDDLE_UP）
 */
int RzctlMouse::upFlagForKey(int key)
{
    if (key == 2)
        return RZ_RIGHT_UP;
    if (key == 3)
        return RZ_MIDDLE_UP;
    return RZ_LEFT_UP;
}

/**
 * 构造函数：加载 rzctl.dll 并解析函数导出地址
 *
 * 初始化流程：
 *   1. 调用 resolveDllPath() 自动定位 DLL 文件
 *   2. 通过 LoadLibraryW 动态加载 DLL
 *   3. 通过 GetProcAddress 获取所有需要的导出函数指针
 *   4. 检查关键函数是否全部解析成功
 *   5. 调用 init() 初始化 Razer 设备
 *
 * 兼容性说明：
 *   - 旧版 DLL 仅导出 mouseMove / mouseClick（无返回值）
 *   - 新版 DLL 额外导出 mouseMoveStatus / mouseClickStatus（返回 BOOL）
 *   代码优先尝试使用带状态返回的新版接口，失败时回退到旧版。
 */
RzctlMouse::RzctlMouse()
{
    dllPath = resolveDllPath();
    rzctl = LoadLibraryW(dllPath.wstring().c_str());
    if (rzctl == nullptr)
    {
        std::wcerr << L"[Razer] Failed to load rzctl.dll from " << dllPath.wstring() << std::endl;
        return;
    }

    init = reinterpret_cast<InitFn>(GetProcAddress(rzctl, "init"));
    mouseMoveStatus = reinterpret_cast<MouseMoveStatusFn>(GetProcAddress(rzctl, "mouse_move_status"));
    mouseClickStatus = reinterpret_cast<MouseClickStatusFn>(GetProcAddress(rzctl, "mouse_click_status"));
    mouseMove = reinterpret_cast<MouseMoveFn>(GetProcAddress(rzctl, "mouse_move"));
    mouseClick = reinterpret_cast<MouseClickFn>(GetProcAddress(rzctl, "mouse_click"));
    keyboardInput = reinterpret_cast<KeyboardInputFn>(GetProcAddress(rzctl, "keyboard_input"));

    // 验证必要的导出函数是否存在：
    // - init 和 keyboardInput 必须存在
    // - 移动相关（mouseMoveStatus 或 mouseMove）至少存在一个
    // - 点击相关（mouseClickStatus 或 mouseClick）至少存在一个
    if (!init || (!mouseMoveStatus && !mouseMove) || (!mouseClickStatus && !mouseClick) || !keyboardInput)
    {
        std::cerr << "[Razer] rzctl.dll is missing one or more required exports." << std::endl;
        return;
    }

    rzctlOk = init();
    if (!rzctlOk)
        std::cerr << "[Razer] RZCONTROL device initialization failed." << std::endl;
}

/**
 * 析构函数：释放 rzctl.dll
 *
 * 确保在 RzctlMouse 对象销毁时，通过 FreeLibrary 卸载动态库，
 * 避免模块泄漏。
 */
RzctlMouse::~RzctlMouse()
{
    if (rzctl != nullptr)
    {
        FreeLibrary(rzctl);
        rzctl = nullptr;
    }
}

/**
 * 鼠标移动
 *
 * 将鼠标移动到屏幕上的指定绝对坐标 (x, y)。
 *
 * 优先使用新版 mouseMoveStatus 接口（支持返回值判断），
 * 若不可用则回退到旧版 mouseMove 接口。
 *
 * @param  x  目标 X 坐标（屏幕绝对坐标）
 * @param  y  目标 Y 坐标（屏幕绝对坐标）
 * @return    操作是否成功（true=成功；旧版接口总是返回 true）
 */
bool RzctlMouse::mouse_xy(int x, int y)
{
    if (rzctlOk && mouseMoveStatus)
        return mouseMoveStatus(x, y, TRUE) == TRUE;

    if (rzctlOk && mouseMove)
    {
        mouseMove(x, y, true);
        return true;
    }

    return false;
}

/**
 * 鼠标按钮按下
 *
 * 模拟指定鼠标按键的按下事件。
 *
 * 优先使用新版 mouseClickStatus 接口（支持返回值判断），
 * 若不可用则回退到旧版 mouseClick 接口。
 *
 * @param  key  按钮编号（1=左键, 2=右键, 3=中键）
 * @return      操作是否成功（true=成功；旧版接口总是返回 true）
 */
bool RzctlMouse::mouse_down(int key)
{
    if (rzctlOk && mouseClickStatus)
        return mouseClickStatus(downFlagForKey(key)) == TRUE;

    if (rzctlOk && mouseClick)
    {
        mouseClick(downFlagForKey(key));
        return true;
    }

    return false;
}

/**
 * 鼠标按钮释放
 *
 * 模拟指定鼠标按键的释放（弹起）事件。
 *
 * 优先使用新版 mouseClickStatus 接口（支持返回值判断），
 * 若不可用则回退到旧版 mouseClick 接口。
 *
 * @param  key  按钮编号（1=左键, 2=右键, 3=中键）
 * @return      操作是否成功（true=成功；旧版接口总是返回 true）
 */
bool RzctlMouse::mouse_up(int key)
{
    if (rzctlOk && mouseClickStatus)
        return mouseClickStatus(upFlagForKey(key)) == TRUE;

    if (rzctlOk && mouseClick)
    {
        mouseClick(upFlagForKey(key));
        return true;
    }

    return false;
}

/**
 * 关闭鼠标设备
 *
 * 返回当前 Razer 设备的初始化状态。
 * 注意：实际的资源释放在析构函数中完成。
 *
 * @return  true 表示设备已成功初始化并可正常使用
 */
bool RzctlMouse::mouse_close()
{
    return rzctlOk;
}
