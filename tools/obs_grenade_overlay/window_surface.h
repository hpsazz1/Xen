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

// 同步标记专门允许桌面录制捕获；不复用道具窗口的截图排除属性。
inline DWORD prepare_sync_window(HWND window) {
    if (!SetLayeredWindowAttributes(window, 0, 255, LWA_ALPHA)) return GetLastError();
    if (!SetWindowDisplayAffinity(window, WDA_NONE)) return GetLastError();
    DWORD affinity = WDA_EXCLUDEFROMCAPTURE;
    if (!GetWindowDisplayAffinity(window, &affinity)) return GetLastError();
    return affinity == WDA_NONE ? ERROR_SUCCESS : ERROR_INVALID_STATE;
}

// 仅绘制独立小窗，不创建OBS源，也不进入中央320×320 NDI场景。
inline bool paint_sync_window(HDC dc, const RECT& client, const wchar_t* text) {
    const auto background = CreateSolidBrush(RGB(18, 38, 64));
    const auto font = CreateFontW(-22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    if (!background || !font) {
        if (background) DeleteObject(background);
        if (font) DeleteObject(font);
        return false;
    }
    const auto old_font = SelectObject(dc, font);
    const auto old_background = SetBkMode(dc, TRANSPARENT);
    const auto old_color = SetTextColor(dc, RGB(255, 255, 255));
    RECT content = client; InflateRect(&content, -12, -8);
    const bool result = FillRect(dc, &client, background) &&
        DrawTextW(dc, text, -1, &content, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX) > 0;
    SetTextColor(dc, old_color); SetBkMode(dc, old_background); SelectObject(dc, old_font);
    DeleteObject(font); DeleteObject(background);
    return result && GdiFlush();
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
