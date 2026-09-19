#ifndef WEAPON_CATALOG_H
#define WEAPON_CATALOG_H

#include <string_view>

namespace weapon {
// 名称目录与 GSI 接收器、点射参数及弹道校准解耦，供所有消费者使用同一身份。
struct NameProfile {
    std::string_view canonical_id;
    std::string_view gsi_name;
    std::string_view display_name;
};
inline constexpr NameProfile kWeaponNames[] = {
#define XEN_WEAPON(id, gsi, display) {id, gsi, display},
#include "../../assets/weapon_catalog.inc"
#undef XEN_WEAPON
};
inline constexpr const NameProfile* find_gsi_name(std::string_view raw) noexcept {
    for (const auto& row : kWeaponNames) if (row.gsi_name == raw) return &row;
    return nullptr;
}
inline constexpr bool same_name(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}
// 仅接受明确的目录名称；尤其不猜裸 m4a1，旧曲线 ID 与 GSI 中此词含义不同。
inline constexpr std::string_view normalize_weapon_id(std::string_view name) noexcept {
    for (const auto& row : kWeaponNames)
        if (same_name(row.canonical_id, name) || same_name(row.display_name, name) || same_name(row.gsi_name, name))
            return row.canonical_id;
    return {};
}
// 对外持久化使用目录中的真实 GSI 名称，不按内部短名拼接 weapon_。
inline constexpr std::string_view gsi_name(std::string_view name) noexcept {
    const auto canonical = normalize_weapon_id(name);
    for (const auto& row : kWeaponNames) if (row.canonical_id == canonical) return row.gsi_name;
    return {};
}
inline constexpr std::string_view display_name(std::string_view id) noexcept {
    const auto canonical = normalize_weapon_id(id);
    for (const auto& row : kWeaponNames) if (row.canonical_id == canonical) return row.display_name;
    return id.empty() ? std::string_view("未知") : id;
}
} // namespace weapon
#endif
