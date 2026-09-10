#include "weapon/weapon_internal.h"
#include <nlohmann/json.hpp>
#include <iostream>

namespace {
int failures = 0;
void expect(bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << "[失败] " << message << '\n'; }
}
nlohmann::json payload(const weapon::GsiConfig& config, std::uint64_t timestamp = 1700000000) {
    return {
        {"auth", {{"token", config.token}}},
        {"provider", {{"appid", 730}, {"steamid", config.expected_player_id}, {"timestamp", timestamp}}},
        {"player", {{"steamid", config.expected_player_id}, {"activity", "playing"},
            {"state", {{"health", 100}}}, {"weapons", {{"weapon_0", {
                {"name", "weapon_ak47"}, {"state", "active"}, {"ammo_clip", 30},
                {"ammo_clip_max", 30}, {"ammo_reserve", 90}}}}}}}
    };
}
}
int main() {
    using namespace weapon;
    using namespace weapon::detail;
    using namespace std::chrono_literals;
    GsiConfig config;
    expect(!valid_config(config), "GSI默认关闭");
    config.enabled = true;
    config.expected_player_id = "76561198000000000"; // 合成测试身份，不是账号读取结果。
    config.token = std::string(32, 't');
    expect(valid_config(config), "显式本地配置");
    auto invalid_config = config;
    invalid_config.bind_address = "0.0.0.0";
    expect(!valid_config(invalid_config), "LAN监听必须限制源peer");
    invalid_config.allowed_peer_ipv4 = "192.0.2.1";
    expect(valid_config(invalid_config), "LAN显式peer配置");
    invalid_config.expected_player_id.clear();
    expect(!valid_config(invalid_config), "必须明确本玩家身份");

    constexpr std::int64_t utc = 1700000000000;
    auto full = payload(config);
    auto parsed = parse_payload(full.dump(), config, utc + 500);
    expect(parsed.valid && parsed.identity_match && parsed.canonical_id == "ak47" && parsed.ammo_clip == 30 &&
           !parsed.source_order_verified, "合法完整状态与不夸大源顺序");
    expect(canonical_weapon_id("weapon_m4a1") == "m4a4" &&
           canonical_weapon_id("weapon_m4a1_silencer") == "m4a1_s" &&
           canonical_weapon_id("weapon_ak47_extra").empty(), "精确武器映射");
    auto bad = full;
    bad["auth"]["token"] = "incorrect";
    expect(parse_payload(bad.dump(), config, utc).status == Status::AUTH_REJECTED, "错误token拒绝");
    bad = full; bad["player"]["steamid"] = "76561198000000001";
    expect(parse_payload(bad.dump(), config, utc).status == Status::IDENTITY_MISMATCH, "观战或身份切换拒绝");
    bad = full; bad["player"]["activity"] = "textinput";
    expect(parse_payload(bad.dump(), config, utc).status == Status::PLAYER_INACTIVE, "文本输入非有效玩家");
    bad = full; bad["player"]["weapons"]["weapon_0"].erase("ammo_clip");
    parsed = parse_payload(bad.dump(), config, utc);
    expect(!parsed.valid && !parsed.ammo_clip, "缺字段不继承上一状态");
    bad = full; bad["player"]["weapons"]["weapon_0"]["name"] = "weapon_future";
    expect(parse_payload(bad.dump(), config, utc).status == Status::UNKNOWN_WEAPON, "未知武器不可用");
    bad = full; bad["player"]["weapons"]["weapon_0"]["state"] = "reloading";
    expect(parse_payload(bad.dump(), config, utc).status == Status::RELOADING, "换弹保留事实但不补偿");
    bad = full; bad["player"]["weapons"]["weapon_1"] = bad["player"]["weapons"]["weapon_0"];
    expect(!parse_payload(bad.dump(), config, utc).valid, "多活动武器拒绝歧义");
    expect(parse_payload(full.dump(), config, utc + 10000).status == Status::CLOCK_REJECTED, "粗墙钟超容差拒绝");
    bad = full; bad["provider"]["timestamp"] = UINT64_MAX;
    expect(!parse_payload(bad.dump(), config, utc).valid, "源时间溢出拒绝");
    std::string deep(25, '['); deep += '0'; deep += std::string(25, ']');
    expect(!parse_payload(deep, config, utc).valid, "JSON深度有界");

    std::size_t length = 0;
    expect(http_body_length("POST / HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 12\r\n\r\n", 256, length) && length == 12,
           "受限HTTP合法POST");
    expect(!http_body_length("GET / HTTP/1.1\r\nContent-Length: 12\r\n\r\n", 256, length), "非POST拒绝");
    expect(!http_body_length("POST / HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 12\r\nContent-Length: 12\r\n\r\n", 256, length), "重复长度拒绝");
    expect(!http_body_length("POST / HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 12\r\nTransfer-Encoding: chunked\r\n\r\n", 256, length), "chunked歧义拒绝");
    expect(!http_body_length("POST / HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 999999999999999999999999999\r\n\r\n", 256, length), "长度溢出拒绝");

    GsiState state;
    state.reset();
    const auto start = Clock::time_point{} + 1s;
    expect(state.ingest(full.dump(), config, start, utc) == Status::READY, "首份完整快照");
    const auto first = state.snapshot(start);
    expect(first.source_epoch != 0 && first.revision != 0, "本地代际与revision存在");
    expect(state.ingest(full.dump(), config, start + 2s, utc + 2000) == Status::DUPLICATE, "重复包不续TTL");
    expect(state.snapshot(start + 2500ms).status == Status::EXPIRED, "重复不能延长过期边界");
    auto late_change = full;
    late_change["player"]["weapons"]["weapon_0"]["ammo_clip"] = 28;
    expect(state.ingest(late_change.dump(), config, start + 2501ms, utc + 2501) == Status::EXPIRED,
           "未见过同秒变化也不能重开过期窗口");
    auto changed = full;
    changed["player"]["weapons"]["weapon_0"]["ammo_clip"] = 29;
    GsiState same_second;
    same_second.reset();
    same_second.ingest(full.dump(), config, start, utc);
    expect(same_second.ingest(changed.dump(), config, start + 100ms, utc + 100) == Status::READY &&
           same_second.snapshot(start + 100ms).ammo_clip == 29, "同秒合法完整变化可用");
    expect(same_second.snapshot(start + 100ms).valid_until == first.valid_until, "同秒变化不延长时间窗");
    expect(same_second.ingest(full.dump(), config, start + 200ms, utc + 200) == Status::DUPLICATE &&
           same_second.snapshot(start + 200ms).ammo_clip == 29, "见过的同秒状态不能回滚当前弹药");
    changed["provider"]["timestamp"] = 1700000001;
    expect(same_second.ingest(changed.dump(), config, start + 1s, utc + 1000) == Status::READY, "源时间前进建立新TTL窗");
    expect(same_second.ingest(full.dump(), config, start + 1100ms, utc + 1100) == Status::OUT_OF_ORDER &&
           same_second.snapshot(start + 1100ms).ammo_clip == 29, "旧秒包不覆盖新状态");
    bad = changed; bad["player"]["steamid"] = "76561198000000001";
    same_second.ingest(bad.dump(), config, start + 1200ms, utc + 1200);
    expect(!same_second.snapshot(start + 1200ms).valid && same_second.snapshot(start + 1200ms).source_epoch != first.source_epoch,
           "身份失效增加本地连续性代际");
    same_second.reset();
    expect(!same_second.snapshot(start + 1201ms).valid, "stop/reset清空旧武器，无手动回退");
    GsiState denied;
    denied.reset();
    denied.ingest(full.dump(), config, start, utc);
    bad = full; bad["auth"]["token"] = "wrong";
    expect(denied.ingest(bad.dump(), config, start + 100ms, utc + 100) == Status::AUTH_REJECTED &&
           denied.snapshot(start + 100ms).revision == 1 && denied.snapshot(start + 100ms).valid_until == first.valid_until,
           "未鉴权请求不能覆盖或续命合法快照");
    // 只测试纯解码/时间状态，不start接收器、不监听、不访问游戏或设备。
    return failures == 0 ? 0 : 1;
}
