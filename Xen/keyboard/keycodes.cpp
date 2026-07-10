// keycodes.cpp - 按键码映射表实现
// 定义 KeyCodes 类的静态成员变量：将字符串形式的按键名称
// 映射到 Windows 虚拟键码（Virtual Key Code）。
// 用于配置文件中的按键绑定（如 button_targeting = "RightMouseButton"）
// 到实际 Win32 API 可识别的键码的转换。

#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include "keycodes.h"

#include <string>
#include <unordered_map>

/**
 * key_code_map - 按键名称到虚拟键码的映射表
 *
 * 覆盖以下类型按键：
 * - 鼠标键：左/右/中/X1/X2
 * - 控制键：Backspace/Tab/Enter/Escape 等
 * - 导航键：方向键/PageUp/Home/End 等
 * - 字母键：A-Z
 * - 数字键：0-9（主键盘）
 * - 功能键：F1-F12
 * - 小键盘键：Numpad0-9/运算符
 * - 修饰键：Shift/Ctrl/Alt（区分左右）
 * - 多媒体键：音量/播放控制
 * - 浏览器键：前进/后退/刷新
 * - 系统键：Windows 键/Applications
 */
std::unordered_map<std::string, int> KeyCodes::key_code_map =
{
    {"None", 0},
    {"LeftMouseButton", VK_LBUTTON},
    {"RightMouseButton", VK_RBUTTON},
    {"ControlBreak", VK_CANCEL},
    {"MiddleMouseButton", VK_MBUTTON},
    {"X1MouseButton", VK_XBUTTON1},
    {"X2MouseButton", VK_XBUTTON2},
    {"Backspace", VK_BACK},
    {"Tab", VK_TAB},
    {"Clear", VK_CLEAR},
    {"Enter", VK_RETURN},
    {"Pause", VK_PAUSE},
    {"CapsLock", VK_CAPITAL},
    {"Escape", VK_ESCAPE},
    {"Space", VK_SPACE},
    {"PageUp", VK_PRIOR},
    {"PageDown", VK_NEXT},
    {"End", VK_END},
    {"Home", VK_HOME},
    {"LeftArrow", VK_LEFT},
    {"UpArrow", VK_UP},
    {"RightArrow", VK_RIGHT},
    {"DownArrow", VK_DOWN},
    {"Select", VK_SELECT},
    {"Print", VK_PRINT},
    {"Execute", VK_EXECUTE},
    {"PrintScreen", VK_SNAPSHOT},
    {"Ins", VK_INSERT},
    {"Delete", VK_DELETE},
    {"Help", VK_HELP},
    {"Key0", '0'},
    {"Key1", '1'},
    {"Key2", '2'},
    {"Key3", '3'},
    {"Key4", '4'},
    {"Key5", '5'},
    {"Key6", '6'},
    {"Key7", '7'},
    {"Key8", '8'},
    {"Key9", '9'},
    {"A", 'A'},
    {"B", 'B'},
    {"C", 'C'},
    {"D", 'D'},
    {"E", 'E'},
    {"F", 'F'},
    {"G", 'G'},
    {"H", 'H'},
    {"I", 'I'},
    {"J", 'J'},
    {"K", 'K'},
    {"L", 'L'},
    {"M", 'M'},
    {"N", 'N'},
    {"O", 'O'},
    {"P", 'P'},
    {"Q", 'Q'},
    {"R", 'R'},
    {"S", 'S'},
    {"T", 'T'},
    {"U", 'U'},
    {"V", 'V'},
    {"W", 'W'},
    {"X", 'X'},
    {"Y", 'Y'},
    {"Z", 'Z'},
    {"LeftWindowsKey", VK_LWIN},
    {"RightWindowsKey", VK_RWIN},
    {"Application", VK_APPS},
    {"Sleep", VK_SLEEP},
    {"NumpadKey0", VK_NUMPAD0},
    {"NumpadKey1", VK_NUMPAD1},
    {"NumpadKey2", VK_NUMPAD2},
    {"NumpadKey3", VK_NUMPAD3},
    {"NumpadKey4", VK_NUMPAD4},
    {"NumpadKey5", VK_NUMPAD5},
    {"NumpadKey6", VK_NUMPAD6},
    {"NumpadKey7", VK_NUMPAD7},
    {"NumpadKey8", VK_NUMPAD8},
    {"NumpadKey9", VK_NUMPAD9},
    {"Multiply", VK_MULTIPLY},
    {"Add", VK_ADD},
    {"Separator", VK_SEPARATOR},
    {"Subtract", VK_SUBTRACT},
    {"Decimal", VK_DECIMAL},
    {"Divide", VK_DIVIDE},
    {"F1", VK_F1},
    {"F2", VK_F2},
    {"F3", VK_F3},
    {"F4", VK_F4},
    {"F5", VK_F5},
    {"F6", VK_F6},
    {"F7", VK_F7},
    {"F8", VK_F8},
    {"F9", VK_F9},
    {"F10", VK_F10},
    {"F11", VK_F11},
    {"F12", VK_F12},
    {"NumLock", VK_NUMLOCK},
    {"ScrollLock", VK_SCROLL},
    {"LeftShift", VK_LSHIFT},
    {"RightShift", VK_RSHIFT},
    {"LeftControl", VK_LCONTROL},
    {"RightControl", VK_RCONTROL},
    {"LeftAlt", VK_LMENU},
    {"RightAlt", VK_RMENU},
    {"BrowserBack", VK_BROWSER_BACK},
    {"BrowserRefresh", VK_BROWSER_REFRESH},
    {"BrowserStop", VK_BROWSER_STOP},
    {"BrowserSearch", VK_BROWSER_SEARCH},
    {"BrowserFavorites", VK_BROWSER_FAVORITES},
    {"BrowserHome", VK_BROWSER_HOME},
    {"VolumeMute", VK_VOLUME_MUTE},
    {"VolumeDown", VK_VOLUME_DOWN},
    {"VolumeUp", VK_VOLUME_UP},
    {"NextTrack", VK_MEDIA_NEXT_TRACK},
    {"PreviousTrack", VK_MEDIA_PREV_TRACK},
    {"StopMedia", VK_MEDIA_STOP},
    {"PlayMedia", VK_MEDIA_PLAY_PAUSE},
    {"StartMailKey", VK_LAUNCH_MAIL},
    {"SelectMedia", VK_LAUNCH_MEDIA_SELECT},
    {"StartApplication1", VK_LAUNCH_APP1},
    {"StartApplication2", VK_LAUNCH_APP2}
};

/**
 * getKeyCode - 根据按键名称获取 Windows 虚拟键码
 * @param key_name 按键名称字符串（如 "RightMouseButton"、"F2"）
 * @return 对应的虚拟键码，未找到返回 -1
 */
int KeyCodes::getKeyCode(const std::string& key_name) {
    auto it = key_code_map.find(key_name);
    if (it != key_code_map.end())
        return it->second;
    else
        return -1;
}
