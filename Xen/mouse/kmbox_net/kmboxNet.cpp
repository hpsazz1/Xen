/**
 * kmboxNet.cpp - Kmbox Net 核心网络协议实现
 *
 * 本文件实现了基于 UDP 的 Kmbox Net 硬件通信协议，提供以下功能：
 * - 鼠标控制：相对移动、按键操作（左/中/右键）、滚轮控制、一键全发、
 *   自动平滑移动模拟、贝塞尔曲线轨迹移动
 * - 键盘控制：按键按下/释放，支持 10 键同时按下的队列管理
 * - 物理设备监控：监听并获取物理鼠标/键盘的实时状态
 * - 输入屏蔽：可选择性地屏蔽物理鼠标的按键、X/Y 轴、滚轮及键盘按键
 * - LCD 显示：控制设备 LCD 屏幕颜色填充和图片显示
 * - 设备配置：修改设备的 IP 地址和端口号
 * - 调试功能：启用设备内部调试打印输出
 */

#include <time.h>

#include "kmbox_net/kmboxNet.h"
#include "kmbox_net/HidTable.h"

/* 监控线程状态常量：正常运行 */
#define monitor_ok    2
/* 监控线程状态常量：已退出 */
#define monitor_exit  0

/* 鼠标和键盘网络通信的 UDP 套接字句柄 */
SOCKET sockClientfd = 0;
/* 物理设备监控的 UDP 套接字句柄 */
SOCKET sockMonitorfd = 0;
/* 待发送给设备的指令数据缓冲区 */
client_tx tx;
/* 从设备接收到的响应数据缓冲区 */
client_tx rx;
/* 设备（Kmbox Net 硬件）的网络地址信息 */
SOCKADDR_IN addrSrv;
/* 软件鼠标状态数据，保存当前鼠标按键、坐标、滚轮值 */
soft_mouse_t    softmouse;
/* 软件键盘状态数据，保存当前键盘控制键和普通按键队列 */
soft_keyboard_t softkeyboard;
/* 物理鼠标/键盘监控运行状态标志。0=未运行，monitor_ok=运行中 */
static int monitor_run = 0;
/* 鼠标/键盘屏蔽标志位，按位记录各屏蔽项的启用状态 */
static int mask_keyboard_mouse_flag = 0;
/* 监控监听端口号，用于接收物理设备上报的数据 */
static short monitor_port = 0;

/* 匿名命名空间：内部辅助工具函数，不对外部暴露 */
namespace
{
/* 初始连接阶段的接收超时时间（毫秒） */
constexpr DWORD kInitialReceiveTimeoutMs = 1000;
/* 普通命令的接收超时时间（毫秒） */
constexpr DWORD kCommandReceiveTimeoutMs = 300;
/* 监控数据接收的超时时间（毫秒） */
constexpr DWORD kMonitorReceiveTimeoutMs = 100;

/**
 * 检查套接字是否有效
 * @param socket 待检查的套接字句柄
 * @return true 表示套接字有效，false 表示无效
 */
bool IsSocketValid(SOCKET socket)
{
	return socket != 0 && socket != INVALID_SOCKET;
}

/**
 * 关闭套接字并将句柄置零
 * @param socket 待关闭的套接字句柄（引用，会被修改为 0）
 */
void CloseSocket(SOCKET& socket)
{
	if (IsSocketValid(socket))
		closesocket(socket);
	socket = 0;
}

/**
 * 设置套接字的接收超时时间
 * @param socket 目标套接字
 * @param timeoutMs 超时时间，单位毫秒
 * @return true 设置成功，false 设置失败
 */
bool SetReceiveTimeout(SOCKET socket, DWORD timeoutMs)
{
	return setsockopt(
		socket,
		SOL_SOCKET,
		SO_RCVTIMEO,
		reinterpret_cast<const char*>(&timeoutMs),
		sizeof(timeoutMs)) != SOCKET_ERROR;
}

/**
 * 设置套接字的发送超时时间
 * @param socket 目标套接字
 * @param timeoutMs 超时时间，单位毫秒
 * @return true 设置成功，false 设置失败
 */
bool SetSendTimeout(SOCKET socket, DWORD timeoutMs)
{
	return setsockopt(
		socket,
		SOL_SOCKET,
		SO_SNDTIMEO,
		reinterpret_cast<const char*>(&timeoutMs),
		sizeof(timeoutMs)) != SOCKET_ERROR;
}

/**
 * 将套接字设置为非阻塞模式
 * @param socket 目标套接字
 * @return true 设置成功，false 设置失败
 */
bool SetNonBlocking(SOCKET socket)
{
	u_long nonBlocking = 1;
	return ioctlsocket(socket, FIONBIO, &nonBlocking) != SOCKET_ERROR;
}

/**
 * 带超时控制的 recvfrom 封装函数。
 * 使用 select 机制实现超时检测，避免因网络异常导致线程永久阻塞。
 * @param socket 源套接字
 * @param buffer 接收数据缓冲区
 * @param length 缓冲区大小
 * @param flags 接收标志位
 * @param from 发送方地址信息（输出参数）
 * @param fromLen 地址结构长度（输入输出参数）
 * @param timeoutMs 超时时间，单位毫秒，默认为 kCommandReceiveTimeoutMs
 * @return 成功时返回接收到的字节数，超时或出错返回 SOCKET_ERROR
 */
int RecvFromWithTimeout(
	SOCKET socket,
	char* buffer,
	int length,
	int flags,
	sockaddr* from,
	int* fromLen,
	DWORD timeoutMs = kCommandReceiveTimeoutMs)
{
	if (!IsSocketValid(socket))
	{
		WSASetLastError(WSAENOTSOCK);
		return SOCKET_ERROR;
	}

	fd_set readSet;
	FD_ZERO(&readSet);
	FD_SET(socket, &readSet);

	timeval timeout{};
	timeout.tv_sec = static_cast<long>(timeoutMs / 1000);
	timeout.tv_usec = static_cast<long>((timeoutMs % 1000) * 1000);

	const int ready = select(0, &readSet, nullptr, nullptr, &timeout);
	if (ready <= 0)
	{
		if (ready == 0)
			WSASetLastError(WSAETIMEDOUT);
		return SOCKET_ERROR;
	}

	return ::recvfrom(socket, buffer, length, flags, from, fromLen);
}

/**
 * 清理客户端套接字资源：关闭套接字并执行 Winsock 清理
 */
void CleanupClientSocket()
{
	CloseSocket(sockClientfd);
	WSACleanup();
}
}

#pragma pack(1)
/**
 * 标准鼠标硬件报告结构体，对应 USB HID 鼠标协议规范。
 * 用于存储从物理设备接收到的鼠标状态数据。
 */
typedef struct {
	unsigned char report_id;    // 报告 ID，标识报告类型
	unsigned char buttons;      // 按键状态位，低 8 位对应 8 个物理按键
	short x;                    // X 轴相对位移，范围 -32767 ~ 32767
	short y;                    // Y 轴相对位移，范围 -32767 ~ 32767
	short wheel;                // 滚轮位移量，范围 -32767 ~ 32767
}standard_mouse_report_t;

/**
 * 标准键盘硬件报告结构体，对应 USB HID 键盘协议规范。
 * 用于存储从物理设备接收到的键盘状态数据。
 */
typedef struct {
	unsigned char report_id;    // 报告 ID，标识报告类型
	unsigned char buttons;      // 控制键状态位，8 位对应 8 个功能控制键
	unsigned char data[10];     // 普通按键扫描码队列，最多支持 10 键同时按下
} standard_keyboard_report_t;
#pragma pack()

/* 硬件鼠标状态缓存，由监控线程从设备获取并更新 */
standard_mouse_report_t     hw_mouse;
/* 硬件键盘状态缓存，由监控线程从设备获取并更新 */
standard_keyboard_report_t  hw_keyboard;

/**
 * 生成区间 [a, b) 内的随机整数
 * @param a 区间起点
 * @param b 区间终点
 * @return [min(a,b), max(a,b)) 范围内的随机整数
 */
int myrand(int a, int b)
{
	int min = a < b ? a : b;
	int max = a > b ? a : b;
	return ((rand() % (max - min)) + min);
}

/**
 * 将十六进制字符串转换为 32 位无符号整数
 * 取前 4 个字节（8 个十六进制字符）拼接为一个 32 位整数值
 * @param pbSrc 十六进制字符串指针
 * @param nLen 要处理的十六进制字符对数量
 * @return 转换后的 32 位整数（大端序：pbDest[0] 为最高字节）
 */
unsigned int StrToHex(char* pbSrc, int nLen)
{
	char h1, h2;
	unsigned char s1, s2;
	int i;
	unsigned int pbDest[16] = { 0 };
	for (i = 0; i < nLen; i++) {
		h1 = pbSrc[2 * i];
		h2 = pbSrc[2 * i + 1];
		s1 = toupper(h1) - 0x30;
		if (s1 > 9)
			s1 -= 7;
		s2 = toupper(h2) - 0x30;
		if (s2 > 9)
			s2 -= 7;
		pbDest[i] = s1 * 16 + s2;
	}
	return pbDest[0] << 24 | pbDest[1] << 16 | pbDest[2] << 8 | pbDest[3];
}

/**
 * 网络接收响应校验处理函数。
 * 比较接收到的响应与已发送请求中的命令码和时间戳是否一致，
 * 用于确认响应与请求的对应关系，防止数据错乱。
 * @param rx 从设备接收到的响应数据指针
 * @param tx 已发送的请求数据指针
 * @return 0 表示校验通过，非零值表示对应错误码
 */
int NetRxReturnHandle(client_tx* rx, client_tx* tx)
{
	if (rx->head.cmd != tx->head.cmd)
		return  err_net_cmd;    // 命令码不匹配，返回命令码错误
	if (rx->head.indexpts != tx->head.indexpts)
		return  err_net_pts;    // 时间戳不匹配，返回时间戳错误
	return 0;                   // 校验通过，返回 0 表示成功
	//return  rx->head.rand;    // 实际返回值（预留字段，暂未启用）
}


/**
 * 连接到 Kmbox Net 硬件盒子。
 * 初始化 Winsock 环境，创建 UDP 套接字，发送连接命令，并等待设备响应。
 * @param ip   盒子的 IP 地址（在设备屏幕上显示，如 192.168.2.88）
 * @param port 盒子的通信端口号（在设备屏幕上显示，如 6234）
 * @param mac  盒子的 MAC 地址（在设备屏幕上显示，如 12345）
 * @return 0 表示连接成功，非零值表示对应错误码
 */
int kmNet_init(char* ip, char* port, char* mac)
{
	WORD wVersionRequested; WSADATA wsaData; int err;
	wVersionRequested = MAKEWORD(1, 1);
	err = WSAStartup(wVersionRequested, &wsaData);
	if (err != 0)        return err_creat_socket;
	if (LOBYTE(wsaData.wVersion) != 1 || HIBYTE(wsaData.wVersion) != 1) {
		WSACleanup();
		sockClientfd = 0;
		return err_net_version;
	}
	srand((unsigned)time(NULL));
	sockClientfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (!IsSocketValid(sockClientfd))
	{
		WSACleanup();
		return err_creat_socket;
	}
	if (!SetNonBlocking(sockClientfd) ||
		!SetReceiveTimeout(sockClientfd, kInitialReceiveTimeoutMs) ||
		!SetSendTimeout(sockClientfd, kCommandReceiveTimeoutMs))
	{
		CleanupClientSocket();
		return err_creat_socket;
	}
	memset(&addrSrv, 0, sizeof(addrSrv));
	addrSrv.sin_addr.S_un.S_addr = inet_addr(ip);
	addrSrv.sin_family = AF_INET;
	addrSrv.sin_port = htons(atoi(port)); // 端口号对应 UUID[1] 高 16 位
	tx.head.mac = StrToHex(mac, 4);         // 盒子 MAC 地址，固定为 UUID[1]
	tx.head.rand = rand();                  // 随机混淆值。后续可用于数据包加密，暂未启用
	tx.head.indexpts = 0;                   // 命令统计序号，从 0 开始
	tx.head.cmd = cmd_connect;              // 连接命令
	memset(&softmouse, 0, sizeof(softmouse));       // 清空软件鼠标状态数据
	memset(&softkeyboard, 0, sizeof(softkeyboard)); // 清空软件键盘状态数据
	err = sendto(sockClientfd, (const char*)&tx, sizeof(cmd_head_t), 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	if (err == SOCKET_ERROR)
	{
		CleanupClientSocket();
		return err_net_tx;
	}
	Sleep(20); // 首次连接可能需要较长时间等待设备响应
	int clen = sizeof(addrSrv);
	err = RecvFromWithTimeout(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&addrSrv, &clen, kInitialReceiveTimeoutMs);
	if (err < 0)
	{
		CleanupClientSocket();
		return err_net_rx_timeout;
	}
	err = NetRxReturnHandle(&rx, &tx);
	if (err != success)
		CleanupClientSocket();
	else if (!SetReceiveTimeout(sockClientfd, kCommandReceiveTimeoutMs) ||
		!SetSendTimeout(sockClientfd, kCommandReceiveTimeoutMs))
	{
		CleanupClientSocket();
		return err_creat_socket;
	}
	return err;
}

/**
 * 断开与 Kmbox Net 的连接，释放网络资源。
 * 关闭 UDP 套接字并清理 Winsock 环境。
 */
void kmNet_close()
{
	CleanupClientSocket();
}

/**
 * 鼠标相对移动。一次性移动，无轨迹模拟，速度最快。
 * 适合在使用自定义轨迹算法时调用（仅发送最终位移量）。
 * @param x X 轴相对位移量
 * @param y Y 轴相对位移量
 * @return 0 表示成功，非零值表示对应错误码
 */
int kmNet_mouse_move(short x, short y)
{
	int err;
	if (!IsSocketValid(sockClientfd))       return err_creat_socket;
	tx.head.indexpts++;              // 命令统计序号递增
	tx.head.cmd = cmd_mouse_move;    // 鼠标移动命令
	tx.head.rand = rand();           // 随机混淆值
	softmouse.x = x;
	softmouse.y = y;
	memcpy(&tx.cmd_mouse, &softmouse, sizeof(soft_mouse_t));
	int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	softmouse.x = 0;
	softmouse.y = 0;
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = RecvFromWithTimeout(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}



/**
 * 鼠标左键按下/释放控制
 * @param isdown 0 表示释放，1 表示按下
 * @return 0 表示成功，非零值表示对应错误码
 */
int kmNet_mouse_left(int isdown)
{
	int err;
	if (!IsSocketValid(sockClientfd))       return err_creat_socket;
	tx.head.indexpts++;              // 命令统计序号递增
	tx.head.cmd = cmd_mouse_left;    // 鼠标左键命令
	tx.head.rand = rand();           // 随机混淆值
	softmouse.button = (isdown ? (softmouse.button | 0x01) : (softmouse.button & (~0x01)));
	memcpy(&tx.cmd_mouse, &softmouse, sizeof(soft_mouse_t));
	int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = RecvFromWithTimeout(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}

/**
 * 鼠标中键按下/释放控制
 * @param isdown 0 表示释放，1 表示按下
 * @return 0 表示成功，非零值表示对应错误码
 */
int kmNet_mouse_middle(int isdown)
{
	int err;
	if (!IsSocketValid(sockClientfd))       return err_creat_socket;
	tx.head.indexpts++;              // 命令统计序号递增
	tx.head.cmd = cmd_mouse_middle;  // 鼠标中键命令
	tx.head.rand = rand();           // 随机混淆值
	softmouse.button = (isdown ? (softmouse.button | 0x04) : (softmouse.button & (~0x04)));
	memcpy(&tx.cmd_mouse, &softmouse, sizeof(soft_mouse_t));
	int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = RecvFromWithTimeout(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}

/**
 * 鼠标右键按下/释放控制
 * @param isdown 0 表示释放，1 表示按下
 * @return 0 表示成功，非零值表示对应错误码
 */
int kmNet_mouse_right(int isdown)
{
	int err;
	if (!IsSocketValid(sockClientfd))       return err_creat_socket;
	tx.head.indexpts++;              // 命令统计序号递增
	tx.head.cmd = cmd_mouse_right;   // 鼠标右键命令
	tx.head.rand = rand();           // 随机混淆值
	softmouse.button = (isdown ? (softmouse.button | 0x02) : (softmouse.button & (~0x02)));
	memcpy(&tx.cmd_mouse, &softmouse, sizeof(soft_mouse_t));
	int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = RecvFromWithTimeout(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
			return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}

/**
 * 鼠标滚轮控制
 * @param wheel 滚轮滚动量，正值为向前滚动，负值为向后滚动
 * @return 0 表示成功，非零值表示对应错误码
 */
int kmNet_mouse_wheel(int wheel)
{
	int err;
	if (!IsSocketValid(sockClientfd))       return err_creat_socket;
	tx.head.indexpts++;              // 命令统计序号递增
	tx.head.cmd = cmd_mouse_wheel;   // 鼠标滚轮命令
	tx.head.rand = rand();           // 随机混淆值
	softmouse.wheel = wheel;
	memcpy(&tx.cmd_mouse, &softmouse, sizeof(soft_mouse_t));
	int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);
	softmouse.wheel = 0;
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = RecvFromWithTimeout(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}


/**
 * 鼠标全功能一次性发送控制。在一次调用中同时设置按键、坐标和滚轮值。
 * 发送后自动清零坐标和滚轮，避免影响后续操作。
 * @param button 按键状态值（位标志组合）
 * @param x X 轴相对位移量
 * @param y Y 轴相对位移量
 * @param wheel 滚轮滚动量
 * @return 0 表示成功，非零值表示对应错误码
 */
int kmNet_mouse_all(int button, int x, int y, int wheel)
{
	int err;
	if (!IsSocketValid(sockClientfd))       return err_creat_socket;
	tx.head.indexpts++;              // 命令统计序号递增
	tx.head.cmd = cmd_mouse_wheel;   // 鼠标全功能命令（复用滚轮命令码，但携带完整鼠标数据）
	tx.head.rand = rand();           // 随机混淆值
	softmouse.button = button;
	softmouse.x = x;
	softmouse.y = y;
	softmouse.wheel = wheel;
	memcpy(&tx.cmd_mouse, &softmouse, sizeof(soft_mouse_t));
	int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);
	softmouse.x = 0;
	softmouse.y = 0;
	softmouse.wheel = 0;
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = RecvFromWithTimeout(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}

/**
 * 鼠标自动平滑移动。模拟类人的平滑轨迹，避免被检测为异常鼠标行为。
 * 如果不自行实现轨迹算法，建议使用此函数代替 kmNet_mouse_move。
 * 该函数不会产生跳跃，以最小步长逼近目标，耗时比 kmNet_mouse_move 更长。
 * @param x 目标 X 轴位移量
 * @param y 目标 Y 轴位移量
 * @param ms 期望的移动耗时（毫秒）。注意：不要设置过小，否则仍可能被检测为异常数据。
 *           尽量模拟人类操作。实际耗时可能小于 ms 值。
 * @return 0 表示成功，非零值表示对应错误码
 */
int kmNet_mouse_move_auto(int x, int y, int ms)
{
	int err;
	if (!IsSocketValid(sockClientfd))       return err_creat_socket;
	tx.head.indexpts++;                  // 命令统计序号递增
	tx.head.cmd = cmd_mouse_automove;    // 自动平滑移动命令
	tx.head.rand = ms;                   // 随机混淆值（此处复用于传递移动耗时）
	softmouse.x = x;
	softmouse.y = y;
	memcpy(&tx.cmd_mouse, &softmouse, sizeof(soft_mouse_t));
	int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);
	softmouse.x = 0;                     // 发送后清零 X 轴数据
	softmouse.y = 0;                     // 发送后清零 Y 轴数据
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = RecvFromWithTimeout(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}


/**
 * 二阶贝塞尔曲线鼠标移动控制。
 * 通过两个控制点定义贝塞尔曲线路径，使鼠标沿曲线轨迹移动到目标点。
 * @param x  目标点 X 坐标
 * @param y  目标点 Y 坐标
 * @param ms 移动耗时（毫秒）
 * @param x1 控制点 P1 的 X 坐标
 * @param y1 控制点 P1 的 Y 坐标
 * @param x2 控制点 P2 的 X 坐标
 * @param y2 控制点 P2 的 Y 坐标
 * @return 0 表示成功，非零值表示对应错误码
 */
int kmNet_mouse_move_beizer(int x, int y, int ms, int x1, int y1, int x2, int y2)
{
	int err;
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	tx.head.indexpts++;               // 命令统计序号递增
	tx.head.cmd = cmd_bazerMove;      // 贝塞尔曲线移动命令
	tx.head.rand = ms;                // 随机混淆值（此处复用于传递移动耗时）
	softmouse.x = x;
	softmouse.y = y;
	softmouse.point[0] = x1;
	softmouse.point[1] = y1;
	softmouse.point[2] = x2;
	softmouse.point[3] = y2;
	memcpy(&tx.cmd_mouse, &softmouse, sizeof(soft_mouse_t));
	int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);
	softmouse.x = 0;
	softmouse.y = 0;
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = RecvFromWithTimeout(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}


/**
 * 键盘按键按下事件。
 * 对于控制键（KEY_LEFTCONTROL ~ KEY_RIGHT_GUI），直接设置对应控制位。
 * 对于普通键，先检查是否已在 10 键队列中：
 *   - 如果已存在，直接发送
 *   - 如果不存在且有空位，加入队列
 *   - 如果队列已满，移除最早加入的键后再加入新键
 * @param vk_key 按键虚拟键码（定义于 HidTable.h）
 * @return 0 表示成功，非零值表示对应错误码
 */
int kmNet_keydown(int vk_key)
{
	int i;
	if (vk_key >= KEY_LEFTCONTROL && vk_key <= KEY_RIGHT_GUI) // 控制键
	{
		switch (vk_key)
		{
		case KEY_LEFTCONTROL: softkeyboard.ctrl |= BIT0; break;
		case KEY_LEFTSHIFT:   softkeyboard.ctrl |= BIT1; break;
		case KEY_LEFTALT:     softkeyboard.ctrl |= BIT2; break;
		case KEY_LEFT_GUI:    softkeyboard.ctrl |= BIT3; break;
		case KEY_RIGHTCONTROL:softkeyboard.ctrl |= BIT4; break;
		case KEY_RIGHTSHIFT:  softkeyboard.ctrl |= BIT5; break;
		case KEY_RIGHTALT:    softkeyboard.ctrl |= BIT6; break;
		case KEY_RIGHT_GUI:   softkeyboard.ctrl |= BIT7; break;
		}
	}
	else
	{   // 普通按键
		for (i = 0; i < 10; i++) // 先检查该键是否已存在于队列中
		{
			if (softkeyboard.button[i] == vk_key)
				goto KM_down_send; // 键已存在队列中，直接发送即可
		}
		// 键不在队列中
		for (i = 0; i < 10; i++) // 遍历队列，将新键加入空位
		{
			if (softkeyboard.button[i] == 0)
			{   // 找到空位，将按键加入队列
				softkeyboard.button[i] = vk_key;
				goto KM_down_send;
			}
		}
		// 队列已满，移除最早进入的按键（即队列第一个元素）
		memcpy(&softkeyboard.button[0], &softkeyboard.button[1], 10);
		softkeyboard.button[9] = vk_key;
	}
KM_down_send:
	int err;
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	tx.head.indexpts++;               // 命令统计序号递增
	tx.head.cmd = cmd_keyboard_all;   // 键盘全键发送命令
	tx.head.rand = rand();            // 随机混淆值
	memcpy(&tx.cmd_keyboard, &softkeyboard, sizeof(soft_keyboard_t));
	int length = sizeof(cmd_head_t) + sizeof(soft_keyboard_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = RecvFromWithTimeout(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}


/**
 * 键盘按键释放事件。
 * 对于控制键，清除对应的控制位。
 * 对于普通键，从 10 键队列中移除，后续元素向前移位。
 * @param vk_key 按键虚拟键码（定义于 HidTable.h）
 * @return 0 表示成功，非零值表示对应错误码
 */
int kmNet_keyup(int vk_key)
{
	int i;
	if (vk_key >= KEY_LEFTCONTROL && vk_key <= KEY_RIGHT_GUI) // 控制键
	{
		switch (vk_key)
		{
		case KEY_LEFTCONTROL: softkeyboard.ctrl &= ~BIT0; break;
		case KEY_LEFTSHIFT:   softkeyboard.ctrl &= ~BIT1; break;
		case KEY_LEFTALT:     softkeyboard.ctrl &= ~BIT2; break;
		case KEY_LEFT_GUI:    softkeyboard.ctrl &= ~BIT3; break;
		case KEY_RIGHTCONTROL:softkeyboard.ctrl &= ~BIT4; break;
		case KEY_RIGHTSHIFT:  softkeyboard.ctrl &= ~BIT5; break;
		case KEY_RIGHTALT:    softkeyboard.ctrl &= ~BIT6; break;
		case KEY_RIGHT_GUI:   softkeyboard.ctrl &= ~BIT7; break;
		}
	}
	else
	{   // 普通按键
		for (i = 0; i < 10; i++) // 先在队列中查找该键
		{
			if (softkeyboard.button[i] == vk_key) // 找到目标按键
			{
				memcpy(&softkeyboard.button[i], &softkeyboard.button[i + 1], 10 - i);
				softkeyboard.button[9] = 0;
				goto KM_up_send;
			}
		}
	}
KM_up_send:
	int err;
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	tx.head.indexpts++;               // 命令统计序号递增
	tx.head.cmd = cmd_keyboard_all;   // 键盘全键发送命令
	tx.head.rand = rand();            // 随机混淆值
	memcpy(&tx.cmd_keyboard, &softkeyboard, sizeof(soft_keyboard_t));
	int length = sizeof(cmd_head_t) + sizeof(soft_keyboard_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = RecvFromWithTimeout(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}


/**
 * 重启 Kmbox Net 硬件盒子。
 * 发送重启命令后等待设备响应，然后清理本地网络连接。
 * @return 0 表示重启命令发送成功，非零值表示对应错误码
 */
int kmNet_reboot(void)
{
	int err;
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	tx.head.indexpts++;               // 命令统计序号递增
	tx.head.cmd = cmd_reboot;         // 重启命令
	tx.head.rand = rand();            // 随机混淆值
	int length = sizeof(cmd_head_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = RecvFromWithTimeout(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
	{
		CleanupClientSocket();
		return err_net_rx_timeout;
	}
	err = NetRxReturnHandle(&rx, &tx);
	CleanupClientSocket();
	return err;

}


/**
 * 监听线程函数：持续接收物理鼠标和键盘的状态数据。
 * 该线程在 kmNet_monitor 启动监控时创建，独立运行于后台。
 * 通过绑定本地监听端口，接收设备主动上报的 HID 硬件报告，
 * 并实时更新 hw_mouse 和 hw_keyboard 全局状态缓存。
 * @param lpParameter 线程参数（未使用）
 * @return 0 表示线程正常退出
 */
DWORD WINAPI ThreadListenProcess(LPVOID lpParameter)
{
	WSADATA wsaData; int ret;
	if (WSAStartup(MAKEWORD(1, 1), &wsaData) != 0)
		return 0;
	sockMonitorfd = socket(AF_INET, SOCK_DGRAM, 0);  // 创建 UDP 套接字用于监听
	if (!IsSocketValid(sockMonitorfd) ||
		!SetNonBlocking(sockMonitorfd) ||
		!SetReceiveTimeout(sockMonitorfd, kMonitorReceiveTimeoutMs))
	{
		CloseSocket(sockMonitorfd);
		WSACleanup();
		return 0;
	}
	sockaddr_in servAddr;
	memset(&servAddr, 0, sizeof(servAddr));          // 将地址结构体每个字节置 0
	servAddr.sin_family = PF_INET;                   // 使用 IPv4 地址族
	servAddr.sin_addr.s_addr = INADDR_ANY;           // 自动绑定本地所有可用 IP 地址
	servAddr.sin_port = htons(monitor_port);         // 设置监听端口号
	ret = bind(sockMonitorfd, (SOCKADDR*)&servAddr, sizeof(SOCKADDR));
	if (ret == SOCKET_ERROR)
	{
		CloseSocket(sockMonitorfd);
		WSACleanup();
		return 0;
	}
	SOCKADDR cliAddr;  // 客户端（设备）地址信息
	int nSize = sizeof(SOCKADDR);
	char buff[1024];   // 数据接收缓冲区
	monitor_run = monitor_ok;
	while (IsSocketValid(sockMonitorfd)) {
		nSize = sizeof(SOCKADDR);
		int ret = RecvFromWithTimeout(sockMonitorfd, buff, 1024, 0, &cliAddr, &nSize, kMonitorReceiveTimeoutMs); // 带超时接收数据
		if (ret > 0)
		{
			memcpy(&hw_mouse, buff, sizeof(hw_mouse));                          // 更新物理鼠标状态
			memcpy(&hw_keyboard, &buff[sizeof(hw_mouse)], sizeof(hw_keyboard)); // 更新物理键盘状态
		}
		else
		{
			if (WSAGetLastError() == WSAETIMEDOUT)
				continue;
			break;
		}
	}
	monitor_run = 0;
	CloseSocket(sockMonitorfd);
	WSACleanup();
	return 0;
}

/**
 * 启用或禁用物理鼠标/键盘监控功能。
 * 当 port 非零时，在后台创建监听线程接收设备上报的硬件状态数据。
 * 当 port 为零时，仅向设备发送停止监控命令，不创建线程。
 * @param port 监听端口号（范围 1024 ~ 49151）。传递 0 表示停止监控。
 * @return 0 表示成功，非零值表示对应错误码
 */
int kmNet_monitor(short port)
{
	int err;
	if (!IsSocketValid(sockClientfd))       return err_creat_socket;
	tx.head.indexpts++;              // 命令统计序号递增
	tx.head.cmd = cmd_monitor;       // 监控命令
	if (port) {
		monitor_port = port;                 // 设置用于监听物理鼠标/键盘数据的端口号
		tx.head.rand = port | 0xaa55 << 16;  // 随机混淆值（低 16 位为端口号，高 16 位为标志）
	}
	else
		tx.head.rand = 0;    // 停止监控，随机混淆值设为 0
	int length = sizeof(cmd_head_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = RecvFromWithTimeout(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (IsSocketValid(sockMonitorfd))   // 如果有旧的监听套接字，先关闭
		CloseSocket(sockMonitorfd);
	if (err < 0)
		return err_net_rx_timeout;
	err = NetRxReturnHandle(&rx, &tx);
	if (err != success)
		return err;
	if (port)
	{
		CreateThread(NULL, 0, ThreadListenProcess, NULL, 0, NULL);
		Sleep(10); // 等待一小段时间确保监听线程启动
	}
	return success;
}


/**
 * 监控物理鼠标左键状态
 * @return -1 表示监控未启用（需要先调用 kmNet_monitor(port) 启动监控）
 *          0 表示物理鼠标左键已释放
 *          1 表示物理鼠标左键已按下
 */
int kmNet_monitor_mouse_left()
{
	if (monitor_run != monitor_ok) return -1;
	return (hw_mouse.buttons & 0x01) ? 1 : 0;
}

/**
 * 监控物理鼠标中键状态
 * @return -1 表示监控未启用（需要先调用 kmNet_monitor(port) 启动监控）
 *          0 表示物理鼠标中键已释放
 *          1 表示物理鼠标中键已按下
 */
int kmNet_monitor_mouse_middle()
{
	if (monitor_run != monitor_ok) return -1;
	return (hw_mouse.buttons & 0x04) ? 1 : 0;
}

/**
 * 监控物理鼠标右键状态
 * @return -1 表示监控未启用（需要先调用 kmNet_monitor(port) 启动监控）
 *          0 表示物理鼠标右键已释放
 *          1 表示物理鼠标右键已按下
 */
int kmNet_monitor_mouse_right()
{
	if (monitor_run != monitor_ok) return -1;
	return (hw_mouse.buttons & 0x02) ? 1 : 0;
}

/**
 * 监控物理鼠标侧键 1（前进键）状态
 * @return -1 表示监控未启用（需要先调用 kmNet_monitor(port) 启动监控）
 *          0 表示物理鼠标侧键 1 已释放
 *          1 表示物理鼠标侧键 1 已按下
 */
int kmNet_monitor_mouse_side1()
{
	if (monitor_run != monitor_ok) return -1;
	return (hw_mouse.buttons & 0x08) ? 1 : 0;
}


/**
 * 监控物理鼠标侧键 2（后退键）状态
 * @return -1 表示监控未启用（需要先调用 kmNet_monitor(port) 启动监控）
 *          0 表示物理鼠标侧键 2 已释放
 *          1 表示物理鼠标侧键 2 已按下
 */
int kmNet_monitor_mouse_side2()
{
	if (monitor_run != monitor_ok) return -1;
	return (hw_mouse.buttons & 0x10) ? 1 : 0;
}


/**
 * 监控指定键盘按键的物理状态。
 * 控制键通过 hw_keyboard.buttons 位标志检查，普通键在 hw_keyboard.data 队列中查找。
 * @param vkey 待查询的虚拟键码
 * @return -1 表示监控未启用（需要先调用 kmNet_monitor(port) 启动监控）
 *          0 表示该键当前已释放
 *          1 表示该键当前已按下
 */
int kmNet_monitor_keyboard(short  vkey)
{
	unsigned char vk_key = vkey & 0xff;
	if (monitor_run != monitor_ok) return -1;
	if (vk_key >= KEY_LEFTCONTROL && vk_key <= KEY_RIGHT_GUI) // 控制键
	{
		switch (vk_key)
		{
		case KEY_LEFTCONTROL: return  hw_keyboard.buttons & BIT0 ? 1 : 0;
		case KEY_LEFTSHIFT:   return  hw_keyboard.buttons & BIT1 ? 1 : 0;
		case KEY_LEFTALT:     return  hw_keyboard.buttons & BIT2 ? 1 : 0;
		case KEY_LEFT_GUI:    return  hw_keyboard.buttons & BIT3 ? 1 : 0;
		case KEY_RIGHTCONTROL:return  hw_keyboard.buttons & BIT4 ? 1 : 0;
		case KEY_RIGHTSHIFT:  return  hw_keyboard.buttons & BIT5 ? 1 : 0;
		case KEY_RIGHTALT:    return  hw_keyboard.buttons & BIT6 ? 1 : 0;
		case KEY_RIGHT_GUI:   return  hw_keyboard.buttons & BIT7 ? 1 : 0;
		}
	}
	else // 普通按键
	{
		for (int i = 0; i < 10; i++)
		{
			if (hw_keyboard.data[i] == vk_key)
			{
				return 1;
			}
		}
	}
	return 0;
}


/**
 * 启用 Kmbox Net 盒子的内部调试打印功能。
 * 调试信息将被发送到指定的端口，用于排查设备运行状态。
 * @param port 调试信息输出端口号
 * @param enable 0 表示禁用调试，非零表示启用调试
 * @return 0 表示成功，非零值表示对应错误码
 */
int kmNet_debug(short port, char enable)
{
	int err;
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	tx.head.indexpts++;                   // 命令统计序号递增
	tx.head.cmd = cmd_debug;              // 调试命令
	tx.head.rand = port | enable << 16;   // 随机混淆值（低 16 位为端口号，高 16 位为启用标志）
	int length = sizeof(cmd_head_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = RecvFromWithTimeout(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);

}


/**
 * 屏蔽（拦截）物理鼠标左键输入
 * @param enable 0 表示取消屏蔽，非零表示启用屏蔽
 * @return 0 表示成功，非零值表示对应错误码
 */
int kmNet_mask_mouse_left(int enable)
{
	int err;
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	tx.head.indexpts++;                   // 命令统计序号递增
	tx.head.cmd = cmd_mask_mouse;         // 鼠标屏蔽命令
	tx.head.rand = enable ? (mask_keyboard_mouse_flag |= BIT0) : (mask_keyboard_mouse_flag &= ~BIT0); // 设置/清除鼠标左键屏蔽位
	int length = sizeof(cmd_head_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = RecvFromWithTimeout(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}

/**
 * 屏蔽（拦截）物理鼠标右键输入
 * @param enable 0 表示取消屏蔽，非零表示启用屏蔽
 * @return 0 表示成功，非零值表示对应错误码
 */
int kmNet_mask_mouse_right(int enable)
{
	int err;
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	tx.head.indexpts++;                   // 命令统计序号递增
	tx.head.cmd = cmd_mask_mouse;         // 鼠标屏蔽命令
	tx.head.rand = enable ? (mask_keyboard_mouse_flag |= BIT1) : (mask_keyboard_mouse_flag &= ~BIT1); // 设置/清除鼠标右键屏蔽位
	int length = sizeof(cmd_head_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = RecvFromWithTimeout(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}

/**
 * 屏蔽（拦截）物理鼠标中键输入
 * @param enable 0 表示取消屏蔽，非零表示启用屏蔽
 * @return 0 表示成功，非零值表示对应错误码
 */
int kmNet_mask_mouse_middle(int enable)
{
	int err;
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	tx.head.indexpts++;                   // 命令统计序号递增
	tx.head.cmd = cmd_mask_mouse;         // 鼠标屏蔽命令
	tx.head.rand = enable ? (mask_keyboard_mouse_flag |= BIT2) : (mask_keyboard_mouse_flag &= ~BIT2); // 设置/清除鼠标中键屏蔽位
	int length = sizeof(cmd_head_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = RecvFromWithTimeout(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}

/**
 * 屏蔽（拦截）物理鼠标侧键 1 输入
 * @param enable 0 表示取消屏蔽，非零表示启用屏蔽
 * @return 0 表示成功，非零值表示对应错误码
 */
int kmNet_mask_mouse_side1(int enable)
{
	int err;
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	tx.head.indexpts++;                   // 命令统计序号递增
	tx.head.cmd = cmd_mask_mouse;         // 鼠标屏蔽命令
	tx.head.rand = enable ? (mask_keyboard_mouse_flag |= BIT3) : (mask_keyboard_mouse_flag &= ~BIT3); // 设置/清除鼠标侧键1屏蔽位
	int length = sizeof(cmd_head_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = RecvFromWithTimeout(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}


/**
 * 屏蔽（拦截）物理鼠标侧键 2 输入
 * @param enable 0 表示取消屏蔽，非零表示启用屏蔽
 * @return 0 表示成功，非零值表示对应错误码
 */
int kmNet_mask_mouse_side2(int enable)
{
	int err;
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	tx.head.indexpts++;                   // 命令统计序号递增
	tx.head.cmd = cmd_mask_mouse;         // 鼠标屏蔽命令
	tx.head.rand = enable ? (mask_keyboard_mouse_flag |= BIT4) : (mask_keyboard_mouse_flag &= ~BIT4); // 设置/清除鼠标侧键2屏蔽位
	int length = sizeof(cmd_head_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = RecvFromWithTimeout(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}

/**
 * 屏蔽（拦截）物理鼠标 X 轴移动
 * @param enable 0 表示取消屏蔽，非零表示启用屏蔽
 * @return 0 表示成功，非零值表示对应错误码
 */
int kmNet_mask_mouse_x(int enable)
{
	int err;
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	tx.head.indexpts++;                   // 命令统计序号递增
	tx.head.cmd = cmd_mask_mouse;         // 鼠标屏蔽命令
	tx.head.rand = enable ? (mask_keyboard_mouse_flag |= BIT5) : (mask_keyboard_mouse_flag &= ~BIT5); // 设置/清除 X 轴屏蔽位
	int length = sizeof(cmd_head_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = RecvFromWithTimeout(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}

/**
 * 屏蔽（拦截）物理鼠标 Y 轴移动
 * @param enable 0 表示取消屏蔽，非零表示启用屏蔽
 * @return 0 表示成功，非零值表示对应错误码
 */
int kmNet_mask_mouse_y(int enable)
{
	int err;
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	tx.head.indexpts++;                   // 命令统计序号递增
	tx.head.cmd = cmd_mask_mouse;         // 鼠标屏蔽命令
	tx.head.rand = enable ? (mask_keyboard_mouse_flag |= BIT6) : (mask_keyboard_mouse_flag &= ~BIT6); // 设置/清除 Y 轴屏蔽位
	int length = sizeof(cmd_head_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = RecvFromWithTimeout(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}

/**
 * 屏蔽（拦截）物理鼠标滚轮输入
 * @param enable 0 表示取消屏蔽，非零表示启用屏蔽
 * @return 0 表示成功，非零值表示对应错误码
 */
int kmNet_mask_mouse_wheel(int enable)
{
	int err;
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	tx.head.indexpts++;                   // 命令统计序号递增
	tx.head.cmd = cmd_mask_mouse;         // 鼠标屏蔽命令
	tx.head.rand = enable ? (mask_keyboard_mouse_flag |= BIT7) : (mask_keyboard_mouse_flag &= ~BIT7); // 设置/清除滚轮屏蔽位
	int length = sizeof(cmd_head_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = RecvFromWithTimeout(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}


/**
 * 屏蔽（拦截）指定的键盘按键。
 * 将当前的鼠标/键盘屏蔽标志与待屏蔽的键盘虚拟键码合并发送给设备。
 * @param vkey 待屏蔽的虚拟键码
 * @return 0 表示成功，非零值表示对应错误码
 */
int kmNet_mask_keyboard(short vkey)
{
	int err;
	BYTE v_key = vkey & 0xff;
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	tx.head.indexpts++;                   // 命令统计序号递增
	tx.head.cmd = cmd_mask_mouse;         // 屏蔽命令（含键盘和鼠标屏蔽）
	tx.head.rand = (mask_keyboard_mouse_flag & 0xff) | (v_key << 8); // 低 8 位为鼠标屏蔽标志，高 8 位为键盘按键码
	int length = sizeof(cmd_head_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = RecvFromWithTimeout(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}


/**
 * 取消指定键盘按键的屏蔽
 * @param vkey 待取消屏蔽的虚拟键码
 * @return 0 表示成功，非零值表示对应错误码
 */
int kmNet_unmask_keyboard(short vkey)
{
	int err;
	BYTE v_key = vkey & 0xff;
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	tx.head.indexpts++;                   // 命令统计序号递增
	tx.head.cmd = cmd_unmask_all;         // 取消屏蔽命令
	tx.head.rand = (mask_keyboard_mouse_flag & 0xff) | (v_key << 8); // 低 8 位为鼠标屏蔽标志，高 8 位为键盘按键码
	int length = sizeof(cmd_head_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = RecvFromWithTimeout(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}


/**
 * 取消所有之前设置的物理输入屏蔽（鼠标按键、轴、滚轮及键盘按键全部解除屏蔽）
 * @return 0 表示成功，非零值表示对应错误码
 */
int kmNet_unmask_all()
{
	int err;
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	tx.head.indexpts++;                   // 命令统计序号递增
	tx.head.cmd = cmd_unmask_all;         // 全部取消屏蔽命令
	mask_keyboard_mouse_flag = 0;
	tx.head.rand = mask_keyboard_mouse_flag;
	int length = sizeof(cmd_head_t);
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = RecvFromWithTimeout(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}


/**
 * 设置设备配置（修改 IP 地址和端口号）。
 * 发送新的 IP 和端口信息到设备，设备将应用新配置。
 * @param ip 新的 IP 地址字符串
 * @param port 新的端口号
 * @return 0 表示成功，非零值表示对应错误码
 */
int kmNet_setconfig(char* ip, unsigned short port)
{
	int err;
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	tx.head.indexpts++;                   // 命令统计序号递增
	tx.head.cmd = cmd_setconfig;          // 设置配置命令
	tx.head.rand = inet_addr(ip);
	tx.u8buff.buff[0] = port >> 8;
	tx.u8buff.buff[1] = port >> 0;
	int length = sizeof(cmd_head_t) + 2;
	sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
	SOCKADDR_IN sclient;
	int clen = sizeof(sclient);
	err = RecvFromWithTimeout(sockClientfd, (char*)&rx, 1024, 0, (struct sockaddr*)&sclient, &clen);
	if (err < 0)
		return err_net_rx_timeout;
	return NetRxReturnHandle(&rx, &tx);
}


/**
 * 将 LCD 屏幕填充为指定颜色。屏幕分辨率为 128x160 像素。
 * 通过逐行发送 RGB565 颜色值实现对整屏的填充。
 * @param rgb565 16 位 RGB565 格式颜色值。传入 0x0000（黑色）可清屏。
 * @return 0 表示成功，非零值表示对应错误码
 */
int kmNet_lcd_color(unsigned short rgb565)
{
	int err;
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	for (int y = 0; y < 40; y++)
	{
		tx.head.indexpts++;           // 命令统计序号递增
		tx.head.cmd = cmd_showpic;    // 显示图片命令
		tx.head.rand = 0 | y * 4;
		for (int c = 0; c < 512; c++)
			tx.u16buff.buff[c] = rgb565;
		int length = sizeof(cmd_head_t) + 1024;
		sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
		SOCKADDR_IN sclient;
		int clen = sizeof(sclient);
		err = RecvFromWithTimeout(sockClientfd, (char*)&rx, length, 0, (struct sockaddr*)&sclient, &clen);
		if (err < 0)
			return err_net_rx_timeout;
	}
	return NetRxReturnHandle(&rx, &tx);

}

/**
 * 在 LCD 屏幕底部显示 128x80 像素的图片。
 * 将图片数据分 20 行逐行发送到设备（每行 1024 字节）。
 * 图片数据格式为 16 位 RGB565，从屏幕垂直中间位置（偏移 80 行）开始显示。
 * @param buff_128_80 128x80 图片的 RGB565 像素数据缓冲区指针
 * @return 0 表示成功，非零值表示对应错误码
 */
int kmNet_lcd_picture_bottom(unsigned char* buff_128_80)
{
	int err;
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	for (int y = 0; y < 20; y++)
	{
		tx.head.indexpts++;           // 命令统计序号递增
		tx.head.cmd = cmd_showpic;    // 显示图片命令
		tx.head.rand = 80 + y * 4;
		memcpy(tx.u8buff.buff, &buff_128_80[y * 1024], 1024);
		int length = sizeof(cmd_head_t) + 1024;
		sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
		SOCKADDR_IN sclient;
		int clen = sizeof(sclient);
		err = RecvFromWithTimeout(sockClientfd, (char*)&rx, length, 0, (struct sockaddr*)&sclient, &clen);
		if (err < 0)
			return err_net_rx_timeout;
	}
	return NetRxReturnHandle(&rx, &tx);
}

/**
 * 在 LCD 屏幕上显示完整的 128x160 像素图片。
 * 将图片数据分 40 行逐行发送到设备（每行 1024 字节）。
 * 图片数据格式为 16 位 RGB565，覆盖整个屏幕区域。
 * @param buff_128_160 128x160 全屏图片的 RGB565 像素数据缓冲区指针
 * @return 0 表示成功，非零值表示对应错误码
 */
int kmNet_lcd_picture(unsigned char* buff_128_160)
{
	int err;
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	for (int y = 0; y < 40; y++)
	{
		tx.head.indexpts++;           // 命令统计序号递增
		tx.head.cmd = cmd_showpic;    // 显示图片命令
		tx.head.rand = y * 4;
		memcpy(tx.u8buff.buff, &buff_128_160[y * 1024], 1024);
		int length = sizeof(cmd_head_t) + 1024;
		sendto(sockClientfd, (const char*)&tx, length, 0, (struct sockaddr*)&addrSrv, sizeof(addrSrv));
		SOCKADDR_IN sclient;
		int clen = sizeof(sclient);
		err = RecvFromWithTimeout(sockClientfd, (char*)&rx, length, 0, (struct sockaddr*)&sclient, &clen);
		if (err < 0)
			return err_net_rx_timeout;
	}
	return NetRxReturnHandle(&rx, &tx);
}
