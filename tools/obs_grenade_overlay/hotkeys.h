// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef XEN_OBS_GRENADE_HOTKEYS_H
#define XEN_OBS_GRENADE_HOTKEYS_H

#include <windows.h>
#include <array>

namespace xen_obs {
struct HotkeyBinding {
    const wchar_t* atom_name;
    UINT virtual_key, modifiers;
    const char* capture_role; // 空值表示原有菜单入口；sync表示录像同步标记。
};
inline constexpr std::array<HotkeyBinding,7> kHotkeys{{
    {L"Xen.OBS.Grenade.Menu.Right",VK_RIGHT,MOD_NOREPEAT,nullptr},
    {L"Xen.OBS.Grenade.Menu.Num6",VK_NUMPAD6,MOD_NOREPEAT,nullptr},
    {L"Xen.OBS.Grenade.Capture.PageDown",VK_NEXT,MOD_NOREPEAT,"aim"},
    {L"Xen.OBS.Grenade.Capture.PageUp",VK_PRIOR,MOD_NOREPEAT,"standing"},
    {L"Xen.OBS.Grenade.Capture.CtrlPageDown",VK_NEXT,MOD_CONTROL|MOD_NOREPEAT,"aim"},
    {L"Xen.OBS.Grenade.Capture.CtrlPageUp",VK_PRIOR,MOD_CONTROL|MOD_NOREPEAT,"standing"},
    {L"Xen.OBS.VideoSync.CtrlF7",VK_F7,MOD_CONTROL|MOD_NOREPEAT,"sync"}
}};
}
#endif
