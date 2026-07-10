#include "../include/serialport.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <vector>
#include <string>
#include <cstring>
#include <chrono>
#include <future>

#ifdef _WIN32
#include <setupapi.h>
#include <devguid.h>
#include <cfgmgr32.h>
#pragma comment(lib, "setupapi.lib")
#else
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <fstream>
#include <regex>
#include <libudev.h>
#include <errno.h>
#include <poll.h>
#endif

namespace makcu {

    // SerialPort 构造函数，初始化串口参数：波特率、超时时间、打开状态和平台相关的句柄
    SerialPort::SerialPort()
        : m_baudRate(115200)           // 默认波特率 115200
        , m_timeout(100)               // 默认超时时间 100ms
        , m_isOpen(false)              // 初始为未打开状态
#ifdef _WIN32
        , m_handle(INVALID_HANDLE_VALUE)  // Windows 串口句柄初始化为无效值
#else
        , m_fd(-1)                        // Linux 文件描述符初始化为 -1
#endif
    {
#ifdef _WIN32
        // 清零 DCB（设备控制块）和超时结构体
        memset(&m_dcb, 0, sizeof(m_dcb));
        memset(&m_timeouts, 0, sizeof(m_timeouts));
#endif
    }

    // SerialPort 析构函数，关闭串口释放资源
    SerialPort::~SerialPort() {
        close();
    }

    // 打开串口连接
    // 构造平台相关的设备路径，打开设备，配置端口参数，然后启动监听线程
    bool SerialPort::open(const std::string& port, uint32_t baudRate) {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_isOpen) {
            close();
        }

        m_portName = port;
        m_baudRate = baudRate;

        // 统一逻辑：构造平台相关的设备路径
#ifdef _WIN32
        std::string devicePath = "\\\\.\\" + port;
#else
        std::string devicePath = "/dev/" + port;
#endif

        if (!platformOpen(devicePath)) {
            return false;
        }

        if (!platformConfigurePort()) {
            platformClose();
            return false;
        }

        m_isOpen = true;

        // 启动高性能监听线程（共享逻辑）
        m_stopListener = false;
        m_listenerThread = std::thread(&SerialPort::listenerLoop, this);

        return true;
    }

    // 关闭串口连接
    // 先停止监听线程，然后取消所有待处理命令，最后执行平台相关的清理
    void SerialPort::close() {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!m_isOpen) {
            return;
        }

        // 先停止监听线程
        m_stopListener.store(true, std::memory_order_release);

        // 等待监听线程结束，带超时保护
        if (m_listenerThread.joinable()) {
            // 使用超时防止无限阻塞
            auto future = std::async(std::launch::async, [this]() {
                m_listenerThread.join();
            });

            if (future.wait_for(std::chrono::milliseconds(1000)) == std::future_status::timeout) {
                // 线程未正常退出 - 这是严重问题，但必须继续清理
                // 该线程将在对象销毁时被销毁
                m_listenerThread.detach();
            }
        }

        // 取消所有待处理命令，使用双重检查锁定确保安全
        // 第一遍：将所有命令移出 pending 列表，防止新的 promise 操作
        std::vector<std::unique_ptr<PendingCommand>> commandsToCancel;
        {
            std::lock_guard<std::mutex> cmdLock(m_commandMutex);
            commandsToCancel.reserve(m_pendingCommands.size());
            for (auto& [id, cmd] : m_pendingCommands) {
                commandsToCancel.push_back(std::move(cmd));
            }
            m_pendingCommands.clear();
        }

        // 第二遍：在互斥锁外取消命令，防止死锁
        for (auto& cmd : commandsToCancel) {
            try {
                cmd->promise.set_exception(std::make_exception_ptr(
                    std::runtime_error("连接已关闭")));
            }
            catch (...) {
                // Promise 已设置或已移动 - 可安全忽略
            }
        }

        // 平台相关的清理
        platformClose();
        m_isOpen.store(false, std::memory_order_release);

        // 重置按钮状态
        m_lastButtonMask.store(0, std::memory_order_release);
    }

    // 检查串口是否已打开
    bool SerialPort::isOpen() const {
        return m_isOpen;
    }

    // 检查设备是否实际已连接（比 isOpen 更严格的检查）
    // 通过平台特定的 API 验证通信是否仍然正常
    bool SerialPort::isActuallyConnected() const {
        if (!m_isOpen) {
            return false;
        }

#ifdef _WIN32
        // Windows：检查句柄是否仍然有效
        if (m_handle == INVALID_HANDLE_VALUE) {
            return false;
        }

        // 尝试获取通信状态以验证设备是否仍在
        DCB dcb;
        return GetCommState(m_handle, &dcb) != 0;
#else
        // Linux：检查文件描述符是否仍然有效
        if (m_fd < 0) {
            return false;
        }

        // 使用 poll 检查设备是否仍连接
        struct pollfd pfd;
        pfd.fd = m_fd;
        pfd.events = POLLERR | POLLHUP | POLLNVAL;
        pfd.revents = 0;

        int result = poll(&pfd, 1, 0);  // 非阻塞检查

        if (result < 0) {
            return false;  // 发生错误
        }

        // 如果设置了任何错误条件，则设备已断开
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            return false;
        }

        return true;
#endif
    }

    // 发送跟踪命令（带 ID 追踪的异步命令）
    // 为命令分配唯一 ID，将其存储在待处理命令映射表中，然后通过串口发送
    // 返回一个 future，用于异步获取命令执行结果
    std::future<std::string> SerialPort::sendTrackedCommand(const std::string& command,
        bool expectResponse,
        std::chrono::milliseconds timeout) {
        // 使用原子加载检查端口状态，防止竞态条件
        if (!m_isOpen.load(std::memory_order_acquire)) {
            std::promise<std::string> promise;
            promise.set_exception(std::make_exception_ptr(
                std::runtime_error("端口未打开")));
            return promise.get_future();
        }

        // 命令长度验证
        constexpr size_t MAX_COMMAND_LENGTH = 512;
        if (command.length() > MAX_COMMAND_LENGTH) {
            std::promise<std::string> promise;
            promise.set_exception(std::make_exception_ptr(
                std::runtime_error("命令过长（最大 " + std::to_string(MAX_COMMAND_LENGTH) + " 字符）")));
            return promise.get_future();
        }

        int cmdId = generateCommandId();
        auto pendingCmd = std::make_unique<PendingCommand>(cmdId, command, expectResponse, timeout);
        auto future = pendingCmd->promise.get_future();

        // 存储待处理命令（共享逻辑）
        {
            std::lock_guard<std::mutex> lock(m_commandMutex);
            m_pendingCommands[cmdId] = std::move(pendingCmd);
        }

        // 发送带 ID 追踪的命令（共享逻辑）
        // 如果期望响应，则在命令后追加 "#ID" 以便关联响应
        std::string trackedCommand = expectResponse ?
            command + "#" + std::to_string(cmdId) + "\r\n" :
            command + "\r\n";

        // 统一写操作
        ssize_t bytesWritten = platformWrite(trackedCommand.c_str(), trackedCommand.length());

        if (bytesWritten != static_cast<ssize_t>(trackedCommand.length())) {
            std::lock_guard<std::mutex> lock(m_commandMutex);
            auto it = m_pendingCommands.find(cmdId);
            if (it != m_pendingCommands.end()) {
                try {
                    std::string errorMsg = "写入失败";
                    if (bytesWritten < 0) {
                        errorMsg += " (" + getLastPlatformError() + ")";
                    } else {
                        errorMsg += "（部分写入：" + std::to_string(bytesWritten) +
                                   "/" + std::to_string(trackedCommand.length()) + " 字节）";
                    }
                    it->second->promise.set_exception(std::make_exception_ptr(
                        std::runtime_error(errorMsg)));
                }
                catch (...) {
                    // Promise 已设置
                }
                m_pendingCommands.erase(it);
            }
        }

        // 统一刷新操作
        platformFlush();

        return future;
    }

    // 发送命令（fire-and-forget，不跟踪响应）
    bool SerialPort::sendCommand(const std::string& command) {
        // 使用原子加载检查端口状态，防止竞态条件
        if (!m_isOpen.load(std::memory_order_acquire)) {
            return false;
        }

        // 命令长度验证
        constexpr size_t MAX_COMMAND_LENGTH = 512;
        if (command.length() > MAX_COMMAND_LENGTH) {
            #ifdef DEBUG
            std::cerr << "SerialPort: 命令过长（" << command.length() << " > " << MAX_COMMAND_LENGTH << "）" << std::endl;
            #endif
            return false;
        }

        std::string fullCommand = command + "\r\n";

        // 统一写入和刷新操作
        ssize_t bytesWritten = platformWrite(fullCommand.c_str(), fullCommand.length());
        if (bytesWritten == static_cast<ssize_t>(fullCommand.length())) {
            return platformFlush();
        }

        return false;
    }

    // 后台监听线程
    // 持续读取串口数据，分离按钮事件（非可打印字符）和文本响应数据
    // 使用优化的读取缓冲区（共享逻辑）和行缓冲区处理换行分隔的响应
    void SerialPort::listenerLoop() {
        // 优化的读取缓冲区（共享逻辑）
        std::vector<uint8_t> readBuffer(BUFFER_SIZE);
        std::vector<uint8_t> lineBuffer(LINE_BUFFER_SIZE);
        size_t linePos = 0;

        auto lastCleanup = std::chrono::steady_clock::now();
        constexpr auto cleanupInterval = std::chrono::milliseconds(50);

        while (!m_stopListener && m_isOpen.load()) {
            try {
                // 统一检查可用字节数
                size_t bytesAvailable = platformBytesAvailable();
                if (bytesAvailable == 0) {
                    std::this_thread::sleep_for(std::chrono::microseconds(500));
                    continue;
                }

                // 统一读取操作
                size_t bytesToRead = std::min<size_t>(bytesAvailable, static_cast<size_t>(BUFFER_SIZE));
                ssize_t bytesRead = platformRead(readBuffer.data(), bytesToRead);

                if (bytesRead <= 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }

                // 共享的字节处理逻辑
                for (ssize_t i = 0; i < bytesRead; ++i) {
                    uint8_t byte = readBuffer[i];

                    // 处理按钮数据（非可打印字符 < 32，排除 CR/LF）
                    if (byte < 32 && byte != 0x0D && byte != 0x0A) {
                        handleButtonData(byte);
                    }
                    else {
                        // 处理文本响应数据
                        if (byte == 0x0A) { // 换行符
                            if (linePos > 0) {
                                std::string line(lineBuffer.begin(), lineBuffer.begin() + linePos);
                                linePos = 0;
                                if (!line.empty()) {
                                    processResponse(line);
                                }
                            }
                        }
                        else if (byte != 0x0D) { // 忽略回车符
                            if (linePos < LINE_BUFFER_SIZE - 1) {
                                lineBuffer[linePos++] = byte;
                            } else {
                                // 缓冲区溢出保护 - 丢弃当前行并重置
                                #ifdef DEBUG
                                std::cerr << "SerialPort: 行缓冲区溢出，丢弃数据" << std::endl;
                                #endif
                                linePos = 0;
                            }
                        }
                    }
                }

                // 定期清理超时命令（共享逻辑）
                auto now = std::chrono::steady_clock::now();
                if (now - lastCleanup > cleanupInterval) {
                    cleanupTimedOutCommands();
                    lastCleanup = now;
                }

            }
            catch (const std::exception& e) {
                // 记录特定异常用于调试，但继续运行
                // 在生产环境中，建议使用适当的日志框架
                #ifdef DEBUG
                std::cerr << "SerialPort 监听线程异常: " << e.what() << std::endl;
                #endif

                // 短暂暂停以防止紧密异常循环
                std::this_thread::sleep_for(std::chrono::milliseconds(10));

                // 异常后检查端口是否仍然打开
                if (!m_isOpen.load(std::memory_order_acquire)) {
                    // 端口已关闭，正常退出
                    break;
                }
            }
            catch (...) {
                // 未知异常 - 更加谨慎
                #ifdef DEBUG
                std::cerr << "SerialPort 监听线程未知异常" << std::endl;
                #endif

                std::this_thread::sleep_for(std::chrono::milliseconds(50));

                // 未知异常后检查端口是否仍然打开
                if (!m_isOpen.load(std::memory_order_acquire)) {
                    // 端口已关闭，正常退出
                    break;
                }
            }
        }
    }

    // 处理按钮数据
    // 解析从设备接收的非可打印字节，提取鼠标按钮状态变化
    // 仅处理发生变化的位，减少不必要的回调调用
    void SerialPort::handleButtonData(uint8_t data) {
        uint8_t lastMask = m_lastButtonMask.load();
        if (data == lastMask) {
            return; // 无变化
        }

        m_lastButtonMask.store(data);

        if (m_buttonCallback) {
            // 仅处理发生变化的位
            uint8_t changedBits = data ^ lastMask;
            for (int bit = 0; bit < 5; ++bit) {
                if (changedBits & (1 << bit)) {
                    bool isPressed = data & (1 << bit);
                    try {
                        m_buttonCallback(bit, isPressed);
                    }
                    catch (...) {
                        // 忽略回调异常
                    }
                }
            }
        }
    }

    // 处理响应数据
    // 解析设备返回的文本响应，通过 "#ID:结果" 格式关联到待处理命令
    // 如果响应没有 ID 标记，则将其分配给最旧的待处理命令
    void SerialPort::processResponse(const std::string& response) {
        // 移除 ">>> " 前缀（如果存在）
        std::string content = response;
        if (content.substr(0, 4) == ">>> ") {
            content = content.substr(4);
        }

        // 检查命令 ID 关联
        size_t hashPos = content.find('#');
        if (hashPos != std::string::npos) {
            // 提取命令 ID
            std::string idStr = content.substr(hashPos + 1);
            size_t colonPos = idStr.find(':');
            if (colonPos != std::string::npos) {
                try {
                    int cmdId = std::stoi(idStr.substr(0, colonPos));
                    std::string result = idStr.substr(colonPos + 1);

                    std::lock_guard<std::mutex> lock(m_commandMutex);
                    auto it = m_pendingCommands.find(cmdId);
                    if (it != m_pendingCommands.end()) {
                        try {
                            it->second->promise.set_value(result);
                        }
                        catch (...) {
                            // Promise 已设置
                        }
                        m_pendingCommands.erase(it);
                    }
                    return;
                }
                catch (...) {
                    // ID 解析失败，视为普通响应
                }
            }
        }

        // 处理未追踪的响应（分配给最旧的待处理命令）
        std::lock_guard<std::mutex> lock(m_commandMutex);
        if (!m_pendingCommands.empty()) {
            auto it = m_pendingCommands.begin();
            try {
                it->second->promise.set_value(content);
            }
            catch (...) {
                // Promise 已设置
            }
            m_pendingCommands.erase(it);
        }
    }

    // 清理超时命令
    // 遍历待处理命令列表，移除已超时的命令并设置异常
    void SerialPort::cleanupTimedOutCommands() {
        auto now = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lock(m_commandMutex);
        auto it = m_pendingCommands.begin();
        while (it != m_pendingCommands.end()) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - it->second->timestamp);

            if (elapsed > it->second->timeout) {
                try {
                    it->second->promise.set_exception(std::make_exception_ptr(
                        std::runtime_error("命令超时")));
                }
                catch (...) {
                    // Promise 已设置
                }
                it = m_pendingCommands.erase(it);
            }
            else {
                ++it;
            }
        }
    }

    // 生成命令 ID（循环递增，范围 1-10000）
    int SerialPort::generateCommandId() {
        return (m_commandCounter.fetch_add(1) % 10000) + 1;
    }


    // 设置按钮回调函数
    void SerialPort::setButtonCallback(ButtonCallback callback) {
        m_buttonCallback = callback;
    }

    // 遗留兼容方法：设置波特率
    bool SerialPort::setBaudRate(uint32_t baudRate) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_baudRate = baudRate;

        if (m_isOpen) {
            // 统一方法：使用新波特率重新配置端口
            return platformConfigurePort();
        }
        return true;
    }

    // 获取当前波特率
    uint32_t SerialPort::getBaudRate() const {
        return m_baudRate;
    }

    // 获取端口名称
    std::string SerialPort::getPortName() const {
        return m_portName;
    }

    // 写入二进制数据到串口
    bool SerialPort::write(const std::vector<uint8_t>& data) {
        return sendCommand(std::string(data.begin(), data.end()));
    }

    // 写入字符串数据到串口
    bool SerialPort::write(const std::string& data) {
        return sendCommand(data);
    }

    // 从串口读取原始字节数据（遗留方法，不推荐用于高性能场景）
    std::vector<uint8_t> SerialPort::read(size_t maxBytes) {
        // 此为遗留方法 - 不推荐用于高性能场景
        std::vector<uint8_t> buffer;
        if (!m_isOpen || maxBytes == 0) {
            return buffer;
        }

        // 统一读取操作
        buffer.resize(maxBytes);
        ssize_t bytesRead = platformRead(buffer.data(), maxBytes);
        if (bytesRead > 0) {
            buffer.resize(bytesRead);
        }
        else {
            buffer.clear();
        }

        return buffer;
    }

    // 从串口读取字符串数据
    std::string SerialPort::readString(size_t maxBytes) {
        auto data = read(maxBytes);
        return std::string(data.begin(), data.end());
    }

    // 获取可用字节数
    size_t SerialPort::available() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_isOpen) {
            return 0;
        }

        // 统一检查可用字节数
        return const_cast<SerialPort*>(this)->platformBytesAvailable();
    }

    // 刷新串口缓冲区
    bool SerialPort::flush() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_isOpen) {
            return false;
        }

        // 统一刷新操作
        return platformFlush();
    }

    // 设置超时时间
    void SerialPort::setTimeout(uint32_t timeoutMs) {
        m_timeout = timeoutMs;
        if (m_isOpen) {
            platformUpdateTimeouts();
        }
    }

    // 获取超时时间
    uint32_t SerialPort::getTimeout() const {
        return m_timeout;
    }

    // 获取所有可用串口
    // Windows：通过注册表 HARDWARE\\DEVICEMAP\\SERIALCOMM 枚举串口
    // Linux：扫描 /dev 目录下的 ttyUSB* 和 ttyACM* 设备
    std::vector<std::string> SerialPort::getAvailablePorts() {
        std::vector<std::string> ports;

#ifdef _WIN32
        HKEY hKey;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM",
            0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            char valueName[256];
            char data[256];
            DWORD valueNameSize, dataSize, dataType;
            DWORD index = 0;

            while (true) {
                valueNameSize = sizeof(valueName);
                dataSize = sizeof(data);

                LONG result = RegEnumValueA(hKey, index++, valueName, &valueNameSize,
                    nullptr, &dataType,
                    reinterpret_cast<BYTE*>(data), &dataSize);

                if (result == ERROR_NO_MORE_ITEMS) {
                    break;
                }

                if (result == ERROR_SUCCESS && dataType == REG_SZ) {
                    ports.emplace_back(data);
                }
            }

            RegCloseKey(hKey);
        }
#else
        // Linux 实现 - 扫描 /dev 目录下的 tty 设备
        DIR* dir = opendir("/dev");
        if (dir) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != nullptr) {
                std::string name(entry->d_name);
                if (name.substr(0, 6) == "ttyUSB" || name.substr(0, 6) == "ttyACM") {
                    ports.push_back(name);
                }
            }
            closedir(dir);
        }
#endif

        std::sort(ports.begin(), ports.end());
        return ports;
    }

    // 查找 MAKCU 设备端口
    // Windows：使用 SetupAPI 枚举端口设备，通过描述字符串匹配 CH343/CH340
    // Linux：使用 udev 查找 VID:PID = 1A86:55D3 的 USB 串口设备
    std::vector<std::string> SerialPort::findMakcuPorts() {
        std::vector<std::string> makcuPorts;

#ifdef _WIN32
        auto allPorts = getAvailablePorts();
        HDEVINFO deviceInfoSet = SetupDiGetClassDevs(&GUID_DEVCLASS_PORTS,
            nullptr, nullptr, DIGCF_PRESENT);
        if (deviceInfoSet == INVALID_HANDLE_VALUE) {
            return makcuPorts;
        }

        SP_DEVINFO_DATA deviceInfoData;
        deviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

        for (DWORD i = 0; SetupDiEnumDeviceInfo(deviceInfoSet, i, &deviceInfoData); i++) {
            char description[256] = { 0 };
            char portName[256] = { 0 };

            if (SetupDiGetDeviceRegistryPropertyA(deviceInfoSet, &deviceInfoData,
                SPDRP_DEVICEDESC, nullptr,
                reinterpret_cast<BYTE*>(description),
                sizeof(description), nullptr)) {
                std::string desc(description);

                if (desc.find("USB-Enhanced-SERIAL CH343") != std::string::npos ||
                    desc.find("USB-SERIAL CH340") != std::string::npos) {

                    HKEY hDeviceKey = SetupDiOpenDevRegKey(deviceInfoSet, &deviceInfoData,
                        DICS_FLAG_GLOBAL, 0,
                        DIREG_DEV, KEY_READ);
                    if (hDeviceKey != INVALID_HANDLE_VALUE) {
                        DWORD portNameSize = sizeof(portName);

                        if (RegQueryValueExA(hDeviceKey, "PortName", nullptr, nullptr,
                            reinterpret_cast<BYTE*>(portName),
                            &portNameSize) == ERROR_SUCCESS) {
                            std::string port(portName);
                            if (std::find(allPorts.begin(), allPorts.end(), port) != allPorts.end()) {
                                makcuPorts.emplace_back(port);
                            }
                        }
                        RegCloseKey(hDeviceKey);
                    }
                }
            }
        }

        SetupDiDestroyDeviceInfoList(deviceInfoSet);
#else
        // Linux 实现：使用 udev 查找 MAKCU 设备
        struct udev* udev = udev_new();
        if (!udev) {
            return makcuPorts;
        }

        struct udev_enumerate* enumerate = udev_enumerate_new(udev);
        udev_enumerate_add_match_subsystem(enumerate, "tty");
        udev_enumerate_scan_devices(enumerate);

        struct udev_list_entry* devices = udev_enumerate_get_list_entry(enumerate);
        struct udev_list_entry* entry;

        udev_list_entry_foreach(entry, devices) {
            const char* path = udev_list_entry_get_name(entry);
            struct udev_device* dev = udev_device_new_from_syspath(udev, path);

            if (dev) {
                struct udev_device* parent = udev_device_get_parent_with_subsystem_devtype(dev, "usb", "usb_device");
                if (parent) {
                    const char* idVendor = udev_device_get_sysattr_value(parent, "idVendor");
                    const char* idProduct = udev_device_get_sysattr_value(parent, "idProduct");

                    // 检查是否为 MAKCU 设备（VID:PID = 1A86:55D3）
                    bool isMakcuDevice = false;

                    // 主要检查：VID/PID 匹配
                    if (idVendor && idProduct &&
                        strcmp(idVendor, "1a86") == 0 && strcmp(idProduct, "55d3") == 0) {
                        isMakcuDevice = true;
                    }

                    // 备用检查：描述字符串匹配（兼容 Windows 实现）
                    if (!isMakcuDevice) {
                        const char* product = udev_device_get_sysattr_value(parent, "product");
                        if (product) {
                            std::string productStr(product);
                            if (productStr.find("USB-Enhanced-SERIAL CH343") != std::string::npos ||
                                productStr.find("USB-SERIAL CH340") != std::string::npos) {
                                isMakcuDevice = true;
                            }
                        }
                    }

                    if (isMakcuDevice) {
                        const char* devNode = udev_device_get_devnode(dev);
                        if (devNode) {
                            std::string portName = std::string(devNode).substr(5); // 移除 "/dev/" 前缀
                            makcuPorts.push_back(portName);
                        }
                    }
                }
                udev_device_unref(dev);
            }
        }
        
        udev_enumerate_unref(enumerate);
        udev_unref(udev);
#endif

        // 去重并排序后返回
        std::sort(makcuPorts.begin(), makcuPorts.end());
        makcuPorts.erase(std::unique(makcuPorts.begin(), makcuPorts.end()), makcuPorts.end());
        return makcuPorts;
    }

    // 平台抽象辅助方法实现

    // platformOpen：打开设备文件
    // Windows：使用 CreateFileA 打开串口设备
    // Linux：使用 open 系统调用打开设备文件
    bool SerialPort::platformOpen(const std::string& devicePath) {
#ifdef _WIN32
        m_handle = CreateFileA(
            devicePath.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        return m_handle != INVALID_HANDLE_VALUE;
#else
        m_fd = ::open(devicePath.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        return m_fd >= 0;
#endif
    }

    // platformClose：关闭设备文件
    void SerialPort::platformClose() {
#ifdef _WIN32
        if (m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
#else
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
#endif
    }

    // platformConfigurePort：配置串口参数
    // Windows：设置 DCB（设备控制块）结构体，配置波特率、数据位、校验、停止位等
    // Linux：使用 termios 结构体，配置对应参数
    bool SerialPort::platformConfigurePort() {
#ifdef _WIN32
        m_dcb.DCBlength = sizeof(DCB);

        if (!GetCommState(m_handle, &m_dcb)) {
            return false;
        }

        m_dcb.BaudRate = m_baudRate;
        m_dcb.ByteSize = 8;
        m_dcb.Parity = NOPARITY;
        m_dcb.StopBits = ONESTOPBIT;
        m_dcb.fBinary = TRUE;
        m_dcb.fParity = FALSE;
        m_dcb.fOutxCtsFlow = FALSE;
        m_dcb.fOutxDsrFlow = FALSE;
        m_dcb.fDtrControl = DTR_CONTROL_DISABLE;
        m_dcb.fDsrSensitivity = FALSE;
        m_dcb.fTXContinueOnXoff = FALSE;
        m_dcb.fOutX = FALSE;
        m_dcb.fInX = FALSE;
        m_dcb.fErrorChar = FALSE;
        m_dcb.fNull = FALSE;
        m_dcb.fRtsControl = RTS_CONTROL_DISABLE;
        m_dcb.fAbortOnError = FALSE;

        if (!SetCommState(m_handle, &m_dcb)) {
            return false;
        }

        platformUpdateTimeouts();
        return true;
#else
        // Linux 实现：使用 termios
        if (tcgetattr(m_fd, &m_oldTermios) != 0) {
            return false;
        }

        m_newTermios = m_oldTermios;

        // 配置串口参数以匹配 Windows DCB 等效设置
        // 控制标志 - 匹配 Windows DCB 设置
        m_newTermios.c_cflag &= ~PARENB;    // 无校验（DCB.fParity = FALSE）
        m_newTermios.c_cflag &= ~CSTOPB;    // 一个停止位（DCB.StopBits = ONESTOPBIT）
        m_newTermios.c_cflag &= ~CSIZE;     // 清除数据位大小位
        m_newTermios.c_cflag |= CS8;        // 8 位数据（DCB.ByteSize = 8）
        m_newTermios.c_cflag &= ~CRTSCTS;   // 无硬件流控（DCB.fRtsControl/fOutxCtsFlow = FALSE）
        m_newTermios.c_cflag |= CREAD | CLOCAL; // 启用接收器，忽略调制解调线（DCB.fDtrControl = DISABLE）

        // 本地标志 - 类似 Windows 二进制模式的原始输入处理
        m_newTermios.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG); // 原始输入（等效于 DCB.fBinary = TRUE）
        m_newTermios.c_lflag &= ~(ECHOK | ECHONL | IEXTEN);      // 额外禁用回显/处理

        // 输出标志 - 类似 Windows 的原始输出
        m_newTermios.c_oflag &= ~OPOST;     // 原始输出（无后处理）
        m_newTermios.c_oflag &= ~(ONLCR | OCRNL | ONOCR | ONLRET); // 无行尾转换

        // 输入标志 - 匹配 Windows 流控设置
        m_newTermios.c_iflag &= ~(IXON | IXOFF | IXANY); // 无软件流控（DCB.fOutX/fInX = FALSE）
        m_newTermios.c_iflag &= ~(INLCR | ICRNL | IGNCR); // 无行尾转换
        m_newTermios.c_iflag &= ~(ISTRIP | INPCK);        // 无校验剥离（DCB.fParity = FALSE）
        m_newTermios.c_iflag &= ~(BRKINT | IGNBRK);       // 类似 Windows 的中断处理

        // 设置超时 - 游戏优化，匹配 Windows（等效 10ms）
        m_newTermios.c_cc[VMIN] = 0;        // 非阻塞读取
        m_newTermios.c_cc[VTIME] = 1;       // 0.1 秒超时（最小粒度）

        // 设置波特率
        speed_t speed;
        switch (m_baudRate) {
            case 9600:   speed = B9600; break;
            case 19200:  speed = B19200; break;
            case 38400:  speed = B38400; break;
            case 57600:  speed = B57600; break;
            case 115200: speed = B115200; break;
            case 230400: speed = B230400; break;
            case 460800: speed = B460800; break;
            case 921600: speed = B921600; break;
            case 1000000: speed = B1000000; break;
            case 1152000: speed = B1152000; break;
            case 1500000: speed = B1500000; break;
            case 2000000: speed = B2000000; break;
            case 2500000: speed = B2500000; break;
            case 3000000: speed = B3000000; break;
            case 3500000: speed = B3500000; break;
            case 4000000: speed = B4000000; break;
            default:     speed = B115200; break;
        }

        cfsetispeed(&m_newTermios, speed);
        cfsetospeed(&m_newTermios, speed);

        if (tcsetattr(m_fd, TCSANOW, &m_newTermios) != 0) {
            return false;
        }

        // 刷新任何现有数据
        tcflush(m_fd, TCIOFLUSH);

        return true;
#endif
    }

    // platformUpdateTimeouts：更新串口超时设置
    void SerialPort::platformUpdateTimeouts() {
#ifdef _WIN32
        // 游戏优化的超时设置 - 比默认值快得多
        m_timeouts.ReadIntervalTimeout = 1;          // 字节间间隔 1ms
        m_timeouts.ReadTotalTimeoutConstant = 10;    // 总读取超时 10ms
        m_timeouts.ReadTotalTimeoutMultiplier = 1;   // 每字节 1ms
        m_timeouts.WriteTotalTimeoutConstant = 10;   // 写入超时 10ms
        m_timeouts.WriteTotalTimeoutMultiplier = 1;  // 每字节 1ms

        SetCommTimeouts(m_handle, &m_timeouts);
#else
        // Linux 游戏优化超时 - 匹配 Windows 性能
        if (m_isOpen && m_fd >= 0) {
            struct termios currentTermios;
            if (tcgetattr(m_fd, &currentTermios) == 0) {
                // 更新超时设置以匹配当前 m_timeout 值
                // VTIME 以分秒（0.1s）为单位，因此需从 ms 转换
                uint8_t vtime = std::min(255, std::max(1, static_cast<int>(m_timeout / 100)));
                currentTermios.c_cc[VTIME] = vtime;
                currentTermios.c_cc[VMIN] = 0;  // 非阻塞
                tcsetattr(m_fd, TCSANOW, &currentTermios);
            }
        }
#endif
    }

    // platformWrite：向串口写入数据
    ssize_t SerialPort::platformWrite(const void* data, size_t length) {
#ifdef _WIN32
        DWORD bytesWritten = 0;
        bool success = WriteFile(m_handle, data, static_cast<DWORD>(length), &bytesWritten, nullptr);
        return success ? static_cast<ssize_t>(bytesWritten) : -1;
#else
        return ::write(m_fd, data, length);
#endif
    }

    // platformRead：从串口读取数据
    ssize_t SerialPort::platformRead(void* buffer, size_t maxBytes) {
#ifdef _WIN32
        DWORD bytesRead = 0;
        bool success = ReadFile(m_handle, buffer, static_cast<DWORD>(maxBytes), &bytesRead, nullptr);
        return success ? static_cast<ssize_t>(bytesRead) : -1;
#else
        return ::read(m_fd, buffer, maxBytes);
#endif
    }

    // platformBytesAvailable：获取可读字节数
    size_t SerialPort::platformBytesAvailable() {
#ifdef _WIN32
        COMSTAT comStat;
        DWORD errors;
        if (ClearCommError(m_handle, &errors, &comStat)) {
            return comStat.cbInQue;
        }
        return 0;
#else
        int bytesAvailable = 0;
        if (ioctl(m_fd, FIONREAD, &bytesAvailable) >= 0) {
            return static_cast<size_t>(bytesAvailable);
        }
        return 0;
#endif
    }

    // platformFlush：刷新串口缓冲区
    bool SerialPort::platformFlush() {
#ifdef _WIN32
        return FlushFileBuffers(m_handle) != 0;
#else
        return tcdrain(m_fd) == 0;
#endif
    }

    // getLastPlatformError：获取最后错误的字符串描述
    std::string SerialPort::getLastPlatformError() {
#ifdef _WIN32
        DWORD error = GetLastError();
        return "Windows error: " + std::to_string(error);
#else
        return "errno: " + std::to_string(errno);
#endif
    }

} // namespace makcu