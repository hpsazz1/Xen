#ifndef XEN_LOG_PANEL_INTERNAL_H
#define XEN_LOG_PANEL_INTERNAL_H

#include <imgui.h>
#include <span>
#include <string>

namespace overlay::detail {
// 生产Overlay与无平台后端的内存交互回归共享同一日志内容渲染。
inline void render_log_entries(std::span<const std::string> lines, const ImVec4& empty_color,
                               bool scroll_to_latest = false) {
    ImGui::BeginChild("log_entries", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
    if (lines.empty()) {
        ImGui::TextColored(empty_color, "暂无可显示日志");
    } else {
        for (const auto& line : lines) ImGui::TextWrapped("%s", line.c_str());
        // 保持底部跟随；滚轮或滚动条向上阅读时不抢回位置，布局变化也沿用同一规则。
        if (scroll_to_latest || ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}
}

#endif
