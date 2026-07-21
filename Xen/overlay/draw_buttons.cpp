// ============================================================================
// 按钮绑定绘制模块
// 负责绘制"热键设置"界面中的按钮绑定行（如瞄准、射击、缩放等）。
// 每个绑定行包含一个下拉组合框（Combo）用于选择按键，以及 +/- 按钮
// 用于添加/删除多键绑定。
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <string>
#include <vector>

#include "imgui/imgui.h"
#include "Xen.h"
#include "overlay.h"
#include "overlay/config_dirty.h"
#include "overlay/ui_sections.h"

namespace
{
/**
 * @brief 根据按键名称查找其在 key_names 数组中的索引。
 *
 * 遍历全局按键名称列表 key_names，返回与 keyName 匹配的元素的索引。
 * 如果未找到匹配项，则返回 0（即"None"，默认值）。
 *
 * @param keyName 要查找的按键名称字符串
 * @return 匹配的索引（int），未匹配时返回 0
 */
int findKeyIndexByName(const std::string& keyName)
{
    // 遍历所有按键名称，寻找匹配项
    for (size_t k = 0; k < key_names.size(); ++k)
    {
        if (key_names[k] == keyName)
            return static_cast<int>(k);
    }
    // 未找到则返回默认索引 0（"None"）
    return 0;
}

/**
 * @brief 绘制单个按钮绑定行（含多键支持）。
 *
 * 为指定的按钮动作（如 "Targeting"、"Shoot"）绘制一行或多行 UI，每行包含：
 *   - 标签（如 "Targeting 1"、"Targeting 2"）
 *   - 按键下拉选择框（Combo）
 *   - "+" 按钮：在当前行之后插入一个新的空绑定行
 *   - "-" 按钮：移除当前行；若 keepAtLeastOne 为 true 且只剩一行，则重置为 "None"
 *
 * 当 bindings 为空且 keepAtLeastOne 为 true 时，自动初始化一个 "None" 条目。
 *
 * @param rowLabel       行的显示名称（如 "Targeting"）
 * @param bindings       按键名称列表（引用，函数会修改此列表）
 * @param keepAtLeastOne 是否强制保持至少一个绑定项（默认 false）
 * @return true 表示绑定值发生了变更
 */
bool drawButtonBindingRows(const char* rowLabel, std::vector<std::string>& bindings, bool keepAtLeastOne)
{
    if (key_names_cstrs.empty())
    {
        ImGui::TextDisabled("无可用按键列表。");
        return false;
    }

    bool changed = false;
    if (bindings.empty() && keepAtLeastOne)
    {
        bindings.push_back("None");
        changed = true;
    }

    // 遍历每一行绑定（手动控制 i 递增，因为在删除元素时 i 不递增）
    for (size_t i = 0; i < bindings.size();)
    {
        std::string& currentKeyName = bindings[i];

        // 根据按键名称查找当前在下拉框中的索引位置
        int currentIndex = findKeyIndexByName(currentKeyName);

        // 构建行标签：若有多行则显示 "Label N"，否则仅显示 "Label"
        const std::string indexedLabel = (bindings.size() > 1)
            ? std::string(rowLabel) + " " + std::to_string(i + 1)
            : std::string(rowLabel);

        // 为每行分配独立的 ImGUI ID，避免控件 ID 冲突
        ImGui::PushID(static_cast<int>(i));

        // 开始标准设置行布局（含标签区域和控件区域）
        const auto row = OverlayUI::BeginSettingRow(indexedLabel.c_str());

        // 计算按钮宽度（使用标准行高作为按钮尺寸）
        const float actionBtnW = ImGui::GetFrameHeight();

        // 计算下拉框宽度：总控件宽度减去两个按钮宽度和间距
        float comboWidth = row.controlWidth - (actionBtnW * 2.0f + 7.0f);
        if (comboWidth < 1.0f)
            comboWidth = 1.0f;
        ImGui::SetNextItemWidth(comboWidth);

        // 绘制按键选择下拉框
        if (ImGui::Combo("##value", &currentIndex, key_names_cstrs.data(), static_cast<int>(key_names_cstrs.size())))
        {
            // 用户选择了新的按键，更新绑定数据
            currentKeyName = key_names[currentIndex];
            changed = true;
        }

        // 绘制 "+" 添加按钮：在当前行后插入一个新的 "None" 绑定
        ImGui::SameLine(0.0f, 4.0f);
        if (ImGui::Button("+", ImVec2(actionBtnW, 0.0f)))
        {
            bindings.insert(bindings.begin() + static_cast<std::vector<std::string>::difference_type>(i + 1), "None");
            changed = true;
        }

        ImGui::SameLine(0.0f, 3.0f);
        bool removedCurrent = false;
        if (ImGui::Button("-", ImVec2(actionBtnW, 0.0f)))
        {
            if (bindings.size() <= 1 && keepAtLeastOne)
            {
                bindings[0] = "None";
            }
            else
            {
                bindings.erase(bindings.begin() + static_cast<std::vector<std::string>::difference_type>(i));
                removedCurrent = true;
            }
            changed = true;
        }

        OverlayUI::EndSettingRow(row);
        ImGui::PopID();

        // 如果当前行已被删除，保持 i 不变继续下一轮循环；
        // 否则递增 i 进入下一行
        if (removedCurrent)
            continue;

        ++i;
    }

    return changed;
}

/**
 * @brief 包装函数：绘制按钮绑定行并标记配置为已脏。
 *
 * 调用 drawButtonBindingRows 绘制绑定行 UI，若用户做出了任何更改
 *（返回 true），则调用 OverlayConfig_MarkDirty() 标记配置为脏，
 * 以便后续在适当时机自动保存。
 *
 * @param rowLabel       行的显示名称（如 "Targeting"）
 * @param bindings       按键名称列表（引用，函数会修改此列表）
 * @param keepAtLeastOne 是否强制保持至少一个绑定项（默认 true）
 */
void drawBindingRowsAndMarkDirty(const char* rowLabel, std::vector<std::string>& bindings, bool keepAtLeastOne = true)
{
    if (drawButtonBindingRows(rowLabel, bindings, keepAtLeastOne))
        OverlayConfig_MarkDirty();
}
}

/**
 * @brief 绘制"热键"设置页面主函数。
 *
 * 此函数是按钮绑定设置的入口，在 overlay 设置面板中绘制 "热键" 章节。
 * 包含以下内容：
 *   - 七个按钮动作的绑定行（Targeting、Shoot、Zoom、Exit、Pause、
 *     Reload Config、Open Overlay），每个都支持多键绑定
 *   - 方向键选项（Arrow Key Options）复选开关
 *
 * 每个绑定行通过 drawBindingRowsAndMarkDirty 自动处理脏标记。
 */
void draw_buttons()
{
    // 开始 "热键" 章节（带可折叠功能）
    if (OverlayUI::BeginSection("按键绑定", "buttons_section_hotkeys"))
    {
        // 为各个按钮动作绘制绑定行，每个动作支持单键或多键绑定
        drawBindingRowsAndMarkDirty("瞄准", config.button_targeting);
        drawBindingRowsAndMarkDirty("射击", config.button_shoot);
        drawBindingRowsAndMarkDirty("缩放", config.button_zoom);
        drawBindingRowsAndMarkDirty("FOV复位", config.button_fov_reset);
        drawBindingRowsAndMarkDirty("退出", config.button_exit);
        drawBindingRowsAndMarkDirty("暂停", config.button_pause);
        drawBindingRowsAndMarkDirty("重载配置", config.button_reload_config);
        drawBindingRowsAndMarkDirty("打开叠加层", config.button_open_overlay);

        // 方向键选项：启用后在覆盖层中使用方向键操作
        const auto row = OverlayUI::BeginSettingRow("方向键选项");
        if (ImGui::Checkbox("##value", &config.enable_arrows_settings))
            OverlayConfig_MarkDirty();
        OverlayUI::EndSettingRow(row);
        OverlayUI::EndSection();
    }
}
