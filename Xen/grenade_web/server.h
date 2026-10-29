#ifndef GRENADE_WEB_SERVER_H
#define GRENADE_WEB_SERVER_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace weapon { class GsiReceiver; }
namespace debug_session { struct Snapshot; }
struct AppConfig;

namespace grenade_web {
struct Config {
    std::string bind_address = "0.0.0.0";
    std::uint16_t port = 5015;
    std::string plans_root = "cache/grenade/plans";
};
struct Snapshot {
    bool running = false;
    std::uint16_t port = 0;
    std::string error;
    std::vector<std::string> urls;
    std::uint64_t video_sync_discarded_records = 0;
};
struct SelectionRequest {
    std::uint64_t id = 0, map_epoch = 0;
    std::string plan_id, plan_path, map, team, generation;
};
// App 持有生命周期；网页只提交选择意图，由 App 载入方案，不调用设备接口。
class Server final {
public:
    Server();
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    bool start(const Config& config, std::shared_ptr<weapon::GsiReceiver> gsi) noexcept;
    // App 主线程发布心跳与不可变选择；只检查 controls 的按键，不保留配置或秘密字段。
    void publish_capture(std::shared_ptr<const debug_session::Snapshot> selection,
                         const AppConfig& controls, bool available, const std::string& reason = {}) noexcept;
    // 只发布诊断录制可用性；视频打点不产生Runtime/设备控制意图。
    void publish_video_sync(const AppConfig& controls, bool available,
                            const std::string& reason = {}) noexcept;
    // 每个paint/receipt只交付一次；JSON保留原始时钟及映射误差，由App后台归档。
    std::optional<std::string> poll_video_sync() noexcept;
    std::optional<SelectionRequest> poll_selection() noexcept;
    bool selection_current(const SelectionRequest& request) const noexcept;
    // 成功快照与选择回执一并发布；空快照表示载入失败，不能先回执成功再等待下一帧补图。
    void finish_selection(const SelectionRequest& request,
                          std::shared_ptr<const debug_session::Snapshot> selection,
                          const std::string& reason = {}) noexcept;
    void stop() noexcept;
    Snapshot snapshot() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
#endif
