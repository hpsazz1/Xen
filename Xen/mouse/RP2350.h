#ifndef RP2350_H
#define RP2350_H

#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "serial/serial.h"

/**
 * @brief RP2350 微控制器鼠标控制类
 *
 * 通过串口与 RP2350（Raspberry Pi Pico 2）设备通信，
 * 实现鼠标移动、按键点击以及物理按钮状态监听。
 */
class RP2350
{
public:
    /**
     * @brief 构造函数
     * @param port 串口端口名
     * @param baud_rate 波特率
     */
    RP2350(const std::string& port, unsigned int baud_rate);
    ~RP2350();

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

    std::atomic<bool> aiming_active;    ///< 是否正在瞄准（来自物理按钮）
    std::atomic<bool> shooting_active;  ///< 是否正在射击（来自物理按钮）
    std::atomic<bool> zooming_active;   ///< 是否正在缩放（来自物理按钮）

private:
    /** @brief 发送命令到串口 */
    void sendCommand(const std::string& command);
    /** @brief 将鼠标移动值拆分为多个部分 */
    std::vector<int> splitValue(int value);

    /** @brief 启动串口监听线程 */
    void startListening();
    /** @brief 监听线程函数 */
    void listeningThreadFunc();
    /** @brief 处理接收到的行数据 */
    void processIncomingLine(const std::string& line);

    serial::Serial serial_;                   ///< 串口对象
    std::atomic<bool> is_open_;               ///< 串口是否已打开

    std::thread listening_thread_;            ///< 监听线程
    std::atomic<bool> listening_;             ///< 监听线程运行标志
    std::mutex write_mutex_;                  ///< 写入互斥锁
};

#endif // RP2350_H
