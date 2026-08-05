#ifndef MOUSE_INPUT_INTERNAL_H
#define MOUSE_INPUT_INTERNAL_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace mouse::detail {

// 只覆盖 HID 键盘报告中可用于 Xen 全局绑定的确定性映射；未知 usage 必须忽略。
inline int hid_usage_to_virtual_key(std::uint8_t usage) noexcept {
    if (usage >= 0x04U && usage <= 0x1dU) {
        return 0x41 + static_cast<int>(usage - 0x04U);
    }
    if (usage >= 0x1eU && usage <= 0x26U) {
        return 0x31 + static_cast<int>(usage - 0x1eU);
    }
    switch (usage) {
        case 0x27: return 0x30;
        case 0x28: return 0x0d;
        case 0x29: return 0x1b;
        case 0x2a: return 0x08;
        case 0x2b: return 0x09;
        case 0x2c: return 0x20;
        case 0x39: return 0x14;
        case 0x4a: return 0x24;
        case 0x49: return 0x2d;
        case 0x4b: return 0x21;
        case 0x4d: return 0x23;
        case 0x4c: return 0x2e;
        case 0x4e: return 0x22;
        case 0x4f: return 0x27;
        case 0x50: return 0x25;
        case 0x51: return 0x28;
        case 0x52: return 0x26;
        case 0x3a: return 0x70;
        case 0x3b: return 0x71;
        case 0x3c: return 0x72;
        case 0x3d: return 0x73;
        case 0x3e: return 0x74;
        case 0x3f: return 0x75;
        case 0x40: return 0x76;
        case 0x41: return 0x77;
        case 0x42: return 0x78;
        case 0x43: return 0x79;
        case 0x44: return 0x7a;
        case 0x45: return 0x7b;
        case 0xe0: return 0xa2;
        case 0xe1: return 0xa0;
        case 0xe2: return 0xa4;
        case 0xe3: return 0x5b;
        case 0xe4: return 0xa3;
        case 0xe5: return 0xa1;
        case 0xe6: return 0xa5;
        case 0xe7: return 0x5c;
        default: return 0;
    }
}

inline void apply_hid_keyboard_report(
        std::uint8_t modifiers,
        const std::uint8_t* keys,
        std::size_t key_count,
        std::array<bool, 256>& active) noexcept {
    active.fill(false);
    static constexpr std::uint8_t kModifierUsages[8] =
        {0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7};
    for (std::size_t index = 0; index < 8U; ++index) {
        const int virtual_key = hid_usage_to_virtual_key(kModifierUsages[index]);
        if (virtual_key != 0) {
            active[static_cast<std::size_t>(virtual_key)] =
                (modifiers & static_cast<std::uint8_t>(1U << index)) != 0;
        }
    }
    for (std::size_t index = 0; index < key_count; ++index) {
        const int virtual_key = hid_usage_to_virtual_key(keys[index]);
        if (virtual_key != 0) {
            active[static_cast<std::size_t>(virtual_key)] = true;
        }
    }
    active[0x11] = active[0xa2] || active[0xa3];
    active[0x10] = active[0xa0] || active[0xa1];
    active[0x12] = active[0xa4] || active[0xa5];
}

} // namespace mouse::detail

#endif // MOUSE_INPUT_INTERNAL_H
