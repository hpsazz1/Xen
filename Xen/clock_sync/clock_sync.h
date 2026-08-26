#ifndef CLOCK_SYNC_H
#define CLOCK_SYNC_H

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace clock_sync {

enum class MappingStatus {
    UNSYNCHRONIZED,
    WARMING,
    VALID,
    STALE,
    INVALID,
};

const char* MappingStatusName(MappingStatus status) noexcept;

struct MappingResult {
    MappingStatus status = MappingStatus::UNSYNCHRONIZED;
    bool valid = false;
    std::chrono::steady_clock::time_point local_time{};
    double uncertainty_ms = 0.0;
    double round_trip_ms = 0.0;
    double clock_rate = 1.0;
    double mapping_age_ms = 0.0;
    std::uint64_t sample_count = 0;
    std::uint64_t source_session_id = 0;
};

struct ClientConfig {
    // 源机时钟响应端点。空值表示显式禁用 source-time 映射。
    std::string source_url;
    int exchange_interval_ms = 250;
    int response_timeout_ms = 200;
    int maximum_mapping_age_ms = 1000;
};

// 请求线程低频执行四时间戳交换；视频热路径只调用 map_utc_100ns()
// 读取固定 8 点映射窗，不执行网络 I/O，也不把失同步伪装成零时延。
class Client final {
public:
    Client() noexcept;
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool open(const ClientConfig& config) noexcept;
    MappingResult map_utc_100ns(
        std::int64_t source_timestamp_100ns,
        std::chrono::steady_clock::time_point local_now) const noexcept;
    void close() noexcept;
    std::string last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct ServerConfig {
    std::string bind_url = "udp://0.0.0.0:5011";
};

// 源机旁路只返回本机 SDK 同域 UTC 时间，不接触 NDI 图像、输入设备或 KMBOX。
class Server final {
public:
    Server() noexcept;
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool open(const ServerConfig& config) noexcept;
    bool serve_once(int timeout_ms) noexcept;
    void close() noexcept;
    std::string last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace clock_sync

#endif // CLOCK_SYNC_H
