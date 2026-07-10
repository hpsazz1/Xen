// 定义 WIN32_LEAN_AND_MEAN 以从 windows.h 中排除较少使用的 API，加快编译速度
#define WIN32_LEAN_AND_MEAN
// 定义 _WINSOCKAPI_ 以防止 winsock.h 被重复包含，避免与 winsock2.h 冲突
#define _WINSOCKAPI_
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <vector>

#include "Xen.h"
#include "RP2350.h"

/**
 * @brief RP2350 类的构造函数
 *
 * 初始化与 RP2350 微控制器的串口连接。RP2350 负责处理鼠标相关的硬件输入
 * （按键、移动、滚轮等），通过串口与上位机通信。
 *
 * @param port       串口端口名称（如 "COM3" 或 "/dev/ttyACM0"）
 * @param baud_rate  串口通信波特率（通常为 115200 或 9600）
 *
 * 构造函数执行以下流程：
 *   1. 初始化成员变量（连接状态、监听标志、按键状态）
 *   2. 配置串口参数（端口号、波特率）并尝试打开连接
 *   3. 若连接成功，则输出成功信息
 *   4. 若配置启用了按键功能（rp2350_enable_keys），则启动按键状态监听线程
 *   5. 若连接失败或发生异常，输出错误信息
 *
 * @note 连接失败不会抛出异常，而是通过 is_open_ 标志标记状态，
 *       后续操作可通过 isOpen() 检查连接是否可用
 */
RP2350::RP2350(const std::string& port, unsigned int baud_rate)
    : is_open_(false),       // 串口连接状态标志，初始为未连接
      listening_(false),     // 按键状态监听线程运行标志，初始为未启动
      aiming_active(false),  // 瞄准按键（ID=5）按下状态
      shooting_active(false),// 射击按键（ID=1）按下状态
      zooming_active(false)  // 缩放按键（ID=2/3）按下状态
{
    try
    {
        serial_.setPort(port);
        serial_.setBaudrate(baud_rate);
        serial_.open();

        if (serial_.isOpen())
        {
            is_open_ = true;
            std::cout << "[RP2350] Connected! PORT: " << port << std::endl;

            if (config.rp2350_enable_keys)
            {
                startListening();
            }
        }
        else
        {
            std::cerr << "[RP2350] Unable to connect to the port: " << port << std::endl;
        }
    }
    catch (std::exception& e)
    {
        std::cerr << "[RP2350] Error: " << e.what() << std::endl;
    }
}

/**
 * @brief RP2350 类的析构函数
 *
 * 安全地关闭串口连接并清理资源。
 *
 * 析构流程：
 *   1. 将 listening_ 置为 false，通知监听线程退出循环
 *   2. 若监听线程正在运行，等待其结束（join）
 *   3. 若串口仍处于打开状态，尝试关闭连接
 *   4. 将 is_open_ 置为 false 标记连接已断开
 *
 * @note 串口关闭时发生的异常被静默捕获，确保析构过程不抛出异常
 */
RP2350::~RP2350()
{
    listening_ = false;

    if (listening_thread_.joinable())
    {
        listening_thread_.join();
    }

    if (serial_.isOpen())
    {
        try { serial_.close(); }
        catch (...) {}
    }

    is_open_ = false;
}

/**
 * @brief 检查串口连接是否已打开
 *
 * @return true  串口连接可用
 * @return false 串口已断开或从未成功连接
 */
bool RP2350::isOpen() const
{
    return is_open_;
}

/**
 * @brief 向 RP2350 串口发送原始数据
 *
 * 线程安全的写入方法，使用互斥锁保护串口写入操作。
 *
 * @param data 要发送的字符串数据（通常包含命令和换行符）
 *
 * 实现说明：
 *   - 通过互斥锁（write_mutex_）确保多线程环境下写入操作的原子性
 *   - 写入前检查连接状态，若未连接则直接返回
 *   - 若写入过程中发生异常，将 is_open_ 和 listening_ 同时置为 false
 *     触发重连或停止监听
 */
void RP2350::write(const std::string& data)
{
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (!is_open_)
        return;

    try
    {
        serial_.write(data);
    }
    catch (...)
    {
        is_open_ = false;
        listening_ = false;
    }
}

/**
 * @brief 从 RP2350 串口读取一行数据
 *
 * 以换行符（\n）为分隔符，从串口缓冲区读取一行数据。
 *
 * @return std::string 读取到的字符串（不含换行符）。
 *                      若连接不可用或读取失败，返回空字符串。
 *
 * 实现说明：
 *   - 内部调用 serial_.readline()，指定最大读取长度 65536 字节
 *   - 使用 "\n" 作为行结束标志
 *   - 读取异常同样会导致连接标记为断开
 */
std::string RP2350::read()
{
    if (!is_open_)
        return std::string();

    try
    {
        return serial_.readline(65536, "\n");
    }
    catch (...)
    {
        is_open_ = false;
        listening_ = false;
    }

    return std::string();
}

/**
 * @brief 发送鼠标单击命令
 *
 * 向 RP2350 发送 "c" 命令，触发一次完整的鼠标点击
 * （按下后立即释放）。
 *
 * 底层调用 sendCommand("c")，实际发送 "c\n" 到串口。
 */
void RP2350::click()
{
    sendCommand("c");
}

/**
 * @brief 发送鼠标按下命令
 *
 * 向 RP2350 发送 "p" 命令，将鼠标左键按下的状态
 * 发送到微控制器，直到收到 release() 命令才释放。
 *
 * 底层调用 sendCommand("p")，实际发送 "p\n" 到串口。
 */
void RP2350::press()
{
    sendCommand("p");
}

/**
 * @brief 发送鼠标释放命令
 *
 * 向 RP2350 发送 "r" 命令，释放之前按下的鼠标按键。
 *
 * 底层调用 sendCommand("r")，实际发送 "r\n" 到串口。
 */
void RP2350::release()
{
    sendCommand("r");
}

/**
 * @brief 发送鼠标移动命令
 *
 * 将鼠标相对移动量 (x, y) 发送到 RP2350 微控制器。
 *
 * @param x 水平方向的相对移动量（正值为右移，负值为左移）
 * @param y 垂直方向的相对移动量（正值为下移，负值为上移）
 *
 * 移动模式说明：
 *
 *   --- 16 位鼠标模式（config.rp2350_16_bit_mouse = true）---
 *   直接发送原始坐标值，不作分割。
 *   格式："mx,y\n" 例如移动 (300, -150) 发送 "m300,-150\n"
 *   此模式下 RP2350 固件支持单次传输 16 位的坐标范围（-32768 ~ 32767），
 *   适用于高精度或大范围移动场景。
 *
 *   --- 分割模式（默认）---
 *   由于标准鼠标 HID 协议的单次移动范围限制为 -127 ~ 127，
 *   若移动量超过此范围，需要将移动量分割成多个不超过 127 的数据包依次发送。
 *   流程：
 *     1. 将 x 和 y 分别通过 splitValue() 拆分成多个不超过 127 的片段
 *     2. 将两个方向的片段数对齐（较短的用 0 补齐）
 *     3. 按顺序逐对发送 "mx,y\n" 格式的移动命令
 *
 *   示例：move(300, -150)
 *     x 拆分：[127, 127, 46]
 *     y 拆分：[-127, -23]
 *     对齐后：x=[127, 127, 46], y=[-127, -23, 0]
 *     发送序列：
 *       "m127,-127\n"
 *       "m127,-23\n"
 *       "m46,0\n"
 *
 * @note 若 x 和 y 均为 0，或串口未打开，则直接返回不执行任何操作
 */
void RP2350::move(int x, int y)
{
    if (!is_open_ || (x == 0 && y == 0))
        return;

    if (config.rp2350_16_bit_mouse)
    {
        write("m" + std::to_string(x) + "," + std::to_string(y) + "\n");
        return;
    }

    std::vector<int> x_parts = splitValue(x);
    std::vector<int> y_parts = splitValue(y);

    size_t max_splits = std::max(x_parts.size(), y_parts.size());
    while (x_parts.size() < max_splits) x_parts.push_back(0);
    while (y_parts.size() < max_splits) y_parts.push_back(0);

    for (size_t i = 0; i < max_splits; ++i)
    {
        write("m" + std::to_string(x_parts[i]) + "," + std::to_string(y_parts[i]) + "\n");
    }
}

/**
 * @brief 发送格式化的串口命令（追加换行符）
 *
 * 向串口写入命令字符串并自动追加换行符，是 click()、press()、
 * release() 等方法的底层调用。
 *
 * @param command 命令字符串（如 "c"、"p"、"r"），不含换行符
 *
 * 实际发送："command\n"
 */
void RP2350::sendCommand(const std::string& command)
{
    write(command + "\n");
}

/**
 * @brief 将绝对值超过 127 的数值拆分为多个不超过 127 的片段
 *
 * 由于标准 USB HID 鼠标协议的相对移动量限制在 -127 ~ 127 范围内，
 * 当需要移动超过此范围时，必须将大数值拆分为多个小数值依次传输。
 *
 * @param value 要拆分的原始移动量（可为正、负或零）
 *
 * @return std::vector<int> 拆分后的数值片段列表，每个元素的绝对值
 *         均不超过 127，且符号与原始值一致。
 *
 * 拆分算法：
 *   1. 记录原始值的符号（正或负）
 *   2. 取绝对值进行计算
 *   3. 循环：当绝对值 > 127 时，取出一个 127（保持原始符号），
 *      从绝对值中减去 127
 *   4. 若剩余部分不为零，将其加入列表
 *   5. 若原始值为 0，直接返回包含 0 的列表
 *
 * 示例：
 *   splitValue(300)  -> [127, 127, 46]
 *   splitValue(-150) -> [-127, -23]
 *   splitValue(0)    -> [0]
 *   splitValue(100)  -> [100]  （无需拆分）
 */
std::vector<int> RP2350::splitValue(int value)
{
    std::vector<int> values;
    int sign = (value < 0) ? -1 : 1;
    int absVal = (value < 0) ? -value : value;

    if (value == 0)
    {
        values.push_back(0);
        return values;
    }

    while (absVal > 127)
    {
        values.push_back(sign * 127);
        absVal -= 127;
    }

    if (absVal != 0)
    {
        values.push_back(sign * absVal);
    }

    return values;
}

/**
 * @brief 启动 RP2350 按键状态监听线程
 *
 * 创建一个后台线程，持续从串口读取数据，解析来自 RP2350 微控制器的
 * 按键事件消息，并更新对应的按键状态标志。
 *
 * 实现说明：
 *   - 将 listening_ 置为 true 后启动监听线程
 *   - 若已有监听线程在运行，先等待其结束再创建新线程
 *   - 监听线程执行的函数为 listeningThreadFunc()
 *
 * @note 此方法仅当 config.rp2350_enable_keys 为 true 时由构造函数自动调用
 */
void RP2350::startListening()
{
    listening_ = true;
    if (listening_thread_.joinable())
        listening_thread_.join();

    listening_thread_ = std::thread(&RP2350::listeningThreadFunc, this);
}

/**
 * @brief 监听线程的主循环函数
 *
 * 在后台持续运行，轮询串口接收缓冲区，读取数据并解析出完整的行，
 * 交给 processIncomingLine() 处理。
 *
 * 工作流程：
 *   1. 检查 listening_ 和 is_open_ 标志，任一为 false 则退出循环
 *   2. 查询串口可用数据量（serial_.available()）
 *   3. 若有可用数据，读取全部并追加到内部缓冲区（buffer）
 *   4. 从缓冲区中按换行符（\n）提取完整的行
 *   5. 去除行尾的回车符（\r）后，调用 processIncomingLine() 处理
 *   6. 若无可用数据，线程休眠 10 毫秒避免空转占用 CPU
 *   7. 若读取过程中发生异常，断开连接标志并退出循环
 *
 * @note 这是一个无限循环，通过 listening_ 标志控制退出
 */
void RP2350::listeningThreadFunc()
{
    std::string buffer;
    while (listening_ && is_open_)
    {
        try
        {
            size_t available = serial_.available();
            if (available > 0)
            {
                std::string data = serial_.read(available);
                buffer += data;

                size_t pos;
                while ((pos = buffer.find('\n')) != std::string::npos)
                {
                    std::string line = buffer.substr(0, pos);
                    buffer.erase(0, pos + 1);
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                    processIncomingLine(line);
                }
            }
            else
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        catch (...)
        {
            is_open_ = false;
            break;
        }
    }
}

/**
 * @brief 处理来自 RP2350 的单行按键事件消息
 *
 * 解析一行串口消息，根据消息前缀区分按键按下（BD:）和按键释放（BU:）事件，
 * 并提取按键 ID，更新对应的按键状态标志。
 *
 * @param line 从串口读取的一行消息（不含换行符和回车符）
 *
 * 消息格式说明：
 *   --- 按键按下消息：BD:{buttonId}
 *       例如 "BD:1" 表示 ID 为 1 的按键被按下
 *   --- 按键释放消息：BU:{buttonId}
 *       例如 "BU:5" 表示 ID 为 5 的按键被释放
 *
 * 按键 ID 映射表：
 *   ID=1 : 射击（shooting）按键
 *         按下时：shooting_active = true, shooting = true
 *         释放时：shooting_active = false, shooting = false
 *   ID=2、3 : 缩放（zooming）按键
 *         按下时：zooming_active = true, zooming = true
 *         释放时：zooming_active = false, zooming = false
 *   ID=5 : 瞄准（aiming）按键
 *         按下时：aiming_active = true, aiming = true
 *         释放时：aiming_active = false, aiming = false
 *   其他 ID : 当前版本不做处理，直接忽略
 *
 * 解析流程：
 *   1. 检查消息是否以 "BD:"（按下）或 "BU:"（释放）开头
 *   2. 若匹配任一前缀，记录按键状态（pressed）并计算前缀长度（3 字节）
 *   3. 提取前缀后的按钮 ID 字符串，转换为 uint16_t 整数
 *   4. 根据按钮 ID 更新对应的原子状态变量
 *   5. unrecognized 格式的消息直接忽略，不产生任何副作用
 *
 * @note 使用原子变量（std::atomic<bool>）确保按键状态在多线程环境下的
 *       可见性和线程安全。shooting_active / aiming_active / zooming_active
 *       为 RP2350 层私有状态，shooting / aiming / zooming 为全局共享状态
 *       （定义于 Xen.h 或相关全局作用域）
 */
void RP2350::processIncomingLine(const std::string& line)
{
    try
    {
        bool pressed = false;
        size_t prefixLen = 0;

        if (line.rfind("BD:", 0) == 0)
        {
            pressed = true;
            prefixLen = 3;
        }
        else if (line.rfind("BU:", 0) == 0)
        {
            pressed = false;
            prefixLen = 3;
        }
        else
        {
            return;
        }

        uint16_t buttonId = static_cast<uint16_t>(std::stoi(line.substr(prefixLen)));
        switch (buttonId)
        {
        case 1:
            shooting_active.store(pressed);
            shooting.store(pressed);
            break;
        case 2:
        case 3:
            zooming_active.store(pressed);
            zooming.store(pressed);
            break;
        case 5:
            aiming_active.store(pressed);
            aiming.store(pressed);
            break;
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[RP2350] Error processing line '" << line << "': " << e.what() << std::endl;
    }
}
