#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#ifdef ERROR
#undef ERROR
#endif

// 单线程测试替换注册边界，直接调用生产handler模拟刚安装即到达的事件。
// 不向系统发送控制台事件，也不影响其他进程。
namespace console_registration_fixture {
inline bool inject = false;
inline bool fail_registration = false;
inline DWORD event = CTRL_C_EVENT;
inline int installs = 0;
inline int removes = 0;
inline BOOL handled = FALSE;
inline void reset(bool should_inject, DWORD control_event, bool fail = false) {
    inject = should_inject; event = control_event; fail_registration = fail;
    installs = removes = 0; handled = FALSE;
}
inline BOOL register_handler(PHANDLER_ROUTINE handler, BOOL add) {
    if (!add) { ++removes; return TRUE; }
    ++installs;
    if (fail_registration) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    if (inject) handled = handler(event);
    return TRUE;
}
}
