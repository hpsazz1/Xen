// kmboxA.cpp : Kmbox A 设备 HID 通信协议层
// 实现通过 USB HID 协议与 Kmbox A 硬件通信，提供鼠标和键盘控制功能。
// 该 DLL 导出的函数供上层应用调用，完成设备枚举、连接、数据收发及脚本编译等操作。
//
// 协议格式：所有 HID 报告固定为 65 字节，
// 前 1 字节为报告 ID（固定 0x00），后续字节为协议内容。
// 协议头格式：Head_Sync(0xBB) + 命令字 + 参数长度 + 参数...

#include "kmboxA.h"
//#include "stdafx.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
// 待办：在 STDAFX.H 中
// 引用此文件所需的其他头文件
#include "hidapi.h"
#include <windows.h>
#include "kmbox_net/HidTable.h"

/* 协议同步头 */
#define Head_Sync 0xbb

/* 全局设备句柄 */
hid_device * fd_kmbox;

/* 多线程互斥锁句柄 */
HANDLE m_hMutex_lock=NULL;

/* 离线脚本编译后的缓冲区（64K 整数数组） */
unsigned int ROM_SCRIPT[64*1024]={0};

/* 字符串转十六进制函数声明（字节序交换版） */
int String2Hex(char *str, char *hex);

/* 字符串转十六进制函数声明（直接转换版） */
int String2Hex1(char *str, char *hex);

/*
 * KM_lock_device - 获取设备互斥锁
 * 用于多线程环境下的线程同步，确保同一时刻只有一个线程访问 HID 设备。
 * 使用 Windows 互斥对象实现等待。
 *
 * 返回值：
 *   0  - 成功获取锁
 *   -1 - 互斥句柄为空、设备句柄为空或等待失败
 */
static int KM_lock_device(void)
{
	DWORD wait_result;
	if (m_hMutex_lock == NULL || fd_kmbox == NULL) {
		return -1;
	}

	wait_result = WaitForSingleObject(m_hMutex_lock, INFINITE);
	if (wait_result != WAIT_OBJECT_0 && wait_result != WAIT_ABANDONED) {
		return -1;
	}

	return 0;
}

/*
 * KM_close - 关闭设备并释放资源
 * 先获取互斥锁，然后关闭 HID 设备句柄、释放互斥锁并销毁互斥对象。
 * 如果无法获取锁，仍会尝试关闭设备句柄。
 *
 * 返回值：0
 */
int KM_close(void)
{
	if (m_hMutex_lock != NULL) {
		DWORD wait_result = WaitForSingleObject(m_hMutex_lock, INFINITE);
		if (wait_result == WAIT_OBJECT_0 || wait_result == WAIT_ABANDONED) {
			if (fd_kmbox != NULL) {
				hid_close(fd_kmbox);
				fd_kmbox = NULL;
			}
			ReleaseMutex(m_hMutex_lock);
		}
		else if (fd_kmbox != NULL) {
			hid_close(fd_kmbox);
			fd_kmbox = NULL;
		}

		CloseHandle(m_hMutex_lock);
		m_hMutex_lock = NULL;
		return 0;
	}

	if (fd_kmbox != NULL) {
		hid_close(fd_kmbox);
		fd_kmbox = NULL;
	}

	return 0;
}

/*
 * KM_init - 初始化 Kmbox A 设备
 * 此函数必须最先调用，用于建立与设备的通信。
 * 通过 VID/PID 枚举 HID 设备，并筛选 usage_page == 0xFF00 的设备进行连接。
 *
 * 参数：
 *   vid - 设备的 USB 供应商 ID
 *   pid - 设备的 USB 产品 ID
 *
 * 返回值：
 *   -1 - 未找到指定 VID/PID 的设备，或创建互斥锁/打开设备失败
 *    0 - 初始化成功
 */
int KM_init(unsigned short vid,unsigned short pid)
{
	hid_device_info *hid_info;
	hid_device_info *selected_info;
	if(m_hMutex_lock==NULL)
	{
		m_hMutex_lock= CreateMutexA(NULL,FALSE,"busy");
		if (m_hMutex_lock == NULL) {
			return -1;
		}
	}

	if (fd_kmbox != NULL) {
		hid_close(fd_kmbox);
		fd_kmbox = NULL;
	}

	hid_info=hid_enumerate(vid,pid);
	if (hid_info == NULL) {
		return -1;
	}

	selected_info = hid_info;
	while (selected_info != NULL) {
		if(selected_info->usage_page==0xff00)
		{
			break;
		}
		selected_info = selected_info->next;
	}

	if (selected_info == NULL) {
		hid_free_enumeration(hid_info);
		return -1;
	}

	fd_kmbox = hid_open_path(selected_info->path);
	hid_free_enumeration(hid_info);
	if (!fd_kmbox) {
		fd_kmbox=NULL;
		return -1;
	}
	return 0;
}


/* 键盘 HID 报告数据结构体 */
static struct keyboard_t
{
	unsigned char head[5];	/* 协议头：报告ID(0x00) + 同步头(0xBB) + 命令字(0x02) + 参数长度(0x0C) + 参数(0x01) */
	unsigned char ctrButton;/* 修饰键位掩码（左/右 Ctrl、Alt、Shift、Win） */
	unsigned char data[59];	/* 普通按键 HID 码队列，最多 10 键同时按下 */
}data_keyboard={0x00,Head_Sync,0x02,0x0C,0x01,0x00};

/*
 * KM_keyboard - 发送键盘 HID 报告
 * 直接发送键盘按键报告到设备，支持修饰键和最多 10 个普通按键同时按下（10 键无冲）。
 *
 * 参数：
 *   ctrButton - 修饰键位掩码（BIT0~BIT7 分别对应左Ctrl/左Shift/左Alt/左Win/右Ctrl/右Shift/右Alt/右Win）
 *   key       - 普通按键 HID 码数组，取前 10 个字节
 *
 * 返回值：
 *   -1 - 发送失败
 *    0 - 发送成功
 */
int KM_keyboard(unsigned char ctrButton,unsigned char *key)
{	int i;
	if (KM_lock_device() != 0) return -1;
	data_keyboard.ctrButton=ctrButton;/* 设置修饰键 */
	for( i=0;i<10;i++)
	{
		data_keyboard.data[i]=key[i];
	}
	i=hid_write(fd_kmbox,(const unsigned char *)&data_keyboard,65);
	// Sleep(1) removed — unnecessary HID delay (was adding ~15ms per report)
	ReleaseMutex(m_hMutex_lock);
	return i==65?0:-1;
}

/* 鼠标 HID 报告数据结构体 */
static struct mouse_t
{
	unsigned char head[5];	/* 协议头：报告ID(0x00) + 同步头(0xBB) + 命令字(0x03) + 参数长度(0x07) + 参数(0x02) */
	unsigned char button;	/* 鼠标按键位掩码（BIT0=左键，BIT1=中键，BIT2=右键，BIT3=侧键1，BIT4=侧键2） */
	short x;				/* X 轴移动增量 */
	short y;				/* Y 轴移动增量 */
	unsigned char wheel;	/* 滚轮滚动增量 */
	unsigned char reserv[54];/* 保留字节，填充至 65 字节 */
}data_mouse={0x00,Head_Sync,0x03,0x07,0X02};

/*
 * KM_mouse - 发送鼠标 HID 报告
 * 直接发送鼠标状态报告到设备，包含按键、移动和滚轮信息。
 *
 * 参数：
 *   lmr_side - 按键状态位掩码
 *   x        - X 轴移动增量
 *   y        - Y 轴移动增量
 *   wheel    - 滚轮增量（正值向下，负值向上）
 *
 * 返回值：
 *   -1 - 发送失败
 *    0 - 发送成功
 */
int KM_mouse(unsigned char lmr_side,short x,short y,unsigned char wheel)
{	int i;
	if (KM_lock_device() != 0) return -1;
	data_mouse.button=lmr_side;
	data_mouse.x=x;
	data_mouse.y=y;
	data_mouse.wheel=wheel;
	i=hid_write(fd_kmbox,(const unsigned char *)&data_mouse,65);
	// Sleep(1) removed — unnecessary HID delay (was adding ~15ms per report)
	ReleaseMutex(m_hMutex_lock);
	return i==65?0:-1;
}



/*
 * KM_down - 按下指定键盘键并保持
 * 修饰键（Ctrl/Alt/Shift/Win）通过位掩码管理；普通按键通过 10 键 FIFO 队列管理。
 * 如果按键已在队列中则直接发送，否则加入队列；队列满时移除最早按键。
 *
 * 参数：
 *   vk_key - 按键的 HID 码
 *
 * 返回值：
 *   -1 - 发送失败
 *    0 - 发送成功
 */
int KM_down(unsigned char vk_key)
{	int i;
	if (KM_lock_device() != 0) return -1;
	if(vk_key>=KEY_LEFTCONTROL&&vk_key<=KEY_RIGHT_GUI)/* 修饰键处理 */
	{
		 switch(vk_key)
		 {  case KEY_LEFTCONTROL: data_keyboard.ctrButton  |=BIT0;break;
	        case KEY_LEFTSHIFT:   data_keyboard.ctrButton |=BIT1;break;
	        case KEY_LEFTALT:     data_keyboard.ctrButton |=BIT2;break;
	        case KEY_LEFT_GUI:    data_keyboard.ctrButton |=BIT3;break;
	        case KEY_RIGHTCONTROL:data_keyboard.ctrButton |=BIT4;break;
	        case KEY_RIGHTSHIFT:  data_keyboard.ctrButton |=BIT5;break;
	        case KEY_RIGHTALT:    data_keyboard.ctrButton |=BIT6;break;
	        case KEY_RIGHT_GUI:   data_keyboard.ctrButton |=BIT7;break;
	    }
	}else
	{/* 普通按键处理 */
		for(i=0;i<10;i++)/* 先检查按键是否已在队列中 */
		{
			if(data_keyboard.data[i]==vk_key)
				goto KM_down_send;/* 按键已在队列中，直接发送 */
		}
		/* 按键不在队列中 */
		for(i=0;i<10;i++)/* 遍历所有槽位，将按键加入队列 */
		{
			if(data_keyboard.data[i]==0)
			{/* 找到空闲槽位，放入按键 */
				data_keyboard.data[i]=vk_key;
				goto KM_down_send;
			}
		}
		/* 队列已满，移除最早按键并将新按键放入队尾 */
		memmove(&data_keyboard.data[0],&data_keyboard.data[1],9);
		data_keyboard.data[9]=vk_key;
	}
KM_down_send:
	i=hid_write(fd_kmbox,(const unsigned char *)&data_keyboard,65);
	// Sleep(1) removed — unnecessary HID delay (was adding ~15ms per report)
	ReleaseMutex(m_hMutex_lock);
	return i==65?0:-1;
}


/*
 * KM_up - 释放指定键盘键
 * 修饰键清除对应的位掩码位；普通按键从 FIFO 队列中移除。
 *
 * 参数：
 *   vk_key - 要释放的按键 HID 码
 *
 * 返回值：
 *   -1 - 发送失败
 *    0 - 发送成功
 */
int KM_up(unsigned char vk_key)
{	int i;
	if (KM_lock_device() != 0) return -1;
	if(vk_key>=KEY_LEFTCONTROL&&vk_key<=KEY_RIGHT_GUI)/* 修饰键处理 */
	{
		 switch(vk_key)
		 {  case KEY_LEFTCONTROL: data_keyboard.ctrButton &=~BIT0;break;
	        case KEY_LEFTSHIFT:   data_keyboard.ctrButton &=~BIT1;break;
	        case KEY_LEFTALT:     data_keyboard.ctrButton &=~BIT2;break;
	        case KEY_LEFT_GUI:    data_keyboard.ctrButton &=~BIT3;break;
	        case KEY_RIGHTCONTROL:data_keyboard.ctrButton &=~BIT4;break;
	        case KEY_RIGHTSHIFT:  data_keyboard.ctrButton &=~BIT5;break;
	        case KEY_RIGHTALT:    data_keyboard.ctrButton &=~BIT6;break;
	        case KEY_RIGHT_GUI:   data_keyboard.ctrButton &=~BIT7;break;
	    }
	}else
	{/* 普通按键处理 */
		for(i=0;i<10;i++)/* 先在队列中查找按键位置 */
		{
			if(data_keyboard.data[i]==vk_key)/* 找到目标按键 */
			{
				memmove(&data_keyboard.data[i],&data_keyboard.data[i+1],9-i);
				data_keyboard.data[9]=0;
				goto KM_up_send;
			}
		}
	}
KM_up_send:
	i=hid_write(fd_kmbox,(const unsigned char *)&data_keyboard,65);
	// Sleep(1) removed — unnecessary HID delay (was adding ~15ms per report)
	ReleaseMutex(m_hMutex_lock);
	return i==65?0:-1;
}


/*
 * KM_press - 按下并立即释放按键（单击操作）
 * 相当于依次调用 KM_down 和 KM_up，模拟一次完整的按键动作。
 *
 * 参数：
 *   vk_key - 按键 HID 码
 *
 * 返回值：KM_up 的返回值
 */
int KM_press(unsigned char vk_key)
{	int ret;
	ret=KM_down(vk_key);
	ret=KM_up(vk_key);
	return ret;
}


/*
 * KM_left - 左键控制
 * 设置鼠标左键的按下或释放状态。
 *
 * 参数：
 *   vk_key - 0 释放，非 0 按下
 *
 * 返回值：
 *   -1 - 发送失败
 *    0 - 发送成功
 */
int KM_left(unsigned char vk_key)
{	int i;
	if (KM_lock_device() != 0) return -1;
	if(vk_key)
		data_mouse.button |=BIT0;
	else
		data_mouse.button &=~BIT0;
	data_mouse.x=0;
	data_mouse.y=0;
	data_mouse.wheel=0;
	i=hid_write(fd_kmbox,(const unsigned char *)&data_mouse,65);
	// Sleep(1) removed — unnecessary HID delay (was adding ~15ms per report)
	ReleaseMutex(m_hMutex_lock);
	return i==65?0:-1;
}

/*
 * KM_middle - 中键控制
 * 设置鼠标中键的按下或释放状态。
 *
 * 参数：
 *   vk_key - 0 释放，非 0 按下
 *
 * 返回值：
 *   -1 - 发送失败
 *    0 - 发送成功
 */
int KM_middle(unsigned char vk_key)
{	int i;
	if (KM_lock_device() != 0) return -1;
	if(vk_key)
		data_mouse.button |=BIT1;
	else
		data_mouse.button &=~BIT1;
	data_mouse.x=0;
	data_mouse.y=0;
	data_mouse.wheel=0;
	i=hid_write(fd_kmbox,(const unsigned char *)&data_mouse,65);
	// Sleep(1) removed — unnecessary HID delay (was adding ~15ms per report)
	ReleaseMutex(m_hMutex_lock);
	return i==65?0:-1;
}


/*
 * KM_right - 右键控制
 * 设置鼠标右键的按下或释放状态。
 *
 * 参数：
 *   vk_key - 0 释放，非 0 按下
 *
 * 返回值：
 *   -1 - 发送失败
 *    0 - 发送成功
 */
int KM_right(unsigned char vk_key)
{	int i;
	if (KM_lock_device() != 0) return -1;
	if(vk_key)
		data_mouse.button |=BIT2;
	else
		data_mouse.button &=~BIT2;
	data_mouse.x=0;
	data_mouse.y=0;
	data_mouse.wheel=0;
	i=hid_write(fd_kmbox,(const unsigned char *)&data_mouse,65);
	// Sleep(1) removed — unnecessary HID delay (was adding ~15ms per report)
	ReleaseMutex(m_hMutex_lock);
	return i==65?0:-1;
}


/*
 * KM_move - 鼠标移动
 * 发送相对移动增量到设备。
 *
 * 参数：
 *   x - X 轴移动增量
 *   y - Y 轴移动增量
 *
 * 返回值：
 *   -1 - 发送失败
 *    0 - 发送成功
 */
int KM_move(short x,short y)
{	int i;
	if (KM_lock_device() != 0) return -1;
	data_mouse.x=x;
	data_mouse.y=y;
	data_mouse.wheel=0;
	i=hid_write(fd_kmbox,(const unsigned char *)&data_mouse,65);
	// Sleep(1) removed — unnecessary HID delay (was adding ~15ms per report)
	ReleaseMutex(m_hMutex_lock);
	return i==65?0:-1;
}



/*
 * KM_wheel - 鼠标滚轮控制
 * 发送滚轮滚动增量。
 *
 * 参数：
 *   w - 滚轮增量值
 *
 * 返回值：
 *   -1 - 发送失败
 *    0 - 发送成功
 */
int KM_wheel(unsigned char  w)
{	int i;
	if (KM_lock_device() != 0) return -1;
	data_mouse.x=0;
	data_mouse.y=0;
	data_mouse.wheel=w;
	i=hid_write(fd_kmbox,(const unsigned char *)&data_mouse,65);
	// Sleep(1) removed
	ReleaseMutex(m_hMutex_lock);
	return i==65?0:-1;
}

/*
 * KM_side1 - 鼠标侧键 1（前进键）控制
 * 设置侧键 1 的按下或释放状态。
 *
 * 参数：
 *   w - 0 释放，非 0 按下
 *
 * 返回值：
 *   -1 - 发送失败
 *    0 - 发送成功
 */
int KM_side1(unsigned char  w)
{
	int i;
	if (KM_lock_device() != 0) return -1;
	if(w)
		data_mouse.button |=BIT3;
	else
		data_mouse.button &=~BIT3;
	data_mouse.x=0;
	data_mouse.y=0;
	data_mouse.wheel=0;
	i=hid_write(fd_kmbox,(const unsigned char *)&data_mouse,65);
	// Sleep(1) removed — unnecessary HID delay (was adding ~15ms per report)
	ReleaseMutex(m_hMutex_lock);
	return i==65?0:-1;
}

/*
 * KM_side2 - 鼠标侧键 2（后退键）控制
 * 设置侧键 2 的按下或释放状态。
 *
 * 参数：
 *   w - 0 释放，非 0 按下
 *
 * 返回值：
 *   -1 - 发送失败
 *    0 - 发送成功
 */
int KM_side2(unsigned char  w)
{
	int i;
	if (KM_lock_device() != 0) return -1;
	if(w)
		data_mouse.button |=BIT4;
	else
		data_mouse.button &=~BIT4;
	data_mouse.x=0;
	data_mouse.y=0;
	data_mouse.wheel=0;
	i=hid_write(fd_kmbox,(const unsigned char *)&data_mouse,65);
	// Sleep(1) removed — unnecessary HID delay (was adding ~15ms per report)
	ReleaseMutex(m_hMutex_lock);
	return i==65?0:-1;
}


/* 获取注册码命令结构体 */
static struct cmd_getregcode_t
{
	unsigned char head[5];	/* 协议头：报告ID(0x00) + 同步头(0xBB) + 命令字(0x05) + 参数长度(0x20) + 参数(0x00) */
	unsigned char data[60];	/* 命令数据区 */
}data_getRegcode={0x00,Head_Sync,0x05,0x20,0x00,0x00};

/*
 * KM_GetRegcode - 获取注册码（验证授权）
 * 向设备发送获取注册码命令，并读取返回的 16 字节注册码。
 *
 * 参数：
 *   outMac - 输出缓冲区，用于接收 16 字节注册码
 *
 * 返回值：
 *   返回 buff[2] 中的状态码
 */
int  KM_GetRegcode(unsigned char *outMac)
{	unsigned char buff[65]={0};
	if (KM_lock_device() != 0) return -1;
	hid_write(fd_kmbox,(const unsigned char *)&data_getRegcode,65);
	hid_read_timeout(fd_kmbox,buff,65,-1);
	ReleaseMutex(m_hMutex_lock);
	memcpy(outMac,&buff[3],16);
	return buff[2];
}

/*
 * hex2string - 将单字节十六进制值转换为两位小写十六进制字符串
 * 例如：0xAB -> "ab"
 *
 * 参数：
 *   pt     - 输入字节指针
 *   retstr - 输出字符串缓冲区（至少 2 字节）
 */
void hex2string(char *pt,char *retstr)
{
	int tmp=*pt;
	int h=(tmp&0xf0)>>4;
	int l=tmp&0xf;
	if(h>=0&&h<=9)        *retstr=h+'0';
	else if(h>=10&&h<=15) *retstr=h+'a'-10;

	if(l>=0&&l<=9)        *(retstr+1)=l+'0';
	else if(l>=10&&l<=15) *(retstr+1)=l+'a'-10;

}

/* 生成注册码命令结构体 */
static struct cmd_make_key
{
	unsigned char head[5];	/* 协议头：报告ID(0x00) + 同步头(0xBB) + 命令字(0xFC) + 参数(0xFC, 0xCF) */
	char data[60];			/* 命令数据区 */
}data_make_key={0x00,Head_Sync,0xfc,0xfc,0xcf};

/*
 * MakeKey - 根据 MAC 地址生成注册码
 * 发送 24 字符的 MAC 十六进制字符串到设备，设备返回 32 字节的注册码。
 * 返回的注册码以十六进制字符串形式输出。
 *
 * 参数：
 *   mac - 24 字符的 MAC 十六进制字符串
 *   key - 输出缓冲区，用于接收 64 字符的注册码十六进制字符串
 *
 * 返回值：
 *   -1 - MAC 长度不为 24
 *    0 - 成功
 */
int MakeKey(char *mac,char *key)
{// cd ab 5a 3d 09 43 30 2c 0f 41 00 20
	if(strlen(mac)!=24) return -1;
	char buff[65]={0};
	sprintf_s(buff, sizeof(buff), "%s", mac);
	String2Hex1(buff, data_make_key.data);
	if (KM_lock_device() != 0) return -1;
	hid_write(fd_kmbox,(const unsigned char *)&data_make_key,65);
	hid_read_timeout(fd_kmbox,(unsigned char *)buff,65,-1);
	ReleaseMutex(m_hMutex_lock);
	for(int i=0;i<32;i++)
	{
		hex2string(&buff[i],key);
		key=key+2;
	}
	return 0;
}


/* 设置注册码命令结构体 */
static struct cmd_Setregcode_t
{
	unsigned char head[4];	/* 协议头：报告ID(0x00) + 同步头(0xBB) + 命令字(0x06) + 参数长度(0x20) */
	unsigned char data[61];	/* 命令数据区 */
}data_SetRegcode={0x00,Head_Sync,0x06,0x20};

/*
 * Char2Hex - 将单个十六进制字符转换为对应的数值
 * 支持 '0'-'9'、'A'-'F'、'a'-'f'。
 *
 * 返回值：转换后的数值（0~15），无效字符返回 -1
 */
char Char2Hex(char ch)
{
  if((ch>='0')&&(ch<='9'))
  return   ch-0x30;
  else   if((ch>='A')&&(ch<='F'))
  return   ch-'A'+10;
  else   if((ch>='a')&&(ch<='f'))
  return   ch-'a'+10;
  else   return   (-1);
}

/*
 * String2Hex - 将十六进制字符串转换为二进制数据（带字节序交换）
 * 每两个字符转换一个字节，然后对每 4 个字节进行字节序交换：0-3, 1-2 互换。
 *
 * 参数：
 *   str - 输入十六进制字符串
 *   hex - 输出二进制缓冲区
 *
 * 返回值：
 *   0  - 成功
 *   -1 - 字符串含有无效字符
 */
int String2Hex(char *str, char *hex)
{
		 int hexh,hexl,n;
		 n=0;
		 for(int   i=0;i<64;i++)
		 {   hexh=Char2Hex( str[i]);  /* 高位字节 */
			 hexl=Char2Hex( str[i+1]);/* 低位字节 */
			 if(hexh!=-1&&hexl!=-1)
			 {
				hex[n]=hexh<<4|hexl;
				n++;
			  }else
				  return -1;
			  i++;
		 }
		for(int i=0;i<32;)
		{
			n=hex[i];
			hex[i]=hex[i+3];
			hex[i+3]=n;
			n=hex[i+1];
			hex[i+1]=hex[i+2];
			hex[i+2]=n;
			i+=4;
		}

		 return 0;
}


/*
 * String2Hex1 - 将十六进制字符串转换为二进制数据（直接转换，无字节序交换）
 * 每两个字符转换一个字节。
 *
 * 参数：
 *   str - 输入十六进制字符串
 *   hex - 输出二进制缓冲区
 *
 * 返回值：
 *   0  - 成功
 *   -1 - 字符串含有无效字符
 */
int String2Hex1(char *str, char *hex)
{
		 int hexh,hexl,n;
		 n=0;
		 for(int   i=0;i<64;i++)
		 {   hexh=Char2Hex( str[i]);  /* 高位字节 */
			 hexl=Char2Hex( str[i+1]);/* 低位字节 */
			 if(hexh!=-1&&hexl!=-1)
			 {
				hex[n]=hexh<<4|hexl;
				n++;
			  }else
				  return -1;
			  i++;
		 }
		 return 0;
}

/* 用于十六进制密钥数据的整数/字节联合体 */
typedef union
{
	unsigned int buff[32];	/* 以 32 位整数方式访问 */
	char u8data[32*4];		/* 以字节方式访问 */
}t_key_int;
//				efab60354ca14e73e78d7aa69f375432cc997769cd26bd9bba7487de92841160
//				efab60354ca14e73e78d7aa69f375432cc997769cd26bd9bba7487de92841160
/*
 * KM_SetRegcode - 设置注册码
 * 将十六进制字符串格式的注册码转换为二进制数据后写入设备。
 * 1905e6d71ac78eeb071e3d32cb406598eec804fafb30dcda67887cb6ea84b490     efab60354ca1
 *
 * 参数：
 *   skey - 十六进制字符串格式的注册码
 *
 * 返回值：
 *   -1 - 转换失败或锁定设备失败
 *    其他 - 设备返回的状态码 (buff[3])
 */
int  KM_SetRegcode(char * skey)
{	unsigned char buff[65]={0};
	t_key_int hexkey={0};
	if(String2Hex1(skey, hexkey.u8data)==0)
	{
		memcpy(data_SetRegcode.data,hexkey.buff,32);
		if (KM_lock_device() != 0) return -1;
		hid_write(fd_kmbox,(const unsigned char *)&data_SetRegcode,65);
		hid_read_timeout(fd_kmbox,buff,65,-1);
		ReleaseMutex(m_hMutex_lock);
		return buff[3];
	}
	return -1;
}


/* LCD 字符串显示命令结构体 */
static struct lcdstr_t
{
	unsigned char head[4];	/* 协议头：报告ID(0x00) + 同步头(0xBB) + 命令字(0x07) + 参数(0x70) */
	unsigned char mode;		/* 显示模式 */
	char x;					/* X 坐标 */
	char y;					/* Y 坐标 */
	unsigned char data[58];	/* 要显示的字符串数据 */
}cmd07_lcdstr={0x00,Head_Sync,0x07,0x70};

/*
 * KM_LCDstr - 在设备 LCD 屏幕上显示字符串
 * 指定显示模式、坐标和文本内容，发送到设备刷新屏幕。
 *
 * 参数：
 *   mode - 显示模式
 *   str  - 要显示的字符串
 *   x    - X 坐标
 *   y    - Y 坐标
 *
 * 返回值：0
 */
int KM_LCDstr(int mode,char *str,int x,int y)
{		unsigned char buff[65]={0};
		cmd07_lcdstr.mode=mode;
		cmd07_lcdstr.x=x;
		cmd07_lcdstr.y=y;
		memset(cmd07_lcdstr.data,0,58);
		memcpy(cmd07_lcdstr.data,str,strlen(str));
		if (KM_lock_device() != 0) return -1;
		hid_write(fd_kmbox,(const unsigned char *)&cmd07_lcdstr,65);
		hid_read_timeout(fd_kmbox,buff,65,-1);
		ReleaseMutex(m_hMutex_lock);
		return 0;
}


/* LCD 全屏图片刷新命令结构体 */
static struct lcdpic_t
{
	unsigned char head[6];	/* 协议头：报告ID(0x00) + 同步头(0xBB) + 命令字(0x04) + 参数(0x40,0x40,0x40) */
	unsigned char data[65];	/* 图片数据区 */
}cmd04_lcdpic={0x00,Head_Sync,0x04,0x40,0x40,0x40};

/*
 * KM_LCDpic - 刷新 LCD 全屏图片
 * 发送指令启动图片传输，然后分 16 块（每块 64 字节）发送完整位图数据。
 *
 * 参数：
 *   bmp - 位图数据指针，共 1024 字节（16 x 64）
 *
 * 返回值：0
 */
int KM_LCDpic(unsigned char *bmp)
{		unsigned char buff[65]={0};
		if (KM_lock_device() != 0) return -1;
		/* 开始传输 */
		hid_write(fd_kmbox,(const unsigned char *)&cmd04_lcdpic,65);
		hid_read_timeout(fd_kmbox,buff,65,-1);
		memset(cmd04_lcdpic.data,0,65);
		for(int i=0;i<16;i++)
		{
			memcpy(&cmd04_lcdpic.data[1],bmp,64);
			hid_write(fd_kmbox,(const unsigned char *)&cmd04_lcdpic.data,65);
			hid_read_timeout(fd_kmbox,buff,65,-1);
			bmp+=64;
		}
		ReleaseMutex(m_hMutex_lock);
		return 0;
}


/******************************** 擦除 Flash ****************************************/

/* Flash 擦除命令结构体 */
static struct cmd08_eraflash_t
{
	unsigned char head[3];	/* 协议头：报告ID(0x00) + 同步头(0xBB) + 命令字(0x08) */
	unsigned char data[62];	/* 命令数据区 */
}cmd08_eraflash={0x00,Head_Sync,0x08};

/*
 * KM_ERASE - 擦除设备 Flash
 * 发送擦除命令，清除设备中存储的所有数据。
 *
 * 返回值：0
 */
int KM_ERASE(void)
{		unsigned char buff[65]={0};
		if (KM_lock_device() != 0) return -1;
		hid_write(fd_kmbox,(const unsigned char *)&cmd08_eraflash,65);
		hid_read_timeout(fd_kmbox,buff,65,-1);
		ReleaseMutex(m_hMutex_lock);
		return 0;
}

/* 设置 VID/PID 命令结构体 */
static struct cmd01_setVIDPID_t
{
	unsigned char head[4];	/* 协议头：报告ID(0x00) + 同步头(0xBB) + 命令字(0x01) + 参数(0xAA) */
	int vid;				/* 要设置的 VID 值 */
	int pid;				/* 要设置的 PID 值 */
	unsigned char data[60];	/* 命令数据区 */
}cmd01_setvid={0x00,Head_Sync,0x01,0xaa};

/*
 * KM_SetVIDPID - 设置设备的 VID 和 PID
 * 将新的 VID/PID 值写入设备，设备重启后将使用新的标识。
 *
 * 参数：
 *   VID - 供应商 ID
 *   PID - 产品 ID
 *
 * 返回值：0
 */
int KM_SetVIDPID(int VID,int PID)
{		unsigned char buff[65]={0};
		cmd01_setvid.vid=VID;
		cmd01_setvid.pid=PID;
		if (KM_lock_device() != 0) return -1;
		hid_write(fd_kmbox,(const unsigned char *)&cmd01_setvid,65);
		hid_read_timeout(fd_kmbox,buff,65,-1);
		ReleaseMutex(m_hMutex_lock);
		return 0;
}

/* 固件下载命令结构体（分块写入） */
static struct cmd0a_t
{
	unsigned char head[3];	/* 协议头：报告ID(0x00) + 同步头(0xBB) + 命令字(0x0A) */
	unsigned char index;	/* 子包索引（每 128 字节分 3 个子包：0x00，0x01，0x02） */
	unsigned int address;	/* Flash 目标地址 */
	unsigned char data[57];	/* 子包数据区 */
}cmd0a_downflash_t={0x00,Head_Sync,0x0a};

/*
 * KM_download - 下载固件/脚本到设备 Flash
 * 将数据以 128 字节对齐的块为单位写入设备 Flash。
 * 每 128 字节分 3 个子包发送（索引 0x00/0x01/0x02），分别承载 56/56/16 字节。
 *
 * 参数：
 *   address - Flash 起始地址
 *   buff    - 要写入的数据缓冲区（按 unsigned int 数组传入）
 *   length  - 数据长度（以 unsigned int 为单位，每 32 个 int 对应 128 字节）
 *
 * 返回值：0
 */
int KM_download(unsigned int address,unsigned int *buff,int length)
{	unsigned char ret[65];
	if (KM_lock_device() != 0) return -1;
	for(int i=0;i<length/32;i++) /* 每 32 个 unsigned int 为一组（128 字节） */
	{
			cmd0a_downflash_t.index=0x00;
			cmd0a_downflash_t.address=address+i*128;
			memset(cmd0a_downflash_t.data,0,57);
			memcpy(cmd0a_downflash_t.data,buff+i*32,56);/* 前 56 字节 */
			hid_write(fd_kmbox,(const unsigned char *)&cmd0a_downflash_t,65);
			hid_read_timeout(fd_kmbox,ret,65,-1);

			cmd0a_downflash_t.index=0x01;
			memset(cmd0a_downflash_t.data,0,57);
			memcpy(cmd0a_downflash_t.data,buff+i*32+14,56);/* 中间 56 字节 */
			hid_write(fd_kmbox,(const unsigned char *)&cmd0a_downflash_t,65);
			hid_read_timeout(fd_kmbox,ret,65,-1);

			cmd0a_downflash_t.index=0x02;
			memset(cmd0a_downflash_t.data,0,57);
			memcpy(cmd0a_downflash_t.data,buff+28+i*32,16);/* 最后 16 字节 */
			hid_write(fd_kmbox,(const unsigned char *)&cmd0a_downflash_t,65);
			hid_read_timeout(fd_kmbox,ret,65,-1);
	}
	ReleaseMutex(m_hMutex_lock);
	return 0;
}


/**************************** 读取脚本信息（分两包返回）****************************************/

/* 读取 Flash 命令结构体 */
static struct cmd09_readflash_t
{
	unsigned char head[4];	/* 协议头：报告ID(0x00) + 同步头(0xBB) + 命令字(0x09) + 参数(0x00) */
	unsigned char index;	/* 读取索引：0=配置摘要，1~5=脚本 1~5 详情 */
	unsigned char data[60];	/* 命令数据区 */
}cmd09_readflash={0x00,Head_Sync,0x09,0x00};


/* 设备配置摘要结构体（从 Flash 读取的设备总体信息） */
typedef struct
{
	unsigned int  NewBoardFlag;	/* 新主板标志 */
	unsigned int  defaultVID;	/* 默认 VID */
	unsigned int  defaultPID;	/* 默认 PID */
	unsigned int  TotalSize;	/* 总可用存储空间 */
	unsigned int  UsedSize;		/* 已使用存储空间 */
	unsigned int  NowIndex;		/* 当前正在运行的脚本索引 */

	unsigned int hostVIDPID;	/* 主机 VID 和 PID */
	unsigned int hostHIDDID;	/* 主机 HID 和 DID */
	unsigned int hostmType_scanTime;/* 主机鼠标类型和扫描时间 */
}config_summary_t;

/* 配置参数联合体：可以按 config_summary_t 或原始字节访问 */
typedef union
{
	unsigned char	 buf[128];/* 128 字节原始数据 */
	config_summary_t ROM;     /* 按结构体字段访问 */
}t_config_param;


/* 脚本参数联合体：可以按 script_detail_t 或原始字节访问 */
typedef union
{
	unsigned char	 buf[128];/* 128 字节原始数据 */
	script_detail_t  ROM;     /* 按脚本详情结构体访问 */
}t_script_param;


/* 全局脚本信息缓存 */
kmbox_t g_script_info;

/*
 * KM_Readscript - 从设备读取全部脚本信息
 * 依次读取配置摘要（index=0）和 5 个脚本详情（index=1~5），
 * 同时写入传入参数和全局缓存。
 *
 * 参数：
 *   kmbox - 输出参数，接收完整的 kmbox_t 结构
 *
 * 返回值：0
 */
int KM_Readscript(kmbox_t *kmbox)
{		t_config_param config;
		t_script_param script;
		if (KM_lock_device() != 0) return -1;
		memset(kmbox,0,sizeof(kmbox_t));
		memset(&g_script_info,0,sizeof(g_script_info));
		cmd09_readflash.index=0;/* 读取 VID/PID 和脚本摘要 */
		hid_write(fd_kmbox,(const unsigned char *)&cmd09_readflash,65);
		hid_read_timeout(fd_kmbox,config.buf,65,-1);
		memcpy(kmbox,config.buf,sizeof(config_summary_t));
		memcpy(&g_script_info,config.buf,sizeof(config_summary_t));
		for(int i=1;i<=5;i++)
		{
			cmd09_readflash.index=i;/* 依次读取每个脚本的详情 */
			hid_write(fd_kmbox,(const unsigned char *)&cmd09_readflash,65);
			hid_read_timeout(fd_kmbox,script.buf,65,-1);
			memcpy(&(kmbox->script[i-1]),script.buf,sizeof(script_detail_t));
			memcpy(&(g_script_info.script[i-1]),script.buf,sizeof(script_detail_t));
		}
		ReleaseMutex(m_hMutex_lock);
		return 0;
}



/* 设置 Flash 脚本信息命令结构体 */
static struct cmd0b_Setflash_t
{
	unsigned char head[4];	/* 协议头：报告ID(0x00) + 同步头(0xBB) + 命令字(0x0B) + 参数(0x00) */
	unsigned char index;	/* 索引：0=设置全局配置，1~5=设置对应脚本 */
	unsigned char data[60];	/* 命令数据区 */
}cmd0b_Setflash={0x00,Head_Sync,0x0b,0x00};


/*
 * KM_Setscript - 设置脚本配置信息
 * 先写入全局配置摘要（index=0），再写入指定索引的脚本详情。
 * 脚本地址由 StartAddr + UsedSize 计算，currlength 用于调整存储空间统计。
 *
 * 参数：
 *   index      - 脚本索引（1~5）
 *   addr       - 当前地址
 *   currlength - 当前脚本字节数
 *
 * 返回值：0
 */
int KM_Setscript(int index,int addr,int currlength)
{		t_config_param config;
		t_script_param script;
		if (KM_lock_device() != 0) return -1;
		cmd0b_Setflash.index=0;/* 写入全局配置摘要 */
		memcpy(cmd0b_Setflash.data,&g_script_info.NewBoardFlag,sizeof(config_summary_t));
		hid_write(fd_kmbox,(const unsigned char *)&cmd0b_Setflash,65);
		hid_read_timeout(fd_kmbox,config.buf,65,-1);


		g_script_info.script[index-1].StartAddr=g_script_info.script[index-1].StartAddr+addr-currlength;
		memcpy(cmd0b_Setflash.data,&g_script_info.script[index-1],sizeof(script_detail_t));
		cmd0b_Setflash.index=index;/* 写入指定脚本详情 */
		hid_write(fd_kmbox,(const unsigned char *)&cmd0b_Setflash,65);
		hid_read_timeout(fd_kmbox,script.buf,65,-1);

		ReleaseMutex(m_hMutex_lock);
		return 0;
}

/* 用户数据读写命令结构体 */
static struct cmd0c_Userdata_t
{
	unsigned char head[4];	/* 协议头：报告ID(0x00) + 同步头(0xBB) + 命令字(0x0C) + 参数(0x00) */
	unsigned char rw;		/* 读写标志：0=读取，1=写入 */
	unsigned char data[64];	/* 用户数据区（最多 64 字节） */
}cmd0c_wr_userdata={0x00,Head_Sync,0x0c,0x00};


/*
 * KM_UserData - 读写用户数据（最多 64 字节）
 * 根据 rw 标志决定读取或写入设备中的用户数据区域。
 *
 * 参数：
 *   rw   - 0 读取，1 写入
 *   buff - 数据缓冲区（读取时输出，写入时输入）
 *
 * 返回值：0
 */
int KM_UserData(int rw,unsigned char *buff)
{		t_config_param config;
		if (KM_lock_device() != 0) return -1;
		cmd0c_wr_userdata.rw=rw;/* 设置读写标志 */
		memcpy(cmd0c_wr_userdata.data,buff,64);
		hid_write(fd_kmbox,(const unsigned char *)&cmd0c_wr_userdata,65);
		hid_read_timeout(fd_kmbox,config.buf,65,-1);
		if(rw==0)
			memcpy(buff,config.buf,64);
		ReleaseMutex(m_hMutex_lock);
		return 0;
}




/* 主机 VID/PID 配置命令结构体 */
static struct cmd0d_HostVIDPID_t
{
	unsigned char head[4];	/* 协议头：报告ID(0x00) + 同步头(0xBB) + 命令字(0x0D) + 参数(0x00) */
	unsigned char rw;		/* 读写标志：0=读取，1=写入 */
	unsigned char data[64];	/* 配置数据区 */
}cmd0d_wr_hostvidpid={0x00,Head_Sync,0x0d,0x00};


/*
 * KM_HostVidpid - 读写外部适配器鼠标的主机参数
 * 管理外部鼠标适配器的 VID/PID、HID/DID 和鼠标类型/扫描时间参数。
 *
 * 参数：
 *   rw    - 0 读取，1 写入
 *   vidpid - 主机 VID 和 PID（读取输出/写入输入）
 *   hiddid - 主机 HID 和 DID（读取输出/写入输入）
 *   mtype - 主机鼠标类型和扫描时间（读取输出/写入输入）
 *
 * 返回值：0
 */
int KM_HostVidpid(int rw,unsigned int *vidpid,unsigned int *hiddid,unsigned int *mtype)
{		t_config_param config;
		if (KM_lock_device() != 0) return -1;
		cmd0d_wr_hostvidpid.rw=0;
		hid_write(fd_kmbox,(const unsigned char *)&cmd0d_wr_hostvidpid,65);
		hid_read_timeout(fd_kmbox,config.buf,65,-1);

		if(rw==0)
		{
			*vidpid=config.ROM.hostVIDPID;
			*hiddid=config.ROM.hostHIDDID;
			*mtype =config.ROM.hostmType_scanTime;
			ReleaseMutex(m_hMutex_lock);
			return 0;
		}else
		{
			config.ROM.hostVIDPID=*vidpid;
			config.ROM.hostHIDDID=*hiddid;
			config.ROM.hostmType_scanTime=*mtype;
			cmd0d_wr_hostvidpid.rw=1;
			memcpy(cmd0d_wr_hostvidpid.data,config.buf,64);
			hid_write(fd_kmbox,(const unsigned char *)&cmd0d_wr_hostvidpid,65);
			hid_read_timeout(fd_kmbox,config.buf,65,-1);
		}
		ReleaseMutex(m_hMutex_lock);
		return 0;
}







/* 脚本中的键盘指令结构体（4 字节） */
typedef struct
{
	unsigned char function;	/* 指令功能码，键盘指令固定为 0x01(按下)/0x02(释放)/0x03(单击) */
	unsigned char  downorup;/* 状态标记 */
	unsigned char  vkey;	/* 按键 HID 码 */
	unsigned char  reserved;/* 保留字节 */
}_keyboard_t;

/* 脚本中的鼠标指令结构体（4 字节） */
typedef struct
{
	unsigned char  function;/* 指令功能码，鼠标指令为 0x10(移动)/0x11(左键)/0x12(中键)/0x13(右键)等 */
	unsigned char  x;		/* X 坐标低 8 位 */
	unsigned char  xy;		/* X/Y 高位组合：低4位为 X>>8，高4位为 Y>>8 */
	unsigned char  y;		/* Y 坐标低 8 位 */
}_mouse_t;

/* 脚本指令联合体：可统一以 unsigned int 或按键盘/鼠标结构体访问 */
typedef union
{	unsigned int   data;	/* 4 字节指令编码 */
	_keyboard_t    keyboard;/* 键盘指令解释 */
	_mouse_t   	   mouse;	/* 鼠标指令解释 */
}Script_t;


/*
 * Compile - 离线脚本编译器
 * 将文本形式的脚本命令编译为 4 字节的 Script_t 指令编码。
 * 支持的命令：press()、down()、up()、move()、left()、right()、
 * middle()、side1()、side2()、wheel()、delay()
 *
 * 参数：
 *   str - 以命令开头的文本行
 *
 * 返回值：
 *   0         - 无法识别的命令
 *   非零值    - 编译后的 4 字节指令编码
 */
int Compile(char *str)
{		Script_t script;
		script.data=0;
		if(strncmp("press(",str,6)==0) /* 按键单击命令 */
		{	script.keyboard.function=0x03;
			script.keyboard.vkey=atoi(str+6);
			//while((*str)!='\n') str++;
		}else if(strncmp("down(",str,5)==0) /* 按键按下命令 */
		{	script.keyboard.function=0x01;
			script.keyboard.vkey=atoi(str+5);
			//while((*str)!='\n') str++;
		}else if(strncmp("up(",str,3)==0) /* 按键释放命令 */
		{	script.keyboard.function=0x02;
			script.keyboard.vkey=atoi(str+3);
		}
		else if(strncmp("move(",str,5)==0)/* 鼠标移动命令 */
		{
			script.keyboard.function=0x10;
			int x=atoi(str+5);
			while((*str)!=',') str++;
			int y=atoi(str+1);
			script.mouse.x=x&0xff;
			script.mouse.xy=(x>>8)&0x0f|(((y>>8)&0x0f)<<4);
			script.mouse.y=y&0xff;
		}else if(strncmp("left(",str,5)==0)/* 左键命令 */
		{	script.mouse.function=0x11;
			script.mouse.y=atoi(str+5);
		}else if(strncmp("right(",str,6)==0)/* 右键命令 */
		{	script.mouse.function=0x13;
			script.mouse.y=atoi(str+6);
		}else if(strncmp("middle(",str,7)==0)/* 中键命令 */
		{	script.mouse.function=0x12;
			script.mouse.y=atoi(str+7);
		}else if(strncmp("side1(",str,6)==0)/* 侧键 1 命令 */
		{
			script.mouse.function=0x14;
			script.mouse.y=atoi(str+6);
		}else if(strncmp("side2(",str,6)==0)/* 侧键 2 命令 */
		{	script.mouse.function=0x15;
			script.mouse.y=atoi(str+6);
		}else if(strncmp("wheel(",str,6)==0)/* 滚轮命令 */
		{
			script.mouse.function=0x16;
			script.mouse.y=atoi(str+6);
		}else if(strncmp("delay(",str,6)==0)/* 延时命令 */
		{	script.keyboard.function=0xde;
			int x=atoi(str+6);
			while((*str)!=',') str++;
			int y=atoi(str+1);
			script.mouse.x=x&0xff;
			script.mouse.xy=(x>>8)&0x0f|(((y>>8)&0x0f)<<4);
			script.mouse.y=y&0xff;
		}
		return script.data;
}

/*
 * KM_WriteScript - 完整脚本写入流水线
 * 执行完整的脚本编写流程：读取设备现有配置 -> 编译文本脚本 -> 检查存储空间 ->
 * 下载编译后的指令到 Flash -> 更新脚本配置信息。
 *
 * 参数：
 *   name     - 脚本名称
 *   index    - 脚本槽位索引（1~5）
 *   trigger  - 脚本触发模式
 *   doneNext - 脚本执行完成后的状态
 *   Switch   - 当前开关索引
 *   str      - 文本格式的脚本内容（每行一条指令）
 *
 * 返回值：
 *   0          - 成功
 *   -1         - 包含无法解析的指令
 *   -2         - 存储空间不足
 *   -3         - 指定槽位已存在脚本
 */
int KM_WriteScript(char *name,int index,int trigger,int doneNext,int Switch,char *str)
{	kmbox_t km;
	KM_Readscript(&km);/* 先读取当前配置 */
	memset(ROM_SCRIPT,0,64*1024);
	size_t reallen=strlen(str); /* 原始脚本字符串长度 */
	size_t done=0;
	int cmdlen=0; /* 编译后的指令长度（以 int 为单位，按 128 字节对齐，每 32 个 int 为一组） */
	int ret;
	do
	{	ret=Compile(str);
		if(ret==0)
			return -1;/* 包含无法解析的字符 */
		else {
			ROM_SCRIPT[cmdlen]=ret;
			cmdlen++;
		}
		while(*str!='\n')
		{	str++;
			done++;
		}
		done++;
		str++;
	}while (done<reallen);

	if((g_script_info.UsedSize+cmdlen)>=g_script_info.TotalSize)
		return -2;/* 存储空间不足 */
	g_script_info.NowIndex=Switch;
	if(	g_script_info.script[index-1].Exist==0)
	{
		g_script_info.script[index-1].Exist=1;			/* 标记脚本存在 */
		g_script_info.script[index-1].Onoff=trigger;	/* 设置触发模式 */
		g_script_info.script[index-1].RunCnt=doneNext;	/* 设置执行完成后状态 */
		memcpy(g_script_info.script[index-1].Name,name,strlen(name));/* 设置脚本名称 */
		g_script_info.script[index-1].Length=cmdlen%32?((cmdlen+32)/32*32):((cmdlen));/* 计算占用的 int 数量（向上 32 对齐） */
		KM_download(g_script_info.script[index-1].StartAddr+g_script_info.UsedSize,ROM_SCRIPT,g_script_info.script[index-1].Length);/* 保存脚本到 Flash */
		g_script_info.UsedSize +=g_script_info.script[index-1].Length*4;/* 更新已用空间字节数 */
		KM_Setscript(index, g_script_info.UsedSize,g_script_info.script[index-1].Length*4);

	}else
	{
		return -3;/* 指定槽位已有脚本 */
	}

	return 0;
}
