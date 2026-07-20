#pragma once

#include <memory>
#include <string>

class LanControlServer
{
public:
    LanControlServer();
    ~LanControlServer();

    LanControlServer(const LanControlServer&) = delete;
    LanControlServer& operator=(const LanControlServer&) = delete;

    bool Start(const std::string& bindAddress, int port);
    void Stop() noexcept;
    bool IsRunning() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
