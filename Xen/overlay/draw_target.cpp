#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include "d3d11.h"
#include "imgui/imgui.h"

#include <cmath>
#include <iostream>
#include <mutex>
#include <vector>

#include "overlay.h"
#include "overlay/config_dirty.h"
#include "draw_settings.h"
#include "overlay/ui_sections.h"
#include "Xen.h"
#include "runtime/thread_loops.h"
#include "other_tools.h"
#include "memory_images.h"

// ============================================================
// 全局纹理资源
// bodyTexture — 身体部位预览纹理的着色器资源视图，用于在 UI 中绘制瞄准偏移参考图
// bodyImageSize — 纹理图像的显示尺寸（像素），在加载纹理后由 load_body_texture() 初始化
// ============================================================
ID3D11ShaderResourceView* bodyTexture = nullptr;
ImVec2 bodyImageSize;

// ============================================================
// 配置变更追踪变量（prev_* 系列）
// 每次 draw_target() / draw_tracker() 执行时，将当前配置值与上一帧的快照进行比对。
// 若任一值发生变更，则调用 OverlayConfig_MarkDirty() 标记配置为"脏状态"，
// 触发持久化存储（写入配置文件）。
// ============================================================
bool prev_disable_headshot = config.disable_headshot;             // 上一帧的"禁用爆头"状态
float prev_body_y_offset = config.body_y_offset;                 // 上一帧的身体 Y 偏移
float prev_head_y_offset = config.head_y_offset;                 // 上一帧的头部 Y 偏移
bool prev_auto_aim = config.auto_aim;                            // 上一帧的自动瞄准开关
bool prev_easynorecoil = config.easynorecoil;                    // 上一帧的简易无后座开关
float prev_easynorecoilstrength = config.easynorecoilstrength;   // 上一帧的无后座力度值
bool prev_tracker_enabled = config.tracker_enabled;              // 上一帧的追踪器启用状态
bool prev_tracker_overlay_table_enabled = config.tracker_overlay_table_enabled; // 上一帧的追踪表格显示开关

// ============================================================
// draw_target() — 绘制"瞄准(Targeting)"设置面板
// 包含三个 UI 区域：
//   1. Targeting（基本瞄准开关）：禁用爆头、自动瞄准
//   2. Offsets（偏移量调节）：身体 Y 偏移、头部 Y 偏移，支持方向键微调
//   3. Preview（预览）：在身体纹理图片上叠加红色（身体）和绿色（头部）参考线，
//      直观展示当前偏移量对应的瞄准位置
// 当 Targeting / Offsets 区域中的任意配置项发生变更时，自动标记配置为脏。
// ============================================================
void draw_target()
{
    // ---- Targeting 区 ----
    // 基本瞄准功能开关：禁用爆头（关闭头部锁定）、自动瞄准（自动对准目标）
    if (OverlayUI::BeginSection("目标选择", "target_section_targeting"))
    {
        OverlayUI::CheckboxRow("禁用爆头", &config.disable_headshot);
        OverlayUI::CheckboxRow("自动瞄准", &config.auto_aim);
        OverlayUI::EndSection();
    }

    // ---- Offsets 区 ----
    // 调整瞄准偏移量以适配不同游戏的角色模型尺寸。
    // body_y_offset: 身体部位瞄准点的垂直偏移（归一化 0~1）
    // head_y_offset: 头部瞄准点的垂直偏移（归一化 0~1）
    // 提示文本说明：方向键单独调整身体偏移，Shift+方向键调整头部偏移。
    if (OverlayUI::BeginSection("瞄准偏移", "target_section_offsets"))
    {
        ImGui::TextColored(ImVec4(0.10f, 0.35f, 0.70f, 1.0f), "方向键：调节身体偏移");
        ImGui::TextColored(ImVec4(0.10f, 0.35f, 0.70f, 1.0f), "Shift+方向键：调节头部偏移");

        OverlayUI::SliderFloatRow("身体Y偏移(近似)", &config.body_y_offset, 0.0f, 1.0f, "%.2f");
        OverlayUI::SliderFloatRow("头部Y偏移(近似)", &config.head_y_offset, 0.0f, 1.0f, "%.2f");
        OverlayUI::EndSection();
    }

    // ---- Preview 区 ----
    // 加载身体模型纹理图片，并在图片上绘制两条参考线：
    //   红色水平线 — 身体瞄准位置（由 body_y_offset 决定）
    //   绿色水平线 — 头部瞄准位置（由 head_y_offset 决定）
    // head_y_offset 的取值范围被映射到图片顶部到身体线之间的区域，
    // 使得头部偏移始终在身体线上方有效。
    if (OverlayUI::BeginSection("预览", "target_section_preview"))
    {
        if (bodyTexture)
        {
            // 在 ImGui 中绘制身体模型纹理图片
            ImGui::Image((ImTextureID)(intptr_t)bodyTexture, bodyImageSize);

            // 获取图片在窗口中的实际位置和尺寸（用于后续参考线坐标计算）
            ImVec2 image_pos = ImGui::GetItemRectMin();
            ImVec2 image_size = ImGui::GetItemRectSize();

            ImDrawList* draw_list = ImGui::GetWindowDrawList();

            // ---- 计算并绘制身体瞄准参考线（红色） ----
            // body_y_offset ∈ [0, 1]，映射到图片垂直范围：
            //   0 → 图片底部，1 → 图片顶部
            // 此处归一化公式使 body_y_offset=1.0 时线在顶部边缘，0 时在底部边缘
            float normalized_body_value = (config.body_y_offset - 1.0f) / 1.0f;
            float body_line_y = image_pos.y + (1.0f + normalized_body_value) * image_size.y;
            ImVec2 body_line_start = ImVec2(image_pos.x, body_line_y);
            ImVec2 body_line_end = ImVec2(image_pos.x + image_size.x, body_line_y);
            draw_list->AddLine(body_line_start, body_line_end, IM_COL32(255, 0, 0, 255), 2.0f);

            // ---- 计算并绘制头部瞄准参考线（绿色） ----
            // head_y_offset ∈ [0, 1]，映射到"图片顶部 → 身体线"之间的区域，
            // 即头部偏移始终限制在身体线上方（0 靠近顶部，1 靠近身体线）。
            // body_y_pos_at_015 表示取 body_y_offset=0.15 时的位置作为身体线参考点，
            // 确保头部线与身体线之间有合理的间距。
            float body_y_pos_at_015 = image_pos.y + (1.0f + (0.15f - 1.0f) / 1.0f) * image_size.y;
            float head_top_pos = image_pos.y;
            float head_line_y = head_top_pos + (config.head_y_offset * (body_y_pos_at_015 - head_top_pos));

            ImVec2 head_line_start = ImVec2(image_pos.x, head_line_y);
            ImVec2 head_line_end = ImVec2(image_pos.x + image_size.x, head_line_y);
            draw_list->AddLine(head_line_start, head_line_end, IM_COL32(0, 255, 0, 255), 2.0f);

            // 在参考线右侧标注文字
            draw_list->AddText(ImVec2(body_line_end.x + 5, body_line_y - 7), IM_COL32(255, 0, 0, 255), "身体");
            draw_list->AddText(ImVec2(head_line_end.x + 5, head_line_y - 7), IM_COL32(0, 255, 0, 255), "头部");
        }
        else
        {
            // 纹理加载失败时显示提示
            ImGui::Text("未找到图片！");
        }
        ImGui::Text("注意：不同游戏角色模型尺寸不同，偏移值需分别调整。");
        OverlayUI::EndSection();
    }

    // ---- 配置变更检测 ----
    // 将当前配置值与上一帧快照逐一比对，若有任一值发生变更则同步快照
    // 并调用 OverlayConfig_MarkDirty() 触发配置持久化。
    // 注意：此处不检测 tracker 相关配置，tracker 的变更在 draw_tracker() 中处理。
    if (prev_disable_headshot != config.disable_headshot ||
        prev_body_y_offset != config.body_y_offset ||
        prev_head_y_offset != config.head_y_offset ||
        prev_auto_aim != config.auto_aim ||
        prev_easynorecoil != config.easynorecoil ||
        prev_easynorecoilstrength != config.easynorecoilstrength)
    {
        prev_disable_headshot = config.disable_headshot;
        prev_body_y_offset = config.body_y_offset;
        prev_head_y_offset = config.head_y_offset;
        prev_auto_aim = config.auto_aim;
        prev_easynorecoil = config.easynorecoil;
        prev_easynorecoilstrength = config.easynorecoilstrength;
        OverlayConfig_MarkDirty();
    }
}

// ============================================================
// draw_tracker() — 绘制"追踪器(Tracker)"设置面板
// 在多线程环境下从 g_trackerDebugMutex 保护的数据中安全读取追踪信息，
// 包含两个 UI 区域：
//   1. Status（状态）：追踪器启用开关、追踪表格显示开关、锁定模式、
//      当前运行时模式、锁定 Track ID、活跃追踪数
//   2. Tracks（追踪列表）：以表格形式展示每条追踪记录的详细信息
//      （ID、类别、锁定状态、观测帧、丢失帧、枢轴坐标、速度）
// 当 Tracker / Show Target Table 开关发生变更时，自动标记配置为脏。
// ============================================================
void draw_tracker()
{
    // ---- 线程安全的数据快照 ----
    // 在互斥锁 g_trackerDebugMutex 保护下，将追踪器调试数据
    // 从共享缓冲区复制到局部变量，避免在 UI 渲染期间发生数据竞争。
    bool changed = false;
    std::vector<TrackDebugInfo> tracks;
    int lockedTrackId = -1;
    {
        std::lock_guard<std::mutex> lk(g_trackerDebugMutex);
        tracks = g_trackerDebugTracks;
        lockedTrackId = g_trackerLockedId;
    }

    // ---- Status 区 ----
    // 显示追踪器的全局状态信息：
    //   Enable Tracker      — 启用/停用追踪功能
    //   Show Target Table   — 控制下方 Tracks 表格的显示
    //   Mode                — 当前固定为 Simple Lock（简单锁定）模式
    //   Runtime             — 当前生效的运行时策略（Tracker / Nearest Target）
    //   Locked Track ID     — 当前锁定的追踪目标 ID（-1 表示未锁定）
    //   Active Tracks       — 当前活跃的追踪目标数量
    if (OverlayUI::BeginSection("追踪状态", "tracker_section_status"))
    {
        changed |= OverlayUI::CheckboxRow("启用追踪器", &config.tracker_enabled);
        changed |= OverlayUI::CheckboxRow("显示目标表格", &config.tracker_overlay_table_enabled);
        ImGui::Text("模式：简单锁定");
        ImGui::Text("运行时：%s", config.tracker_enabled ? "追踪" : "最近目标");
        ImGui::Text("锁定追踪ID：%d", lockedTrackId);
        ImGui::Text("活跃追踪数：%d", static_cast<int>(tracks.size()));
        OverlayUI::EndSection();
    }

    if (config.tracker_overlay_table_enabled && OverlayUI::BeginSection("活跃目标", "tracker_section_tracks"))
    {
        // ---- Tracks 区（追踪详细信息表格） ----
        // 仅在 tracker_overlay_table_enabled 为 true 时渲染。
        // 表格包含 7 列，每条 TrackDebugInfo 占一行：
        //   ID       — 追踪目标的唯一标识符
        //   Class    — 目标类别 ID（如玩家、载具等）
        //   Locked   — 是否被追踪器锁定
        //   Observed — 当前帧是否被观测到
        //   Missed   — 连续丢失帧数（观测失败的帧计数）
        //   Pivot    — 目标的枢轴点坐标 (pivotX, pivotY)
        //   Speed    — 目标的移动速度（由 velocityX/Y 计算出的欧几里得距离）
        if (tracks.empty())
        {
            ImGui::TextDisabled("无活跃追踪");
        }
        else if (ImGui::BeginTable("tracker_tracks_table", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("ID");
            ImGui::TableSetupColumn("类别");
            ImGui::TableSetupColumn("锁定");
            ImGui::TableSetupColumn("已观测");
            ImGui::TableSetupColumn("丢失帧");
            ImGui::TableSetupColumn("枢轴");
            ImGui::TableSetupColumn("速度");
            ImGui::TableHeadersRow();

            for (const auto& track : tracks)
            {
                const double speed = std::hypot(track.velocityX, track.velocityY);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%d", track.trackId);
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%d", track.classId);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%s", track.isLocked ? "是" : "否");
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%s", track.observedThisFrame ? "是" : "否");
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%d", track.missedFrames);
                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%.0f, %.0f", track.pivotX, track.pivotY);
                ImGui::TableSetColumnIndex(6);
                ImGui::Text("%.0f", speed);
            }

            ImGui::EndTable();
        }
        OverlayUI::EndSection();
    }

    // ---- 配置变更检测 ----
    // 检测 tracker_enabled 和 tracker_overlay_table_enabled 的变更，
    // 若发生变更则同步快照并触发配置持久化。
    if (changed ||
        prev_tracker_enabled != config.tracker_enabled ||
        prev_tracker_overlay_table_enabled != config.tracker_overlay_table_enabled)
    {
        prev_tracker_enabled = config.tracker_enabled;
        prev_tracker_overlay_table_enabled = config.tracker_overlay_table_enabled;
        OverlayConfig_MarkDirty();
    }
}

// ============================================================
// load_body_texture() — 从 Base64 编码的内存数据加载身体模型纹理
// 将三段 Base64 编码的图片数据（bodyImageBase64_1/2/3）拼接为完整字符串，
// 调用 LoadTextureFromMemory() 解码并创建 D3D11 着色器资源视图。
// 加载成功后初始化 bodyImageSize；失败时向 stderr 输出错误信息。
// ============================================================
void load_body_texture()
{
    // 拼接三段 Base64 编码的图片数据（定义在 memory_images.h 中）
    int image_width = 0;
    int image_height = 0;

    std::string body_image = std::string(bodyImageBase64_1) + std::string(bodyImageBase64_2) + std::string(bodyImageBase64_3);

    // 调用 LoadTextureFromMemory() 解码 Base64 → 图像 → D3D11 纹理资源
    bool ret = LoadTextureFromMemory(body_image, g_pd3dDevice, &bodyTexture, &image_width, &image_height);
    if (!ret)
    {
        std::cerr << "[Overlay] Can't load image!" << std::endl;
    }
    else
    {
        // 保存纹理图像尺寸，供预览区绘制时使用
        bodyImageSize = ImVec2((float)image_width, (float)image_height);
    }
}

// ============================================================
// release_body_texture() — 释放身体模型纹理资源
// 安全释放 bodyTexture 指向的 D3D11 着色器资源视图并将其置空，
// 防止资源泄漏和悬挂指针。通常在 Overlay 关闭或纹理重载时调用。
// ============================================================
void release_body_texture()
{
    if (bodyTexture)
    {
        bodyTexture->Release();
        bodyTexture = nullptr;
    }
}
