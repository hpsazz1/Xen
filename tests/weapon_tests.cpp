#include "weapon/weapon_internal.h"
#include "weapon/weapon_catalog.h"
#include <nlohmann/json.hpp>
#include <iostream>

namespace {
int failures = 0;
void expect(bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << "[失败] " << message << '\n'; }
}
nlohmann::json payload(const weapon::GsiConfig& config, std::uint64_t timestamp = 1700000000) {
    return {
        {"provider", {{"appid", 730}, {"steamid", "76561198000000000"}, {"timestamp", timestamp}}},
        {"map", {{"phase", "live"}}}, {"round", {{"phase", "live"}}},
        {"player", {{"steamid", "76561198000000000"}, {"activity", "playing"},
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
    expect(valid_config(config), "显式本地配置");
    auto invalid_config = config;
    invalid_config.bind_address = "0.0.0.0";
    expect(!valid_config(invalid_config), "LAN监听必须限制源peer");
    invalid_config.allowed_peer_ipv4 = "192.0.2.1";
    expect(valid_config(invalid_config), "LAN显式peer配置");
    expect(valid_config(invalid_config), "不需要固定玩家身份配置");

    constexpr std::int64_t utc = 1700000000000;
    auto full = payload(config);
    auto parsed = parse_payload(full.dump(), config, utc + 500);
    expect(parsed.team == Team::UNKNOWN && parsed.valid, "旧payload缺阵营仍保留武器有效性");
    auto team_payload = full;
    team_payload["player"]["team"] = "CT";
    expect(parse_payload(team_payload.dump(), config, utc).team == Team::CT, "精确读取本地CT阵营");
    team_payload["player"]["team"] = "T";
    expect(parse_payload(team_payload.dump(), config, utc).team == Team::T, "精确读取本地T阵营");
    for (const auto& value : {nlohmann::json("ct"), nlohmann::json("SPECTATOR"), nlohmann::json(3), nlohmann::json(nullptr)}) {
        team_payload["player"]["team"] = value;
        const auto unknown_team = parse_payload(team_payload.dump(), config, utc);
        expect(unknown_team.team == Team::UNKNOWN && unknown_team.valid, "阵营未知值不猜测且不改变武器契约");
    }
    team_payload["player"]["team"] = "CT";
    team_payload["player"]["steamid"] = "76561198000000001";
    expect(parse_payload(team_payload.dump(), config, utc).team == Team::UNKNOWN, "观战阵营不作为自身阵营");
    team_payload = full; team_payload["player"]["team"] = "CT";
    team_payload["player"]["state"]["health"] = 0;
    expect(parse_payload(team_payload.dump(), config, utc).team == Team::UNKNOWN, "死亡快照不提供可信阵营");
    expect(parse_payload(team_payload.dump(), config, utc).game_phase == GamePhase::UNKNOWN, "死亡快照也不提供准备态豁免");
    auto phases = full;
    expect(parse_payload(phases.dump(), config, utc).game_phase == GamePhase::ACTIVE, "正式地图正式回合为敌方过滤阶段");
    phases["round"]["phase"] = "over";
    expect(parse_payload(phases.dump(), config, utc).game_phase == GamePhase::ACTIVE, "回合结束仍维持敌方过滤");
    phases["round"]["phase"] = "freezetime";
    expect(parse_payload(phases.dump(), config, utc).game_phase == GamePhase::PREPARATION, "正式地图冻结时间为准备阶段");
    phases["map"]["phase"] = "warmup"; phases.erase("round");
    parsed = parse_payload(phases.dump(), config, utc);
    expect(parsed.game_phase == GamePhase::PREPARATION && parsed.team == Team::UNKNOWN && parsed.valid,
           "可信热身不依赖round或已知阵营且不改变武器有效性");
    for (const auto& value : {nlohmann::json("gameover"), nlohmann::json("intermission"), nlohmann::json("Warmup"), nlohmann::json(1)}) {
        phases["map"]["phase"] = value;
        expect(parse_payload(phases.dump(), config, utc).game_phase == GamePhase::UNKNOWN, "比赛结束或未知地图阶段不可当准备");
    }
    phases = full; phases.erase("map");
    expect(parse_payload(phases.dump(), config, utc).game_phase == GamePhase::UNKNOWN, "缺地图不继承准备状态");
    phases = full; phases.erase("round");
    expect(parse_payload(phases.dump(), config, utc).game_phase == GamePhase::UNKNOWN, "正式地图缺回合不放行");
    phases["round"]["phase"] = "paused";
    expect(parse_payload(phases.dump(), config, utc).game_phase == GamePhase::UNKNOWN, "不猜测未支持的回合阶段");
    parsed = parse_payload(full.dump(), config, utc + 500);
    expect(parsed.valid && parsed.identity_match && parsed.canonical_id == "ak47" && parsed.ammo_clip == 30 &&
           !parsed.source_order_verified, "合法完整状态与不夸大源顺序");
    expect(canonical_weapon_id("weapon_m4a1") == "m4a4" &&
           canonical_weapon_id("weapon_m4a1_silencer") == "m4a1_s" &&
           canonical_weapon_id("weapon_ak47_extra").empty(), "精确武器映射");
    expect(display_name("m4a1_s") == "M4A1-S" && display_name("weapon_m4a1") == "M4A4" &&
           normalize_weapon_id("m4a1").empty(), "旧文件名不可混淆两把M4身份");
    for (const auto& name : kWeaponNames) {
        auto named = full;
        named["player"]["weapons"]["weapon_0"]["name"] = name.gsi_name;
        const auto identified = parse_payload(named.dump(), config, utc);
        expect(identified.valid && identified.canonical_id == name.canonical_id &&
               normalize_weapon_id(name.display_name) == name.canonical_id, "GSI解析与展示名称共用身份");
    }
    expect(canonical_weapon_id("AK-47").empty() && canonical_weapon_id("WEAPON_AK47").empty(),
           "显示别名不放宽GSI协议入口");
    auto bad = full;
    bad["auth"]["token"] = "incorrect";
    expect(parse_payload(bad.dump(), config, utc).status == Status::READY, "旧auth字段忽略，无需令牌");
    bad = full; bad["player"]["steamid"] = "76561198000000001";
    expect(parse_payload(bad.dump(), config, utc).status == Status::IDENTITY_MISMATCH, "观战身份不匹配拒绝");
    bad = full; bad["player"]["activity"] = "textinput";
    expect(parse_payload(bad.dump(), config, utc).status == Status::PLAYER_INACTIVE, "文本输入非有效玩家");
    bad = full; bad["player"]["state"]["health"] = 0;
    parsed = parse_payload(bad.dump(), config, utc);
    expect(!parsed.valid && parsed.status == Status::PLAYER_INACTIVE, "自身死亡使共享武器上下文无效，不能维持人工急停");
    expect(parsed.player_playing && parsed.player_health == 0, "独立保留确认死亡，不放宽Trigger原valid");
    bad["player"]["state"].erase("health");
    parsed = parse_payload(bad.dump(), config, utc);
    expect(!parsed.valid && !parsed.player_health && parsed.status == Status::PLAYER_INACTIVE,
           "缺失健康与确认死亡可辨，但Trigger原状态不变");
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
    GsiState phase_state;
    auto preparation = full; preparation["map"]["phase"] = "warmup";
    phase_state.ingest(preparation.dump(), config, start, utc);
    const auto first_preparation = phase_state.snapshot(start);
    preparation["player"]["weapons"]["weapon_0"]["ammo_clip"] = 29;
    phase_state.ingest(preparation.dump(), config, start + 1ms, utc + 1);
    expect(phase_state.snapshot(start + 1ms).team_epoch == first_preparation.team_epoch,
           "准备阶段阵营未知的普通武器变化不反复撤销策略");
    auto active = preparation; active["map"]["phase"] = "live"; active["player"]["team"] = "CT";
    phase_state.ingest(active.dump(), config, start + 2ms, utc + 2);
    const auto first_active = phase_state.snapshot(start + 2ms);
    expect(first_active.game_phase == GamePhase::ACTIVE && first_active.team_epoch > first_preparation.team_epoch &&
           first_active.control_safety_epoch == first_preparation.control_safety_epoch,
           "同秒准备转正式会发布独立策略断点而不改变旧武器控制代际");
    expect(phase_state.ingest(preparation.dump(), config, start + 3ms, utc + 3) == Status::DUPLICATE,
           "同秒旧准备帧仍受重放去重");
    const auto ambiguous_phase = phase_state.snapshot(start + 3ms);
    expect(ambiguous_phase.game_phase == GamePhase::UNKNOWN && ambiguous_phase.team_epoch > first_active.team_epoch &&
           ambiguous_phase.valid && ambiguous_phase.valid_until == first_active.valid_until,
           "同秒阶段回退撤销旧准备许可但不改武器或续期");
    preparation["provider"]["timestamp"] = 1700000001;
    phase_state.ingest(preparation.dump(), config, start + 1s, utc + 1000);
    expect(phase_state.snapshot(start + 1s).game_phase == GamePhase::PREPARATION &&
           phase_state.snapshot(start + 1s).team_epoch > first_preparation.team_epoch,
           "跳过正式与未知中间快照也可识别准备策略已换代");
    expect(phase_state.snapshot(start + 4s).game_phase == GamePhase::UNKNOWN,
           "过期准备态不可继续放行全类别");
    GsiState phase_only;
    auto same_team = full; same_team["player"]["team"] = "CT"; same_team["round"]["phase"] = "freezetime";
    phase_only.ingest(same_team.dump(), config, start, utc);
    const auto freezing = phase_only.snapshot(start);
    same_team["round"]["phase"] = "live";
    expect(phase_only.ingest(same_team.dump(), config, start + 1ms, utc + 1) == Status::READY &&
           phase_only.snapshot(start + 1ms).game_phase == GamePhase::ACTIVE &&
           phase_only.snapshot(start + 1ms).team_epoch > freezing.team_epoch,
           "只改变回合阶段的同秒快照不会被去重吞掉");
    GsiState teams;
    auto ct = full; ct["player"]["team"] = "CT";
    auto t = full; t["player"]["team"] = "T";
    teams.ingest(ct.dump(), config, start, utc);
    const auto initial_team = teams.snapshot(start);
    expect(initial_team.team == Team::CT && initial_team.team_epoch > 0, "初始可信阵营含独立代际");
    expect(teams.ingest(ct.dump(), config, start + 1ms, utc + 1) == Status::DUPLICATE &&
           teams.snapshot(start + 1ms).team_epoch == initial_team.team_epoch, "相同阵营重复包不制造断点");
    expect(teams.ingest(t.dump(), config, start + 2ms, utc + 2) == Status::READY, "同秒阵营变化不会被去重吞掉");
    const auto changed_team = teams.snapshot(start + 2ms);
    expect(changed_team.team == Team::T && changed_team.team_epoch > initial_team.team_epoch &&
           changed_team.control_safety_epoch == initial_team.control_safety_epoch &&
           changed_team.recoil_safety_epoch == initial_team.recoil_safety_epoch &&
           changed_team.source_epoch == initial_team.source_epoch, "阵营变化只更新独立阵营代际");
    expect(teams.ingest(ct.dump(), config, start + 3ms, utc + 3) == Status::DUPLICATE,
           "同秒已见过完整状态仍保留武器去重");
    const auto ambiguous_team = teams.snapshot(start + 3ms);
    expect(ambiguous_team.team == Team::UNKNOWN && ambiguous_team.team_epoch > changed_team.team_epoch &&
           ambiguous_team.valid && ambiguous_team.revision == changed_team.revision &&
           ambiguous_team.valid_until == changed_team.valid_until, "同秒阵营回退不可判序时撤销阵营并保留武器快照");
    ct["provider"]["timestamp"] = 1700000001;
    teams.ingest(ct.dump(), config, start + 1s, utc + 1000);
    const auto restored_team = teams.snapshot(start + 1s);
    expect(restored_team.team == Team::CT && restored_team.team_epoch > initial_team.team_epoch,
           "消费者跳过CT到T到CT中间状态仍能识别代际断点");
    expect(teams.snapshot(start + 4s).team == Team::UNKNOWN, "过期快照不能继续提供阵营");
    ct["provider"]["timestamp"] = 1700000004;
    teams.ingest(ct.dump(), config, start + 4s, utc + 4000);
    const auto after_expiry = teams.snapshot(start + 4s);
    expect(after_expiry.team == Team::CT && after_expiry.team_epoch > restored_team.team_epoch,
           "同阵营过期后恢复也保留可信断点");
    auto unknown = ct; unknown["player"].erase("team");
    teams.ingest(unknown.dump(), config, start + 4001ms, utc + 4001);
    ct["provider"]["timestamp"] = 1700000005;
    teams.ingest(ct.dump(), config, start + 5s, utc + 5000);
    expect(teams.snapshot(start + 5s).team == Team::CT &&
           teams.snapshot(start + 5s).team_epoch > after_expiry.team_epoch,
           "跳过缺失阵营中间快照后同阵营也不能沿用旧目标");
    auto spectator = ct; spectator["player"]["steamid"] = "76561198000000001";
    teams.ingest(spectator.dump(), config, start + 5001ms, utc + 5001);
    const auto after_identity_break = teams.snapshot(start + 5001ms);
    expect(after_identity_break.team == Team::UNKNOWN && after_identity_break.team_epoch > after_expiry.team_epoch,
           "身份失信持久撤销阵营连续性");
    expect(state.ingest(full.dump(), config, start, utc) == Status::READY, "首份完整快照");
    const auto first = state.snapshot(start);
    for (const auto& [name, type] : {std::pair{"weapon_knife", "Knife"}, {"weapon_knife_t", "Knife"},
            {"weapon_bayonet", "Knife"}, {"weapon_hegrenade", "Grenade"}, {"weapon_flashbang", "Grenade"},
            {"weapon_smokegrenade", "Grenade"}, {"weapon_molotov", "Grenade"}, {"weapon_incgrenade", "Grenade"},
            {"weapon_decoy", "Grenade"}, {"weapon_c4", "C4"}}) {
        GsiState transition;
        transition.ingest(full.dump(), config, start, utc);
        const auto before = transition.snapshot(start);
        auto item = full;
        item["player"]["weapons"]["weapon_0"] = {{"name", name}, {"type", type}, {"state", "active"}};
        expect(transition.ingest(item.dump(), config, start + 1ms, utc + 1) == Status::NON_FIREARM,
               "明确非枪装备无需虚构弹药字段");
        const auto held = transition.snapshot(start + 1ms);
        expect(!held.valid && !held.ammo_clip && held.canonical_id.empty() &&
               held.control_safety_epoch == before.control_safety_epoch &&
               held.recoil_safety_epoch != before.recoil_safety_epoch && held.source_epoch != before.source_epoch,
               "非枪暂挂控制并撤销旧工作，保留压枪独立撤销");
        auto restored = full;
        restored["provider"]["timestamp"] = 1700000001;
        transition.ingest(restored.dump(), config, start + 1s, utc + 1000);
        expect(transition.snapshot(start + 1s).control_safety_epoch == before.control_safety_epoch &&
               transition.snapshot(start + 1s).source_epoch != held.source_epoch, "回枪保持持键会话但新建工作");
        item["player"]["weapons"]["weapon_0"]["type"] = "Rifle";
        expect(parse_payload(item.dump(), config, utc).status == Status::UNKNOWN_WEAPON, "非枪类型矛盾不自动恢复");
        item["player"]["weapons"]["weapon_0"].erase("type");
        expect(parse_payload(item.dump(), config, utc).status == Status::UNKNOWN_WEAPON, "缺失装备类型不猜测");
    }
    GsiState recoil_continuity;
    recoil_continuity.reset();
    recoil_continuity.ingest(full.dump(), config, start, utc);
    const auto trusted = recoil_continuity.snapshot(start).recoil_safety_epoch;
    const auto control_trusted = recoil_continuity.snapshot(start).control_safety_epoch;
    auto ordinary = full;
    ordinary["player"]["weapons"]["weapon_0"]["state"] = "reloading";
    recoil_continuity.ingest(ordinary.dump(), config, start + 1ms, utc + 1);
    expect(recoil_continuity.snapshot(start + 1ms).recoil_safety_epoch == trusted,
           "普通换弹不撤销压枪可信代际");
    expect(recoil_continuity.snapshot(start + 1ms).control_safety_epoch == control_trusted,
           "正常换弹保留控制持键会话");
    ordinary = full; ordinary["player"]["state"]["health"] = 0;
    recoil_continuity.ingest(ordinary.dump(), config, start + 2ms, utc + 2);
    expect(recoil_continuity.snapshot(start + 2ms).recoil_safety_epoch == trusted,
           "确认死亡是普通武器停止原因");
    expect(recoil_continuity.snapshot(start + 2ms).control_safety_epoch != control_trusted,
           "确认死亡撤销Aim和扳机持键会话，保持压枪既有独立语义");
    ordinary["player"]["state"].erase("health");
    recoil_continuity.ingest(ordinary.dump(), config, start + 3ms, utc + 3);
    expect(recoil_continuity.snapshot(start + 3ms).recoil_safety_epoch != trusted,
           "同秒同PLAYER_INACTIVE但缺health不能去重成已确认死亡");
    auto recovered = payload(config, 1700000001);
    recoil_continuity.ingest(recovered.dump(), config, start + 1s, utc + 1000);
    const auto after_unknown = recoil_continuity.snapshot(start + 1s).recoil_safety_epoch;
    expect(after_unknown != trusted && recoil_continuity.snapshot(start + 1s).valid,
           "消费者跳过UNKNOWN中间帧仍能看到可信代际改变");
    recovered = payload(config, 1700000005);
    recoil_continuity.ingest(recovered.dump(), config, start + 5s, utc + 5000);
    expect(recoil_continuity.snapshot(start + 5s).recoil_safety_epoch != after_unknown,
           "未读取过期快照也不能跳过TTL间断后自动恢复");
    const auto before_identity = recoil_continuity.snapshot(start + 5s).recoil_safety_epoch;
    recovered["provider"]["steamid"] = "76561198000000002";
    recovered["player"]["steamid"] = "76561198000000002";
    recoil_continuity.ingest(recovered.dump(), config, start + 5001ms, utc + 5001);
    expect(recoil_continuity.snapshot(start + 5001ms).recoil_safety_epoch != before_identity,
           "合法新身份不能按同枪换枪静默继承压枪会话");
    expect(first.source_epoch != 0 && first.revision != 0, "本地代际与revision存在");
    GsiState accounts;
    accounts.reset();
    accounts.ingest(full.dump(), config, start, utc);
    auto switched = full;
    switched["provider"]["steamid"] = "76561198000000002";
    switched["player"]["steamid"] = "76561198000000002";
    expect(accounts.ingest(switched.dump(), config, start + 10ms, utc + 10) == Status::READY &&
           accounts.snapshot(start + 10ms).source_epoch != first.source_epoch,
           "同秒同枪同弹药换账号自动接受并撤销旧代际");
    expect(accounts.ingest(full.dump(), config, start + 20ms, utc + 20) == Status::DUPLICATE &&
           accounts.snapshot(start + 20ms).player_id == "76561198000000002",
           "换账号不清空同秒去重，旧账号包不能回滚");
    for (const auto* id : {"", "invalid", "123456789012345678901234567890123"}) {
        auto invalid_identity = full;
        invalid_identity["provider"]["steamid"] = id;
        invalid_identity["player"]["steamid"] = id;
        expect(parse_payload(invalid_identity.dump(), config, utc).status == Status::IDENTITY_MISMATCH,
               "自动身份拒绝空值、非数字和超长值");
    }
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
    expect(denied.ingest(bad.dump(), config, start + 100ms, utc + 100) == Status::DUPLICATE &&
           denied.snapshot(start + 100ms).revision == 1 && denied.snapshot(start + 100ms).valid_until == first.valid_until,
           "旧auth字段变化不能绕过去重或续命");
    // 只测试纯解码/时间状态，不start接收器、不监听、不访问游戏或设备。
    return failures == 0 ? 0 : 1;
}
