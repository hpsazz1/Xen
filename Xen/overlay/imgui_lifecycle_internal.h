#ifndef OVERLAY_IMGUI_LIFECYCLE_INTERNAL_H
#define OVERLAY_IMGUI_LIFECYCLE_INTERNAL_H

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

namespace overlay::detail {

inline void shutdown_imgui_context(ImGuiContext*& context) noexcept {
    if (!context) return;
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(context);
    // 后端可能在整体init失败前已分配资源；只关闭这个context实际拥有的后端。
    const auto& io = ImGui::GetIO();
    if (io.BackendRendererUserData) ImGui_ImplDX11_Shutdown();
    if (io.BackendPlatformUserData) ImGui_ImplWin32_Shutdown();
    const bool restore_previous = previous != context;
    ImGui::DestroyContext(context);
    context = nullptr;
    if (restore_previous) ImGui::SetCurrentContext(previous);
}

} // namespace overlay::detail

#endif // OVERLAY_IMGUI_LIFECYCLE_INTERNAL_H
