#ifndef MAKCU_CONNECTION_H
#define MAKCU_CONNECTION_H

#include <string>
#include <atomic>
#include <mutex>

#include "../modules/makcu/include/makcu.h"

/**
 * @brief Makcu 设备连接管理类
 *
 * 封装 Makcu HID 鼠标设备的通信接口，
 * 支持鼠标移动、按键点击和状态查询。
 */
class MakcuConnection
{
public:
    /**
     * @brief 构造函数
     * @param port 串口端口名
     * @param baud_rate 波特率
     */
    MakcuConnection(const std::string& port, unsigned int baud_rate);
    ~MakcuConnection();

    /** @brief 设备是否已打开 */
    bool isOpen() const;

    /** @brief 点击鼠标按键 */
    void click(int button);
    /** @brief 按下鼠标按键 */
    void press(int button);
    /** @brief 释放鼠标按键 */
    void release(int button);
    /** @brief 鼠标相对移动 (x, y) */
    void move(int x, int y);

    bool aiming_active;    ///< 是否正在瞄准
    bool shooting_active;  ///< 是否正在射击
    bool zooming_active;   ///< 是否正在缩放

private:
    /** @brief 按钮回调函数，处理设备上报的按钮事件 */
    void onButtonCallback(makcu::MouseButton button, bool pressed);

private:
    makcu::Device device_;                ///< Makcu 设备对象
    std::atomic<bool> is_open_;           ///< 设备是否已打开
    std::mutex write_mutex_;              ///< 写入互斥锁
};

#endif // MAKCU_CONNECTION_H