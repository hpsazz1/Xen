#ifndef XEN_LINEUP_CONTROL_IPC_H
#define XEN_LINEUP_CONTROL_IPC_H
// Lineup 与 Runtime 的独立本地通道；不修改 GSI 只读协议，不提供投掷命令。
#include "lineup/execution_internal.h"
#include <memory>
#include <nlohmann/json.hpp>
namespace weapon { struct GsiConfig; }
namespace lineup::control {
enum class Mode { LOCATE, CANCEL, OBSERVATION };
struct Request {
    Mode mode = Mode::CANCEL;
    detail::ExecutionObservation observation;
    std::uint64_t reference_version = 0;
    nlohmann::json throw_action = nullptr;
};
struct Snapshot {
    bool connected = false, available = false;
    std::uint64_t connection_epoch = 0, message_sequence = 0, locate_sequence = 0;
    Request request;
    Clock::time_point received_at{}, valid_until{};
    std::string reason = "disconnected";
};
// 仅由现有 GSI 配置派生隔离键，不新增持久配置。
std::string channel_key(const weapon::GsiConfig &config);
class Server final {
  public:
    Server();
    ~Server();
    bool start(const std::string &key) noexcept;
    void stop() noexcept;
    Snapshot snapshot() noexcept;
    void publish_status(const nlohmann::json &status) noexcept;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
class Client final {
  public:
    Client();
    ~Client();
    bool start(const std::string &key) noexcept;
    void stop() noexcept;
    bool connected() const noexcept;
    // 只保存一个最新待发快照；未连接时拒绝，不在重连后重放。
    bool publish(const Request &request) noexcept;
    nlohmann::json status() const noexcept;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
#endif
