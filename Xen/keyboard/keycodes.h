#ifndef KEYCODES_H
#define KEYCODES_H

#include <string>
#include <unordered_map>

// 按键码映射类，将按键名称（如 "VK_LCONTROL"）转换为虚拟键码
class KeyCodes
{
public:
    // 根据按键名称获取虚拟键码
    static int getKeyCode(const std::string& key_name);
    // 按键名称到虚拟键码的静态映射表
    static std::unordered_map<std::string, int> key_code_map;
};

#endif // KEYCODES_H