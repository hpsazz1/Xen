#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "runtime/application_shutdown.h"

#include "Xen.h"
#include "capture.h"
#include "runtime/thread_loops.h"

namespace
{
HANDLE g_shutdownCompleteEvent = nullptr;

BOOL WINAPI ConsoleShutdownHandler(DWORD controlType)
{
    switch (controlType)
    {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        // 处理器运行在系统线程中，只请求幂等关机并唤醒等待者；实际资源释放仍由主线程完成。
        RequestApplicationShutdown();
        // 对关闭窗口/注销/关机事件，Windows 会在处理器返回后结束进程。最多留出 4 秒让主线程
        // 完成 join 和设备断开；超过系统宽限期时仍应返回，避免控制台主机永久等待。
        if (controlType == CTRL_CLOSE_EVENT ||
            controlType == CTRL_LOGOFF_EVENT ||
            controlType == CTRL_SHUTDOWN_EVENT)
        {
            if (g_shutdownCompleteEvent)
                WaitForSingleObject(g_shutdownCompleteEvent, 4000);
        }
        return TRUE;
    default:
        return FALSE;
    }
}
}

void RequestApplicationShutdown() noexcept
{
    shouldExit.store(true);
    gameOverlayShouldExit.store(true);

#ifdef USE_CUDA
    trt_detector.requestStop();
#else
    if (dml_detector)
        dml_detector->requestStop();
#endif

    frameCV.notify_all();
    detectionBuffer.cv.notify_all();
}

bool InstallConsoleShutdownHandler() noexcept
{
    if (!g_shutdownCompleteEvent)
        g_shutdownCompleteEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_shutdownCompleteEvent)
        return false;
    return SetConsoleCtrlHandler(ConsoleShutdownHandler, TRUE) != FALSE;
}

void MarkApplicationShutdownComplete() noexcept
{
    if (g_shutdownCompleteEvent)
        SetEvent(g_shutdownCompleteEvent);
}
