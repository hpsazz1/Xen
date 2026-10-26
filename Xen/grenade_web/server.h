#ifndef GRENADE_WEB_SERVER_H
#define GRENADE_WEB_SERVER_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace weapon { class GsiReceiver; }

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
};
// App 持有生命周期；只读共享 GSI 与正式方案库，不接触 Runtime、Session 或设备。
class Server final {
public:
    Server();
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    bool start(const Config& config, std::shared_ptr<weapon::GsiReceiver> gsi) noexcept;
    void stop() noexcept;
    Snapshot snapshot() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
#endif
