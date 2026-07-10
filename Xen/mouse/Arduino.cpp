/******************************************************************************
 * Arduino.cpp
 *
 * 本文件实现了 Arduino 类，负责通过串口与 Arduino 微控制器通信，实现鼠标控制。
 * 系统支持两种通信协议：
 *
 * 1. Legacy 协议（传统协议）：
 *    - 使用文本命令与 Arduino 通信
 *    - 鼠标移动命令格式："m x,y\n"，例如 "m 100,-50\n"
 *    - 鼠标点击命令："c\n"（点击）、"p\n"（按下）、"r\n"（松开）
 *    - 当 arduino_16_bit_mouse 为 false 时，大数值移动会被自动分割为多个
 *      不超过 127 的小块发送（由 splitValue 逻辑处理）
 *
 * 2. Teensy41 协议：
 *    - 使用二进制按钮掩码（button mask）表示按键状态
 *    - 鼠标移动命令格式："move x y 0 0\n"
 *    - 按键状态通过位运算维护在 button_mask_ 中，并通过 "buttons <mask>\n" 发送
 *    - 支持从 Arduino 接收按键消息，格式为 "BD:<id>\n"（按下）和 "BU:<id>\n"（松开）
 *
 * 关键设计：
 *   - 串口写入操作受互斥锁（write_mutex_）保护，确保线程安全
 *   - 独立的监听线程持续读取串口数据，解析 Arduino 返回的按键状态消息
 *   - 对 Legacy 协议的大幅度鼠标移动进行自动分割，避免超出 Arduino 处理能力
 ******************************************************************************/

#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <windows.h>
#include <iostream>
#include <vector>
#include <algorithm>

#include "Xen.h"
#include "Arduino.h"

/**
 * 构造函数：初始化并打开串口连接
 *
 * 功能说明：
 *   尝试打开指定的串口端口并设置波特率。连接成功后，标记 is_open_ 为 true；
 *   若启用了键盘功能（arduino_enable_keys）或使用 Teensy41 协议，则自动启动
 *   监听线程以接收来自 Arduino 的按键状态消息。
 *
 * 参数：
 *   port       - 串口名称，例如 "COM3" 或 "/dev/ttyACM0"
 *   baud_rate  - 波特率，例如 9600、115200 等
 *   protocol   - 通信协议，Legacy（默认）或 Teensy41
 */
Arduino::Arduino(const std::string& port, unsigned int baud_rate, ArduinoProtocol protocol)
    : protocol_(protocol),       // 通信协议类型
      button_mask_(0),           // 按键掩码初始化为 0（所有按键松开状态）
      is_open_(false),           // 串口连接状态，初始为未连接
      timer_running_(false),     // 定时器线程运行标志
      listening_(false),         // 监听线程运行标志
      aiming_active(false),      // 瞄准按键激活状态
      shooting_active(false),    // 射击按键激活状态
      zooming_active(false)      // 缩放按键激活状态
{
    try
    {
        serial_.setPort(port);
        serial_.setBaudrate(baud_rate);
        serial_.open();

        if (serial_.isOpen())
        {
            is_open_ = true;
            std::cout << "[Arduino] Connected! PORT: " << port << std::endl;

            if (config.arduino_enable_keys || protocol_ == ArduinoProtocol::Teensy41)
            {
                startListening();
            }
        }
        else
        {
            std::cerr << "[Arduino] Unable to connect to the port: " << port << std::endl;
        }
    }
    catch (std::exception& e)
    {
        std::cerr << "[Arduino] Error: " << e.what() << std::endl;
    }
}

/**
 * 析构函数：关闭串口并清理线程资源
 *
 * 功能说明：
 *   先设置监听标志为 false 以通知监听线程退出，然后关闭串口连接，
 *   最后等待监听线程安全结束。确保程序退出时不会留下悬挂线程或未关闭的串口。
 */
Arduino::~Arduino()
{
    listening_ = false;
    if (serial_.isOpen())
    {
        try { serial_.close(); }
        catch (...) {}
    }
    if (listening_thread_.joinable())
    {
        listening_thread_.join();
    }
    is_open_ = false;
}

/**
 * 检查串口是否已成功打开并处于连接状态
 *
 * 返回值：
 *   true  - 串口已连接并可用
 *   false - 串口未连接或已断开
 */
bool Arduino::isOpen() const
{
    return is_open_;
}

/**
 * 向串口写入数据（线程安全）
 *
 * 功能说明：
 *   使用互斥锁保护写入操作，确保在多线程环境下（例如主线程调用 move()
 *   的同时监听线程也在工作）不会发生数据竞争。写入失败时静默吞掉异常。
 *
 * 参数：
 *   data - 要发送的字符串数据
 */
void Arduino::write(const std::string& data)
{
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (is_open_)
    {
        try
        {
            serial_.write(data);
        }
        catch (...)
        {

        }
    }
}

/**
 * 从串口读取一行数据
 *
 * 功能说明：
 *   从串口缓冲区中读取一行以换行符结尾的数据。若读取过程中发生异常
 *   （例如串口已断开），则将 is_open_ 置为 false 并返回空字符串。
 *
 * 返回值：
 *   读取到的字符串，若串口未打开或读取失败则返回空字符串
 */
std::string Arduino::read()
{
    if (!is_open_)
        return std::string();

    std::string result;
    try
    {
        result = serial_.readline(65536, "\n");
    }
    catch (...)
    {
        is_open_ = false;
    }
    return result;
}

/**
 * 执行鼠标点击操作
 *
 * 功能说明：
 *   根据当前协议执行点击：
 *   - Teensy41 协议：通过设置 bit 0（按下）然后清除 bit 0（松开）来模拟点击
 *   - Legacy 协议：发送文本命令 "c\n"
 */
void Arduino::click()
{
    if (protocol_ == ArduinoProtocol::Teensy41)
    {
        press();
        release();
        return;
    }

    sendCommand("c");
}

/**
 * 执行鼠标按键按下操作
 *
 * 功能说明：
 *   根据当前协议执行按下：
 *   - Teensy41 协议：将 button_mask_ 的 bit 0 置 1（左键按下），然后发送掩码
 *   - Legacy 协议：发送文本命令 "p\n"
 */
void Arduino::press()
{
    if (protocol_ == ArduinoProtocol::Teensy41)
    {
        button_mask_ |= 1;
        sendButtons();
        return;
    }

    sendCommand("p");
}

/**
 * 执行鼠标按键松开操作
 *
 * 功能说明：
 *   根据当前协议执行松开：
 *   - Teensy41 协议：将 button_mask_ 的 bit 0 置 0（左键松开），然后发送掩码
 *   - Legacy 协议：发送文本命令 "r\n"
 */
void Arduino::release()
{
    if (protocol_ == ArduinoProtocol::Teensy41)
    {
        button_mask_ &= static_cast<uint8_t>(~1u);
        sendButtons();
        return;
    }

    sendCommand("r");
}

/**
 * 执行鼠标移动操作
 *
 * 功能说明：
 *   根据当前协议发送鼠标移动指令：
 *   - Teensy41 协议：发送 "move x y 0 0\n" 格式的命令
 *   - Legacy 协议 + 16位模式：发送 "m x,y\n"，支持 -32768 ~ 32767 范围
 *   - Legacy 协议 + 非16位模式：使用 splitValue 将大幅移动拆分为多个
 *     不超过 127 的小块，逐块发送 "m x,y\n"
 *
 *   当 x 和 y 均为 0 时直接返回，避免发送无效的空移动命令。
 *
 * 参数：
 *   x - X 轴移动量（正数向右，负数向左）
 *   y - Y 轴移动量（正数向下，负数向上）
 */
void Arduino::move(int x, int y)
{
    if (!is_open_)
        return;

    if (x == 0 && y == 0)
    {
        return;
    }

    if (protocol_ == ArduinoProtocol::Teensy41)
    {
        write("move " + std::to_string(x) + " " + std::to_string(y) + " 0 0\n");
        return;
    }

    if (config.arduino_16_bit_mouse)
    {
        std::string data = "m" + std::to_string(x) + "," + std::to_string(y) + "\n";
        write(data);
    }
    else
    {
        std::vector<int> x_parts = splitValue(x);
        std::vector<int> y_parts = splitValue(y);

        size_t max_splits = std::max(x_parts.size(), y_parts.size());
        while (x_parts.size() < max_splits) x_parts.push_back(0);
        while (y_parts.size() < max_splits) y_parts.push_back(0);

        for (size_t i = 0; i < max_splits; ++i)
        {
            std::string data = "m" + std::to_string(x_parts[i]) + "," + std::to_string(y_parts[i]) + "\n";
            write(data);
        }
    }
}

/**
 * 发送单行文本命令到串口
 *
 * 功能说明：
 *   在命令字符串末尾追加换行符后通过串口发送。Arduino 端通常以换行符
 *   作为命令结束的标志进行解析。
 *
 * 参数：
 *   command - 命令字符串（不含换行符）
 */
void Arduino::sendCommand(const std::string& command)
{
    write(command + "\n");
}

/**
 * 发送按键状态掩码到串口
 *
 * 功能说明：
 *   将当前 button_mask_ 的值以 "buttons <mask>\n" 格式发送给 Arduino。
 *   mask 是一个 8 位无符号整数，每一位代表一个按键的按下/松开状态。
 */
void Arduino::sendButtons()
{
    write("buttons " + std::to_string(button_mask_) + "\n");
}

/**
 * 将较大的鼠标移动值分割为多个不超过 127 的小块
 *
 * 功能说明：
 *   由于部分 Arduino 固件对单次鼠标移动有 127 的最大值限制（这是 USB HID
 *   协议中鼠标位移字段的标准限制），当需要移动超过 127 的距离时，将该值
 *   分割为若干段，每段最大 127，符号保持与原值一致。
 *
 *   例如：value = 300 → 分割为 [127, 127, 46]
 *         value = -300 → 分割为 [-127, -127, -46]
 *         value = 0 → 分割为 [0]
 *
 * 参数：
 *   value - 原始移动量（正数或负数）
 *
 * 返回值：
 *   包含分割后各小块数值的 vector，每个元素的绝对值不超过 127
 */
std::vector<int> Arduino::splitValue(int value)
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
 * 定时器线程函数：定期检查并维护监听线程的状态
 *
 * 功能说明：
 *   该函数在独立线程中运行，每 100 毫秒检查一次配置状态：
 *   - 如果启用了按键功能（arduino_enable_keys）或使用 Teensy41 协议，
 *     且监听线程未启动，则调用 startListening() 启动监听
 *   - 如果按键功能被禁用且不是 Teensy41 协议，但监听线程仍在运行，
 *     则设置 listening_ = false 并等待线程结束
 *
 *   这种设计确保监听线程的生命周期与配置状态保持同步。
 */
void Arduino::timerThreadFunc()
{
    while (timer_running_)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!is_open_)
            continue;

        bool arduino_enable_keys_local;
        {
            arduino_enable_keys_local = config.arduino_enable_keys;
        }

        if (arduino_enable_keys_local || protocol_ == ArduinoProtocol::Teensy41)
        {
            if (!listening_)
            {
                startListening();
            }
        }
        else
        {
            if (listening_)
            {
                listening_ = false;
                if (listening_thread_.joinable())
                {
                    listening_thread_.join();
                }
            }
        }
    }
}

/**
 * 启动监听线程
 *
 * 功能说明：
 *   将 listening_ 标志置为 true，若已有监听线程在运行则先等待其结束，
 *   然后创建新的线程执行 listeningThreadFunc 函数，持续读取串口返回的数据。
 */
void Arduino::startListening()
{
    listening_ = true;
    if (listening_thread_.joinable())
        listening_thread_.join();

    listening_thread_ = std::thread(&Arduino::listeningThreadFunc, this);
}

/**
 * 监听线程函数：持续读取串口数据并解析按键消息
 *
 * 功能说明：
 *   循环检查串口是否有可用数据。当有数据到达时，读取并追加到内部缓冲区中，
 *   然后按换行符分割提取完整行，每行交由 processIncomingLine 处理。
 *   如果无数据，短暂休眠 10 毫秒以降低 CPU 占用。
 *   如果读取过程中发生异常（如串口断开），设置 is_open_ = false 并退出循环。
 *
 *   消息格式说明：
 *     - "BD:<buttonId>\n"  表示指定 ID 的按键被按下（Button Down）
 *     - "BU:<buttonId>\n"  表示指定 ID 的按键被松开（Button Up）
 */
void Arduino::listeningThreadFunc()
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
 * 处理来自 Arduino 的按键状态消息
 *
 * 功能说明：
 *   解析以 "BD:"（按键按下）或 "BU:"（按键松开）为前缀的文本行，
 *   提取按键 ID，然后更新对应的按键状态标志和原子变量。
 *
 *   按键 ID 映射规则：
 *     ID 1 → shooting_active / shooting   （射击/左键）
 *     ID 2 → 在 Teensy41 协议下映射为 zooming（缩放），
 *             在 Legacy 协议下映射为 aiming（瞄准）
 *     ID 5 → aiming_active / aiming       （瞄准/侧键）
 *     其他 ID → 忽略
 *
 * 参数：
 *   line - 从串口读取的一行文本，不含末尾换行符
 */
void Arduino::processIncomingLine(const std::string& line)
{
    try
    {
        auto applyButtonState = [&](uint16_t buttonId, bool pressed)
        {
            switch (buttonId)
            {
            case 1:
                shooting_active = pressed;
                shooting.store(pressed);
                break;
            case 2:
                if (protocol_ == ArduinoProtocol::Teensy41)
                {
                    zooming_active = pressed;
                    zooming.store(pressed);
                }
                else
                {
                    aiming_active = pressed;
                    aiming.store(pressed);
                }
                break;
            case 5:
                aiming_active = pressed;
                aiming.store(pressed);
                break;
            default:
                break;
            }
        };

        if (line.rfind("BD:", 0) == 0)
        {
            uint16_t buttonId = static_cast<uint16_t>(std::stoi(line.substr(3)));
            applyButtonState(buttonId, true);
        }
        else if (line.rfind("BU:", 0) == 0)
        {
            uint16_t buttonId = static_cast<uint16_t>(std::stoi(line.substr(3)));
            applyButtonState(buttonId, false);
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Arduino] Error processing line '" << line << "': " << e.what() << std::endl;
    }
}
