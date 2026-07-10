#ifndef ARDUINO_H
#define ARDUINO_H

#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstdint>

#include "serial/serial.h"

/**
 * @brief Arduino 通信协议枚举
 *
 * 定义与 Arduino 通信的协议版本。
 */
enum class ArduinoProtocol
{
    Legacy,    ///< 旧版协议
    Teensy41   ///< Teensy 4.1 协议
};

/**
 * @brief Arduino 鼠标控制类
 *
 * 通过串口与 Arduino 设备通信，实现鼠标移动、按键点击
 * 以及物理按钮状态监听。支持 Legacy 和 Teensy 4.1 两种协议。
 */
class Arduino
{
public:
    /**
     * @brief 构造函数
     * @param port 串口端口名
     * @param baud_rate 波特率
     * @param protocol 通信协议（默认为 Legacy）
     */
    Arduino(const std::string& port, unsigned int baud_rate, ArduinoProtocol protocol = ArduinoProtocol::Legacy);
    ~Arduino();

    /** @brief 串口是否已打开 */
    bool isOpen() const;

    /** @brief 向串口写入数据 */
    void write(const std::string& data);
    /** @brief 从串口读取数据 */
    std::string read();

    /** @brief 鼠标左键点击 */
    void click();
    /** @brief 按下鼠标左键 */
    void press();
    /** @brief 释放鼠标左键 */
    void release();
    /** @brief 鼠标相对移动 (x, y) */
    void move(int x, int y);

    bool aiming_active;    ///< 是否正在瞄准（来自物理按钮）
    bool shooting_active;  ///< 是否正在射击（来自物理按钮）
    bool zooming_active;   ///< 是否正在缩放（来自物理按钮）

private:
    /** @brief 发送命令到串口 */
    void sendCommand(const std::string& command);
    /** @brief 发送按钮状态数据 */
    void sendButtons();
    /** @brief 将鼠标移动值拆分为多个部分（用于协议传输） */
    std::vector<int> splitValue(int value);

    /** @brief 启动定时器线程 */
    void startTimer();
    /** @brief 启动串口监听线程 */
    void startListening();
    /** @brief 处理从串口接收到的行数据 */
    void processIncomingLine(const std::string& line);

    /** @brief 定时器线程函数 */
    void timerThreadFunc();
    /** @brief 监听线程函数 */
    void listeningThreadFunc();
    std::mutex write_mutex_;   ///< 写入互斥锁

private:
    serial::Serial serial_;               ///< 串口对象
    ArduinoProtocol protocol_;            ///< 通信协议
    uint8_t button_mask_ = 0;             ///< 按钮掩码
    std::atomic<bool> is_open_;           ///< 串口是否已打开

    std::thread timer_thread_;            ///< 定时器线程
    std::atomic<bool> timer_running_;     ///< 定时器线程运行标志

    std::thread listening_thread_;        ///< 监听线程
    std::atomic<bool> listening_;         ///< 监听线程运行标志
};

#endif // ARDUINO_H
