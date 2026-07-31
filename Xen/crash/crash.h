#ifndef CRASH_H
#define CRASH_H

#include <atomic>
#include <string>

// 进程级 Windows 崩溃诊断入口。安装阶段允许创建目录和规范化路径；异常
// 处理阶段只使用预分配状态、栈缓冲与 Win32 同步文件 API。
class CrashHandler {
public:
    CrashHandler() noexcept = default;
    ~CrashHandler();

    CrashHandler(const CrashHandler&) = delete;
    CrashHandler& operator=(const CrashHandler&) = delete;
    CrashHandler(CrashHandler&&) = delete;
    CrashHandler& operator=(CrashHandler&&) = delete;

    // 报告固定追加到 <log_dir>/crash_tail.log。同一进程同时只允许一个实例。
    bool install(const std::string& log_dir) noexcept;
    void uninstall() noexcept;
    bool installed() const noexcept;

private:
    std::atomic<bool> installed_{false};
};

#endif // CRASH_H
