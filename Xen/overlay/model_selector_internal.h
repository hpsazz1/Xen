#ifndef XEN_MODEL_SELECTOR_INTERNAL_H
#define XEN_MODEL_SELECTOR_INTERNAL_H

#include <imgui.h>
#include <algorithm>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace overlay::detail {
// 与生产模型下拉共用，允许无平台后端验证文件名和选择行为。
inline bool model_selectable(std::string_view directory, const std::string& name, bool selected) {
    // 完整字面目录/文件名参与身份，排序不参与；先散列避免###改变ImGui的ID语义。
    ImGui::PushID(static_cast<int>(std::hash<std::string_view>{}(directory)));
    ImGui::PushID(static_cast<int>(std::hash<std::string_view>{}(name)));
    const auto position = ImGui::GetCursorScreenPos();
    const auto text_size = ImGui::CalcTextSize(name.c_str(), nullptr, false);
    const bool pressed = ImGui::Selectable("##model_entry", selected, 0,
        {std::max(text_size.x, ImGui::GetContentRegionAvail().x), text_size.y});
    if (ImGui::IsItemVisible()) {
        ImGui::GetWindowDrawList()->AddText(position, ImGui::GetColorU32(ImGuiCol_Text),
            name.c_str(), name.c_str() + name.size());
    }
    ImGui::PopID(); ImGui::PopID();
    return pressed;
}

inline void render_model_combo(std::span<const std::string> names, std::string_view directory,
                               std::string& selection) {
    const bool available = std::find(names.begin(), names.end(), selection) != names.end();
    std::string preview = selection.empty() ? "未选择" : selection;
    if (!selection.empty() && !available) preview += "（不可用）";
    ImGui::BeginDisabled(names.empty());
    ImGui::PushID(static_cast<int>(std::hash<std::string_view>{}(directory)));
    auto* preview_draw = ImGui::GetWindowDrawList();
    const auto position = ImGui::GetCursorScreenPos();
    const float width = ImGui::CalcItemWidth();
    const float height = ImGui::GetFrameHeight();
    const auto padding = ImGui::GetStyle().FramePadding;
    // 空标签保留标准Combo交互；文字单独按字面绘制，不能让##截断当前选择预览。
    const bool open = ImGui::BeginCombo("##model_name", "");
    const ImVec2 text_position{position.x + padding.x, position.y + padding.y};
    preview_draw->PushClipRect(text_position,
        {position.x + width - height - padding.x, position.y + height}, true);
    preview_draw->AddText(text_position, ImGui::GetColorU32(ImGuiCol_Text),
        preview.c_str(), preview.c_str() + preview.size());
    preview_draw->PopClipRect();
    if (open) {
        for (const auto& name : names) {
            const bool selected = name == selection;
            if (model_selectable(directory, name, selected)) selection = name;
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
    ImGui::EndDisabled();
}
}
#endif
