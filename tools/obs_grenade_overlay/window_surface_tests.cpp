// SPDX-License-Identifier: GPL-2.0-or-later
#include "window_surface.h"
#include <iostream>

int main() {
    int failures = 0;
    const auto check = [&](bool value, const char* reason) {
        if (!value) { std::cerr << reason << '\n'; ++failures; }
    };
    for (const DWORD extra : {DWORD(0), DWORD(WS_EX_NOACTIVATE | WS_EX_TRANSPARENT)}) {
        const auto window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED | extra,
            L"STATIC", L"Xen截图排除离线检查", WS_POPUP, -10000, -10000, 800, 430,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window) { check(false, "创建测试窗口失败"); continue; }
        // 走菜单和参考卡共用的生产初始化函数；不创建OBS场景、不显示窗口或输出输入。
        check(xen_obs::prepare_desktop_window(window, 235) == ERROR_SUCCESS, "透明属性初始化后截图排除应成功");
        DWORD affinity = WDA_NONE;
        check(GetWindowDisplayAffinity(window, &affinity) && affinity == WDA_EXCLUDEFROMCAPTURE,
            "真实窗口应回读0x11，不能只记录调用过API");
        for (const int height : {430, 576, 112}) {
            SetWindowPos(window, nullptr, -10000, -10000, 800, height, SWP_NOACTIVATE | SWP_NOZORDER);
            check(xen_obs::round_desktop_window(window, 800, height), "菜单、参考卡与通知尺寸均应建立圆角");
            const auto region = CreateRectRgn(0, 0, 0, 0);
            check(region && GetWindowRgn(window, region) != ERROR, "窗口应具有实际裁剪区域");
            if (region) {
                check(!PtInRegion(region, 0, 0) && PtInRegion(region, 400, height / 2),
                    "圆角排除外角但保留内容中心");
                DeleteObject(region);
            }
            check(GetWindowDisplayAffinity(window, &affinity) && affinity == WDA_EXCLUDEFROMCAPTURE,
                "调整尺寸和圆角后截图排除保持");
        }
        DestroyWindow(window);
    }
    return failures ? 1 : 0;
}
