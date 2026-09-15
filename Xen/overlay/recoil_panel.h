#ifndef OVERLAY_RECOIL_PANEL_H
#define OVERLAY_RECOIL_PANEL_H
#include <memory>
#include "config/config.h"
#include "runtime/runtime.h"

class RecoilPanel {
public:
    RecoilPanel();
    ~RecoilPanel();
    RecoilPanel(const RecoilPanel&) = delete;
    RecoilPanel& operator=(const RecoilPanel&) = delete;
    void render(const RuntimeSnapshot& snapshot, AppConfig& config, bool can_edit) noexcept;
    void render_tools(const RuntimeSnapshot& snapshot, AppConfig& config, bool can_edit) noexcept;
    void render_connections(AppConfig& config, bool can_edit) noexcept;
    void render_diagnostics(const RuntimeSnapshot& snapshot) noexcept;
    // 每帧由宿主调用；关闭时请求取消并保持消息循环，直到 busy 返回 false。
    void poll() noexcept;
    bool busy() const noexcept;
    void request_cancel() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
#endif
