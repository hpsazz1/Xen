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
enum class Status { DISABLED, UNAVAILABLE, READY, EXPIRED, AUTH_REJECTED, INVALID_PAYLOAD,
                    IDENTITY_MISMATCH, PLAYER_INACTIVE, UNKNOWN_WEAPON, RELOADING, EMPTY,
                    CLOCK_REJECTED, OUT_OF_ORDER, DUPLICATE, COUNTER_EXHAUSTED };
const char* status_name(Status status) noexcept;

struct GsiConfig {
    bool enabled = false;
    std::string bind_address = "127.0.0.1";
    std::uint16_t port = 5013;
    // 仅由调用方从环境注入；不在模块日志、状态或错误中回显。
    std::string token;
    std::string expected_player_id;
    std::string allowed_peer_ipv4;
    int ttl_ms = 2500;
    int request_timeout_ms = 1000;
    std::size_t max_body_bytes = 65536;
    int max_clock_skew_ms = 5000;
};

struct WeaponSnapshot {
    bool valid = false;
    bool identity_match = false;
    Status status = Status::UNAVAILABLE;
    std::string raw_name;
    std::string canonical_id;
    WeaponState state = WeaponState::UNKNOWN;
    std::optional<int> ammo_clip, ammo_clip_max, ammo_reserve;
    std::optional<std::uint64_t> provider_timestamp_seconds;
    // 这是本地接收连续性代际/发布序号，不冒充游戏进程session或逐发时间。
    std::uint64_t source_epoch = 0;
    std::uint64_t revision = 0;
    Clock::time_point received_at{}, valid_until{};
    // 原生GSI没有逐包序号；同秒完整变化可采纳，但无法证明源端严格顺序。
    // TTL从同一provider timestamp第一次本地接收计时；重复不续命。
    // peer/token/粗粒度墙钟容差仅缩小陈旧数据窗口，不证明网络时延或当前逐发相位。
    // 未见过的同秒旧状态无法与新状态区分；重启后也无法证明首包属于新的游戏session。
    // 更大timestamp仍必须通过当前接收墙钟容差；该容差依赖两机时钟配置正确。
    // 本模块不能单独作为开火、前台或停稳许可；双机GSI URI需显式指向接收主机。
    bool source_order_verified = false;
};

bool valid_config(const GsiConfig& config) noexcept;
std::string canonical_weapon_id(const std::string& raw_name);

class GsiReceiver final {
public:
    GsiReceiver();
    ~GsiReceiver();
    GsiReceiver(const GsiReceiver&) = delete;
    GsiReceiver& operator=(const GsiReceiver&) = delete;
    // 生命周期调用由单owner串行推进；snapshot跨线程可读。
    bool start(const GsiConfig& config) noexcept;
    void stop() noexcept;
    WeaponSnapshot snapshot() const;
    std::string last_error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace weapon
#endif
