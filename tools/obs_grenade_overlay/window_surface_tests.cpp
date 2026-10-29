// SPDX-License-Identifier: GPL-2.0-or-later
#include "window_surface.h"
#include "hotkeys.h"
#include <iostream>
#include <set>
#include <string_view>

int main() {
    int failures = 0;
    const auto check = [&](bool value, const char* reason) {
        if (!value) { std::cerr << reason << '\n'; ++failures; }
    };
    check(xen_obs::kHotkeys.size()==7,"热键表应包含原六个菜单/采集入口及独立视频同步组合");
    check(xen_obs::kHotkeys[0].virtual_key==VK_RIGHT&&xen_obs::kHotkeys[1].virtual_key==VK_NUMPAD6&&
        !xen_obs::kHotkeys[0].capture_role&&!xen_obs::kHotkeys[1].capture_role,"原Right/Num6菜单入口保持");
    std::set<std::wstring_view> names;
    std::set<std::pair<UINT,UINT>> keys;
    std::set<ATOM> atoms;
    for (const auto& binding : xen_obs::kHotkeys) {
        check(binding.virtual_key!=VK_F9&&binding.virtual_key!=VK_F10,"旧F9/F10采集热键不得继续注册");
        check(names.insert(binding.atom_name).second,"每个热键应使用独立atom名称");
        check(keys.insert({binding.virtual_key,binding.modifiers}).second,"热键表不能重复键位和修饰键组合");
        check((binding.modifiers&MOD_NOREPEAT)!=0,"普通和蹲姿采集均应保持按住不重复");
        const auto atom=GlobalAddAtomW(binding.atom_name);
        check(atom!=0&&atoms.insert(atom).second,"生产表中的atom应能建立且互不重复");
    }
    for (const auto atom : atoms) GlobalDeleteAtom(atom);
    {
        const auto& sync=xen_obs::kHotkeys.back();
        check(sync.virtual_key==VK_F7 && sync.modifiers==(MOD_CONTROL|MOD_NOREPEAT) &&
            sync.capture_role && std::string_view(sync.capture_role)=="sync",
            "同步只用Ctrl+F7，不复用会启停Runtime的F8基础键");
        const auto window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_LAYERED|
            WS_EX_NOACTIVATE|WS_EX_TRANSPARENT,L"STATIC",L"Xen同步录制离线检查",WS_POPUP,
            -10000,-10000,560,64,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        check(window!=nullptr,"创建独立同步窗口");
        if (window) {
            check(xen_obs::prepare_sync_window(window)==ERROR_SUCCESS,"同步窗口生产初始化允许捕获");
            DWORD affinity=WDA_EXCLUDEFROMCAPTURE;
            check(GetWindowDisplayAffinity(window,&affinity) && affinity==WDA_NONE,
                "同步窗口回读WDA_NONE；菜单与参考卡仍独立截图排除");
            const auto style=GetWindowLongPtrW(window,GWL_EXSTYLE);
            check((style&WS_EX_NOACTIVATE) && (style&WS_EX_TRANSPARENT),"同步窗不抢焦点且鼠标穿透");
            const auto screen=GetDC(nullptr), bitmap_dc=CreateCompatibleDC(screen);
            const auto bitmap=CreateCompatibleBitmap(screen,560,64);
            const auto old=bitmap ? SelectObject(bitmap_dc,bitmap) : nullptr;
            const RECT client{0,0,560,64};
            check(screen && bitmap_dc && bitmap && xen_obs::paint_sync_window(bitmap_dc,client,L"SYNC-obs-test-1"),
                "生产同步绘制函数在隐藏位图中完成，不启动游戏或显示覆盖窗");
            if (old) SelectObject(bitmap_dc,old);
            if (bitmap) DeleteObject(bitmap);
            if (bitmap_dc) DeleteDC(bitmap_dc);
            if (screen) ReleaseDC(nullptr,screen);
            DestroyWindow(window);
        }
    }
    for (const auto key : {UINT(VK_NEXT),UINT(VK_PRIOR)}) {
        const xen_obs::HotkeyBinding* ordinary=nullptr;
        const xen_obs::HotkeyBinding* crouched=nullptr;
        for (const auto& binding : xen_obs::kHotkeys) {
            if(binding.virtual_key!=key) continue;
            if(binding.modifiers==MOD_NOREPEAT) ordinary=&binding;
            if(binding.modifiers==(MOD_CONTROL|MOD_NOREPEAT)) crouched=&binding;
        }
        check(ordinary&&ordinary->capture_role,"PgUp/PgDn应进入单次采集入口");
        check(crouched&&crouched->capture_role,"手动保持Ctrl蹲姿时也应有PgUp/PgDn采集入口");
        if (ordinary&&crouched&&ordinary->capture_role&&crouched->capture_role)
            check(std::string_view(ordinary->capture_role)==crouched->capture_role&&
                std::string_view(ordinary->capture_role)==(key==VK_NEXT?"aim":"standing"),
                "Ctrl采集必须与普通键路由到相同图片角色");
        if (ordinary&&crouched)
            check(std::wstring_view(ordinary->atom_name).ends_with(key==VK_NEXT?L".PageDown":L".PageUp")&&
                std::wstring_view(crouched->atom_name).ends_with(key==VK_NEXT?L".CtrlPageDown":L".CtrlPageUp"),
                "PgUp/PgDn及Ctrl组合应使用各自新atom名称");
    }
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
