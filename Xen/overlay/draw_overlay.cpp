#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#include <algorithm>

#include "imgui/imgui.h"
#include "Xen.h"
#include "overlay.h"
#include "overlay/config_dirty.h"
#include "overlay/ui_sections.h"

// 绘制叠加层UI的主函数，包含视觉设置（透明度、UI缩放、录制隐藏）
void draw_overlay()
{
    // 最小可读透明度常量：低于此值则文字无法清晰辨认
    constexpr int kMinReadableOpacity = 252;

    // 进入"Visual（视觉）"设置分区，对应界面上方的视觉分类标签
    if (OverlayUI::BeginSection("面板外观", "overlay_section_visual"))
    {
        // --- 叠加层透明度滑块 ---
        {
            const auto row = OverlayUI::BeginSettingRow("叠加层透明度");
            int prev_opacity = config.overlay_opacity;
            // SliderInt 在 [kMinReadableOpacity, 255] 范围内调节透明度
            if (ImGui::SliderInt("##overlay_opacity_slider", &config.overlay_opacity, kMinReadableOpacity, 255))
            {
                // 限制透明度值在合法范围内，低于最小值时回弹
                if (config.overlay_opacity < kMinReadableOpacity) config.overlay_opacity = kMinReadableOpacity;
                if (config.overlay_opacity > 255) config.overlay_opacity = 255;

                Overlay_SetOpacity(config.overlay_opacity);

                // 值发生变化时标记配置为"已修改"，供后续保存使用
                if (config.overlay_opacity != prev_opacity)
                    OverlayConfig_MarkDirty();
            }
            OverlayUI::EndSettingRow(row);
        }

        // 对 UI 缩放值施加硬边界限定，防止用户配置越界
        float ui_scale = std::clamp(config.overlay_ui_scale, 0.85f, 1.35f);
        // 若截断后的值与配置不同则写回，保证持久化值始终在合法范围
        if (ui_scale != config.overlay_ui_scale)
            config.overlay_ui_scale = ui_scale;

        // --- UI 精细缩放滑块 ---
        {
            const auto row = OverlayUI::BeginSettingRow("UI精细缩放");
            // SliderFloat 在 [0.85, 1.35] 范围内以两位小数精度调节
            if (ImGui::SliderFloat("##overlay_ui_scale_slider", &ui_scale, 0.85f, 1.35f, "%.2f"))
            {
                config.overlay_ui_scale = ui_scale;
                OverlayConfig_MarkDirty();
            }
            OverlayUI::EndSettingRow(row);
        }

        // --- "录制时隐藏叠加层"复选框 ---
        {
            const auto row = OverlayUI::BeginSettingRow("录制时隐藏叠加层");
            // Checkbox 绑定 config.overlay_exclude_from_capture，勾选后叠加层不出现在录屏/截图中
            if (ImGui::Checkbox("##hide_overlay_from_recording", &config.overlay_exclude_from_capture))
            {
                Overlay_ApplyCaptureExclusion();
                OverlayConfig_MarkDirty();
            }
            OverlayUI::EndSettingRow(row);
        }

        // 结束"Visual"设置分区
        OverlayUI::EndSection();
    }
}
