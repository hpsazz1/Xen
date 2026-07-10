#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <commdlg.h>
#include <algorithm>
#include <string>
#include <cstring>

#include "imgui/imgui.h"
#include "config.h"
#include "Xen.h"
#include "overlay/config_dirty.h"
#include "overlay/ui_sections.h"

extern std::string g_iconLastError;

namespace
{
// 游戏叠加层设置页面枚举
// All: 显示所有设置页面; General: 仅显示常规设置; Visuals: 仅显示视觉设置; Icon: 仅显示图标设置
enum class GameOverlaySettingsPage
{
    All,
    General,
    Visuals,
    Icon
};

// 判断是否应绘制指定页面：当当前页面为 All（全部）或当前页面与目标页面匹配时返回 true
bool shouldDrawGameOverlayPage(GameOverlaySettingsPage current, GameOverlaySettingsPage wanted)
{
    return current == GameOverlaySettingsPage::All || current == wanted;
}
}

// 主绘制函数：根据传入的页面枚举值绘制对应的叠加层设置界面
// page 参数控制显示的页面范围，由 draw_game_overlay_settings/draw_game_overlay_general 等公共入口调用
static void draw_game_overlay_page(GameOverlaySettingsPage page)
{
    // ========== 常规设置（General） ==========
    // 包含启用开关、最大帧率、检测框、延迟补偿、未来位置、轨迹模拟调试尾迹及目标修正等选项
    if (shouldDrawGameOverlayPage(page, GameOverlaySettingsPage::General) &&
        OverlayUI::BeginSection("常规", "game_overlay_section_general"))
    {
        if (OverlayUI::CheckboxRow("启用", &config.game_overlay_enabled))
            OverlayConfig_MarkDirty();

        if (OverlayUI::SliderIntRow("叠加层最大帧率(0=不限)", &config.game_overlay_max_fps, 0, 256))
            OverlayConfig_MarkDirty();

        if (OverlayUI::CheckboxRow("绘制检测框", &config.game_overlay_draw_boxes))
            OverlayConfig_MarkDirty();

        if (OverlayUI::CheckboxRow("补偿叠加延迟", &config.game_overlay_compensate_latency))
            OverlayConfig_MarkDirty();

        if (OverlayUI::CheckboxRow("绘制未来位置", &config.game_overlay_draw_future))
            OverlayConfig_MarkDirty();

        if (OverlayUI::CheckboxRow("绘制轨迹模拟调试尾迹", &config.game_overlay_draw_wind_tail))
            OverlayConfig_MarkDirty();

        if (OverlayUI::CheckboxRow("显示目标修正", &config.game_overlay_show_target_correction))
            OverlayConfig_MarkDirty();

        OverlayUI::EndSection();
    }

    // ========== 方框颜色（Box Color） ==========
    // 调整检测框的 ARGB 颜色分量及边框粗细
    if (shouldDrawGameOverlayPage(page, GameOverlaySettingsPage::Visuals) &&
        OverlayUI::BeginSection("方框颜色", "game_overlay_section_box_color"))
    {
        // 标记颜色值是否发生变化，若变化则调用 clampGameOverlayColor 确保分量在合法范围内
        bool colorChanged = false;

        colorChanged |= OverlayUI::SliderIntRow("透明度", &config.game_overlay_box_a, 0, 255);
        colorChanged |= OverlayUI::SliderIntRow("红色", &config.game_overlay_box_r, 0, 255);
        colorChanged |= OverlayUI::SliderIntRow("绿色", &config.game_overlay_box_g, 0, 255);
        colorChanged |= OverlayUI::SliderIntRow("蓝色", &config.game_overlay_box_b, 0, 255);

        if (OverlayUI::SliderFloatRow("方框粗细", &config.game_overlay_box_thickness, 0.5f, 10.0f, "%.1f"))
            OverlayConfig_MarkDirty();

        if (colorChanged)
        {
            config.clampGameOverlayColor();
            OverlayConfig_MarkDirty();
        }

        OverlayUI::EndSection();
    }

    // ========== 捕获框（Capture Frame） ==========
    // 控制捕获框与圆形辅助线的绘制开关、帧颜色 ARGB 分量及帧边框粗细
    if (shouldDrawGameOverlayPage(page, GameOverlaySettingsPage::Visuals) &&
        OverlayUI::BeginSection("捕获框", "game_overlay_section_capture_frame"))
    {
        if (OverlayUI::CheckboxRow("绘制捕获框", &config.game_overlay_draw_frame))
            OverlayConfig_MarkDirty();

        if (OverlayUI::CheckboxRow("绘制圆形辅助线", &config.game_overlay_draw_circle_fov))
            OverlayConfig_MarkDirty();

        bool frameColorChanged = false;

        frameColorChanged |= OverlayUI::SliderIntRow("透明度", &config.game_overlay_frame_a, 0, 255);
        frameColorChanged |= OverlayUI::SliderIntRow("红色", &config.game_overlay_frame_r, 0, 255);
        frameColorChanged |= OverlayUI::SliderIntRow("绿色", &config.game_overlay_frame_g, 0, 255);
        frameColorChanged |= OverlayUI::SliderIntRow("蓝色", &config.game_overlay_frame_b, 0, 255);

        if (OverlayUI::SliderFloatRow("边框粗细", &config.game_overlay_frame_thickness, 0.5f, 10.0f, "%.1f"))
            OverlayConfig_MarkDirty();

        if (frameColorChanged)
        {
            config.clampGameOverlayColor();
            OverlayConfig_MarkDirty();
        }

        OverlayUI::EndSection();
    }

    // ========== 未来位置点样式（Future Point Style） ==========
    // 调整预测轨迹点的半径大小及透明度随步数的衰减速度
    if (shouldDrawGameOverlayPage(page, GameOverlaySettingsPage::Visuals) &&
        OverlayUI::BeginSection("未来点样式", "game_overlay_section_future_style"))
    {
        if (OverlayUI::SliderFloatRow("点半径", &config.game_overlay_future_point_radius, 1.0f, 20.0f, "%.1f"))
            OverlayConfig_MarkDirty();

        if (OverlayUI::SliderFloatRow("逐点透明度衰减", &config.game_overlay_future_alpha_falloff, 0.1f, 5.0f, "%.2f"))
            OverlayConfig_MarkDirty();

        OverlayUI::EndSection();
    }

    // ========== 图标叠加（Icon Overlay） ==========
    // 配置图标叠加层的启用开关、图标路径、大小、偏移量、过滤类别及锚点位置
    // 当图标叠加未启用时，该区域内的所有控件将被禁用（灰色不可操作状态）
    if (shouldDrawGameOverlayPage(page, GameOverlaySettingsPage::Icon) &&
        OverlayUI::BeginSection("图标叠加", "game_overlay_section_icon"))
    {
        if (OverlayUI::CheckboxRow("启用图标叠加", &config.game_overlay_icon_enabled))
            OverlayConfig_MarkDirty();

        if (!config.game_overlay_icon_enabled)
        {
            ImGui::BeginDisabled();
        }

        // 将持久化配置中的图标路径初始化到本地静态缓冲区中，仅首次绘制时执行一次
        // 使用静态缓冲区而非直接绑定 config 字符串，以便在输入过程中独立编辑
        static bool pathInit = false;
        static char iconPathBuf[512];

        if (!pathInit)
        {
            pathInit = true;
            memset(iconPathBuf, 0, sizeof(iconPathBuf));
            std::string p = config.game_overlay_icon_path;
            if (p.size() >= sizeof(iconPathBuf)) p = p.substr(0, sizeof(iconPathBuf) - 1);
            memcpy(iconPathBuf, p.c_str(), p.size());
        }

        {
            // 图标路径行：左侧为文本标签，右侧为输入框和浏览（Browse）按钮
            // 用户可直接输入路径，或点击 Browse 打开系统文件选择对话框选择图片文件
            const auto row = OverlayUI::BeginSettingRow("图标路径");
            const float browseW = 76.0f;
            const float inputW = std::max(1.0f, row.controlWidth - browseW - ImGui::GetStyle().ItemSpacing.x);
            ImGui::SetNextItemWidth(inputW);
            if (ImGui::InputText("##value", iconPathBuf, IM_ARRAYSIZE(iconPathBuf)))
            {
                config.game_overlay_icon_path = iconPathBuf;
                OverlayConfig_MarkDirty();
            }
            ImGui::SameLine();
            if (ImGui::Button("浏览", ImVec2(browseW, 0.0f)))
            {
                // 使用 Windows 通用文件对话框（GetOpenFileNameA）选择图标图片
                // 支持的格式：png、jpg、jpeg、bmp、ico
                char filePath[MAX_PATH] = {};
                OPENFILENAMEA ofn = {};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = nullptr;
                ofn.lpstrFile = filePath;
                ofn.nMaxFile = sizeof(filePath);
                ofn.lpstrFilter = "Image Files\0*.png;*.jpg;*.jpeg;*.bmp;*.ico\0All Files\0*.*\0";
                ofn.nFilterIndex = 1;
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

                if (GetOpenFileNameA(&ofn))
                {
                    strncpy_s(iconPathBuf, filePath, sizeof(iconPathBuf) - 1);
                    config.game_overlay_icon_path = iconPathBuf;
                    OverlayConfig_MarkDirty();
                }
            }
            OverlayUI::EndSettingRow(row);
        }

        if (OverlayUI::SliderIntRow("图标宽度", &config.game_overlay_icon_width, 4, 512))
            OverlayConfig_MarkDirty();

        if (OverlayUI::SliderIntRow("图标高度", &config.game_overlay_icon_height, 4, 512))
            OverlayConfig_MarkDirty();

        if (OverlayUI::SliderFloatRow("图标X偏移", &config.game_overlay_icon_offset_x, -500.0f, 500.0f, "%.1f"))
            OverlayConfig_MarkDirty();

        if (OverlayUI::SliderFloatRow("图标Y偏移", &config.game_overlay_icon_offset_y, -500.0f, 500.0f, "%.1f"))
            OverlayConfig_MarkDirty();

        if (OverlayUI::InputIntRow("图标类别(-1=全部)", &config.game_overlay_icon_class))
        {
            if (config.game_overlay_icon_class < -1) config.game_overlay_icon_class = -1;
            OverlayConfig_MarkDirty();
        }

        // 图标锚点（Icon Anchor）下拉选择框
        // 锚点控制图标相对于目标位置的附着方式：居中（center）、顶部（top）、底部（bottom）或头部（head）
        const char* anchors[] = { "center", "top", "bottom", "head" };
        int currentAnchor = 0;
        for (int i = 0; i < (int)(sizeof(anchors) / sizeof(anchors[0])); ++i)
        {
            if (config.game_overlay_icon_anchor == anchors[i])
            {
                currentAnchor = i;
                break;
            }
        }

        if (OverlayUI::ComboRow("图标锚点", &currentAnchor, anchors, IM_ARRAYSIZE(anchors)))
        {
            config.game_overlay_icon_anchor = anchors[currentAnchor];
            OverlayConfig_MarkDirty();
        }

        if (!config.game_overlay_icon_enabled)
        {
            ImGui::EndDisabled();
            ImGui::TextDisabled("启用图标叠加后才能编辑设置。");
        }

        OverlayUI::EndSection();
    }

    // ========== 错误信息（Errors） ==========
    // 当 g_iconLastError 不为空时，显示图标加载/运行过程中的错误信息（红色字体）
    if (shouldDrawGameOverlayPage(page, GameOverlaySettingsPage::Icon) && !g_iconLastError.empty())
    {
        if (OverlayUI::BeginSection("错误信息", "game_overlay_section_errors"))
        {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 100, 100, 255));
            ImGui::TextWrapped("%s", g_iconLastError.c_str());
            ImGui::PopStyleColor();
            OverlayUI::EndSection();
        }
    }

}

// 公共入口：显示游戏叠加层的所有设置页面
// 调用 draw_game_overlay_page 并传入 All 枚举值，绘制 General、Visuals、Icon 全部页面
void draw_game_overlay_settings()
{
    draw_game_overlay_page(GameOverlaySettingsPage::All);
}

// 公共入口：仅显示游戏叠加层的常规设置页面
// 供外部仅需展示 General 标签时调用
void draw_game_overlay_general()
{
    draw_game_overlay_page(GameOverlaySettingsPage::General);
}

// 公共入口：仅显示游戏叠加层的视觉设置页面
// 供外部仅需展示 Visuals（方框颜色、捕获框、未来位置点样式）标签时调用
void draw_game_overlay_visuals()
{
    draw_game_overlay_page(GameOverlaySettingsPage::Visuals);
}

// 公共入口：仅显示游戏叠加层的图标设置页面
// 供外部仅需展示 Icon（图标叠加配置及错误信息）标签时调用
void draw_game_overlay_icon()
{
    draw_game_overlay_page(GameOverlaySettingsPage::Icon);
}
