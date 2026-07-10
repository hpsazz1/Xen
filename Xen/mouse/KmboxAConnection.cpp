/**
 * @file KmboxAConnection.cpp
 * @brief KmboxA 连接管理类的实现
 *
 * 本文件实现了 KmboxAConnection 类，用于封装 Kmbox A 硬件设备的连接生命周期
 * 以及鼠标控制操作（移动、按键、滚轮等）。底层依赖 kmboxA.h 中提供的 C 接口。
 */

#include "KmboxAConnection.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>

#include "kmboxA.h"

/**
 * @brief 解析 PID/VID 十六进制字符串
 *
 * 从格式为 "PPPPVVVV" 的 8 位十六进制字符串中提取 PID（产品 ID）和 VID（厂商 ID）。
 * 输入字符串可以包含非十六进制字符（如连字符、空格等），函数会自动过滤掉无效字符。
 *
 * 解析规则：
 *   输入示例 "0123ABCD" → 过滤后得到 "0123ABCD"（仅保留 0-9 A-F a-f）
 *   前 4 位 "0123" → PID = 0x0123
 *   后 4 位 "ABCD" → VID = 0xABCD
 *
 * @param[in]  pidvid  输入的 PID/VID 字符串，期望包含 8 个有效的十六进制字符
 * @param[out] pid     解析成功后将产品 ID 写入此参数
 * @param[out] vid     解析成功后将厂商 ID 写入此参数
 * @return true  解析成功，pid 和 vid 已赋值
 * @return false 解析失败（过滤后不足 8 个十六进制字符，或数字转换异常）
 */
bool KmboxAConnection::parsePidVid(const std::string& pidvid, unsigned short& pid, unsigned short& vid)
{
    /* 用于存放过滤后的纯十六进制字符 */
    std::string hex;
    hex.reserve(pidvid.size());

    /* 遍历输入字符串，仅保留十六进制字符并转为大写 */
    for (unsigned char c : pidvid)
    {
        if (std::isxdigit(c))
        {
            hex.push_back(static_cast<char>(std::toupper(c)));
        }
    }

    /* 过滤后必须恰好为 8 个字符：前 4 位为 PID，后 4 位为 VID */
    if (hex.size() != 8)
    {
        return false;
    }

    try
    {
        /* 将字符串按基数为 16（十六进制）转换为无符号整数：
           前 4 个字符 → PID，后 4 个字符 → VID */
        pid = static_cast<unsigned short>(std::stoul(hex.substr(0, 4), nullptr, 16));
        vid = static_cast<unsigned short>(std::stoul(hex.substr(4, 4), nullptr, 16));
        return true;
    }
    catch (...)
    {
        /* std::stoul 抛出异常（如无效数字）时返回 false */
        return false;
    }
}

/**
 * @brief 构造函数 — 解析 PID/VID 字符串并建立与 Kmbox A 设备的连接
 *
 * 流程：
 *   1. 调用 parsePidVid() 将 "PPPPVVVV" 格式字符串解析为数值
 *   2. 调用 KM_init(vid, pid) 初始化设备连接
 *   3. 根据 KM_init 的返回值判断连接是否成功（返回 0 表示成功）
 *
 * KM_init() 是 kmboxA.h 中声明的 C 接口函数，原型：
 *   int KM_init(unsigned short vid, unsigned short pid);
 * 它根据 VID（厂商 ID）和 PID（产品 ID）在 USB 总线上查找并打开设备。
 *
 * @param[in] pidvid  "PPPPVVVV" 格式的 8 位十六进制字符串，
 *                    例如 "0123ABCD" 表示 PID=0x0123, VID=0xABCD
 */
KmboxAConnection::KmboxAConnection(const std::string& pidvid)
    : is_open_(false)  /* 初始化为未连接状态 */
{
    unsigned short pid = 0;
    unsigned short vid = 0;

    /* 解析 PID/VID 字符串，失败时打印错误信息并提前返回 */
    if (!parsePidVid(pidvid, pid, vid))
    {
        std::cerr << "[KmboxA] PID/VID 格式无效，应为 8 位十六进制字符 (PPPPVVVV)。" << std::endl;
        return;
    }

    /* 调用底层接口初始化连接，返回 0 表示成功 */
    const int ret = KM_init(vid, pid);
    is_open_ = (ret == 0);
    if (!is_open_)
    {
        std::cerr << "[KmboxA] 连接失败, ret=" << ret << " (VID=0x"
            << std::hex << std::uppercase << vid << ", PID=0x" << pid << std::dec << ")" << std::endl;
    }
}

/**
 * @brief 析构函数 — 关闭与 Kmbox A 设备的连接
 *
 * 如果当前处于已连接状态（is_open_ 为 true），则调用 KM_close() 关闭设备，
 * 并将 is_open_ 置为 false。
 *
 * KM_close() 是 kmboxA.h 中声明的 C 接口函数，原型：
 *   int KM_close(void);
 * 它释放设备句柄并断开 USB 连接。
 */
KmboxAConnection::~KmboxAConnection()
{
    if (!is_open_) return;
    KM_close();
    is_open_ = false;
}

/**
 * @brief 鼠标相对移动
 *
 * 调用底层 KM_move() 接口，使鼠标从当前位置沿 X/Y 轴移动指定的偏移量。
 *
 * KM_move() 是 kmboxA.h 中声明的 C 接口函数，原型：
 *   int KM_move(short x, short y);
 * 参数 x 和 y 为相对位移量（有符号短整型），负值表示向左/向上，正值表示向右/向下。
 *
 * @param[in] x X 轴相对位移（可为负数）
 * @param[in] y Y 轴相对位移（可为负数）
 */
void KmboxAConnection::move(int x, int y)
{
    if (!is_open_) return;
    KM_move(static_cast<short>(x), static_cast<short>(y));
}

/**
 * @brief 按下鼠标左键
 *
 * KM_left() 是 kmboxA.h 中声明的 C 接口函数，原型：
 *   int KM_left(unsigned char vk_key);
 * 参数为 1 表示按下，为 0 表示释放。
 */
void KmboxAConnection::leftDown()
{
    if (!is_open_) return;
    KM_left(1);
}

/**
 * @brief 释放鼠标左键
 *
 * KM_left(0) 将鼠标左键置为释放状态。
 */
void KmboxAConnection::leftUp()
{
    if (!is_open_) return;
    KM_left(0);
}

/**
 * @brief 按下鼠标右键
 *
 * KM_right() 是 kmboxA.h 中声明的 C 接口函数，原型：
 *   int KM_right(unsigned char vk_key);
 * 参数为 1 表示按下，为 0 表示释放。
 */
void KmboxAConnection::rightDown()
{
    if (!is_open_) return;
    KM_right(1);
}

/**
 * @brief 释放鼠标右键
 *
 * KM_right(0) 将鼠标右键置为释放状态。
 */
void KmboxAConnection::rightUp()
{
    if (!is_open_) return;
    KM_right(0);
}

/**
 * @brief 按下鼠标中键
 *
 * KM_middle() 是 kmboxA.h 中声明的 C 接口函数，原型：
 *   int KM_middle(unsigned char vk_key);
 * 参数为 1 表示按下，为 0 表示释放。
 */
void KmboxAConnection::middleDown()
{
    if (!is_open_) return;
    KM_middle(1);
}

/**
 * @brief 释放鼠标中键
 *
 * KM_middle(0) 将鼠标中键置为释放状态。
 */
void KmboxAConnection::middleUp()
{
    if (!is_open_) return;
    KM_middle(0);
}

/**
 * @brief 鼠标滚轮滚动
 *
 * 将传入的滚动量限制在 [-127, 127] 范围内，然后调用 KM_wheel() 执行滚动。
 *
 * KM_wheel() 是 kmboxA.h 中声明的 C 接口函数，原型：
 *   int KM_wheel(unsigned char w);
 * 正值表示向前滚动（滚轮向上），负值表示向后滚动（滚轮向下）。
 * 底层接口接受 unsigned char 类型，因此先 clamp 再转换。
 *
 * @param[in] delta 滚轮滚动量，正值为向前，负值为向后。
 *                  超出 [-127, 127] 的值会被钳制到边界。
 */
void KmboxAConnection::wheel(int delta)
{
    if (!is_open_) return;
    const int clamped = std::clamp(delta, -127, 127);
    const signed char wheel_delta = static_cast<signed char>(clamped);
    KM_wheel(static_cast<unsigned char>(wheel_delta));
}
