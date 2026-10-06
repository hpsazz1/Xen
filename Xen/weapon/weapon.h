#ifndef WEAPON_H
#define WEAPON_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace weapon {
using Clock = std::chrono::steady_clock;
enum class WeaponState { UNKNOWN, ACTIVE, RELOADING, HOLSTERED };
enum class Team { UNKNOWN, CT, T };
const char* team_name(Team team) noexcept;
enum class GamePhase { UNKNOWN, PREPARATION, ACTIVE };
const char* game_phase_name(GamePhase phase) noexcept;
enum class Status { DISABLED, UNAVAILABLE, READY, EXPIRED, INVALID_PAYLOAD,
                    IDENTITY_MISMATCH, PLAYER_INACTIVE, UNKNOWN_WEAPON, RELOADING, EMPTY,
                    CLOCK_REJECTED, OUT_OF_ORDER, DUPLICATE, COUNTER_EXHAUSTED, NON_FIREARM };
const char* status_name(Status status) noexcept;

struct GsiConfig {
    bool enabled = false;
    std::string bind_address = "127.0.0.1";
    std::uint16_t port = 5013;
    std::string allowed_peer_ipv4;
    int ttl_ms = 2500;
    int request_timeout_ms = 1000;
    std::size_t max_body_bytes = 65536;
    int max_clock_skew_ms = 5000;
};

struct WeaponSnapshot {
    bool valid = false;
    bool identity_match = false;
    // 地图/本地阵营仅供目录筛选，独立于枪械、弹药和控制策略有效性。
    // 观战他人、身份/活动缺失、过期或同秒上下文回退均撤销。
    bool context_valid = false;
    std::uint64_t context_epoch = 0; // 筛选上下文断点，不是武器或控制许可。
    std::string map_name;
    Team local_team = Team::UNKNOWN;
    // 仅为普通压枪区分确认死亡与缺失/非游戏状态，不放宽 valid。
    bool player_playing = false;
    std::optional<int> player_health;
    Team team = Team::UNKNOWN;
    GamePhase game_phase = GamePhase::UNKNOWN;
    // 独立阵营/阶段策略代际；未知也保留断点，非零本身不代表可信。
    std::uint64_t team_epoch = 0;
    Status status = Status::UNAVAILABLE;
    std::string player_id; // 自动从客户端身份取得，不是固定配置。
    std::string raw_name;
    std::string canonical_id;
    WeaponState state = WeaponState::UNKNOWN;
    std::optional<int> ammo_clip, ammo_clip_max, ammo_reserve;
    std::optional<std::uint64_t> provider_timestamp_seconds;
    // 这是本地接收连续性代际/发布序号，不冒充游戏进程session或逐发时间。
    std::uint64_t source_epoch = 0;
    // 不可信间断的持久代际；普通死亡、换弹、空弹不改变它。
    std::uint64_t recoil_safety_epoch = 0;
    // Aim/Trigger/AutoStop仅允许健康玩家的普通武器过渡，死亡也撤销持键会话。
    std::uint64_t control_safety_epoch = 0;
    std::uint64_t revision = 0;
    Clock::time_point received_at{}, valid_until{};
    // 原生GSI没有逐包序号；同秒完整变化可采纳，但无法证明源端严格顺序。
    // TTL从同一provider timestamp第一次本地接收计时；重复不续命。
    // peer/粗粒度墙钟容差仅缩小陈旧数据窗口，不证明网络时延或当前逐发相位。
    // 未见过的同秒旧状态无法与新状态区分；重启后也无法证明首包属于新的游戏session。
    // 更大timestamp仍必须通过当前接收墙钟容差；该容差依赖两机时钟配置正确。
    // 本模块不能单独作为开火、前台或停稳许可；双机GSI URI需显式指向接收主机。
    bool source_order_verified = false;
};

bool valid_config(const GsiConfig& config) noexcept;
std::string canonical_weapon_id(const std::string& raw_name);
// 只规范普通名称或 workshop/<数字 ID>/<名称>，不合并不同创意工坊项目。
std::string canonical_map_id(const std::string& raw_name);

class GsiReceiver final {
public:
    GsiReceiver();
    ~GsiReceiver();
    GsiReceiver(const GsiReceiver&) = delete;
    GsiReceiver& operator=(const GsiReceiver&) = delete;
    // 生命周期调用由单owner串行推进；snapshot跨线程可读。
    bool start(const GsiConfig& config, bool publish_context = false) noexcept;
    void stop() noexcept;
    WeaponSnapshot snapshot() const;
    std::string last_error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// 只读本地投影，单线程调用；返回值永远不授予武器/控制许可。
class GsiContextReader final {
public:
    GsiContextReader();
    ~GsiContextReader();
    void configure(const GsiConfig& config);
    WeaponSnapshot snapshot();
    std::string last_error() const;
    // snapshot 后读取；首次连接仅建立基线，过期/重连事件不重放。
    bool consume_lineup_locate() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace weapon
#endif
