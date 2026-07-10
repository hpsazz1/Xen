/**
 * @file Teensy41RawHid.cpp
 * @brief Teensy 4.1 微控制器 RawHID 通信实现
 *
 * 本文件实现了与 Teensy 4.1 微控制器的 RawHID（原始 HID）通信协议。
 * 负责通过 USB HID 接口枚举、连接和收发自定义格式的数据包，
 * 实现主机端（PC）与微控制器之间的双向通信。
 * 数据包包含鼠标移动、滚轮、按键状态等输入指令，
 * 以及从设备返回的按键事件（射击、瞄准、缩放）。
 */
#include "Teensy41RawHid.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>

#include "config.h"
#include "hidapi.h"

// ============================================================
// 匿名命名空间：文件内部辅助函数
// 这些函数仅在本翻译单元内可见，用于简化数值转换和字符串处理
// ============================================================
namespace
{
/**
 * @brief 将 int 值安全截断为 uint16_t，若超出有效范围则返回默认值
 *
 * 将传入的整数值限制在 [1, 0xFFFF] 范围内。若输入值小于 1 或大于
 * 0xFFFF（即 uint16_t 最大值），则返回预设的 fallback 默认值。
 * 用于保护 HID 设备描述符中的 usage page / usage ID 参数。
 *
 * @param value    待转换的原始整数值
 * @param fallback 当 value 超出范围时返回的默认值
 * @return 转换后的 uint16_t 值，或 fallback
 */
uint16_t clampToUint16(int value, uint16_t fallback)
{
    if (value < 1 || value > 0xFFFF)
        return fallback;
    return static_cast<uint16_t>(value);
}

/**
 * @brief 判断字符串是否是"自动"（AUTO）模式
 *
 * 检查传入的字符串是否为空字符串或"auto"（不区分大小写）。
 * 在 HID 设备过滤配置中使用：当设置为 AUTO 时表示不启用该过滤项，
 * 允许匹配任意设备。
 *
 * @param value 待检查的字符串
 * @return true  如果字符串为空或等于"AUTO"（不区分大小写）
 * @return false 其他情况
 */
bool isAuto(const std::string& value)
{
    if (value.empty())
        return true;

    std::string upper = value;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return upper == "AUTO";
}

/**
 * @brief 解析十六进制格式的过滤器字符串，用于 VID/PID 过滤
 *
 * 将类似 "0xFFAB" 或 "FFAB" 格式的字符串解析为 uint16_t 值。
 * 如果字符串为 AUTO 或无法解析，则返回 false。
 * 用于从配置文件中读取 VID（供应商ID）和 PID（产品ID）过滤器。
 *
 * @param value  十六进制字符串（如 "0x16C0" 或 "16C0"）
 * @param parsed 输出参数，存放解析后的 uint16_t 值
 * @return true  解析成功，parsed 中为有效值
 * @return false 解析失败（字符串为 AUTO、格式错误或超出范围）
 */
bool parseHexFilter(const std::string& value, uint16_t& parsed)
{
    if (isAuto(value))
        return false;

    char* end = nullptr;
    unsigned long result = std::strtoul(value.c_str(), &end, 16);
    if (end == value.c_str() || *end != '\0' || result > 0xFFFF)
        return false;

    parsed = static_cast<uint16_t>(result);
    return true;
}

/**
 * @brief 将宽字符串（wchar_t*）窄化为普通字符串（std::string）
 *
 * HID API 返回的设备序列号为宽字符格式。本函数将其转换为普通
 * 单字节字符串用于比较。仅保留 ASCII 范围内的字符（0x00-0x7F），
 * 超出范围的字符替换为问号 '?'。
 *
 * @param value 宽字符字符串指针（可能为空指针）
 * @return 转换后的窄字符串，空指针时返回空字符串
 */
std::string narrow(const wchar_t* value)
{
    if (!value)
        return {};

    std::string out;
    while (*value)
    {
        wchar_t wc = *value++;
        out.push_back(wc >= 0 && wc <= 0x7F ? static_cast<char>(wc) : '?');
    }
    return out;
}

/**
 * @brief 将 int 值钳制到 int16_t 的有效范围内
 *
 * 确保数值在 [-32768, 32767] 范围内，防止数据包中的 dx/dy/wheel
 * 字段溢出。HID 数据包使用 int16_t 表示鼠标移动量和滚轮值。
 *
 * @param value 待钳制的原始整数值
 * @return 钳制后的 int 值，保证可安全转换为 int16_t
 */
int clampInt16(int value)
{
    return std::clamp(value,
        static_cast<int>(std::numeric_limits<int16_t>::min()),
        static_cast<int>(std::numeric_limits<int16_t>::max()));
}
}

/**
 * @brief HID 设备自定义删除器：关闭设备句柄
 *
 * 用于 std::unique_ptr 的 RAII（资源获取即初始化）管理。
 * 当 device_ 智能指针析构或重置时自动调用，确保 hid_device
 * 句柄被正确关闭，防止资源泄漏。
 *
 * @param device HID 设备指针（可为空）
 */
void Teensy41RawHid::HidDeviceDeleter::operator()(hid_device* device) const
{
    if (device)
        hid_close(device);
}

/**
 * @brief 构造函数：初始化 Teensy 4.1 RawHID 通信器
 *
 * 从配置对象中读取 HID 过滤参数（usage page、usage ID、VID、PID、
 * 序列号过滤器）和通信参数（打开索引、超时、重连间隔）。
 * 立即尝试打开设备，并启动后台读取线程监听设备事件。
 *
 * @param config 配置对象，包含所有 teensy_hid_* 配置项
 *
 * 成员变量说明：
 * - usagePage_    : HID 用途页（Usage Page），用于枚举过滤，默认 0xFFAB
 * - usageId_      : HID 用途 ID（Usage ID），用于枚举过滤，默认 0x0200
 * - openIndex_    : 当多台设备匹配时选择第几个（从 0 开始）
 * - packetTimeoutMs_ : 每次 HID 读取的超时时间（毫秒）
 * - reconnectIntervalMs_ : 断开后重连等待间隔（毫秒）
 * - serialFilter_ : 设备序列号过滤字符串，AUTO 表示不启用
 * - vidFilter_    : 供应商 ID 过滤（十六进制字符串），AUTO 表示不启用
 * - pidFilter_    : 产品 ID 过滤（十六进制字符串），AUTO 表示不启用
 * - connected_    : 原子布尔值，标记当前是否已连接
 * - readerThread_ : 后台读取线程，处理设备上报的按键事件
 */
Teensy41RawHid::Teensy41RawHid(const Config& config)
    : usagePage_(clampToUint16(config.teensy_hid_usage_page, 0xFFAB)),             // HID Usage Page，默认 0xFFAB（自定义用途页）
      usageId_(clampToUint16(config.teensy_hid_usage_id, 0x0200)),                 // HID Usage ID，默认 0x0200
      openIndex_(std::clamp(config.teensy_hid_open_index, 0, 32)),                 // 多设备匹配时的选择索引
      packetTimeoutMs_(std::clamp(config.teensy_hid_packet_timeout_ms, 0, 100)),   // 每次读取超时，0-100ms
      reconnectIntervalMs_(std::clamp(config.teensy_hid_reconnect_interval_ms, 50, 10000)), // 重连间隔，50-10000ms
      serialFilter_(config.teensy_hid_serial),         // 序列号过滤字符串
      vidFilter_(config.teensy_hid_vid_filter),         // 供应商 ID 过滤（十六进制）
      pidFilter_(config.teensy_hid_pid_filter)          // 产品 ID 过滤（十六进制）
{
    // 尝试打开设备，并用 memory_order_release 语义更新连接状态
    connected_.store(open(), std::memory_order_release);
    // 启动后台读取线程，执行 readerLoop 循环
    readerThread_ = std::thread(&Teensy41RawHid::readerLoop, this);
}

/**
 * @brief 析构函数：清理资源并停止后台读取线程
 *
 * 设置停止标志位，等待读取线程退出，最后释放设备句柄。
 * 使用 memory_order_release 语义确保其他线程能正确看到 stop 信号。
 */
Teensy41RawHid::~Teensy41RawHid()
{
    // 通知后台读取线程停止循环
    stopReader_.store(true, std::memory_order_release);
    // 等待线程安全退出，避免资源泄漏
    if (readerThread_.joinable())
        readerThread_.join();
    // 关闭并释放 HID 设备句柄
    device_.reset();
}

/**
 * @brief 检查设备是否已打开并处于连接状态
 *
 * 双重检查：首先检查原子标志位 connected_（快速路径），
 * 若已标记为连接，再通过互斥锁检查 device_ 智能指针是否非空。
 *
 * @return true  设备已打开且可通信
 * @return false 设备未打开或已断开
 */
bool Teensy41RawHid::isOpen() const
{
    // 快速检查：原子标志位是否指示已连接
    if (!connected_.load(std::memory_order_acquire))
        return false;

    // 精确检查：加锁确认 device_ 指针非空
    std::lock_guard<std::mutex> lock(writeMutex_);
    return device_ != nullptr;
}

/**
 * @brief 发送鼠标移动指令到 Teensy 4.1
 *
 * 构造 Move 命令数据包，包含 dx（水平位移）、dy（垂直位移）、
 * wheel（垂直滚轮）和 wheelH（水平滚轮）值，发送到设备。
 * 同时携带当前主机端按键状态。
 *
 * @param dx    水平相对移动量（正=右，负=左）
 * @param dy    垂直相对移动量（正=下，负=上）
 * @param wheel 垂直滚轮滚动量（正=向下滚，负=向上滚）
 * @param wheelH 水平滚轮滚动量（正=向右滚，负=向左滚）
 * @return true  发送成功
 * @return false 发送失败（未连接或写入错误）
 */
bool Teensy41RawHid::move(int dx, int dy, int wheel, int wheelH)
{
    return sendPacket(Teensy41RawHidCommand::Move, dx, dy, wheel, wheelH, hostButtons_.load(std::memory_order_acquire));
}

/**
 * @brief 发送鼠标左键按下指令
 *
 * 将主机端按键状态中的左键位（Teensy41RawHidButtonLeft）置 1，
 * 并通过 Buttons 命令将更新后的按键掩码发送到设备。
 *
 * @return true  发送成功
 * @return false 发送失败
 */
bool Teensy41RawHid::press()
{
    return setHostButtons(static_cast<uint8_t>(hostButtons_.load(std::memory_order_acquire) | Teensy41RawHidButtonLeft));
}

/**
 * @brief 发送鼠标左键释放指令
 *
 * 将主机端按键状态中的左键位（Teensy41RawHidButtonLeft）置 0，
 * 并通过 Buttons 命令将更新后的按键掩码发送到设备。
 *
 * @return true  发送成功
 * @return false 发送失败
 */
bool Teensy41RawHid::release()
{
    return setHostButtons(static_cast<uint8_t>(hostButtons_.load(std::memory_order_acquire) & ~Teensy41RawHidButtonLeft));
}

/**
 * @brief 设置主机端按键掩码并同步发送到设备
 *
 * 更新内存中的主机按键状态，然后立即构造 Buttons 命令包
 * 发送到 Teensy 4.1，通知设备当前哪些主机按键处于按下状态。
 *
 * @param mask 8位按键掩码，每一位代表一个按键状态
 * @return true  发送成功
 * @return false 发送失败
 */
bool Teensy41RawHid::setHostButtons(uint8_t mask)
{
    hostButtons_.store(mask, std::memory_order_release);
    return sendPacket(Teensy41RawHidCommand::Buttons, 0, 0, 0, 0, mask);
}

/**
 * @brief 查询瞄准（Aiming）按键当前是否激活
 *
 * 读取设备上报的 aiming 状态。当 Teensy 4.1 上的 Button ID 5
 * 被按下时此状态为 true，用于触发瞄准相关功能。
 *
 * @return true  瞄准按键正在按下
 * @return false 瞄准按键未按下
 */
bool Teensy41RawHid::aimingActive() const
{
    return aimingActive_.load(std::memory_order_acquire);
}

/**
 * @brief 查询射击（Shooting）按键当前是否激活
 *
 * 读取设备上报的 shooting 状态。当 Teensy 4.1 上的 Button ID 1
 * 被按下时此状态为 true，用于触发射击相关功能。
 *
 * @return true  射击按键正在按下
 * @return false 射击按键未按下
 */
bool Teensy41RawHid::shootingActive() const
{
    return shootingActive_.load(std::memory_order_acquire);
}

/**
 * @brief 查询缩放（Zooming）按键当前是否激活
 *
 * 读取设备上报的 zooming 状态。当 Teensy 4.1 上的 Button ID 2
 * 被按下时此状态为 true，用于触发缩放相关功能。
 *
 * @return true  缩放按键正在按下
 * @return false 缩放按键未按下
 */
bool Teensy41RawHid::zoomingActive() const
{
    return zoomingActive_.load(std::memory_order_acquire);
}

/**
 * @brief 枚举并打开匹配的 Teensy 4.1 HID 设备
 *
 * HID 设备枚举与过滤逻辑：
 * 1. 通过 hid_enumerate 枚举所有 HID 设备（可按 VID/PID 预过滤）
 * 2. 遍历设备列表，依次按以下条件过滤：
 *    a. Usage Page 必须匹配（默认 0xFFAB，Teensy RawHID 自定义用途页）
 *    b. Usage ID 必须匹配（默认 0x0200）
 *    c. 若配置了 VID 过滤器，则 vendors_id 必须匹配
 *    d. 若配置了 PID 过滤器，则 product_id 必须匹配
 *    e. 若配置了序列号过滤器，则序列号必须精确匹配
 * 3. 在第 openIndex_ 个匹配设备处停止过滤
 * 4. 调用 hid_open_path 打开该设备路径
 * 5. 设置非阻塞模式并返回
 *
 * @return true  成功打开设备
 * @return false 未找到匹配设备或打开失败
 */
bool Teensy41RawHid::open()
{
    uint16_t vid = 0;   // 解析后的供应商 ID
    uint16_t pid = 0;   // 解析后的产品 ID
    // 尝试解析 VID/PID 十六进制过滤器，若为 AUTO 则不启用对应过滤
    const bool useVid = parseHexFilter(vidFilter_, vid);
    const bool usePid = parseHexFilter(pidFilter_, pid);

    // 枚举 HID 设备，若指定了 VID/PID 则向 hidapi 传递以缩小枚举范围
    hid_device_info* devices = hid_enumerate(useVid ? vid : 0, usePid ? pid : 0);
    int matchedIndex = 0;  // 记录匹配到第几个设备
    for (hid_device_info* cur = devices; cur; cur = cur->next)
    {
        // ========== 过滤条件检查 ==========
        // 1. Usage Page（用途页）和 Usage ID（用途 ID）必须匹配
        if (cur->usage_page != usagePage_ || cur->usage != usageId_)
            continue;
        // 2. 若启用 VID 过滤，供应商 ID 必须匹配
        if (useVid && cur->vendor_id != vid)
            continue;
        // 3. 若启用 PID 过滤，产品 ID 必须匹配
        if (usePid && cur->product_id != pid)
            continue;
        // 4. 若启用序列号过滤，需将宽字符串序列号窄化后比较
        if (!isAuto(serialFilter_) && serialFilter_ != narrow(cur->serial_number))
            continue;

        // 计数匹配设备，只有达到 openIndex_ 时才打开
        if (matchedIndex++ != openIndex_)
            continue;

        // 打开匹配设备的路径
        hid_device* opened = hid_open_path(cur->path);
        if (opened)
        {
            // 成功打开：存入智能指针，设置为非阻塞模式，释放枚举列表
            device_.reset(opened);
            hid_set_nonblocking(device_.get(), 1);
            hid_free_enumeration(devices);
            return true;
        }
    }

    // 未找到匹配设备或打开失败
    hid_free_enumeration(devices);
    return false;
}

/**
 * @brief 构造并发送 RawHID 数据包到 Teensy 4.1
 *
 * 数据包格式（Teensy41RawHidHostPacket）：
 * - command    : 命令类型（1 字节），如 Move / Buttons
 * - buttonMask : 主机端按键掩码（1 字节）
 * - dx         : 水平位移（2 字节，int16_t）
 * - dy         : 垂直位移（2 字节，int16_t）
 * - wheel      : 垂直滚轮（2 字节，int16_t）
 * - wheelH     : 水平滚轮（2 字节，int16_t）
 * - sequence   : 递增序列号（4 字节），用于去重和追踪
 *
 * HID 报告以 0x00 作为报告 ID（report[0]），数据从 report[1] 开始写入。
 *
 * @param command    命令枚举值（Move 或 Buttons）
 * @param dx         水平移动量（自动钳制到 int16_t 范围）
 * @param dy         垂直移动量（自动钳制到 int16_t 范围）
 * @param wheel      垂直滚轮量（自动钳制到 int16_t 范围）
 * @param wheelH     水平滚轮量（自动钳制到 int16_t 范围）
 * @param buttonMask 8位按键掩码
 * @return true  写入成功且写入字节数等于数据包大小
 * @return false 写入失败或未连接
 */
bool Teensy41RawHid::sendPacket(Teensy41RawHidCommand command, int dx, int dy, int wheel, int wheelH, uint8_t buttonMask)
{
    // 快速检查连接状态
    if (!connected_.load(std::memory_order_acquire))
        return false;

    // 填充主机端数据包结构体
    Teensy41RawHidHostPacket packet;
    packet.command = static_cast<uint8_t>(command);     // 命令类型
    packet.buttonMask = buttonMask;                     // 按键掩码
    packet.dx = static_cast<int16_t>(clampInt16(dx));   // 水平位移（钳制到 int16_t）
    packet.dy = static_cast<int16_t>(clampInt16(dy));   // 垂直位移（钳制到 int16_t）
    packet.wheel = static_cast<int16_t>(clampInt16(wheel));     // 垂直滚轮（钳制到 int16_t）
    packet.wheelH = static_cast<int16_t>(clampInt16(wheelH));   // 水平滚轮（钳制到 int16_t）

    // HID 报告缓冲区：第 0 字节为报告 ID，随后是数据包数据
    std::array<unsigned char, Teensy41RawHidPacketSize + 1> report{};

    // 加锁确保写入操作的线程安全性
    std::lock_guard<std::mutex> lock(writeMutex_);
    if (!device_)
        return false;

    // 递增序列号并填充数据包
    packet.sequence = ++sequence_;
    // 报告 ID 置为 0（对应 HID 报告的 Report ID）
    report[0] = 0;
    // 将数据包拷贝到报告缓冲区（偏移 1 字节）
    std::memcpy(report.data() + 1, &packet, sizeof(packet));

    // 通过 HID API 写入数据到设备
    int written = hid_write(device_.get(), report.data(), report.size());
    if (written < 0)
    {
        // 写入失败，标记为断开连接
        connected_.store(false, std::memory_order_release);
        return false;
    }
    // 成功条件：写入的字节数必须等于完整的报告大小
    return written == static_cast<int>(report.size());
}

/**
 * @brief 后台读取线程主循环
 *
 * 在独立线程中运行的无限循环，负责以下工作：
 *
 * 1. 断线重连机制：
 *    - 若 isOpen() 返回 false，先释放 device_，然后调用 open()
 *      重新尝试连接
 *    - 每次重连后等待 reconnectIntervalMs_ 毫秒再重试
 *    - 循环检查 stopReader_ 标志位以便安全退出
 *
 * 2. 数据包读取（带超时）：
 *    - 使用 hid_read_timeout 以 packetTimeoutMs_ 为超时读取数据
 *    - 读取返回 0 表示超时（无数据），睡眠 1ms 后继续
 *    - 读取返回负值表示错误，标记连接断开
 *
 * 3. 数据包解析：
 *    - 自动检测是否有报告 ID 前缀（buffer[0] == 0 时偏移 1 字节）
 *    - 检查数据包 Magic 和 Version 字段以验证格式有效
 *    - 读取 Teensy41RawHidDevicePacket 结构体
 *
 * 4. 按键事件处理：
 *    - 若 packet.event == Button 事件，调用 applyButtonEvent
 *    - 将设备上报的 buttonId 和 pressed 状态映射到对应的原子状态变量
 *
 * 5. 退出条件：
 *    - stopReader_ 被置为 true 时退出循环（由析构函数触发）
 */
void Teensy41RawHid::readerLoop()
{
    while (!stopReader_.load(std::memory_order_acquire))
    {
        // ========== 设备未连接：执行重连 ==========
        if (!isOpen())
        {
            {
                // 先释放旧的设备句柄（若有）
                std::lock_guard<std::mutex> lock(writeMutex_);
                device_.reset();
                // 尝试重新打开设备，并更新连接状态
                connected_.store(open(), std::memory_order_release);
            }
            // 等待重连间隔时间后再试，避免忙等待
            std::this_thread::sleep_for(std::chrono::milliseconds(reconnectIntervalMs_));
            continue;
        }

        // ========== 设备已连接：读取数据 ==========
        // HID 报告缓冲区：第 0 字节为报告 ID，随后是设备数据包
        std::array<unsigned char, Teensy41RawHidPacketSize + 1> buffer{};
        int read = hid_read_timeout(device_.get(), buffer.data(), buffer.size(), packetTimeoutMs_);
        if (read == 0)
        {
            // 超时无数据：短暂休眠后继续轮询
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (read < 0)
        {
            // 读取错误：标记连接断开，将在下一次循环中触发重连
            connected_.store(false, std::memory_order_release);
            continue;
        }

        // ========== 数据包解析 ==========
        // 检测是否包含报告 ID 头：若 buffer[0] == 0，则数据从 index 1 开始
        const size_t offset = (read == static_cast<int>(Teensy41RawHidPacketSize + 1) && buffer[0] == 0) ? 1u : 0u;
        // 确保剩余数据长度足够存放设备数据包
        if (read - static_cast<int>(offset) < static_cast<int>(sizeof(Teensy41RawHidDevicePacket)))
            continue;

        // 拷贝设备数据包到结构体
        Teensy41RawHidDevicePacket packet;
        std::memcpy(&packet, buffer.data() + offset, sizeof(packet));
        // 验证 Magic 和 Version 字段，确保数据包格式有效
        if (packet.magic != Teensy41RawHidDeviceMagic || packet.version != Teensy41RawHidVersion)
            continue;

        // ========== 事件分发 ==========
        // 若设备上报的是按键事件，则解析并更新对应的状态变量
        if (packet.event == static_cast<uint8_t>(Teensy41RawHidEvent::Button))
            applyButtonEvent(packet.buttonId, packet.pressed != 0);
    }
}

/**
 * @brief 应用设备端按键事件，更新对应的状态原子变量
 *
 * 将 Teensy 4.1 上报的物理按键按下/释放事件映射到高层的
 * 状态标志位，供上层逻辑查询。
 *
 * 按键 ID 与功能映射（依据设备端固件定义）：
 * - Button ID 1 => shootingActive_（射击模式）：按下时为 true
 * - Button ID 2 => zoomingActive_（缩放模式）：按下时为 true
 * - Button ID 5 => aimingActive_（瞄准模式）：按下时为 true
 * - 其他 ID    => 忽略（不处理）
 *
 * @param buttonId 设备端物理按键编号（1、2 或 5）
 * @param pressed  按下状态：true 表示按下，false 表示释放
 */
void Teensy41RawHid::applyButtonEvent(uint8_t buttonId, bool pressed)
{
    switch (buttonId)
    {
    case 1:
        // Button 1：射击模式激活/解除
        shootingActive_.store(pressed, std::memory_order_release);
        break;
    case 2:
        // Button 2：缩放模式激活/解除
        zoomingActive_.store(pressed, std::memory_order_release);
        break;
    case 5:
        // Button 5：瞄准模式激活/解除
        aimingActive_.store(pressed, std::memory_order_release);
        break;
    default:
        // 未知按键 ID，不做处理
        break;
    }
}
