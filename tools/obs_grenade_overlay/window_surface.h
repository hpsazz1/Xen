// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>

namespace xen_obs {
// 先初始化分层窗口透明属性，再设置截图排除；本机Win10在相反顺序下返回error=8。
inline DWORD prepare_desktop_window(HWND window, BYTE opacity) {
    if (!SetLayeredWindowAttributes(window, 0, opacity, LWA_ALPHA)) return GetLastError();
    if (!SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE)) return GetLastError();
    DWORD affinity = WDA_NONE;
    if (!GetWindowDisplayAffinity(window, &affinity)) return GetLastError();
    return affinity == WDA_EXCLUDEFROMCAPTURE ? ERROR_SUCCESS : ERROR_INVALID_STATE;
}

// 仅裁剪原生窗口轮廓；内容仍全部由OBS绘制。
inline bool round_desktop_window(HWND window, int width, int height) {
    const auto region = CreateRoundRectRgn(0, 0, width + 1, height + 1, 24, 24);
    if (!region) return false;
    if (SetWindowRgn(window, region, TRUE)) return true; // 成功后由系统接管region。
    DeleteObject(region);
    return false;
}
}
