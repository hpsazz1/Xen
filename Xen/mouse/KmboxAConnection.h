#pragma once

#include <string>

/**
 * @brief Kmbox A 型设备连接管理类
 *
 * 封装 Kmbox A（老版本）设备的通信接口，
 * 支持鼠标移动、按键点击和滚轮操作。
 */
class KmboxAConnection
{
public:
    /**
     * @brief 构造函数
     * @param pidvid "VID:PID" 格式的设备标识字符串
     */
    explicit KmboxAConnection(const std::string& pidvid);
    ~KmboxAConnection();

    /** @brief 设备是否已打开 */
    bool isOpen() const { return is_open_; }

    /** @brief 鼠标相对移动 (x, y) */
    void move(int x, int y);
    /** @brief 按下左键 */
    void leftDown();
    /** @brief 释放左键 */
    void leftUp();
    /** @brief 按下右键 */
    void rightDown();
    /** @brief 释放右键 */
    void rightUp();
    /** @brief 按下中键 */
    void middleDown();
    /** @brief 释放中键 */
    void middleUp();
    /** @brief 滚轮滚动 */
    void wheel(int delta);

private:
    /** @brief 从 "VID:PID" 字符串解析出厂商 ID 和产品 ID */
    static bool parsePidVid(const std::string& pidvid, unsigned short& pid, unsigned short& vid);

    bool is_open_;  ///< 设备是否已打开
};

