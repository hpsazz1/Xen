#ifndef OVERLAY_RECOIL_TARGET_PANEL_H
#define OVERLAY_RECOIL_TARGET_PANEL_H
#include <memory>
struct AppConfig;
struct OverlayActions;
struct ID3D11Device;
namespace debug_session { struct Snapshot; }

class RecoilTargetPanel {
public:
    RecoilTargetPanel();
    ~RecoilTargetPanel();
    RecoilTargetPanel(const RecoilTargetPanel&) = delete;
    RecoilTargetPanel& operator=(const RecoilTargetPanel&) = delete;
    void render(AppConfig& config, OverlayActions& actions,
        const debug_session::Snapshot* snapshot, ID3D11Device* device, bool can_edit) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
#endif
