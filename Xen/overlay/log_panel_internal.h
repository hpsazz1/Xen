#ifndef XEN_LOG_PANEL_INTERNAL_H
#define XEN_LOG_PANEL_INTERNAL_H

#include <imgui.h>
#include <array>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace overlay::detail {
using LogFilter = std::array<char, 256>;

inline bool render_log_filter(LogFilter& filter) {
    ImGui::SetNextItemWidth(-1.0f);
    const bool changed = ImGui::InputTextWithHint("##log_filter", "筛选日志（区分大小写）",
        filter.data(), filter.size());
    ImGui::SetItemTooltip("按输入文本匹配完整日志，不使用正则表达式；清空恢复全部。\n点击条目旁的复制，只复制该条完整日志。");
    return changed;
}

// 生产Overlay与无平台后端的内存交互回归共享同一日志内容渲染。
inline void render_log_entries(std::span<const std::string> lines, const ImVec4& empty_color,
                               bool scroll_to_latest = false, std::string_view filter = {}) {
    // 筛选框与条目共用焦点范围，Tab可直接到达复制按钮。
    ImGui::BeginChild("log_entries", ImVec2(0.0f, 0.0f), ImGuiChildFlags_NavFlattened);
    bool any_visible = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto& line = lines[i];
        if (line.find(filter) == std::string::npos) continue;
        any_visible = true;
        ImGui::PushID(static_cast<int>(i));
        // 内容参与ID：按下与松开之间环形缓冲换行时，取消点击而不复制新条目。
        // 先散列避免正文中的###被当作ImGui标签的ID分隔符。
        ImGui::PushID(static_cast<int>(std::hash<std::string>{}(line)));
        ImGui::BeginDisabled(line.empty());
        if (ImGui::SmallButton("复制")) ImGui::SetClipboardText(line.c_str());
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("仅复制此条完整日志（保留时间、等级、模块和多行正文）");
        ImGui::SameLine();
        ImGui::TextWrapped("%s", line.c_str());
        ImGui::PopID();
        ImGui::PopID();
    }
    if (!any_visible) {
        ImGui::TextColored(empty_color, lines.empty() ? "暂无可显示日志" : "没有匹配日志；清空筛选可恢复全部");
    } else {
        // 保持底部跟随；滚轮或滚动条向上阅读时不抢回位置，布局变化也沿用同一规则。
        if (scroll_to_latest || ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}
}

#endif
