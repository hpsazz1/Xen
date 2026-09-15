#ifndef WEAPON_TIMING_H
#define WEAPON_TIMING_H

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace weapon {
// 点射资料独立于GSI接收器和后坐力启用状态；按住从DOWN确认计时，间隔从DOWN提交计时。
struct TimingProfile {
    std::string_view canonical_id;
    int shot_hold_ms = 60;
    int fire_interval_ms = 300;
    bool enabled = true;
};
struct TimingCatalog {
    std::uint64_t revision = 1;
    std::array<TimingProfile, 33> profiles;
};
std::string_view timing_catalog_source() noexcept;
// 默认值来自用户人工测试，不表示逐发击发或后坐力复位测量。
TimingCatalog default_timing_catalog() noexcept;
// 只读快照查找，无分配、锁、文件访问；返回disabled项供UI显示，消费者须检查enabled。
const TimingProfile* find_timing(const TimingCatalog& catalog, std::string_view canonical_id) noexcept;
bool valid_timing_catalog(const TimingCatalog& catalog) noexcept;
// 冷路径接口；失败保留原快照/原文件。加载后ID指向静态存储，可安全复制快照。
bool load_timing_catalog(const std::filesystem::path& path, TimingCatalog& output, std::string& error) noexcept;
bool save_timing_catalog(const std::filesystem::path& path, const TimingCatalog& catalog, std::string& error) noexcept;
} // namespace weapon
#endif
