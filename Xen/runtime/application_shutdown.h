#pragma once

// Requests a process-wide shutdown and wakes every worker that may be blocked
// on a condition variable. Safe to call repeatedly and from any worker thread.
void RequestApplicationShutdown() noexcept;

// 注册 Windows 控制台关闭/Ctrl+C 处理器，使关闭 DOS 窗口也走统一线程唤醒和资源清理流程。
bool InstallConsoleShutdownHandler() noexcept;

// 主线程完成全部 join 和设备释放后通知控制台关闭处理器，避免系统在清理完成前结束进程。
void MarkApplicationShutdownComplete() noexcept;
