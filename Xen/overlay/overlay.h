#ifndef OVERLAY_H
#define OVERLAY_H

#include <memory>
#include <string>
#include <vector>

#include "config/config.h"
#include "runtime/runtime.h"

struct OverlayActions {
    bool start_requested = false;
    bool stop_requested = false;
    bool save_config_requested = false;
    std::vector<RuntimeIntent> runtime_intents;
};

class Overlay {
public:
    Overlay();
    ~Overlay();

    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    bool init(const UiConfig& config) noexcept;
    bool pump_messages() noexcept;
    bool render(const RuntimeSnapshot& snapshot,
                AppConfig& config,
                const std::string& app_message,
                OverlayActions& actions) noexcept;
    void shutdown() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif // OVERLAY_H
