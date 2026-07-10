#pragma once
#include <string>
#include <atomic>
#include <mutex>
#include <thread>

#include "kmbox_net/kmboxNet.h"

/**
 * @brief Kmbox Net 网络连接管理类
 *
 * 封装 Kmbox Net（网络版）设备的通信接口。
 * 支持 TCP 网络通信、鼠标键盘控制、物理设备监控
 * 以及 LCD 屏幕显示等功能。
 */
class KmboxNetConnection
{
public:
    /**
     * @brief 构造函数
     * @param ip 设备 IP 地址
     * @param port 设备端口号
     * @param uuid 设备 UUID
     */
    KmboxNetConnection(const std::string& ip, const std::string& port, const std::string& uuid);
    ~KmboxNetConnection();

    /** @brief 监控线程主函数 */
    void monitorThread();
    /** @brief 设备是否已打开 */
    bool isOpen() const { return is_open_; }

    /** @brief 鼠标相对移动 (x, y) */
    void move(int x, int y);
    /** @brief 模拟人工鼠标移动（指定时间） */
    void moveAuto(int x, int y, int ms);
    /** @brief 贝塞尔曲线鼠标移动 */
    void moveBezier(int x, int y, int ms, int x1, int y1, int x2, int y2);
    /** @brief 左键按下 */
    void leftDown();
    /** @brief 左键释放 */
    void leftUp();
    /** @brief 右键按下 */
    void rightDown();
    /** @brief 右键释放 */
    void rightUp();
    /** @brief 中键按下 */
    void middleDown();
    /** @brief 中键释放 */
    void middleUp();
    /** @brief 滚轮滚动 */
    void wheel(int wheel);
    /** @brief 鼠标综合控制（按钮、坐标、滚轮） */
    void mouseAll(int button, int x, int y, int wheel);

    /** @brief 按下虚拟键 */
    void keyDown(int vkey);
    /** @brief 释放虚拟键 */
    void keyUp(int vkey);

    /** @brief 启用物理设备监控 */
    void monitor(short port);
    /** @brief 查询物理鼠标左键状态 */
    int monitorMouseLeft();
    /** @brief 查询物理鼠标右键状态 */
    int monitorMouseRight();
    /** @brief 查询物理鼠标中键状态 */
    int monitorMouseMiddle();
    /** @brief 查询物理鼠标侧键 1 状态 */
    int monitorMouseSide1();
    /** @brief 查询物理鼠标侧键 2 状态 */
    int monitorMouseSide2();
    /** @brief 查询指定键盘按键状态 */
    int monitorKeyboard(short vkey);

    /** @brief 屏蔽物理鼠标左键 */
    void maskMouseLeft(bool enable);
    /** @brief 屏蔽物理鼠标右键 */
    void maskMouseRight(bool enable);
    /** @brief 屏蔽物理鼠标中键 */
    void maskMouseMiddle(bool enable);
    /** @brief 屏蔽物理鼠标侧键 1 */
    void maskMouseSide1(bool enable);
    /** @brief 屏蔽物理鼠标侧键 2 */
    void maskMouseSide2(bool enable);
    /** @brief 屏蔽物理鼠标 X 轴 */
    void maskMouseX(bool enable);
    /** @brief 屏蔽物理鼠标 Y 轴 */
    void maskMouseY(bool enable);
    /** @brief 屏蔽物理鼠标滚轮 */
    void maskMouseWheel(bool enable);
    /** @brief 屏蔽指定键盘按键 */
    void maskKeyboard(short vkey);
    /** @brief 取消屏蔽指定键盘按键 */
    void unmaskKeyboard(short vkey);
    /** @brief 取消所有屏蔽 */
    void unmaskAll();

    /** @brief 重启设备 */
    void reboot();
    /** @brief 设置设备 IP 配置 */
    void setConfig(const std::string& ip, unsigned short port);
    /** @brief 启用调试信息输出 */
    void debug(short port, char enable);

    /** @brief LCD 填充指定颜色 */
    void lcdColor(unsigned short rgb565);
    /** @brief LCD 底部显示 128x80 图片 */
    void lcdPictureBottom(unsigned char* buff_128_80);
    /** @brief LCD 全屏显示 128x160 图片 */
    void lcdPicture(unsigned char* buff_128_160);

    std::atomic<bool> aiming_active{ false };    ///< 物理鼠标是否正在瞄准
    std::atomic<bool> shooting_active{ false };  ///< 物理鼠标是否正在射击
    std::atomic<bool> zooming_active{ false };   ///< 物理鼠标是否正在缩放

private:
    bool is_open_ = false;              ///< 连接是否已打开
    bool monitor_ = false;              ///< 监控是否已启用
    std::thread monitor_thread_;        ///< 监控线程
    std::atomic<bool> monitor_running_{ false };  ///< 监控线程运行标志
    std::string ip_, port_, uuid_;      ///< 连接参数
};
