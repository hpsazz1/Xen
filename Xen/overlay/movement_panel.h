#ifndef OVERLAY_MOVEMENT_PANEL_H
#define OVERLAY_MOVEMENT_PANEL_H

#include <memory>
#include "debug_session/debug_session.h"

struct OverlayActions;
// 页面只编辑草稿；文件读写、准备及设备执行交由统一调试会话。
class MovementPanel {
public:
    MovementPanel();
    ~MovementPanel();
    MovementPanel(const MovementPanel&) = delete;
    MovementPanel& operator=(const MovementPanel&) = delete;
    void render(const AppConfig&, const debug_session::Snapshot*, OverlayActions&) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif
