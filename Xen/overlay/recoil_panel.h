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
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
#endif
