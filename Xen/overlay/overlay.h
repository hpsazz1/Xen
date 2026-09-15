#ifndef OVERLAY_H
#define OVERLAY_H

#include <memory>
#include <string>
#include <vector>

#include "config/config.h"
#include "runtime/runtime.h"
#include "model_workspace/model_workspace.h"
#include "debug_session/debug_session.h"

struct OverlayActions {
    debug_session::Action debug_action = debug_session::Action::NONE;
    debug_session::Request debug_request;
    bool debug_plan_edited = false;
    std::string debug_prepared_id;
    bool debug_allow_physical_output = false;
    std::string debug_confirmation;
    model_workspace::Action workspace_action = model_workspace::Action::NONE;
    bool start_requested = false;
    bool stop_requested = false;
    bool reload_detector_requested = false;
    bool refresh_models_requested = false;
    bool save_config_requested = false;
    bool log_level_changed = false;
    bool preview_enabled_changed = false;
    bool preview_enabled = false;
    bool hotkey_capture_consumed = false;
    bool training_start_requested = false;
    bool training_stop_requested = false;
    bool training_load_requested = false;
    std::string training_directory;
    std::string training_load_path;
    std::vector<RuntimeIntent> runtime_intents;
};

// Overlay 只消费 App 在启动或显式刷新时生成的不可变清单，不在逐帧渲染中访问文件系统。
struct OverlayModelCatalog {
    std::string directory;
    std::vector<std::string> model_names;
};

// 发布环境只展示清单授权且属于当前 Worker 的后端；开发环境默认包含全部后端。
struct OverlayBackendCatalog {
    std::vector<BackendType> backends;
};

class Overlay {
public:
    Overlay();
    ~Overlay();

    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    bool init(const UiConfig& config) noexcept;
    bool pump_messages(bool defer_close = false) noexcept;
    bool render(const RuntimeSnapshot& snapshot,
                const std::shared_ptr<const RuntimePreviewFrame>& preview,
                const OverlayModelCatalog& model_catalog,
                const OverlayBackendCatalog& backend_catalog,
                AppConfig& config,
                model_workspace::Settings& workspace_settings,
                const model_workspace::Snapshot& workspace_snapshot,
                const std::string& app_message,
                OverlayActions& actions,
                const KeyboardPollResult* keyboard_poll = nullptr,
                const debug_session::Snapshot* debug_snapshot = nullptr) noexcept;
    bool close_requested() const noexcept;
    bool background_busy() const noexcept;
    void poll_background() noexcept;
    void cancel_background() noexcept;
    const std::string& last_error() const noexcept;
    void shutdown() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif // OVERLAY_H
