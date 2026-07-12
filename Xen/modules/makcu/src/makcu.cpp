#include "../include/makcu.h"
#include "../include/serialport.h"
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <condition_variable>

namespace makcu {

    // 常量定义
    // MAKCU 设备的 USB VID（供应商 ID）
    constexpr uint16_t MAKCU_VID = 0x1A86;
    // MAKCU 设备的 USB PID（产品 ID）
    constexpr uint16_t MAKCU_PID = 0x55D3;
    // 设备描述字符串
    constexpr const char* TARGET_DESC = "USB-Enhanced-SERIAL CH343";
    // 默认串口名称
    constexpr const char* DEFAULT_NAME = "USB-SERIAL CH340";
    // 初始波特率（标准模式）
    constexpr uint32_t INITIAL_BAUD_RATE = 115200;
    // 高速模式波特率
    constexpr uint32_t HIGH_SPEED_BAUD_RATE = 4000000;

    // PerformanceProfiler 静态成员定义
    // 性能分析器是否启用（原子标志）
    std::atomic<bool> PerformanceProfiler::s_enabled{ false };
    // 性能分析器互斥锁，保护统计数据的线程安全访问
    std::mutex PerformanceProfiler::s_mutex;
    // 性能分析统计数据：命令名称 -> (调用次数, 总耗时微秒数)
    std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> PerformanceProfiler::s_stats;

    // 命令缓存结构体，用于最大化性能
    // 预计算所有鼠标操作的命令字符串，避免运行时字符串拼接开销
    struct CommandCache {
        // 预计算的按键命令映射表：鼠标按钮 -> 按键命令字符串
        std::unordered_map<MouseButton, std::string> press_commands;
        // 预计算的释放命令映射表：鼠标按钮 -> 释放命令字符串
        std::unordered_map<MouseButton, std::string> release_commands;
        // 预计算的锁定命令映射表：锁定目标 -> 锁定命令字符串
        std::unordered_map<std::string, std::string> lock_commands;
        // 预计算的解锁命令映射表：锁定目标 -> 解锁命令字符串
        std::unordered_map<std::string, std::string> unlock_commands;
        // 预计算的查询命令映射表：锁定目标 -> 查询命令字符串
        std::unordered_map<std::string, std::string> query_commands;

        CommandCache() {
            // 预计算所有鼠标按钮的按键命令
            press_commands[MouseButton::LEFT] = "km.left(1)";
            press_commands[MouseButton::RIGHT] = "km.right(1)";
            press_commands[MouseButton::MIDDLE] = "km.middle(1)";
            press_commands[MouseButton::SIDE1] = "km.ms1(1)";
            press_commands[MouseButton::SIDE2] = "km.ms2(1)";

            // 预计算所有鼠标按钮的释放命令
            release_commands[MouseButton::LEFT] = "km.left(0)";
            release_commands[MouseButton::RIGHT] = "km.right(0)";
            release_commands[MouseButton::MIDDLE] = "km.middle(0)";
            release_commands[MouseButton::SIDE1] = "km.ms1(0)";
            release_commands[MouseButton::SIDE2] = "km.ms2(0)";

            // 预计算所有锁定命令
            lock_commands["X"] = "km.lock_mx(1)";
            lock_commands["Y"] = "km.lock_my(1)";
            lock_commands["LEFT"] = "km.lock_ml(1)";
            lock_commands["RIGHT"] = "km.lock_mr(1)";
            lock_commands["MIDDLE"] = "km.lock_mm(1)";
            lock_commands["SIDE1"] = "km.lock_ms1(1)";
            lock_commands["SIDE2"] = "km.lock_ms2(1)";

            // 预计算所有解锁命令
            unlock_commands["X"] = "km.lock_mx(0)";
            unlock_commands["Y"] = "km.lock_my(0)";
            unlock_commands["LEFT"] = "km.lock_ml(0)";
            unlock_commands["RIGHT"] = "km.lock_mr(0)";
            unlock_commands["MIDDLE"] = "km.lock_mm(0)";
            unlock_commands["SIDE1"] = "km.lock_ms1(0)";
            unlock_commands["SIDE2"] = "km.lock_ms2(0)";

            // 预计算所有查询命令
            query_commands["X"] = "km.lock_mx()";
            query_commands["Y"] = "km.lock_my()";
            query_commands["LEFT"] = "km.lock_ml()";
            query_commands["RIGHT"] = "km.lock_mr()";
            query_commands["MIDDLE"] = "km.lock_mm()";
            query_commands["SIDE1"] = "km.lock_ms1()";
            query_commands["SIDE2"] = "km.lock_ms2()";
        }
    };

    // 高性能 PIMPL 实现类
    // 将 Device 的实现细节隐藏在 Impl 类中，提供 ABI 兼容性并优化编译速度
    class Device::Impl {
    public:
        // 串口对象，用于与 MAKCU 设备通信
        std::unique_ptr<SerialPort> serialPort;
        // 设备信息结构体（端口、描述、VID/PID、连接状态）
        DeviceInfo deviceInfo;
        // 连接状态（同步访问用）
        ConnectionStatus status;
        // 原子连接状态，用于线程安全的无锁读取
        std::atomic<ConnectionStatus> atomicStatus{ConnectionStatus::DISCONNECTED};
        // 原子连接标志
        std::atomic<bool> connected;
        // 高性能模式标志（启用后使用 fire-and-forget 模式发送命令）
        std::atomic<bool> highPerformanceMode;
        // 可变互斥锁，保护内部状态的并发访问
        mutable std::mutex mutex;

        // 命令缓存，用于超快速查找预计算命令字符串
        CommandCache commandCache;

        // 状态缓存，使用位运算进行快速锁定状态管理（类似 Python v2.0 的实现方式）
        std::atomic<uint16_t> lockStateCache{ 0 };  // 16 位，分别存储不同锁定状态
        // 锁定状态缓存是否有效
        std::atomic<bool> lockStateCacheValid{ false };

        // 按钮状态跟踪
        std::atomic<uint8_t> currentButtonMask{ 0 };  // 当前按下的鼠标按钮位掩码
        // 按钮监控是否启用
        std::atomic<bool> buttonMonitoringEnabled{ false };

        // 回调函数
        Device::MouseButtonCallback mouseButtonCallback;  // 鼠标按钮事件回调
        Device::ConnectionCallback connectionCallback;    // 连接状态变化回调

        // 预分配的字符串缓冲区，用于不同命令类型
        // 通过复用缓冲区避免频繁内存分配，提升性能
        mutable std::string moveCommandBuffer;     // 移动命令缓冲区
        mutable std::string smoothCommandBuffer;   // 平滑移动命令缓冲区
        mutable std::string bezierCommandBuffer;   // 贝塞尔曲线移动命令缓冲区
        mutable std::string wheelCommandBuffer;    // 滚轮命令缓冲区
        mutable std::string generalCommandBuffer;  // 通用命令缓冲区
        // 命令缓冲区的互斥锁
        mutable std::mutex commandBufferMutex;

        // 连接监控
        std::thread monitoringThread;              // 连接监控线程
        std::atomic<bool> stopMonitoring{false};   // 停止监控标志
        std::condition_variable monitoringCondition;  // 用于唤醒监控线程的条件变量
        std::mutex monitoringMutex;                // 监控互斥锁

        // 安全的线程清理，带超时保护
        // 使用条件变量通知 + 超时等待机制，防止线程无法退出导致死锁
        void cleanupMonitoringThread() {
            if (!monitoringThread.joinable()) {
                return;
            }

            // 设置停止标志，使用释放语义确保所有先前写入对监控线程可见
            stopMonitoring.store(true, std::memory_order_release);

            // 立即唤醒监控线程，使其能够检查停止标志
            {
                std::lock_guard<std::mutex> lock(monitoringMutex);
                monitoringCondition.notify_all();
            }

            // 等待线程退出，设置超时防止无限阻塞
            auto future = std::async(std::launch::async, [this]() {
                monitoringThread.join();
            });

            if (future.wait_for(std::chrono::milliseconds(2000)) == std::future_status::timeout) {
                // 线程未在超时时间内正常退出
                // 正常情况下条件变量信号应能正常唤醒线程，此处作为安全兜底
                #ifdef DEBUG
                std::cerr << "警告：监控线程清理超时，正在分离线程" << std::endl;
                #endif
                monitoringThread.detach();
            }
        }

        // Impl 构造函数
        // 初始化串口对象、连接状态和命令缓冲区
        Impl() : serialPort(std::make_unique<SerialPort>())
            , status(ConnectionStatus::DISCONNECTED)
            , connected(false)
            , highPerformanceMode(false) {
            deviceInfo.isConnected = false;

            // 预分配命令缓冲区，避免频繁内存分配
            moveCommandBuffer.reserve(128);
            smoothCommandBuffer.reserve(128);
            bezierCommandBuffer.reserve(192);
            wheelCommandBuffer.reserve(64);
            generalCommandBuffer.reserve(256);

            // 设置串口的按钮回调，用于接收设备按钮事件
            serialPort->setButtonCallback([this](uint8_t button, bool pressed) {
                handleButtonEvent(button, pressed);
                });
        }

        // Impl 析构函数（默认）
        ~Impl() = default;

        // 私有静态方法：执行核心波特率切换协议
        // 发送波特率变更命令到设备，然后关闭串口并以新波特率重新打开
        static bool performBaudRateChange(SerialPort* serialPort, uint32_t baudRate) {
            if (!serialPort->isOpen()) {
                return false;
            }

            // 构造 MAKCU 波特率变更命令
            // 协议格式：0xDE 0xAD [大小_u16] 0xA5 [波特率_u32]
            std::vector<uint8_t> baudChangeCommand = {
                0xDE, 0xAD,                                    // 标准头部
                0x05, 0x00,                                    // 数据大小（5字节：命令 + 4字节波特率）
                0xA5,                                          // 波特率变更命令
                static_cast<uint8_t>(baudRate & 0xFF),         // 波特率字节（小端序）
                static_cast<uint8_t>((baudRate >> 8) & 0xFF),
                static_cast<uint8_t>((baudRate >> 16) & 0xFF),
                static_cast<uint8_t>((baudRate >> 24) & 0xFF)
            };

            // 发送波特率变更命令
            if (!serialPort->write(baudChangeCommand)) {
                return false;
            }

            // 刷新缓冲区，确保命令被发送
            if (!serialPort->flush()) {
                return false;
            }

            // 关闭串口，然后以新波特率重新打开
            std::string portName = serialPort->getPortName();
            serialPort->close();

            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            if (!serialPort->open(portName, baudRate)) {
                return false;
            }

            return true;
        }

        // 初始化设备
        // 执行设备初始化序列：等待稳定、刷新缓冲区、启用按钮输入、测试通信
        bool initializeDevice() {
            if (!serialPort->isOpen()) {
                return false;
            }

            // 等待设备稳定
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            // 刷新串口缓冲区，清除残留数据
            serialPort->flush();

            // 发送命令启用按钮输入
            serialPort->sendCommand("km.buttons(1)");

            // 等待设备处理命令
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            // 发送查询命令并等待响应，验证通信正常
            auto response = serialPort->sendTrackedCommand("km.buttons()", true,
                std::chrono::milliseconds(100));

            return true;
        }

        // 处理按钮事件
        // 原子更新按钮位掩码，并在设置回调时通知用户层
        void handleButtonEvent(uint8_t button, bool pressed) {
            // 原子更新按钮掩码
            uint8_t currentMask = currentButtonMask.load();
            if (pressed) {
                currentMask |= (1 << button);
            }
            else {
                currentMask &= ~(1 << button);
            }
            currentButtonMask.store(currentMask);

            // 如果设置了用户回调，则调用它
            if (mouseButtonCallback && button < 5) {
                MouseButton mouseBtn = static_cast<MouseButton>(button);
                try {
                    mouseButtonCallback(mouseBtn, pressed);
                }
                catch (...) {
                    // 忽略回调异常，防止异常传播破坏串口通信
                }
            }
        }

        // 通知连接状态变化
        // 调用连接状态回调函数，如果回调抛出异常则禁用以防止反复崩溃
        void notifyConnectionChange(bool isConnected) {
            if (connectionCallback) {
                try {
                    connectionCallback(isConnected);
                }
                catch (...) {
                    // 回调异常后禁用回调，防止持续异常
                    connectionCallback = nullptr;
                }
            }
        }

        // 连接监控循环
        // 在后台线程中运行，定期检查设备连接状态
        // 使用指数退避策略减少 CPU 占用，并通过条件变量支持及时唤醒
        void connectionMonitoringLoop() {
            int pollInterval = 150;          // 初始轮询间隔（毫秒）
            const int maxPollInterval = 500; // 最大轮询间隔
            const int pollIncrement = 50;    // 轮询间隔增量（指数退避）

            while (!stopMonitoring.load(std::memory_order_acquire)) {
                // 使用获取语义双重检查连接状态，确保看到所有更新
                bool currentlyConnected = connected.load(std::memory_order_acquire);
                if (!currentlyConnected) {
                    break;
                }

                // 使用平台特定方法检查实际连接状态
                bool actuallyConnected = serialPort->isActuallyConnected();

                if (!actuallyConnected) {
                    // 设备断开连接 - 使用 compare_exchange 防止竞态条件
                    bool expectedConnected = true;
                    if (connected.compare_exchange_strong(expectedConnected, false, std::memory_order_acq_rel)) {
                        // 成功将连接状态从已连接切换为断开
                        // 原子更新所有其他状态
                        atomicStatus.store(ConnectionStatus::DISCONNECTED, std::memory_order_release);
                        status = ConnectionStatus::DISCONNECTED;
                        deviceInfo.isConnected = false;
                        currentButtonMask.store(0, std::memory_order_release);
                        lockStateCacheValid.store(false, std::memory_order_release);
                        buttonMonitoringEnabled.store(false, std::memory_order_release);

                        // 所有状态更新完成后触发回调
                        notifyConnectionChange(false);
                    }

                    // 无论谁更新了状态，都退出循环
                    break;
                }

                // 使用条件变量实现可中断的休眠，支持指数退避
                std::unique_lock<std::mutex> lock(monitoringMutex);
                if (monitoringCondition.wait_for(lock, std::chrono::milliseconds(pollInterval),
                    [this] { return stopMonitoring.load(std::memory_order_acquire); })) {
                    // 条件被触发（请求停止）
                    break;
                }

                // 指数退避以减少 CPU 占用
                pollInterval = std::min<int>(maxPollInterval, pollInterval + pollIncrement);
            }
        }

        // 高性能命令执行
        // 发送命令到设备，如果启用高速模式则使用 fire-and-forget 方式
        // 同时记录命令执行时间用于性能分析
        bool executeCommand(const std::string& command) {
            if (!connected.load(std::memory_order_acquire)) {
                return false;
            }

            auto start = std::chrono::high_resolution_clock::now();

            bool result;
            if (highPerformanceMode.load()) {
                // 高速模式：fire-and-forget，不等待响应（适用于游戏场景）
                result = serialPort->sendCommand(command);
            }
            else {
                // 标准模式：带最小跟踪
                result = serialPort->sendCommand(command);
            }

            // 性能分析记录
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
            makcu::PerformanceProfiler::logCommandTiming(command, duration);

            return result;
        }


        // 优化的移动命令执行，带缓冲区复用和边界检查
        bool executeMoveCommand(int32_t x, int32_t y) {
            // 验证坐标范围，防止缓冲区溢出
            constexpr int32_t MAX_COORD = 32767;
            constexpr int32_t MIN_COORD = -32768;

            if (x < MIN_COORD || x > MAX_COORD || y < MIN_COORD || y > MAX_COORD) {
                #ifdef DEBUG
                std::cerr << "移动坐标超出范围：(" << x << "," << y << ")" << std::endl;
                #endif
                return false;
            }

            std::lock_guard<std::mutex> lock(commandBufferMutex);
            moveCommandBuffer.clear();
            moveCommandBuffer.reserve(64); // 增大缓冲区以确保安全

            moveCommandBuffer = "km.move(";
            moveCommandBuffer += std::to_string(x);
            moveCommandBuffer += ",";
            moveCommandBuffer += std::to_string(y);
            moveCommandBuffer += ")";

            // 额外的长度检查
            if (moveCommandBuffer.length() > 512) {
                return false;
            }

            return executeCommand(moveCommandBuffer);
        }

        // 优化的平滑移动命令，带缓冲区复用
        bool executeSmoothMoveCommand(int32_t x, int32_t y, uint32_t segments) {
            // 验证输入参数
            constexpr int32_t MAX_COORD = 32767;
            constexpr int32_t MIN_COORD = -32768;

            if (x < MIN_COORD || x > MAX_COORD || y < MIN_COORD || y > MAX_COORD) {
                return false;
            }
            if (segments > 1000) { // 合理限制
                return false;
            }

            std::lock_guard<std::mutex> lock(commandBufferMutex);
            smoothCommandBuffer.clear();

            smoothCommandBuffer = "km.move(";
            smoothCommandBuffer += std::to_string(x);
            smoothCommandBuffer += ",";
            smoothCommandBuffer += std::to_string(y);
            smoothCommandBuffer += ",";
            smoothCommandBuffer += std::to_string(segments);
            smoothCommandBuffer += ")";

            return executeCommand(smoothCommandBuffer);
        }

        // 优化的贝塞尔曲线移动命令，带缓冲区复用
        bool executeBezierMoveCommand(int32_t x, int32_t y, uint32_t segments, int32_t ctrl_x, int32_t ctrl_y) {
            // 验证输入参数
            constexpr int32_t MAX_COORD = 32767;
            constexpr int32_t MIN_COORD = -32768;

            if (x < MIN_COORD || x > MAX_COORD || y < MIN_COORD || y > MAX_COORD ||
                ctrl_x < MIN_COORD || ctrl_x > MAX_COORD || ctrl_y < MIN_COORD || ctrl_y > MAX_COORD) {
                return false;
            }
            if (segments > 1000) { // 合理限制
                return false;
            }

            std::lock_guard<std::mutex> lock(commandBufferMutex);
            bezierCommandBuffer.clear();

            bezierCommandBuffer = "km.move(";
            bezierCommandBuffer += std::to_string(x);
            bezierCommandBuffer += ",";
            bezierCommandBuffer += std::to_string(y);
            bezierCommandBuffer += ",";
            bezierCommandBuffer += std::to_string(segments);
            bezierCommandBuffer += ",";
            bezierCommandBuffer += std::to_string(ctrl_x);
            bezierCommandBuffer += ",";
            bezierCommandBuffer += std::to_string(ctrl_y);
            bezierCommandBuffer += ")";

            return executeCommand(bezierCommandBuffer);
        }

        // 优化的滚轮命令，带缓冲区复用
        bool executeWheelCommand(int32_t delta) {
            // 验证滚轮增量范围
            if (delta < -32768 || delta > 32767) {
                return false;
            }

            std::lock_guard<std::mutex> lock(commandBufferMutex);
            wheelCommandBuffer.clear();

            wheelCommandBuffer = "km.wheel(";
            wheelCommandBuffer += std::to_string(delta);
            wheelCommandBuffer += ")";

            return executeCommand(wheelCommandBuffer);
        }

        // 基于缓存的锁定状态管理
        // 使用位运算快速更新和查询锁定状态，避免频繁与设备通信
        void updateLockStateCache(const std::string& target, bool locked) {
            static const std::unordered_map<std::string, int> lockBitMap = {
                {"X", 0}, {"Y", 1}, {"LEFT", 2}, {"RIGHT", 3},
                {"MIDDLE", 4}, {"SIDE1", 5}, {"SIDE2", 6}
            };

            auto it = lockBitMap.find(target);
            if (it != lockBitMap.end()) {
                uint16_t cache = lockStateCache.load();
                if (locked) {
                    cache |= (1 << it->second);
                }
                else {
                    cache &= ~(1 << it->second);
                }
                lockStateCache.store(cache);
                lockStateCacheValid.store(true);
            }
        }

        // 从缓存获取锁定状态
        bool getLockStateFromCache(const std::string& target) const {
            static const std::unordered_map<std::string, int> lockBitMap = {
                {"X", 0}, {"Y", 1}, {"LEFT", 2}, {"RIGHT", 3},
                {"MIDDLE", 4}, {"SIDE1", 5}, {"SIDE2", 6}
            };

            if (!lockStateCacheValid.load()) {
                return false; // 缓存无效
            }

            auto it = lockBitMap.find(target);
            if (it != lockBitMap.end()) {
                return (lockStateCache.load() & (1 << it->second)) != 0;
            }
            return false;
        }
    };

    // Device 构造函数
    Device::Device() : m_impl(std::make_unique<Impl>()) {}

    // Device 析构函数
    Device::~Device() {
        disconnect();
    }

    // 查找所有可用的 MAKCU 设备
    std::vector<DeviceInfo> Device::findDevices() {
        std::vector<DeviceInfo> devices;
        auto ports = SerialPort::findMakcuPorts();

        for (const auto& port : ports) {
            DeviceInfo info;
            info.port = port;
            info.description = TARGET_DESC;
            info.vid = MAKCU_VID;
            info.pid = MAKCU_PID;
            info.isConnected = false;
            devices.push_back(info);
        }

        return devices;
    }

    // 查找第一个可用设备，返回端口名称
    std::string Device::findFirstDevice() {
        auto devices = findDevices();
        return devices.empty() ? "" : devices[0].port;
    }

    // 连接到指定端口的设备
    // 执行完整的连接序列：打开串口 -> 切换波特率 -> 初始化设备 -> 验证通信
    bool Device::connect(const std::string& port) {
        std::lock_guard<std::mutex> lock(m_impl->mutex);

        if (m_impl->connected.load()) {
            return true;
        }

        std::string targetPort = port.empty() ? findFirstDevice() : port;
        if (targetPort.empty()) {
            m_impl->status = ConnectionStatus::CONNECTION_ERROR;
            return false;
        }

        m_impl->status = ConnectionStatus::CONNECTING;

        // 以初始波特率打开串口
        if (!m_impl->serialPort->open(targetPort, INITIAL_BAUD_RATE)) {
            m_impl->status = ConnectionStatus::CONNECTION_ERROR;
            return false;
        }

        // 切换到高速模式
        if (!Impl::performBaudRateChange(m_impl->serialPort.get(), HIGH_SPEED_BAUD_RATE)) {
            m_impl->serialPort->close();
            m_impl->status = ConnectionStatus::CONNECTION_ERROR;
            m_impl->atomicStatus.store(ConnectionStatus::CONNECTION_ERROR, std::memory_order_release);
            m_impl->deviceInfo.isConnected = false;
            return false;
        }

        // 验证波特率切换后的连接状态
        if (!m_impl->serialPort->isOpen() || !m_impl->serialPort->isActuallyConnected()) {
            m_impl->serialPort->close();
            m_impl->status = ConnectionStatus::CONNECTION_ERROR;
            m_impl->atomicStatus.store(ConnectionStatus::CONNECTION_ERROR, std::memory_order_release);
            m_impl->deviceInfo.isConnected = false;
            return false;
        }

        // 初始化设备
        if (!m_impl->initializeDevice()) {
            m_impl->serialPort->close();
            m_impl->status = ConnectionStatus::CONNECTION_ERROR;
            m_impl->atomicStatus.store(ConnectionStatus::CONNECTION_ERROR, std::memory_order_release);
            m_impl->deviceInfo.isConnected = false;
            return false;
        }

        // 最终验证：检查设备是否响应
        try {
            // 通过发送简单命令测试设备响应
            auto future = m_impl->serialPort->sendTrackedCommand("km.version()", true,
                std::chrono::milliseconds(100));

            // 等待响应，设置超时
            if (future.wait_for(std::chrono::milliseconds(150)) == std::future_status::timeout) {
                m_impl->serialPort->close();
                m_impl->status = ConnectionStatus::CONNECTION_ERROR;
                m_impl->atomicStatus.store(ConnectionStatus::CONNECTION_ERROR, std::memory_order_release);
                m_impl->deviceInfo.isConnected = false;
                return false;
            }

            // 获取结果，确保没有异常
            future.get();
        }
        catch (...) {
            // 设备未正常响应
            m_impl->serialPort->close();
            m_impl->status = ConnectionStatus::CONNECTION_ERROR;
            m_impl->atomicStatus.store(ConnectionStatus::CONNECTION_ERROR, std::memory_order_release);
            m_impl->deviceInfo.isConnected = false;
            return false;
        }

        // 更新设备信息
        m_impl->deviceInfo.port = targetPort;
        m_impl->deviceInfo.description = TARGET_DESC;
        m_impl->deviceInfo.vid = MAKCU_VID;
        m_impl->deviceInfo.pid = MAKCU_PID;
        m_impl->deviceInfo.isConnected = true;

        // 在启动监控线程之前原子更新所有连接状态
        m_impl->stopMonitoring.store(false, std::memory_order_release);
        m_impl->atomicStatus.store(ConnectionStatus::CONNECTED, std::memory_order_release);
        m_impl->status = ConnectionStatus::CONNECTED;

        // 使用释放-获取语义确保所有状态在 connected 标志设置前可见
        std::atomic_thread_fence(std::memory_order_release);
        m_impl->connected.store(true, std::memory_order_release);

        // 在所有状态建立完成后启动连接监控线程
        // 这防止监控线程看到不一致的状态
        try {
            m_impl->monitoringThread = std::thread(&Impl::connectionMonitoringLoop, m_impl.get());
        } catch (const std::system_error&) {
            // 线程创建失败 - 清理并返回错误
            m_impl->connected.store(false, std::memory_order_release);
            m_impl->atomicStatus.store(ConnectionStatus::CONNECTION_ERROR, std::memory_order_release);
            m_impl->status = ConnectionStatus::CONNECTION_ERROR;
            m_impl->deviceInfo.isConnected = false;
            m_impl->serialPort->close();
            return false;
        }

        m_impl->notifyConnectionChange(true);

        return true;
    }

    // 异步连接到设备
    // 如果设备已连接则立即返回已就绪的 future，否则在新线程中执行连接
    std::future<bool> Device::connectAsync(const std::string& port) {
        // 优化：已连接状态直接返回
        if (m_impl->connected.load(std::memory_order_acquire)) {
            // 更高效地创建已就绪 future
            std::packaged_task<bool()> task([]() { return true; });
            auto future = task.get_future();
            task();
            return future;
        }

        // 实际连接是 I/O 密集型操作，在新线程中执行
        return std::async(std::launch::async, [this, port]() {
            return connect(port);
        });
    }

    // 断开设备连接
    // 线程安全：先清理监控线程，再使用 compare_exchange 防止竞态
    void Device::disconnect() {
        std::lock_guard<std::mutex> lock(m_impl->mutex);

        // 无论连接状态如何，始终先清理监控线程
        m_impl->cleanupMonitoringThread();

        // 使用 compare_exchange 防止与监控线程的竞态条件
        bool expectedConnected = true;
        if (!m_impl->connected.compare_exchange_strong(expectedConnected, false, std::memory_order_acq_rel)) {
            // 已被其他线程（如监控线程）断开连接
            return;
        }

        // 成功从已连接切换为断开
        // 立即更新原子状态
        m_impl->atomicStatus.store(ConnectionStatus::DISCONNECTED, std::memory_order_release);

        // 关闭串口
        if (m_impl->serialPort->isOpen()) {
            m_impl->serialPort->close();
        }

        // 串口关闭后更新其余状态
        m_impl->status = ConnectionStatus::DISCONNECTED;
        m_impl->deviceInfo.isConnected = false;
        m_impl->currentButtonMask.store(0, std::memory_order_release);
        m_impl->lockStateCacheValid.store(false, std::memory_order_release);
        m_impl->buttonMonitoringEnabled.store(false, std::memory_order_release);

        // 所有状态更新完毕后通知回调
        m_impl->notifyConnectionChange(false);
    }


    // 检查设备是否已连接
    bool Device::isConnected() const {
        return m_impl->connected.load(std::memory_order_acquire);
    }

    // 获取当前连接状态
    ConnectionStatus Device::getStatus() const {
        return m_impl->atomicStatus.load(std::memory_order_acquire);
    }

    // 获取设备信息
    DeviceInfo Device::getDeviceInfo() const {
        return m_impl->deviceInfo;
    }

    // 获取设备版本信息
    std::string Device::getVersion() const {
        if (!m_impl->connected.load()) {
            return "";
        }

        // 短暂延时确保清除任何待处理的响应
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        auto future = m_impl->serialPort->sendTrackedCommand("km.version()", true,
            std::chrono::milliseconds(50));
        try {
            return future.get();
        }
        catch (...) {
            return "";
        }
    }


    // 高性能鼠标控制方法

    // 按下鼠标按钮
    // 使用命令缓存中的预计算命令字符串，避免运行时拼接
    bool Device::mouseDown(MouseButton button) {
        if (!m_impl->connected.load()) {
            return false;
        }

        auto it = m_impl->commandCache.press_commands.find(button);
        if (it != m_impl->commandCache.press_commands.end()) {
            return m_impl->executeCommand(it->second);
        }
        return false;
    }

    // 释放鼠标按钮
    bool Device::mouseUp(MouseButton button) {
        if (!m_impl->connected.load()) {
            return false;
        }

        auto it = m_impl->commandCache.release_commands.find(button);
        if (it != m_impl->commandCache.release_commands.end()) {
            return m_impl->executeCommand(it->second);
        }
        return false;
    }

    // 点击鼠标按钮（按下 + 释放）
    bool Device::click(MouseButton button) {
        if (!m_impl->connected.load()) {
            return false;
        }

        // 为最大化性能，批量执行按下和释放
        auto pressIt = m_impl->commandCache.press_commands.find(button);
        auto releaseIt = m_impl->commandCache.release_commands.find(button);

        if (pressIt != m_impl->commandCache.press_commands.end() &&
            releaseIt != m_impl->commandCache.release_commands.end()) {

            bool result1 = m_impl->executeCommand(pressIt->second);
            bool result2 = m_impl->executeCommand(releaseIt->second);
            return result1 && result2;
        }
        return false;
    }




    // 获取鼠标按钮状态（按下/释放）
    // 使用缓存的按钮掩码进行高性能查询
    bool Device::mouseButtonState(MouseButton button) {
        if (!m_impl->connected.load()) {
            return false;
        }

        // 使用缓存的按钮状态以获得高性能
        uint8_t mask = m_impl->currentButtonMask.load();
        return (mask & (1 << static_cast<uint8_t>(button))) != 0;
    }


    // 高性能移动方法

    // 鼠标绝对移动
    bool Device::mouseMove(int32_t x, int32_t y) {
        if (!m_impl->connected.load()) {
            return false;
        }

        return m_impl->executeMoveCommand(x, y);
    }

    // 鼠标平滑移动（插值移动，分段执行）
    bool Device::mouseMoveSmooth(int32_t x, int32_t y, uint32_t segments) {
        if (!m_impl->connected.load()) {
            return false;
        }

        return m_impl->executeSmoothMoveCommand(x, y, segments);
    }

    // 鼠标贝塞尔曲线移动
    bool Device::mouseMoveBezier(int32_t x, int32_t y, uint32_t segments,
        int32_t ctrl_x, int32_t ctrl_y) {
        if (!m_impl->connected.load()) {
            return false;
        }

        return m_impl->executeBezierMoveCommand(x, y, segments, ctrl_x, ctrl_y);
    }

    // 高性能拖拽操作

    // 鼠标拖拽（按下 -> 移动 -> 释放）
    bool Device::mouseDrag(MouseButton button, int32_t x, int32_t y) {
        if (!m_impl->connected.load()) {
            return false;
        }

        // 使用命令缓存中的预计算命令实现最佳性能
        auto pressIt = m_impl->commandCache.press_commands.find(button);
        if (pressIt == m_impl->commandCache.press_commands.end()) {
            return false;
        }

        auto releaseIt = m_impl->commandCache.release_commands.find(button);
        if (releaseIt == m_impl->commandCache.release_commands.end()) {
            return false;
        }

        // 执行拖拽序列：按下 -> 移动 -> 释放
        bool result1 = m_impl->executeCommand(pressIt->second);
        bool result2 = m_impl->executeMoveCommand(x, y);
        bool result3 = m_impl->executeCommand(releaseIt->second);

        return result1 && result2 && result3;
    }

    // 鼠标平滑拖拽（按下 -> 平滑移动 -> 释放）
    bool Device::mouseDragSmooth(MouseButton button, int32_t x, int32_t y, uint32_t segments) {
        if (!m_impl->connected.load()) {
            return false;
        }

        auto pressIt = m_impl->commandCache.press_commands.find(button);
        if (pressIt == m_impl->commandCache.press_commands.end()) {
            return false;
        }

        auto releaseIt = m_impl->commandCache.release_commands.find(button);
        if (releaseIt == m_impl->commandCache.release_commands.end()) {
            return false;
        }

        // 执行平滑拖拽序列：按下 -> 平滑移动 -> 释放
        bool result1 = m_impl->executeCommand(pressIt->second);
        bool result2 = m_impl->executeSmoothMoveCommand(x, y, segments);
        bool result3 = m_impl->executeCommand(releaseIt->second);

        return result1 && result2 && result3;
    }

    // 鼠标贝塞尔曲线拖拽（按下 -> 贝塞尔移动 -> 释放）
    bool Device::mouseDragBezier(MouseButton button, int32_t x, int32_t y, uint32_t segments,
        int32_t ctrl_x, int32_t ctrl_y) {
        if (!m_impl->connected.load()) {
            return false;
        }

        auto pressIt = m_impl->commandCache.press_commands.find(button);
        if (pressIt == m_impl->commandCache.press_commands.end()) {
            return false;
        }

        auto releaseIt = m_impl->commandCache.release_commands.find(button);
        if (releaseIt == m_impl->commandCache.release_commands.end()) {
            return false;
        }

        // 执行贝塞尔拖拽序列：按下 -> 贝塞尔移动 -> 释放
        bool result1 = m_impl->executeCommand(pressIt->second);
        bool result2 = m_impl->executeBezierMoveCommand(x, y, segments, ctrl_x, ctrl_y);
        bool result3 = m_impl->executeCommand(releaseIt->second);

        return result1 && result2 && result3;
    }




    // 鼠标滚轮
    bool Device::mouseWheel(int32_t delta) {
        if (!m_impl->connected.load()) {
            return false;
        }

        return m_impl->executeWheelCommand(delta);
    }


    // 鼠标锁定方法（带状态缓存）
    // 锁定/解锁鼠标 X 轴移动
    bool Device::lockMouseX(bool lock) {
        if (!m_impl->connected.load()) return false;

        const std::string& command = lock ?
            m_impl->commandCache.lock_commands.at("X") :
            m_impl->commandCache.unlock_commands.at("X");

        bool result = m_impl->executeCommand(command);
        if (result) {
            m_impl->updateLockStateCache("X", lock);
        }
        return result;
    }

    // 锁定/解锁鼠标 Y 轴移动
    bool Device::lockMouseY(bool lock) {
        if (!m_impl->connected.load()) return false;

        const std::string& command = lock ?
            m_impl->commandCache.lock_commands.at("Y") :
            m_impl->commandCache.unlock_commands.at("Y");

        bool result = m_impl->executeCommand(command);
        if (result) {
            m_impl->updateLockStateCache("Y", lock);
        }
        return result;
    }

    // 锁定/解锁鼠标左键
    bool Device::lockMouseLeft(bool lock) {
        if (!m_impl->connected.load()) return false;

        const std::string& command = lock ?
            m_impl->commandCache.lock_commands.at("LEFT") :
            m_impl->commandCache.unlock_commands.at("LEFT");

        bool result = m_impl->executeCommand(command);
        if (result) {
            m_impl->updateLockStateCache("LEFT", lock);
        }
        return result;
    }

    // 锁定/解锁鼠标中键
    bool Device::lockMouseMiddle(bool lock) {
        if (!m_impl->connected.load()) return false;

        const std::string& command = lock ?
            m_impl->commandCache.lock_commands.at("MIDDLE") :
            m_impl->commandCache.unlock_commands.at("MIDDLE");

        bool result = m_impl->executeCommand(command);
        if (result) {
            m_impl->updateLockStateCache("MIDDLE", lock);
        }
        return result;
    }

    // 锁定/解锁鼠标右键
    bool Device::lockMouseRight(bool lock) {
        if (!m_impl->connected.load()) return false;

        const std::string& command = lock ?
            m_impl->commandCache.lock_commands.at("RIGHT") :
            m_impl->commandCache.unlock_commands.at("RIGHT");

        bool result = m_impl->executeCommand(command);
        if (result) {
            m_impl->updateLockStateCache("RIGHT", lock);
        }
        return result;
    }

    // 锁定/解锁鼠标侧键 1
    bool Device::lockMouseSide1(bool lock) {
        if (!m_impl->connected.load()) return false;

        const std::string& command = lock ?
            m_impl->commandCache.lock_commands.at("SIDE1") :
            m_impl->commandCache.unlock_commands.at("SIDE1");

        bool result = m_impl->executeCommand(command);
        if (result) {
            m_impl->updateLockStateCache("SIDE1", lock);
        }
        return result;
    }

    // 锁定/解锁鼠标侧键 2
    bool Device::lockMouseSide2(bool lock) {
        if (!m_impl->connected.load()) return false;

        const std::string& command = lock ?
            m_impl->commandCache.lock_commands.at("SIDE2") :
            m_impl->commandCache.unlock_commands.at("SIDE2");

        bool result = m_impl->executeCommand(command);
        if (result) {
            m_impl->updateLockStateCache("SIDE2", lock);
        }
        return result;
    }

    // 快速缓存的锁定状态查询

    // 查询 X 轴是否锁定
    bool Device::isMouseXLocked() const {
        return m_impl->getLockStateFromCache("X");
    }

    // 查询 Y 轴是否锁定
    bool Device::isMouseYLocked() const {
        return m_impl->getLockStateFromCache("Y");
    }

    // 查询鼠标左键是否锁定
    bool Device::isMouseLeftLocked() const {
        return m_impl->getLockStateFromCache("LEFT");
    }

    // 查询鼠标中键是否锁定
    bool Device::isMouseMiddleLocked() const {
        return m_impl->getLockStateFromCache("MIDDLE");
    }

    // 查询鼠标右键是否锁定
    bool Device::isMouseRightLocked() const {
        return m_impl->getLockStateFromCache("RIGHT");
    }

    // 查询鼠标侧键 1 是否锁定
    bool Device::isMouseSide1Locked() const {
        return m_impl->getLockStateFromCache("SIDE1");
    }

    // 查询鼠标侧键 2 是否锁定
    bool Device::isMouseSide2Locked() const {
        return m_impl->getLockStateFromCache("SIDE2");
    }

    // 获取所有锁定状态的映射
    std::unordered_map<std::string, bool> Device::getAllLockStates() const {
        return {
            {"X", isMouseXLocked()},
            {"Y", isMouseYLocked()},
            {"LEFT", isMouseLeftLocked()},
            {"RIGHT", isMouseRightLocked()},
            {"MIDDLE", isMouseMiddleLocked()},
            {"SIDE1", isMouseSide1Locked()},
            {"SIDE2", isMouseSide2Locked()}
        };
    }

    // 鼠标输入捕获方法
    // 捕获鼠标左键输入次数
    uint8_t Device::catchMouseLeft() {
        if (!m_impl->connected.load()) return 0;

        auto future = m_impl->serialPort->sendTrackedCommand("km.catch_ml()", true,
            std::chrono::milliseconds(50));
        try {
            std::string response = future.get();
            return static_cast<uint8_t>(std::stoi(response));
        }
        catch (...) {
            return 0;
        }
    }

    // 捕获鼠标中键输入次数
    uint8_t Device::catchMouseMiddle() {
        if (!m_impl->connected.load()) return 0;

        auto future = m_impl->serialPort->sendTrackedCommand("km.catch_mm()", true,
            std::chrono::milliseconds(50));
        try {
            std::string response = future.get();
            return static_cast<uint8_t>(std::stoi(response));
        }
        catch (...) {
            return 0;
        }
    }

    // 捕获鼠标右键输入次数
    uint8_t Device::catchMouseRight() {
        if (!m_impl->connected.load()) return 0;

        auto future = m_impl->serialPort->sendTrackedCommand("km.catch_mr()", true,
            std::chrono::milliseconds(50));
        try {
            std::string response = future.get();
            return static_cast<uint8_t>(std::stoi(response));
        }
        catch (...) {
            return 0;
        }
    }

    // 捕获鼠标侧键 1 输入次数
    uint8_t Device::catchMouseSide1() {
        if (!m_impl->connected.load()) return 0;

        auto future = m_impl->serialPort->sendTrackedCommand("km.catch_ms1()", true,
            std::chrono::milliseconds(50));
        try {
            std::string response = future.get();
            return static_cast<uint8_t>(std::stoi(response));
        }
        catch (...) {
            return 0;
        }
    }

    // 捕获鼠标侧键 2 输入次数
    uint8_t Device::catchMouseSide2() {
        if (!m_impl->connected.load()) return 0;

        auto future = m_impl->serialPort->sendTrackedCommand("km.catch_ms2()", true,
            std::chrono::milliseconds(50));
        try {
            std::string response = future.get();
            return static_cast<uint8_t>(std::stoi(response));
        }
        catch (...) {
            return 0;
        }
    }

    // 按钮监控方法
    // 启用或禁用鼠标按钮事件监控
    bool Device::enableButtonMonitoring(bool enable) {
        if (!m_impl->connected.load(std::memory_order_acquire)) {
            return false;
        }

        std::string command = enable ? "km.buttons(1)" : "km.buttons(0)";
        bool result = m_impl->executeCommand(command);
        if (result) {
            m_impl->buttonMonitoringEnabled.store(enable, std::memory_order_release);
        }
        return result;
    }

    // 查询按钮监控是否启用
    bool Device::isButtonMonitoringEnabled() const {
        return m_impl->buttonMonitoringEnabled.load(std::memory_order_acquire);
    }

    // 获取当前按下的按钮位掩码
    uint8_t Device::getButtonMask() const {
        return m_impl->currentButtonMask.load();
    }

    // 串口欺骗方法
    // 获取鼠标序列号
    std::string Device::getMouseSerial() {
        if (!m_impl->connected.load()) return "";

        // 短暂延时确保清除任何待处理的响应
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        auto future = m_impl->serialPort->sendTrackedCommand("km.serial()", true,
            std::chrono::milliseconds(50));
        try {
            return future.get();
        }
        catch (...) {
            return "";
        }
    }

    // 设置鼠标序列号（串口欺骗）
    bool Device::setMouseSerial(const std::string& serial) {
        if (!m_impl->connected.load()) return false;

        std::string command = "km.serial('" + serial + "')";
        return m_impl->executeCommand(command);
    }

    // 重置鼠标序列号为默认值
    bool Device::resetMouseSerial() {
        if (!m_impl->connected.load()) return false;
        return m_impl->executeCommand("km.serial(0)");
    }



    // 设置波特率
    // 根据 MAKCU 协议将波特率限制在有效范围内，支持可选的通信验证
    bool Device::setBaudRate(uint32_t baudRate, bool validateCommunication) {
        if (!m_impl->connected.load()) {
            return false;
        }

        // 将波特率限制在 MAKCU 协议的有效范围内
        if (baudRate < 115200) {
            baudRate = 115200;
        } else if (baudRate > 4000000) {
            baudRate = 4000000;
        }

        // 使用静态辅助方法执行核心波特率变更
        if (!Impl::performBaudRateChange(m_impl->serialPort.get(), baudRate)) {
            return false;
        }

        // 如果请求验证（针对手动 setBaudRate 调用），测试通信是否正常
        if (validateCommunication) {
            try {
                auto future = m_impl->serialPort->sendTrackedCommand("km.version()", true, std::chrono::milliseconds(1000));
                auto response = future.get();

                // 检查是否收到包含 "km.MAKCU" 的有效响应
                if (response.find("km.MAKCU") != std::string::npos) {
                    return true;
                } else {
                    // 通信测试失败，重新连接回 115200 波特率
                    setBaudRate(115200, false);  // 不带验证的递归调用以避免无限循环
                    return false;
                }
            } catch (...) {
                // 发生异常，重新连接回 115200 波特率
                setBaudRate(115200, false);  // 不带验证的递归调用以避免无限循环
                return false;
            }
        }

        return true;
    }

    // 设置鼠标按钮事件回调
    void Device::setMouseButtonCallback(MouseButtonCallback callback) {
        m_impl->mouseButtonCallback = callback;
    }

    // 设置连接状态变化回调
    void Device::setConnectionCallback(ConnectionCallback callback) {
        m_impl->connectionCallback = callback;
    }

    // 高级自动化方法

    // 点击序列：依次点击多个按钮
    bool Device::clickSequence(const std::vector<MouseButton>& buttons,
        std::chrono::milliseconds delay) {
        if (!m_impl->connected.load()) {
            return false;
        }

        for (const auto& button : buttons) {
            if (!click(button)) {
                return false;
            }
            if (delay.count() > 0) {
                std::this_thread::sleep_for(delay);
            }
        }
        return true;
    }


    // 移动模式：按指定路径移动鼠标
    bool Device::movePattern(const std::vector<std::pair<int32_t, int32_t>>& points,
        bool smooth, uint32_t segments) {
        if (!m_impl->connected.load()) {
            return false;
        }

        for (const auto& [x, y] : points) {
            if (smooth) {
                if (!mouseMoveSmooth(x, y, segments)) {
                    return false;
                }
            }
            else {
                if (!mouseMove(x, y)) {
                    return false;
                }
            }
        }
        return true;
    }

    // 启用或禁用高性能模式
    void Device::enableHighPerformanceMode(bool enable) {
        m_impl->highPerformanceMode.store(enable);
    }

    // 查询高性能模式是否启用
    bool Device::isHighPerformanceModeEnabled() const {
        return m_impl->highPerformanceMode.load();
    }

    // 批量命令构建器实现

    // 创建新的批量命令构建器
    Device::BatchCommandBuilder Device::createBatch() {
        return BatchCommandBuilder(this);
    }

    // 批量添加移动命令
    Device::BatchCommandBuilder& Device::BatchCommandBuilder::move(int32_t x, int32_t y) {
        m_commands.push_back("km.move(" + std::to_string(x) + "," + std::to_string(y) + ")");
        return *this;
    }

    // 批量添加平滑移动命令
    Device::BatchCommandBuilder& Device::BatchCommandBuilder::moveSmooth(int32_t x, int32_t y, uint32_t segments) {
        m_commands.push_back("km.move(" + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(segments) + ")");
        return *this;
    }

    // 批量添加贝塞尔移动命令
    Device::BatchCommandBuilder& Device::BatchCommandBuilder::moveBezier(int32_t x, int32_t y, uint32_t segments,
        int32_t ctrl_x, int32_t ctrl_y) {
        m_commands.push_back("km.move(" + std::to_string(x) + "," + std::to_string(y) + "," +
            std::to_string(segments) + "," + std::to_string(ctrl_x) + "," + std::to_string(ctrl_y) + ")");
        return *this;
    }

    // 批量添加点击命令（按下 + 释放）
    Device::BatchCommandBuilder& Device::BatchCommandBuilder::click(MouseButton button) {
        auto& cache = m_device->m_impl->commandCache;
        auto pressIt = cache.press_commands.find(button);
        auto releaseIt = cache.release_commands.find(button);

        if (pressIt != cache.press_commands.end() && releaseIt != cache.release_commands.end()) {
            m_commands.push_back(pressIt->second);
            m_commands.push_back(releaseIt->second);
        }
        return *this;
    }

    // 批量添加按键命令
    Device::BatchCommandBuilder& Device::BatchCommandBuilder::press(MouseButton button) {
        auto& cache = m_device->m_impl->commandCache;
        auto it = cache.press_commands.find(button);
        if (it != cache.press_commands.end()) {
            m_commands.push_back(it->second);
        }
        return *this;
    }

    // 批量添加释放命令
    Device::BatchCommandBuilder& Device::BatchCommandBuilder::release(MouseButton button) {
        auto& cache = m_device->m_impl->commandCache;
        auto it = cache.release_commands.find(button);
        if (it != cache.release_commands.end()) {
            m_commands.push_back(it->second);
        }
        return *this;
    }

    // 批量添加滚轮命令
    Device::BatchCommandBuilder& Device::BatchCommandBuilder::scroll(int32_t delta) {
        m_commands.push_back("km.wheel(" + std::to_string(delta) + ")");
        return *this;
    }

    // 批量添加拖拽命令（按下 -> 移动 -> 释放）
    Device::BatchCommandBuilder& Device::BatchCommandBuilder::drag(MouseButton button, int32_t x, int32_t y) {
        auto& cache = m_device->m_impl->commandCache;
        auto pressIt = cache.press_commands.find(button);
        auto releaseIt = cache.release_commands.find(button);

        if (pressIt != cache.press_commands.end() && releaseIt != cache.release_commands.end()) {
            // 添加按下、移动、释放命令到批量中（与普通 mouseDrag 格式一致）
            m_commands.push_back(pressIt->second);
            std::string moveCommand = "km.move(" + std::to_string(x) + "," + std::to_string(y) + ")";
            m_commands.push_back(moveCommand);
            m_commands.push_back(releaseIt->second);
        }
        return *this;
    }

    // 批量添加平滑拖拽命令
    Device::BatchCommandBuilder& Device::BatchCommandBuilder::dragSmooth(MouseButton button, int32_t x, int32_t y, uint32_t segments) {
        auto& cache = m_device->m_impl->commandCache;
        auto pressIt = cache.press_commands.find(button);
        auto releaseIt = cache.release_commands.find(button);

        if (pressIt != cache.press_commands.end() && releaseIt != cache.release_commands.end()) {
            // 添加按下、平滑移动、释放命令到批量中
            m_commands.push_back(pressIt->second);
            m_commands.push_back("km.move(" + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(segments) + ")");
            m_commands.push_back(releaseIt->second);
        }
        return *this;
    }

    // 批量添加贝塞尔拖拽命令
    Device::BatchCommandBuilder& Device::BatchCommandBuilder::dragBezier(MouseButton button, int32_t x, int32_t y, uint32_t segments,
        int32_t ctrl_x, int32_t ctrl_y) {
        auto& cache = m_device->m_impl->commandCache;
        auto pressIt = cache.press_commands.find(button);
        auto releaseIt = cache.release_commands.find(button);

        if (pressIt != cache.press_commands.end() && releaseIt != cache.release_commands.end()) {
            // 添加按下、贝塞尔移动、释放命令到批量中
            m_commands.push_back(pressIt->second);
            m_commands.push_back("km.move(" + std::to_string(x) + "," + std::to_string(y) + "," +
                std::to_string(segments) + "," + std::to_string(ctrl_x) + "," + std::to_string(ctrl_y) + ")");
            m_commands.push_back(releaseIt->second);
        }
        return *this;
    }

    // 执行批量命令
    bool Device::BatchCommandBuilder::execute() {
        if (!m_device->m_impl->connected.load()) {
            return false;
        }

        for (const auto& command : m_commands) {
            if (!m_device->m_impl->executeCommand(command)) {
                return false;
            }
        }
        return true;
    }

    // 遗留的原始命令接口（不推荐使用）
    bool Device::sendRawCommand(const std::string& command) const {
        if (!m_impl->connected.load()) {
            return false;
        }

        return m_impl->serialPort->sendCommand(command);
    }

    // 接收原始响应（已废弃，不推荐使用）
    std::string Device::receiveRawResponse() const {
        // 此方法已废弃，不推荐使用
        // 请使用异步方法替代
        return "";
    }


    // 工具函数

    // 鼠标按钮枚举转字符串
    std::string mouseButtonToString(MouseButton button) {
        switch (button) {
        case MouseButton::LEFT: return "LEFT";
        case MouseButton::RIGHT: return "RIGHT";
        case MouseButton::MIDDLE: return "MIDDLE";
        case MouseButton::SIDE1: return "SIDE1";
        case MouseButton::SIDE2: return "SIDE2";
        }
        return "UNKNOWN";
    }

    // 字符串转鼠标按钮枚举
    MouseButton stringToMouseButton(const std::string& buttonName) {
        std::string upper = buttonName;
        std::transform(upper.begin(), upper.end(), upper.begin(),
            [](unsigned char c) { return std::toupper(c); });

        if (upper == "LEFT") return MouseButton::LEFT;
        if (upper == "RIGHT") return MouseButton::RIGHT;
        if (upper == "MIDDLE") return MouseButton::MIDDLE;
        if (upper == "SIDE1") return MouseButton::SIDE1;
        if (upper == "SIDE2") return MouseButton::SIDE2;

        return MouseButton::LEFT; // 默认回退
    }

} // namespace makcu