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
    bool reload_detector_requested = false;
    bool refresh_models_requested = false;
    bool save_config_requested = false;
    bool preview_enabled_changed = false;
    bool preview_enabled = false;
    std::vector<RuntimeIntent> runtime_intents;
};

// Overlay 只消费 App 在启动或显式刷新时生成的不可变清单，不在逐帧渲染中访问文件系统。
struct OverlayModelCatalog {
    std::string directory;
    std::vector<std::string> model_names;
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
                const std::shared_ptr<const RuntimePreviewFrame>& preview,
                const OverlayModelCatalog& model_catalog,
                AppConfig& config,
                const std::string& app_message,
                OverlayActions& actions) noexcept;
    void shutdown() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif // OVERLAY_H
