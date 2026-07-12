#ifndef OVERLAY_UI_SECTIONS_H
#define OVERLAY_UI_SECTIONS_H

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "imgui/imgui.h"

/**
 * @brief 覆盖层 UI 工具命名空间
 *
 * 提供设置界面常用的 UI 组件，包括：
 * - 自适应控件宽度计算
 * - 动画过渡效果
 * - 颜色插值
 * - 带标签的设置行控件
 * - 区域标题和分组
 */
namespace OverlayUI
{

/**
 * @brief 设置行布局信息
 *
 * 包含设置行的位置、尺寸和控件宽度信息。
 */
struct SettingRow
{
    ImVec2 min;           ///< 设置行左上角
    ImVec2 max;           ///< 设置行右下角
    float controlWidth;   ///< 控件可用宽度
};

/**
 * @brief 计算自适应控件宽度
 * @param ratio 控件占可用宽度的比例（默认 0.64）
 * @return 计算后的控件宽度
 */
inline float AdaptiveItemWidth(float ratio = 0.64f) noexcept
{
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail <= 1.0f)
        return 1.0f;

    const float maxW = avail * 0.92f;
    const float minW = avail * ((avail < 280.0f) ? 0.62f : 0.42f);
    const float boundedMin = (minW < maxW) ? minW : maxW;
    const float target = avail * ratio;
    return std::clamp(target, boundedMin, maxW);
}

/**
 * @brief 动画化的浮点值过渡
 * @param id 动画 ID（用于存储状态）
 * @param target 目标值
 * @param speed 动画速度（默认 16.0）
 * @param initial 初始值（默认 0.0）
 * @return 当前动画值
 */
inline float AnimateFloat(const char* id, float target, float speed = 16.0f, float initial = 0.0f) noexcept
{
    ImGuiStorage* storage = ImGui::GetStateStorage();
    float* value = storage->GetFloatRef(ImGui::GetID(id), initial);
    const float dt = std::min(ImGui::GetIO().DeltaTime, 1.0f / 15.0f);
    const float step = 1.0f - std::exp(-speed * dt);
    *value += (target - *value) * step;
    if (std::abs(*value - target) < 0.001f)
        *value = target;
    return *value;
}

/**
 * @brief 在两个 ImU32 颜色之间插值
 * @param a 起始颜色
 * @param b 结束颜色
 * @param t 插值因子 [0, 1]
 * @return 插值后的颜色
 */
inline ImU32 LerpColor(ImU32 a, ImU32 b, float t) noexcept
{
    t = std::clamp(t, 0.0f, 1.0f);
    const ImVec4 av = ImGui::ColorConvertU32ToFloat4(a);
    const ImVec4 bv = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(
        av.x + (bv.x - av.x) * t,
        av.y + (bv.y - av.y) * t,
        av.z + (bv.z - av.z) * t,
        av.w + (bv.w - av.w) * t));
}

/**
 * @brief 开始一个带标签的设置行
 * @param label 行标签文本
 * @param height 行高（默认 58.0）
 * @param controlRatio 控件宽度比例（默认 0.44）
 * @return 设置行布局信息
 */
inline SettingRow BeginSettingRow(const char* label, float height = 52.0f, float controlRatio = 0.44f) noexcept
{
    ImGui::PushID(label);

    const ImGuiStyle& style = ImGui::GetStyle();
    const float avail = ImGui::GetContentRegionAvail().x;
    const ImVec2 rowMin = ImGui::GetCursorScreenPos();
    const ImVec2 rowMax(rowMin.x + avail, rowMin.y + height);
    const bool hoverable = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    const bool hovered = hoverable && ImGui::IsMouseHoveringRect(rowMin, rowMax);
    const float hoverAnim = AnimateFloat("##row_hover_anim", hovered ? 1.0f : 0.0f, 16.0f, 0.0f);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    // 行背景：深色面板色 + 悬停时微亮
    draw->AddRectFilled(rowMin, rowMax,
        LerpColor(IM_COL32(16, 20, 30, 220), IM_COL32(22, 28, 40, 238), hoverAnim), 4.0f);
    // 悬停时右侧边缘青色辉光
    if (hoverAnim > 0.01f)
    {
        const float glowW = 3.0f;
        const ImU32 glowCol = ImGui::ColorConvertFloat4ToU32(
            ImVec4(0.0f, 0.90f, 1.0f, 0.12f * hoverAnim));
        draw->AddRectFilled(ImVec2(rowMax.x - glowW, rowMin.y), rowMax, glowCol);
    }
    draw->AddRect(rowMin, rowMax,
        IM_COL32(30, 38, 54, static_cast<int>(40.0f + 30.0f * hoverAnim)), 4.0f, 0, 1.0f);

    const ImVec2 labelSize = ImGui::CalcTextSize(label);
    const float labelY = rowMin.y + (height - labelSize.y) * 0.5f;
    // 标签文字使用次色调
    draw->AddText(ImVec2(rowMin.x + 15.0f, labelY), IM_COL32(176, 184, 200, 240), label);

    const float maxControlW = std::min(320.0f, std::max(180.0f, avail - 205.0f));
    const float minControlW = std::min(210.0f, maxControlW);
    const float controlW = std::clamp(avail * controlRatio, minControlW, maxControlW);
    const float controlY = rowMin.y + (height - ImGui::GetFrameHeight()) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(rowMax.x - controlW - 15.0f, controlY));
    ImGui::SetNextItemWidth(controlW);

    return { rowMin, rowMax, controlW };
}

/** @brief 结束一个设置行 */
inline void EndSettingRow(const SettingRow& row) noexcept
{
    ImGui::SetCursorScreenPos(row.min);
    ImGui::Dummy(ImVec2(row.max.x - row.min.x, row.max.y - row.min.y));
    ImGui::PopID();
}

/** @brief 复选框设置行 */
inline bool CheckboxRow(const char* label, bool* value, const char* id = "##value", const char* tooltip = nullptr) noexcept
{
    const SettingRow row = BeginSettingRow(label);
    const bool changed = ImGui::Checkbox(id, value);
    if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tooltip);
    EndSettingRow(row);
    return changed;
}

/** @brief 整数滑块设置行 */
inline bool SliderIntRow(const char* label, int* value, int minValue, int maxValue, const char* format = "%d", const char* id = "##value", const char* tooltip = nullptr) noexcept
{
    const SettingRow row = BeginSettingRow(label);
    const bool changed = ImGui::SliderInt(id, value, minValue, maxValue, format);
    if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tooltip);
    EndSettingRow(row);
    return changed;
}

/** @brief 浮点滑块设置行 */
inline bool SliderFloatRow(const char* label, float* value, float minValue, float maxValue, const char* format = "%.3f", const char* id = "##value", const char* tooltip = nullptr) noexcept
{
    const SettingRow row = BeginSettingRow(label);
    const bool changed = ImGui::SliderFloat(id, value, minValue, maxValue, format);
    if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tooltip);
    EndSettingRow(row);
    return changed;
}

/** @brief 文本输入设置行 */
inline bool InputTextRow(const char* label, char* buffer, size_t bufferSize, ImGuiInputTextFlags flags = 0, const char* id = "##value") noexcept
{
    const SettingRow row = BeginSettingRow(label);
    const bool changed = ImGui::InputText(id, buffer, bufferSize, flags);
    EndSettingRow(row);
    return changed;
}

/** @brief 整数输入设置行 */
inline bool InputIntRow(const char* label, int* value, int step = 1, int stepFast = 100, ImGuiInputTextFlags flags = 0, const char* id = "##value") noexcept
{
    const SettingRow row = BeginSettingRow(label);
    const bool changed = ImGui::InputInt(id, value, step, stepFast, flags);
    EndSettingRow(row);
    return changed;
}

/** @brief 按钮设置行 */
inline bool ButtonRow(const char* label, const char* buttonText, const char* id = nullptr) noexcept
{
    ImGui::PushID(id ? id : label);
    const SettingRow row = BeginSettingRow(label);
    const bool clicked = ImGui::Button(buttonText, ImVec2(row.controlWidth, 0.0f));
    EndSettingRow(row);
    ImGui::PopID();
    return clicked;
}

/** @brief 下拉选择框设置行 */
inline bool ComboRow(const char* label, int* currentItem, const char* const items[], int itemsCount, const char* id = "##value") noexcept
{
    const SettingRow row = BeginSettingRow(label);
    const bool changed = ImGui::Combo(id, currentItem, items, itemsCount);
    EndSettingRow(row);
    return changed;
}

/** @brief 纯文本提示行 */
inline void TextRow(const char* text, ImU32 color = IM_COL32(255, 180, 60, 255), float height = 30.0f) noexcept
{
    const float avail = ImGui::GetContentRegionAvail().x;
    const ImVec2 rowMin = ImGui::GetCursorScreenPos();
    const ImVec2 rowMax(rowMin.x + avail, rowMin.y + height);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(rowMin, rowMax, IM_COL32(16, 20, 30, 145), 4.0f);
    draw->AddRect(rowMin, rowMax, IM_COL32(30, 38, 54, 80), 4.0f, 0, 1.0f);
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    draw->AddText(ImVec2(rowMin.x + 14.0f, rowMin.y + (height - textSize.y) * 0.5f), color, text);
    ImGui::Dummy(ImVec2(avail, height + 6.0f));
}

/** @brief 绘制主体区域边框 */
inline void DrawBodyFrame(const ImVec2& min, const ImVec2& max, bool subsection = false) noexcept
{
    IM_UNUSED(min);
    IM_UNUSED(max);
    IM_UNUSED(subsection);
}

/**
 * @brief 绘制区域标题
 * @param label 标题文本
 * @param subsection 是否为子区域标题
 */
inline void DrawSectionHeader(const char* label, bool subsection = false) noexcept
{
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float height = subsection ? 26.0f : 30.0f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 max(pos.x + avail, pos.y + height);

    ImGui::InvisibleButton(subsection ? "##subsection_header" : "##section_header", ImVec2(avail, height));
    const bool hovered = ImGui::IsItemHovered();
    const ImU32 bg = hovered
        ? IM_COL32(22, 28, 42, 240)
        : IM_COL32(16, 22, 32, 225);
    const ImU32 border = IM_COL32(30, 38, 54, subsection ? 100 : 140);
    const float rounding = subsection ? 4.0f : 4.0f;
    drawList->AddRectFilled(pos, max, bg, rounding);
    drawList->AddRect(pos, max, border, rounding, 0, 1.0f);

    // 左侧青色强调条
    const float barW = 3.0f;
    drawList->AddRectFilled(
        ImVec2(pos.x + 2.0f, pos.y + 4.0f),
        ImVec2(pos.x + 2.0f + barW, max.y - 4.0f),
        IM_COL32(0, 229, 255, subsection ? 160 : 210), 2.0f);

    const ImVec2 textSize = ImGui::CalcTextSize(label);
    const float textY = pos.y + (height - textSize.y) * 0.5f;
    drawList->AddText(ImVec2(pos.x + 16.0f, textY),
        subsection ? IM_COL32(200, 205, 215, 240) : IM_COL32(0, 229, 255, 245), label);
}

/** @brief 开始主体分组（设置样式和间距） */
inline void BeginBodyGroup(bool subsection = false) noexcept
{
    ImGui::BeginGroup();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(7.0f, subsection ? 5.0f : 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(9.0f, 5.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(22, 27, 40, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(28, 35, 53, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(36, 48, 74, 255));
    ImGui::PushItemWidth(AdaptiveItemWidth(subsection ? 0.68f : 0.64f));
    ImGui::Dummy(ImVec2(0.0f, subsection ? 4.0f : 6.0f));
}

/** @brief 结束主体分组 */
inline void EndBodyGroup(bool subsection = false) noexcept
{
    IM_UNUSED(subsection);
    ImGui::PopItemWidth();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(4);
    ImGui::EndGroup();
}

/** @brief 开始一个设置区域 */
inline bool BeginSection(const char* label, const char* id = nullptr, bool defaultOpen = true) noexcept
{
    ImGui::PushID(id ? id : label);

    IM_UNUSED(defaultOpen);
    IM_UNUSED(label);

    BeginBodyGroup(false);
    return true;
}

/** @brief 结束一个设置区域 */
inline void EndSection() noexcept
{
    EndBodyGroup(false);
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0.0f, 7.0f));
}

/** @brief 开始一个子区域 */
inline bool BeginSubsection(const char* label, bool defaultOpen = true) noexcept
{
    IM_UNUSED(defaultOpen);
    ImGui::PushID(label);
    BeginBodyGroup(true);
    return true;
}

/** @brief 结束一个子区域 */
inline void EndSubsection() noexcept
{
    EndBodyGroup(true);
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0.0f, 5.0f));
}

} // namespace OverlayUI

#endif // OVERLAY_UI_SECTIONS_H
