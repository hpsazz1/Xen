#ifndef OVERLAY_DEBUG_PANEL_H
#define OVERLAY_DEBUG_PANEL_H
#include <memory>
#include "debug_session/debug_session.h"
struct OverlayActions;
// 仅持有实验草稿及浏览状态；文件、设备和报告工作由 App 的 Session 执行。
class DebugPanel {
public:
    DebugPanel();
    ~DebugPanel();
    void render_status(const debug_session::Snapshot*, OverlayActions&) noexcept;
    void render_counterpulse(const AppConfig&, const debug_session::Snapshot*, OverlayActions&) noexcept;
    void render_fire(const AppConfig&, const debug_session::Snapshot*, OverlayActions&) noexcept;
    void render_manual(const debug_session::Snapshot*, OverlayActions&) noexcept;
    void render_results(const debug_session::Snapshot*) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
#endif
