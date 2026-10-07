#ifndef XEN_LINEUP_SERVICE_H
#define XEN_LINEUP_SERVICE_H
#include "lineup/lineup.h"
#include "lineup/control_ipc.h"
#include "weapon/weapon.h"
#include <nlohmann/json.hpp>
#include <vector>
namespace lineup {
using Json = nlohmann::json;
struct CommandResult {
    int status;
    Json json;
};
class Service {
  public:
    explicit Service(const std::filesystem::path &root, ReferenceMode mode = ReferenceMode::FULL_FRAME,
                     std::string source_identity = "");
    ~Service();
    Json state();
    bool wants_input() const;
    // 仅消费本次明确定位事件，不产生游戏输入；无锁定、停止或忙时忽略。
    bool request_locate(std::uint64_t trigger_sequence = 0);
    // 仅由独立入口配置可信收件目录；网页不能选择路径，轮询不创建设备或定位请求。
    bool enable_host_capture(const std::filesystem::path &inbox, cv::Size source_size);
    void poll_host_captures();
    control::Request control_request(bool *preparing = nullptr);
    void reset_control(const std::string &reason);
    void set_execution_status(const Json &status);
    void update_gsi(const weapon::WeaponSnapshot &snapshot, const std::string &receiver_error = "");
    CommandResult command(const Json &command);
    void submit(const CapturedFrame &frame, const std::string &session);
    void disconnect(const std::string &reason);
    std::optional<std::vector<unsigned char>> preview(const std::string &frame_id);
    std::optional<std::vector<unsigned char>> reference(const std::string &id);
    void close();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace lineup
#endif
