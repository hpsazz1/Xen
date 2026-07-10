#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 脚本详细信息结构体
 *
 * 描述 Kmbox A 中存储的一段脚本的相关信息。
 */
typedef struct {
    unsigned int Onoff;       ///< 脚本开关状态
    unsigned int StartAddr;   ///< 脚本起始地址
    unsigned int Length;      ///< 脚本长度
    unsigned int RunCnt;      ///< 运行次数计数
    int Exist;                ///< 是否存在
    char Name[12];            ///< 脚本名称
} script_detail_t;

/**
 * @brief Kmbox 设备信息结构体
 *
 * 描述 Kmbox 设备的固件版本、存储使用情况以及包含的脚本信息。
 */
typedef struct {
    unsigned int NewBoardFlag;     ///< 新版主板标志
    unsigned int defaultVID;       ///< 默认厂商 ID
    unsigned int defaultPID;       ///< 默认产品 ID
    unsigned int TotalSize;        ///< 总存储大小
    unsigned int UsedSize;         ///< 已用存储大小
    unsigned int NowIndex;         ///< 当前索引
    script_detail_t script[5];     ///< 脚本数组（最多 5 个）
} kmbox_t;

/** @brief 初始化 Kmbox，指定 VID 和 PID */
int KM_init(unsigned short vid, unsigned short pid);
/** @brief 关闭 Kmbox 连接 */
int KM_close(void);

/** @brief 按下并释放虚拟键码对应的按键 */
int KM_press(unsigned char vk_key);
/** @brief 按下虚拟键码对应的按键 */
int KM_down(unsigned char vk_key);
/** @brief 释放虚拟键码对应的按键 */
int KM_up(unsigned char vk_key);
/** @brief 键盘组合输入 */
int KM_keyboard(unsigned char ctrButton, unsigned char* key);

/** @brief 鼠标移动+按键综合控制 */
int KM_mouse(unsigned char lmr_side, short x, short y, unsigned char wheel);
/** @brief 控制鼠标左键 */
int KM_left(unsigned char vk_key);
/** @brief 控制鼠标中键 */
int KM_middle(unsigned char vk_key);
/** @brief 控制鼠标右键 */
int KM_right(unsigned char vk_key);
/** @brief 鼠标相对移动 */
int KM_move(short x, short y);
/** @brief 鼠标滚轮控制 */
int KM_wheel(unsigned char w);
/** @brief 鼠标侧键 1 */
int KM_side1(unsigned char w);
/** @brief 鼠标侧键 2 */
int KM_side2(unsigned char w);

/** @brief 获取注册码 */
int KM_GetRegcode(unsigned char* outMac);
/** @brief 设置注册码 */
int KM_SetRegcode(char* skey);

/** @brief LCD 字符串显示 */
int KM_LCDstr(int mode, char* str, int x, int y);
/** @brief LCD 图片显示 */
int KM_LCDpic(unsigned char* bmp);

/** @brief 擦除用户数据 */
int KM_ERASE(void);
/** @brief 读写用户数据 */
int KM_UserData(int rw, unsigned char* buff);
/** @brief 读取设备上的脚本 */
int KM_Readscript(kmbox_t* km);
/** @brief 设置设备的 VID 和 PID */
int KM_SetVIDPID(int VID, int PID);

/** @brief 向设备写入脚本 */
int KM_WriteScript(char* name, int index, int trigger, int doneNext, int Switch, char* str);
/** @brief 读写主机 VID/PID 信息 */
int KM_HostVidpid(int rw, unsigned int* vidpid, unsigned int* hiddid, unsigned int* mtype);

/** @brief 根据 MAC 生成注册密钥 */
int MakeKey(char* mac, char* key);

#ifdef __cplusplus
}
#endif
