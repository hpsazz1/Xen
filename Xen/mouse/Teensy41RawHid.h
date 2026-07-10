#ifndef TEENSY41_RAWHID_H
#define TEENSY41_RAWHID_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class Config;
struct hid_device_;
typedef struct hid_device_ hid_device;

constexpr size_t Teensy41RawHidPacketSize = 64;     ///< HID 数据包大小（字节）
constexpr uint16_t Teensy41RawHidHostMagic = 0x3448; ///< 主机魔数
constexpr uint16_t Teensy41RawHidDeviceMagic = 0x4834; ///< 设备魔数
constexpr uint8_t Teensy41RawHidVersion = 1;          ///< 协议版本号

/// 鼠标按键掩码定义
constexpr uint8_t Teensy41RawHidButtonLeft   = 0x01;  ///< 左键
constexpr uint8_t Teensy41RawHidButtonRight  = 0x02;  ///< 右键
constexpr uint8_t Teensy41RawHidButtonMiddle = 0x04;  ///< 中键
constexpr uint8_t Teensy41RawHidButtonBack   = 0x08;  ///< 后退键
constexpr uint8_t Teensy41RawHidButtonForward = 0x10; ///< 前进键

/** @brief 主机命令枚举 */
enum class Teensy41RawHidCommand : uint8_t
{
    Move = 1,   ///< 鼠标移动命令
    Buttons = 2 ///< 鼠标按键命令
};

/** @brief 设备事件枚举 */
enum class Teensy41RawHidEvent : uint8_t
{
    Button = 1, ///< 按钮事件
    Status = 2  ///< 状态事件
};

#pragma pack(push, 1)

/** @brief 主机到设备的 HID 数据包结构 */
struct Teensy41RawHidPacket
{
    uint16_t magic = Teensy41RawHidHostMagic;               ///< 魔数
    uint8_t version = Teensy41RawHidVersion;                 ///< 协议版本
    uint8_t command = static_cast<uint8_t>(Teensy41RawHidCommand::Move); ///< 命令
    uint8_t buttonMask = 0;                                  ///< 按键掩码
    int16_t dx = 0;                                          ///< X 轴移动量
    int16_t dy = 0;                                          ///< Y 轴移动量
    int16_t wheel = 0;                                       ///< 滚轮
    int16_t wheelH = 0;                                      ///< 横向滚轮
    uint32_t sequence = 0;                                   ///< 序列号
    uint8_t reserved[47] = {};                               ///< 保留字节
};

/** @brief 主机到设备的数据包（与 Teensy41RawHidPacket 相同，别名） */
struct Teensy41RawHidHostPacket
{
    uint16_t magic = Teensy41RawHidHostMagic;               ///< 魔数
    uint8_t version = Teensy41RawHidVersion;                 ///< 协议版本
    uint8_t command = static_cast<uint8_t>(Teensy41RawHidCommand::Move); ///< 命令
    uint8_t buttonMask = 0;                                  ///< 按键掩码
    int16_t dx = 0;                                          ///< X 轴移动量
    int16_t dy = 0;                                          ///< Y 轴移动量
    int16_t wheel = 0;                                       ///< 滚轮
    int16_t wheelH = 0;                                      ///< 横向滚轮
    uint32_t sequence = 0;                                   ///< 序列号
    uint8_t reserved[47] = {};                               ///< 保留字节
};

/** @brief 设备到主机的 HID 数据包结构 */
struct Teensy41RawHidDevicePacket
{
    uint16_t magic = Teensy41RawHidDeviceMagic;              ///< 魔数
    uint8_t version = Teensy41RawHidVersion;                 ///< 协议版本
    uint8_t event = static_cast<uint8_t>(Teensy41RawHidEvent::Button); ///< 事件类型
    uint8_t buttonId = 0;                                    ///< 按键 ID
    uint8_t pressed = 0;                                     ///< 是否按下
    uint8_t hostButtonMask = 0;                              ///< 主机按键掩码
    uint32_t sequenceAck = 0;                                ///< 序列号确认
    uint8_t reserved[53] = {};                               ///< 保留字节
};
#pragma pack(pop)

// 编译时断言，确保所有数据包大小符合 HID 规范
static_assert(sizeof(Teensy41RawHidPacket) == Teensy41RawHidPacketSize, "Teensy41RawHidPacket must stay 64 bytes.");
static_assert(sizeof(Teensy41RawHidHostPacket) == Teensy41RawHidPacketSize, "Teensy41RawHidHostPacket must stay 64 bytes.");
static_assert(sizeof(Teensy41RawHidDevicePacket) == Teensy41RawHidPacketSize, "Teensy41RawHidDevicePacket must stay 64 bytes.");

/**
 * @brief Teensy 4.1 Raw HID 鼠标控制类
 *
 * 通过 HID 原始接口与 Teensy 4.1 微控制器通信，
 * 实现低延迟的鼠标控制和物理按钮状态查询。
 */
class Teensy41RawHid
{
public:
    /**
     * @brief 构造函数
     * @param config 程序配置
     */
    explicit Teensy41RawHid(const Config& config);
    ~Teensy41RawHid();

    /** @brief 禁止拷贝 */
    Teensy41RawHid(const Teensy41RawHid&) = delete;
    Teensy41RawHid& operator=(const Teensy41RawHid&) = delete;

    /** @brief HID 设备是否已打开 */
    bool isOpen() const;
    /** @brief 鼠标相对移动 */
    bool move(int dx, int dy, int wheel = 0, int wheelH = 0);
    /** @brief 按下鼠标按键 */
    bool press();
    /** @brief 释放鼠标按键 */
    bool release();
    /** @brief 设置主机按键掩码 */
    bool setHostButtons(uint8_t mask);

    /** @brief 是否正在瞄准（来自物理按钮） */
    bool aimingActive() const;
    /** @brief 是否正在射击（来自物理按钮） */
    bool shootingActive() const;
    /** @brief 是否正在缩放（来自物理按钮） */
    bool zoomingActive() const;

private:
    /** @brief HID 设备删除器 */
    struct HidDeviceDeleter
    {
        void operator()(hid_device* device) const;
    };

    /** @brief 打开 HID 设备连接 */
    bool open();
    /** @brief 发送 HID 数据包到设备 */
    bool sendPacket(Teensy41RawHidCommand command, int dx, int dy, int wheel, int wheelH, uint8_t buttonMask);
    /** @brief 读取线程主循环 */
    void readerLoop();
    /** @brief 处理设备上报的按钮事件 */
    void applyButtonEvent(uint8_t buttonId, bool pressed);

    std::unique_ptr<hid_device, HidDeviceDeleter> device_;  ///< HID 设备句柄
    mutable std::mutex writeMutex_;                          ///< 写入互斥锁
    std::thread readerThread_;                               ///< 读取线程
    std::atomic<bool> connected_{ false };                   ///< 是否已连接
    std::atomic<bool> stopReader_{ false };                  ///< 停止读取线程标志
    std::atomic<bool> aimingActive_{ false };                ///< 瞄准状态
    std::atomic<bool> shootingActive_{ false };              ///< 射击状态
    std::atomic<bool> zoomingActive_{ false };               ///< 缩放状态
    uint32_t sequence_ = 0;                                  ///< 数据包序列号
    std::atomic<uint8_t> hostButtons_{ 0 };                  ///< 主机按键掩码
    uint16_t usagePage_ = 0xFFAB;                            ///< HID Usage Page
    uint16_t usageId_ = 0x0200;                              ///< HID Usage ID
    int openIndex_ = 0;                                      ///< 打开设备索引
    int packetTimeoutMs_ = 2;                                ///< 数据包超时（毫秒）
    int reconnectIntervalMs_ = 500;                          ///< 重连间隔（毫秒）
    std::string serialFilter_;                                ///< 序列号过滤
    std::string vidFilter_;                                  ///< VID 过滤
    std::string pidFilter_;                                  ///< PID 过滤
};

#endif // TEENSY41_RAWHID_H
