#ifndef KMBOX_NET_H
#define KMBOX_NET_H
#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <Windows.h>
#include <stdio.h>

#include <cmath>
#pragma warning(disable : 4996)

// 命令代码
#define     cmd_connect         0xaf3c2828   ///< 连接到盒子
#define     cmd_mouse_move      0xaede7345   ///< 鼠标移动
#define     cmd_mouse_left      0x9823AE8D   ///< 鼠标左键控制
#define     cmd_mouse_middle    0x97a3AE8D   ///< 鼠标中键控制
#define     cmd_mouse_right     0x238d8212   ///< 鼠标右键控制
#define     cmd_mouse_wheel     0xffeead38   ///< 鼠标滚轮控制
#define     cmd_mouse_automove  0xaede7346   ///< 模拟人工鼠标移动
#define     cmd_keyboard_all    0x123c2c2f   ///< 键盘参数控制
#define     cmd_reboot          0xaa8855aa   ///< 盒子重启
#define     cmd_bazerMove       0xa238455a   ///< 贝塞尔曲线鼠标移动
#define     cmd_monitor         0x27388020   ///< 监控物理鼠标/键盘数据
#define     cmd_debug           0x27382021   ///< 启用调试信息
#define     cmd_mask_mouse      0x23234343   ///< 屏蔽物理鼠标/键盘
#define     cmd_unmask_all      0x23344343   ///< 取消物理鼠标/键盘屏蔽
#define     cmd_setconfig       0x1d3d3323   ///< 设置 IP 配置信息
#define     cmd_showpic         0x12334883   ///< 显示图片

extern SOCKET sockClientfd; ///< Socket 通信句柄

/** @brief 命令头部结构 */
typedef struct
{
    unsigned int  mac;        ///< 盒子 MAC 地址（必需）
    unsigned int  rand;       ///< 随机值
    unsigned int  indexpts;   ///< 时间戳
    unsigned int  cmd;        ///< 命令代码
} cmd_head_t;

/** @brief 字节数据缓冲区（1024 字节） */
typedef struct
{
    unsigned char buff[1024];	//
}cmd_data_t;

/** @brief 16 位数据缓冲区（512 字） */
typedef struct
{
    unsigned short buff[512];	//
}cmd_u16_t;

/** @brief 鼠标数据结构 */
typedef struct
{
    int button;             ///< 按钮状态
    int x;                  ///< X 坐标
    int y;                  ///< Y 坐标
    int wheel;              ///< 滚轮值
    int point[10];          ///< 贝塞尔曲线控制点（最多保留 5 阶导数的空间）
} soft_mouse_t;

/** @brief 键盘数据结构 */
typedef struct
{
    char ctrl;              ///< 控制键
    char resvel;            ///< 保留
    char button[10];        ///< 按键数据
} soft_keyboard_t;

/** @brief 联合数据结构（包含命令头部和多种数据载荷） */
typedef struct
{
    cmd_head_t head;            ///< 命令头部
    union {
        cmd_data_t      u8buff;         ///< 字节缓冲区
        cmd_u16_t       u16buff;        ///< 16 位缓冲区
        soft_mouse_t    cmd_mouse;      ///< 鼠标命令
        soft_keyboard_t cmd_keyboard;   ///< 键盘命令
    };
} client_tx;

/** @brief 错误码枚举 */
enum
{
    err_creat_socket = -9000,   ///< 创建 Socket 失败
    err_net_version,            ///< Socket 版本错误
    err_net_tx,                 ///< Socket 发送失败
    err_net_rx_timeout,         ///< Socket 接收超时
    err_net_cmd,                ///< 命令错误
    err_net_pts,                ///< 时间戳错误
    success = 0,                ///< 成功
    usb_dev_tx_timeout,         ///< USB 设备发送失败
};

/**
 * @brief 连接到 kmboxNet 盒子
 * @param ip   盒子 IP 地址（屏幕上显示）
 * @param port 通信端口号（屏幕上显示）
 * @param mac  盒子 MAC 地址（屏幕上显示）
 * @return 0 表示连接成功，其他值见错误码
 */
int kmNet_init(char* ip, char* port, char* mac);
/** @brief 关闭 kmboxNet 连接 */
void kmNet_close();
/** @brief 鼠标相对移动 (x, y) */
int kmNet_mouse_move(short x, short y);
/** @brief 鼠标左键控制 */
int kmNet_mouse_left(int isdown);
/** @brief 鼠标右键控制 */
int kmNet_mouse_right(int isdown);
/** @brief 鼠标中键控制 */
int kmNet_mouse_middle(int isdown);
/** @brief 鼠标滚轮控制 */
int kmNet_mouse_wheel(int wheel);
/** @brief 鼠标综合控制（按钮、坐标、滚轮） */
int kmNet_mouse_all(int button, int x, int y, int wheel);
/** @brief 模拟人工鼠标移动（指定时间） */
int kmNet_mouse_move_auto(int x, int y, int time_ms);
/** @brief 二阶贝塞尔曲线鼠标移动 */
int kmNet_mouse_move_beizer(int x, int y, int ms, int x1, int y1, int x2, int y2);

// 键盘功能
int kmNet_keydown(int vkey);   ///< 按下虚拟键
int kmNet_keyup(int vkey);     ///< 释放虚拟键

// 监控系列
int kmNet_monitor(short port);                    ///< 启用/禁用物理鼠标/键盘监控
int kmNet_monitor_mouse_left();                   ///< 查询物理鼠标左键状态
int kmNet_monitor_mouse_middle();                 ///< 查询物理鼠标中键状态
int kmNet_monitor_mouse_right();                  ///< 查询物理鼠标右键状态
int kmNet_monitor_mouse_side1();                  ///< 查询物理鼠标侧键 1 状态
int kmNet_monitor_mouse_side2();                  ///< 查询物理鼠标侧键 2 状态
int kmNet_monitor_keyboard(short vk_key);         ///< 查询指定键盘按键状态

// 物理鼠标/键盘屏蔽系列
int kmNet_mask_mouse_left(int enable);            ///< 屏蔽鼠标左键
int kmNet_mask_mouse_right(int enable);           ///< 屏蔽鼠标右键
int kmNet_mask_mouse_middle(int enable);          ///< 屏蔽鼠标中键
int kmNet_mask_mouse_side1(int enable);           ///< 屏蔽鼠标侧键 1
int kmNet_mask_mouse_side2(int enable);           ///< 屏蔽鼠标侧键 2
int kmNet_mask_mouse_x(int enable);               ///< 屏蔽鼠标 X 轴
int kmNet_mask_mouse_y(int enable);               ///< 屏蔽鼠标 Y 轴
int kmNet_mask_mouse_wheel(int enable);           ///< 屏蔽鼠标滚轮
int kmNet_mask_keyboard(short vkey);              ///< 屏蔽指定键盘按键
int kmNet_unmask_keyboard(short vkey);            ///< 取消屏蔽指定键盘按键
int kmNet_unmask_all();                           ///< 取消所有已设置的物理屏蔽

// 配置功能
/** @brief 重启盒子 */
int kmNet_reboot(void);
/** @brief 配置盒子 IP 地址 */
int kmNet_setconfig(char* ip, unsigned short port);
/** @brief 启用调试 */
int kmNet_debug(short port, char enable);
/** @brief 用指定颜色填充整个 LCD 屏幕，使用黑色清除屏幕 */
int kmNet_lcd_color(unsigned short rgb565);
/** @brief 在 LCD 底部显示 128x80 图片 */
int kmNet_lcd_picture_bottom(unsigned char* buff_128_80);
/** @brief 在 LCD 全屏显示 128x160 图片 */
int kmNet_lcd_picture(unsigned char* buff_128_160);
#endif // KMBOX_NET_H
