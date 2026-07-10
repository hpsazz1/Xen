#ifndef KEYBOARD_LISTENER_H
#define KEYBOARD_LISTENER_H

#include <string>
#include <vector>

// 键盘监听线程函数，在独立线程中轮询按键状态
void keyboardListener();
// 检查按键列表中的任意键是否被按下（完整检测，支持所有输入方式）
bool isAnyKeyPressed(const std::vector<std::string>& keys);
// 仅通过 Win32 API 检查按键状态（不依赖其他输入设备）
bool isAnyKeyPressedWin32Only(const std::vector<std::string>& keys);

#endif // KEYBOARD_LISTENER_H
