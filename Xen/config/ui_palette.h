#ifndef XEN_UI_PALETTE_H
#define XEN_UI_PALETTE_H
#include "config/ui_theme.h"

namespace xen_ui {
// codex-theme-v1 的颜色被收敛为语义令牌，页面代码不直接散落颜色值。
constexpr unsigned int kInk = 0x1a1c1f;
constexpr unsigned int kMutedInk = 0x626a73;
constexpr unsigned int kFaintInk = 0x66707c;
constexpr unsigned int kAccent = 0x339cff;
constexpr unsigned int kAccentStrong = 0x0d6fc2;
constexpr unsigned int kAccentSoft = 0xeaf4ff;
constexpr unsigned int kSurface = 0xffffff;
constexpr unsigned int kCanvas = 0xf4f4f5;
constexpr unsigned int kSidebar = 0xf3f3f4;
constexpr unsigned int kGroupSurface = 0xfcfcfc;
constexpr unsigned int kFieldSurface = 0xf7f7f8;
constexpr unsigned int kBorder = 0xe6e6e7;
constexpr unsigned int kBorderStrong = 0xd8d8da;
constexpr unsigned int kNavSelected = 0xe5e5e7;
constexpr unsigned int kNavHovered = 0xebebed;
constexpr unsigned int kSuccess = 0x00a240;
constexpr unsigned int kSuccessSoft = 0xe7f6ed;
constexpr unsigned int kWarning = 0x9a5a00;
constexpr unsigned int kWarningSoft = 0xfff3df;
constexpr unsigned int kDanger = 0xba2623;
constexpr unsigned int kDangerSoft = 0xffeceb;
constexpr unsigned int kSkill = 0x924ff7;
constexpr unsigned int kOnAccent = 0xfefefe;


constexpr unsigned int themed_rgb(UiTheme theme, unsigned int rgb) noexcept {
    if (theme == UiTheme::LIGHT) return rgb;
    switch (rgb) {
        case kInk: return 0xffffff;
        case kMutedInk: return 0xa7abb1;
        case kFaintInk: return 0x858b93;
        case kAccentStrong: return 0x5aadff;
        case kAccentSoft: return 0x203448;
        case kSurface: return 0x181818;
        case kCanvas: return 0x101010;
        case kSidebar: return 0x141414;
        case kGroupSurface: return 0x1f1f1f;
        case kFieldSurface: return 0x292929;
        case kBorder: return 0x2d2d2d;
        case kBorderStrong: return 0x3a3a3a;
        case kNavSelected: return 0x2b2b2b;
        case kNavHovered: return 0x242424;
        case kSuccess: return 0x40c977;
        case kSuccessSoft: return 0x183626;
        case kWarning: return 0xf0ad4e;
        case kWarningSoft: return 0x3a2c18;
        case kDanger: return 0xfa423e;
        case kDangerSoft: return 0x3d1f1f;
        case kSkill: return 0xad7bf9;
        case 0xd9eaff: return 0x263b4e;
        case 0xd8ebff: return 0x21374b;
        case 0xaeb7c3: return 0x666b72;
        case 0xe3e6e9: return 0x303030;
        case 0xd9dde1: return 0x303030;
        case 0xdde9f6: return 0x283c50;
        case 0xcfe2f5: return 0x2c465e;
        case 0xf4f6f8: return 0x242424;
        case 0xf5f6f8: return 0x242424;
        case 0xf8f9fa: return 0x202020;
        case 0xe7eaee: return 0x383838;
        case 0xeff1f3: return 0x2a2a2a;
        case 0xc2c7cc: return 0x60646a;
        case 0xcbd0d5: return 0x55595f;
        default: return rgb;
    }
}
} // namespace xen_ui
#endif
