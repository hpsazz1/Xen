#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "weapon/weapon_timing.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <set>
#include <stdexcept>
#include <vector>

namespace weapon {
namespace {
constexpr TimingCatalog kDefaults{1, {{
    {"p250",60,350,true}, {"deagle",60,600,true}, {"cz75",60,350,true},
    {"fiveseven",60,350,true}, {"glock",60,300,true}, {"tec9",60,300,true},
    {"revolver",300,800,false}, {"p2000",60,300,true}, {"dual_berettas",60,300,true},
    {"usp_s",60,300,true}, {"xm1014",60,500,true}, {"negev",60,300,true},
    {"m249",60,300,true}, {"mag7",60,500,true}, {"sawedoff",60,500,true},
    {"nova",60,500,true}, {"mp7",60,300,true}, {"mp9",60,300,true},
    {"mac10",60,300,true}, {"ump45",60,300,true}, {"p90",60,300,true},
    {"mp5sd",60,300,true}, {"bizon",60,300,true}, {"m4a4",60,300,true},
    {"m4a1_s",60,300,true}, {"aug",60,300,true}, {"famas",60,300,true},
    {"ak47",60,300,true}, {"sg553",60,300,true}, {"galil",60,300,true},
    {"awp",60,1000,true}, {"ssg08",60,800,true}, {"scar20",60,350,true}
}}};
using Json = nlohmann::json;
bool bounded_integer(const Json& value, int maximum) {
    if (!value.is_number_integer()) return false;
    if (value.is_number_unsigned()) return value.get<std::uint64_t>() >= 1 && value.get<std::uint64_t>() <= maximum;
    return value.get<std::int64_t>() >= 1 && value.get<std::int64_t>() <= maximum;
}
} // namespace
std::string_view timing_catalog_source() noexcept { return "weapons-user-20260915"; }
TimingCatalog default_timing_catalog() noexcept { return kDefaults; }
const TimingProfile* find_timing(const TimingCatalog& catalog, std::string_view id) noexcept {
    for (const auto& profile : catalog.profiles) if (profile.canonical_id == id) return &profile;
    return nullptr;
}
bool valid_timing_catalog(const TimingCatalog& catalog) noexcept {
    if (catalog.revision == 0) return false;
    std::array<bool, 33> seen{};
    for (const auto& profile : catalog.profiles) {
        const auto* known = find_timing(kDefaults, profile.canonical_id);
        if (!known || profile.shot_hold_ms < 1 || profile.shot_hold_ms > 500 ||
            profile.fire_interval_ms <= profile.shot_hold_ms || profile.fire_interval_ms > 2000 ||
            (profile.canonical_id == "revolver" && profile.enabled)) return false;
        const auto index = static_cast<std::size_t>(known - kDefaults.profiles.data());
        if (seen[index]) return false;
        seen[index] = true;
    }
    return true;
}
bool load_timing_catalog(const std::filesystem::path& path, TimingCatalog& output, std::string& error) noexcept {
    try {
        error.clear();
        std::ifstream file(path, std::ios::binary);
        if (!file) { error = "无法打开武器点射资料"; return false; }
        // 同时限制读取大小与解析深度，避免错误文件拖慢配置线程。
        std::string text(65537, '\0');
        file.read(text.data(), static_cast<std::streamsize>(text.size()));
        text.resize(static_cast<std::size_t>(file.gcount()));
        if (file.bad() || text.size() > 65536) { error = "武器点射资料读取失败或超过64KiB"; return false; }
        std::vector<std::set<std::string>> keys;
        const auto json = Json::parse(text, [&](int depth, Json::parse_event_t event, Json& parsed) {
            if (depth > 8) throw std::runtime_error("depth");
            if (event == Json::parse_event_t::object_start) keys.emplace_back();
            if (event == Json::parse_event_t::key && !keys.back().insert(parsed.get<std::string>()).second)
                throw std::runtime_error("duplicate key");
            if (event == Json::parse_event_t::object_end) keys.pop_back();
            return true;
        });
        if (!json.is_object() || json.size() != 4 || !json.contains("schema_version") ||
            !bounded_integer(json.at("schema_version"), 1) || !json.contains("revision") || !json.at("revision").is_number_unsigned() || json.at("revision").get<std::uint64_t>() == 0 ||
            !json.contains("source") || !json.at("source").is_string() ||
            json.at("source").get_ref<const std::string&>() != timing_catalog_source() || !json.contains("profiles") ||
            !json.at("profiles").is_array() || json.at("profiles").size() != kDefaults.profiles.size()) {
            error = "武器点射资料版本或完整条目数量无效"; return false;
        }
        auto candidate = kDefaults;
        candidate.revision = json.at("revision").get<std::uint64_t>();
        std::array<bool, 33> seen{};
        for (const auto& row : json.at("profiles")) {
            if (!row.is_object() || row.size() != 4 || !row.contains("canonical_id") ||
                !row.at("canonical_id").is_string() || !row.contains("enabled") || !row.at("enabled").is_boolean() ||
                !row.contains("shot_hold_ms") || !bounded_integer(row.at("shot_hold_ms"),500) ||
                !row.contains("fire_interval_ms") || !bounded_integer(row.at("fire_interval_ms"),2000)) {
                error = "武器点射条目字段或整数范围无效"; return false;
            }
            const auto* known = find_timing(kDefaults, row.at("canonical_id").get_ref<const std::string&>());
            if (!known) { error = "武器点射资料包含未知武器"; return false; }
            const auto index = static_cast<std::size_t>(known - kDefaults.profiles.data());
            if (seen[index]) { error = "武器点射资料包含重复武器"; return false; }
            seen[index] = true;
            candidate.profiles[index].shot_hold_ms = row.at("shot_hold_ms").get<int>();
            candidate.profiles[index].fire_interval_ms = row.at("fire_interval_ms").get<int>();
            candidate.profiles[index].enabled = row.at("enabled").get<bool>();
        }
        if (!valid_timing_catalog(candidate)) { error = "按下间隔须大于按住时长，R8仅留档不可启用"; return false; }
        output = candidate;
        return true;
    } catch (...) { error = "武器点射资料解析失败"; return false; }
}
bool save_timing_catalog(const std::filesystem::path& path, const TimingCatalog& catalog, std::string& error) noexcept {
    std::filesystem::path temporary;
    try {
        error.clear();
        if (!valid_timing_catalog(catalog)) { error = "武器点射资料无效，未保存"; return false; }
        Json rows = Json::array();
        for (const auto& profile : catalog.profiles) rows.push_back({{"canonical_id", profile.canonical_id},
            {"shot_hold_ms", profile.shot_hold_ms}, {"fire_interval_ms", profile.fire_interval_ms}, {"enabled", profile.enabled}});
        const std::string content = Json{{"schema_version",1},{"revision",catalog.revision},{"source",timing_catalog_source()},{"profiles",rows}}.dump(2) + "\n";
        const auto absolute = std::filesystem::absolute(path);
        std::filesystem::create_directories(absolute.parent_path());
        wchar_t name[MAX_PATH]{};
        if (!GetTempFileNameW(absolute.parent_path().c_str(), L"xwt", 0, name)) {
            error = "无法创建武器点射资料临时文件"; return false;
        }
        temporary = name;
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            stream.write(content.data(), static_cast<std::streamsize>(content.size()));
            stream.close();
            if (!stream) throw std::runtime_error("write");
        }
        if (!MoveFileExW(temporary.c_str(), absolute.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("replace");
        return true;
    } catch (...) {
        if (!temporary.empty()) { std::error_code ignored; std::filesystem::remove(temporary, ignored); }
        error = "武器点射资料保存失败，原文件保持不变";
        return false;
    }
}
} // namespace weapon
