#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <shellapi.h>
#include <algorithm>

#include "imgui/imgui.h"
#include <imgui_internal.h>

#include "Xen.h"
#include "overlay/config_dirty.h"
#include "overlay/ui_sections.h"
#include "include/other_tools.h"
#include "kmbox_net/picture.h"

// 获取当前安装的 GHUB 版本号（用于输入法兼容性检测）
std::string ghub_version = get_ghub_version();

// ===== 鼠标设置全局脏检测变量 =====
// 以下 prev_* 变量用于在每帧检测配置是否发生变化，
// 一旦检测到差异则同步更新到 mouseThread 并标记配置为已修改。
// 视野（FOV）相关
int prev_fovX = config.fovX;
int prev_fovY = config.fovY;
// 速度倍率
float prev_minSpeedMultiplier = config.minSpeedMultiplier;
float prev_maxSpeedMultiplier = config.maxSpeedMultiplier;
// 预测
float prev_predictionInterval = config.predictionInterval;
std::string prev_prediction_mode = config.prediction_mode;
// 卡尔曼滤波
bool  prev_kalman_enabled = config.kalman_enabled;
float prev_kalman_process_noise_position = config.kalman_process_noise_position;
float prev_kalman_process_noise_velocity = config.kalman_process_noise_velocity;
float prev_kalman_measurement_noise = config.kalman_measurement_noise;
float prev_kalman_velocity_damping = config.kalman_velocity_damping;
float prev_kalman_max_velocity = config.kalman_max_velocity;
int   prev_kalman_warmup_frames = config.kalman_warmup_frames;
bool  prev_kalman_compensate_detection_delay = config.kalman_compensate_detection_delay;
float prev_kalman_additional_prediction_ms = config.kalman_additional_prediction_ms;
float prev_kalman_reset_timeout_sec = config.kalman_reset_timeout_sec;
// 目标修正（吸附半径、近距半径、速度曲线、吸附增益）
float prev_snapRadius = config.snapRadius;
float prev_nearRadius = config.nearRadius;
float prev_speedCurveExponent = config.speedCurveExponent;
float prev_snapBoostFactor = config.snapBoostFactor;

// 轨迹模拟
bool  prev_wind_mouse_enabled = config.wind_mouse_enabled;
float prev_wind_G = config.wind_G;
float prev_wind_W = config.wind_W;
float prev_wind_M = config.wind_M;
float prev_wind_D = config.wind_D;

// 自动射击
bool prev_auto_shoot = config.auto_shoot;
float prev_bScope_multiplier = config.bScope_multiplier;

// Bezier / EMA / 开火拟人化 / 急停 / 解锁Y / 射击修正
bool  prev_bezier_enabled = config.bezier_enabled;
float prev_bezier_strength = config.bezier_strength;
bool  prev_move_ema_enabled = config.move_ema_enabled;
float prev_move_ema_alpha = config.move_ema_alpha;
int   prev_trigger_stable_frames = config.trigger_stable_frames;
float prev_trigger_random_delay_ms = config.trigger_random_delay_ms;
float prev_trigger_delay_jitter_ms = config.trigger_delay_jitter_ms;
float prev_trigger_hold_ms = config.trigger_hold_ms;
float prev_trigger_hold_jitter_ms = config.trigger_hold_jitter_ms;
float prev_trigger_shot_cooldown_ms = config.trigger_shot_cooldown_ms;
bool  prev_auto_stop_enabled = config.auto_stop_enabled;
float prev_auto_stop_hold_ms = config.auto_stop_hold_ms;
bool  prev_unlock_y_enabled = config.unlock_y_enabled;
float prev_unlock_y_threshold_ms = config.unlock_y_threshold_ms;
float prev_unlock_y_strength = config.unlock_y_strength;
float prev_fire_correction_strength = config.fire_correction_strength;

namespace
{
// 鼠标设置页面枚举
enum class MouseSettingsPage
{
    All,         // 所有页面
    Movement,    // 移动相关（FOV、速度倍率、目标修正）
    Trajectory,  // 轨迹相关（Wind Mouse、Bezier、EMA）
    Prediction,  // 预测（预测间隔 + 卡尔曼滤波）
    Assist,      // 辅助（自动射击 / 开火拟人 / 急停 / 解锁Y / 射击修正）
    Profiles,    // 配置文件管理（Game Profile + Manage Profiles）
    Input        // 输入法
};

// 辅助页子页签
enum class AssistSubPage { Shooting, Tactical };

bool shouldDrawMousePage(MouseSettingsPage current, MouseSettingsPage wanted)
{
    return current == MouseSettingsPage::All || current == wanted;
}
}

// ===== 鼠标设置主绘制函数 =====
// 根据 page 参数选择性渲染各子页面（Movement / Prediction / Assist / Profiles / Input）
// 每个子页面内部包含控件绑定与 OverlayConfig_MarkDirty 标记
// 函数末尾包含全局脏检测块，当任意配置项变化时同步更新 mouseThread
static void draw_mouse_page(MouseSettingsPage page)
{
    // ========== FOV（视野）设置 ==========
    if (shouldDrawMousePage(page, MouseSettingsPage::Movement) &&
        OverlayUI::BeginSection("视野(FOV)", "mouse_section_fov"))
    {
        OverlayUI::SliderIntRow("水平视野(FOV X)", &config.fovX, 10, 120);
        OverlayUI::SliderIntRow("垂直视野(FOV Y)", &config.fovY, 10, 120);
        OverlayUI::EndSection();
    }

    // ========== Speed Multiplier（速度倍率）设置 ==========
    if (shouldDrawMousePage(page, MouseSettingsPage::Movement) &&
        OverlayUI::BeginSection("速度倍率", "mouse_section_speed_multiplier"))
    {
        OverlayUI::SliderFloatRow("最小速度倍率", &config.minSpeedMultiplier, 0.1f, 5.0f, "%.1f");
        OverlayUI::SliderFloatRow("最大速度倍率", &config.maxSpeedMultiplier, 0.1f, 5.0f, "%.1f");
        OverlayUI::EndSection();
    }

    // ========== Prediction（预测）设置 ==========
    if (shouldDrawMousePage(page, MouseSettingsPage::Prediction) &&
        OverlayUI::BeginSection("预测模式", "mouse_section_prediction_mode"))
    {
        const char* predModes[]  = { "关闭", "仅延迟补偿", "线性预测", "卡尔曼滤波" };
        const char* predKeys[]   = { "off", "delay", "linear", "kalman" };
        int curMode = 0;
        for (int i = 0; i < 4; ++i)
            if (config.prediction_mode == predKeys[i]) { curMode = i; break; }

        if (OverlayUI::ComboRow("预测模式", &curMode, predModes, 4))
        {
            config.prediction_mode = predKeys[curMode];
            OverlayConfig_MarkDirty();
        }
        OverlayUI::EndSection();
    }

    // ========== 预测参数 ==========
    if (shouldDrawMousePage(page, MouseSettingsPage::Prediction) &&
        OverlayUI::BeginSection("预测参数", "mouse_section_prediction"))
    {
        bool predActive = (config.prediction_mode != "off");

        OverlayUI::SliderFloatRow("预测前瞻(s)", &config.predictionInterval, 0.0f, 0.12f, "%.3f");
        if (!predActive) ImGui::BeginDisabled();

        OverlayUI::SliderIntRow("预测点数", &config.prediction_futurePositions, 4, 30);
        OverlayUI::CheckboxRow("绘制预测点", &config.draw_futurePositions);

        if (!predActive) { ImGui::EndDisabled(); }

        OverlayUI::EndSection();
    }

    // ========== 卡尔曼高级参数（仅在 kalman 模式显示）==========
    if (config.prediction_mode == "kalman" &&
        shouldDrawMousePage(page, MouseSettingsPage::Prediction) &&
        OverlayUI::BeginSection("卡尔曼滤波", "mouse_section_kalman"))
    {
        if (OverlayUI::CheckboxRow("启用卡尔曼滤波", &config.kalman_enabled))
            OverlayConfig_MarkDirty();
        OverlayUI::SliderFloatRow("过程噪声(位置)", &config.kalman_process_noise_position, 0.001f, 5000.0f, "%.1f");
        OverlayUI::SliderFloatRow("过程噪声(速度)", &config.kalman_process_noise_velocity, 0.001f, 50000.0f, "%.1f");
        OverlayUI::SliderFloatRow("测量噪声", &config.kalman_measurement_noise, 0.001f, 5000.0f, "%.1f");
        OverlayUI::SliderFloatRow("速度阻尼", &config.kalman_velocity_damping, 0.0f, 3.0f, "%.3f");
        OverlayUI::SliderFloatRow("最大速度", &config.kalman_max_velocity, 100.0f, 60000.0f, "%.0f");
        OverlayUI::SliderIntRow("预热帧数", &config.kalman_warmup_frames, 0, 20);
        OverlayUI::CheckboxRow("补偿推理延迟", &config.kalman_compensate_detection_delay);
        OverlayUI::SliderFloatRow("额外预测(ms)", &config.kalman_additional_prediction_ms, -80.0f, 120.0f, "%.1f");
        OverlayUI::SliderFloatRow("重置超时(秒)", &config.kalman_reset_timeout_sec, 0.05f, 3.0f, "%.2f");
        OverlayUI::EndSection();
    }

    // ========== Target Correction（目标修正）设置 ==========
    // 控制鼠标在目标周围的吸附行为：吸附半径、近距过渡、速度曲线、吸附增强
    if (shouldDrawMousePage(page, MouseSettingsPage::Movement) &&
        OverlayUI::BeginSection("目标修正", "mouse_section_target_correction"))
    {
        // 吸附半径：鼠标距离目标小于此值时触发吸附
        OverlayUI::SliderFloatRow("吸附半径", &config.snapRadius, 0.1f, 5.0f, "%.1f");
        // 近距半径：在此范围内速度倍率逐渐降低，实现平滑过渡
        OverlayUI::SliderFloatRow("近距半径", &config.nearRadius, 1.0f, 40.0f, "%.1f");
        // 速度曲线指数：指数越大，远距时速度增长越快
        OverlayUI::SliderFloatRow("速度曲线指数", &config.speedCurveExponent, 0.1f, 10.0f, "%.1f");
        // 吸附增益因子：吸附效果的强度倍率
        OverlayUI::SliderFloatRow("吸附增强因子", &config.snapBoostFactor, 0.01f, 4.00f, "%.2f");
        OverlayUI::EndSection();
    }

    // ========== Game Profile（游戏配置文件）设置 ==========
    // 每个游戏可以独立保存灵敏度、Yaw、Pitch、FOV 缩放等参数
    if (shouldDrawMousePage(page, MouseSettingsPage::Profiles) &&
        OverlayUI::BeginSection("游戏配置文件", "mouse_section_game_profile"))
    {
        // 收集所有配置文件名并排序
        std::vector<std::string> profile_names;
        for (const auto& kv : config.game_profiles)
            profile_names.push_back(kv.first);
        std::sort(profile_names.begin(), profile_names.end());

        // 定位当前活跃配置在列表中的索引
        static int selected_index = 0;
        for (size_t i = 0; i < profile_names.size(); ++i)
        {
            if (profile_names[i] == config.active_game)
            {
                selected_index = static_cast<int>(i);
                break;
            }
        }

        std::vector<const char*> profile_items;
        for (const auto& name : profile_names)
            profile_items.push_back(name.c_str());

        // 游戏配置切换下拉框
        if (OverlayUI::ComboRow("当前游戏配置", &selected_index, profile_items.data(), static_cast<int>(profile_items.size())))
        {
            config.active_game = profile_names[selected_index];
            OverlayConfig_MarkDirty();
            globalMouseThread->updateConfig(
                config.detection_resolution,
                config.fovX,
                config.fovY,
                config.minSpeedMultiplier,
                config.maxSpeedMultiplier,
                config.predictionInterval,
                config.auto_shoot,
                config.bScope_multiplier
            );
        }

        // 显示当前配置文件的详细参数（只读摘要）
        const auto& gp = config.currentProfile();

        ImGui::Text("当前配置：%s", gp.name.c_str());
        ImGui::Text("灵敏度：%.4f", gp.sens);
        ImGui::Text("偏航：%.4f", gp.yaw);
        ImGui::Text("俯仰：%.4f", gp.pitch);
        ImGui::Text("FOV缩放：%s", gp.fovScaled ? "true" : "false");

        // "UNIFIED" 为内置通用配置，不可编辑
        if (gp.name != "UNIFIED")
        {
            Config::GameProfile& modifiable = config.game_profiles[gp.name];
            bool changed = false;

            float sens_f = static_cast<float>(modifiable.sens);
            float yaw_f = static_cast<float>(modifiable.yaw);
            float pitch_f = static_cast<float>(modifiable.pitch);
            float baseFOV_f = static_cast<float>(modifiable.baseFOV);

            // 灵敏度、Yaw、Pitch 编辑滑块
            changed |= OverlayUI::SliderFloatRow("灵敏度", &sens_f, 0.001f, 10.0f, "%.4f");
            changed |= OverlayUI::SliderFloatRow("偏航", &yaw_f, 0.001f, 0.1f, "%.4f");
            changed |= OverlayUI::SliderFloatRow("俯仰", &pitch_f, 0.001f, 0.1f, "%.4f");

            // FOV 缩放：开启后根据当前 FOV 自动调整灵敏度
            changed |= OverlayUI::CheckboxRow("FOV缩放", &modifiable.fovScaled);
            if (modifiable.fovScaled)
            {
                // 基准 FOV（用于缩放计算）
                changed |= OverlayUI::SliderFloatRow("基准FOV", &baseFOV_f, 10.0f, 180.0f, "%.1f");
            }

            // 有改动时写回配置结构体并标记脏
            if (changed)
            {
                modifiable.sens = static_cast<double>(sens_f);
                modifiable.yaw = static_cast<double>(yaw_f);

                modifiable.pitch = static_cast<double>(pitch_f);

                modifiable.baseFOV = static_cast<double>(baseFOV_f);

                OverlayConfig_MarkDirty();
            }
        }

        OverlayUI::EndSection();
    }

    // ========== Manage Profiles（管理配置文件） ==========
    // 支持添加新配置文件、删除已有配置
    if (shouldDrawMousePage(page, MouseSettingsPage::Profiles) &&
        OverlayUI::BeginSection("管理配置文件", "mouse_section_manage_profiles"))
    {
        // 添加新配置文件的输入框 + 按钮
        static char new_profile_name[64] = "";
        bool addProfile = false;
        {
            const auto row = OverlayUI::BeginSettingRow("新配置文件名");
            const float buttonW = 96.0f;
            const float inputW = std::max(1.0f, row.controlWidth - buttonW - ImGui::GetStyle().ItemSpacing.x);
            ImGui::SetNextItemWidth(inputW);
            ImGui::InputText("##value", new_profile_name, sizeof(new_profile_name));
            ImGui::SameLine();
            addProfile = ImGui::Button("添加", ImVec2(buttonW, 0.0f));
            OverlayUI::EndSettingRow(row);
        }
        // 添加逻辑：名称非空且不重复时创建默认配置文件
        if (addProfile)
        {
            std::string name = std::string(new_profile_name);
            if (!name.empty() && config.game_profiles.count(name) == 0)
            {
                Config::GameProfile gp;
                gp.name = name;
                gp.sens = 1.0;
                gp.yaw = 0.022;
                gp.pitch = 0.022;
                gp.fovScaled = false;
                gp.baseFOV = 90.0;
                config.game_profiles[name] = gp;
                config.active_game = name;
                OverlayConfig_MarkDirty();
                new_profile_name[0] = '\0'; // 清空输入框
            }
        }

        // 删除当前配置文件（"UNIFIED" 不可删除）
        const auto& gp = config.currentProfile();
        if (gp.name != "UNIFIED")
        {
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(200, 50, 50, 255));
            if (OverlayUI::ButtonRow("Profile", "删除当前配置", "delete_current_profile"))
            {
                config.game_profiles.erase(gp.name);
                // 删除后回退到 UNIFIED 或首个可用配置
                if (config.game_profiles.count("UNIFIED") != 0)
                    config.active_game = "UNIFIED";
                else if (!config.game_profiles.empty())
                    config.active_game = config.game_profiles.begin()->first;
                else
                    config.active_game = "UNIFIED";

                OverlayConfig_MarkDirty();
            }
            ImGui::PopStyleColor();
        }

        OverlayUI::EndSection();
    }

    // ================================================================
    // 辅助页：子页签切换（射击 / 战术）
    // ================================================================
    if (shouldDrawMousePage(page, MouseSettingsPage::Assist))
    {
        static AssistSubPage assistTab = AssistSubPage::Shooting;

        ImGui::Spacing();
        ImGui::Indent(4.0f);
        // 子页签按钮行 —— 类似浏览器 tab
        {
            bool isShooting = (assistTab == AssistSubPage::Shooting);
            bool isTactical = (assistTab == AssistSubPage::Tactical);

            ImGui::PushStyleColor(ImGuiCol_Button,        isShooting ? ImVec4(0.22f, 0.30f, 0.22f, 1.0f) : ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.28f, 0.36f, 0.28f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.18f, 0.24f, 0.18f, 1.0f));
            if (ImGui::SmallButton("射击"))
                assistTab = AssistSubPage::Shooting;
            ImGui::PopStyleColor(3);

            ImGui::SameLine(0.0f, 2.0f);

            ImGui::PushStyleColor(ImGuiCol_Button,        isTactical ? ImVec4(0.22f, 0.30f, 0.22f, 1.0f) : ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.28f, 0.36f, 0.28f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.18f, 0.24f, 0.18f, 1.0f));
            if (ImGui::SmallButton("战术"))
                assistTab = AssistSubPage::Tactical;
            ImGui::PopStyleColor(3);
        }
        ImGui::Unindent(4.0f);
        ImGui::Spacing();

        // ── 射击子页：Auto Shoot + 开火拟人化 ──
        bool showShooting = (assistTab == AssistSubPage::Shooting);

        if (showShooting && OverlayUI::BeginSection("自动射击", "mouse_section_auto_shoot"))
        {
            OverlayUI::CheckboxRow("自动射击", &config.auto_shoot);
            if (!config.auto_shoot) ImGui::BeginDisabled();
            OverlayUI::SliderFloatRow("机瞄倍率", &config.bScope_multiplier, 0.5f, 2.0f, "%.1f");
            if (!config.auto_shoot) { ImGui::EndDisabled(); ImGui::TextDisabled("启用自动射击后才能编辑设置。"); }
            OverlayUI::EndSection();
        }

        if (showShooting && OverlayUI::BeginSection("开火拟人化", "mouse_section_trigger_humanize"))
        {
            if (!config.auto_shoot) ImGui::BeginDisabled();
            OverlayUI::SliderIntRow("确认帧数", &config.trigger_stable_frames, 0, 10);
            OverlayUI::SliderFloatRow("反应延迟(ms)", &config.trigger_random_delay_ms, 0.0f, 200.0f, "%.0f");
            OverlayUI::SliderFloatRow("延迟抖动(ms)", &config.trigger_delay_jitter_ms, 0.0f, 80.0f, "%.0f");
            OverlayUI::SliderFloatRow("按下时长(ms)", &config.trigger_hold_ms, 1.0f, 100.0f, "%.0f");
            OverlayUI::SliderFloatRow("时长抖动(ms)", &config.trigger_hold_jitter_ms, 0.0f, 50.0f, "%.0f");
            OverlayUI::SliderFloatRow("冷却间隔(ms)", &config.trigger_shot_cooldown_ms, 0.0f, 300.0f, "%.0f");
            if (!config.auto_shoot) ImGui::EndDisabled();
            OverlayUI::EndSection();
        }

        // ── 战术子页：简易压枪 + 自动急停 + 解锁Y + 射击修正（均独立于 auto_shoot）──
        bool showTactical = (assistTab == AssistSubPage::Tactical);

        if (showTactical && OverlayUI::BeginSection("简易压枪", "mouse_section_easy_no_recoil"))
        {
            if (OverlayUI::CheckboxRow("简易压枪", &config.easynorecoil)) OverlayConfig_MarkDirty();
            if (!config.easynorecoil) ImGui::BeginDisabled();
            if (OverlayUI::SliderFloatRow("压枪强度", &config.easynorecoilstrength, 0.1f, 500.0f, "%.1f")) OverlayConfig_MarkDirty();
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "左右方向键：以10为单位调节压枪强度");
            if (config.easynorecoilstrength >= 100.0f)
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "警告：高强度压枪可能被检测。");
            if (!config.easynorecoil) { ImGui::EndDisabled(); ImGui::TextDisabled("启用简易压枪后才能编辑设置。"); }
            OverlayUI::EndSection();
        }

        if (showTactical && OverlayUI::BeginSection("自动急停 (KMBOX)", "mouse_section_auto_stop"))
        {
            OverlayUI::CheckboxRow("启用自动急停", &config.auto_stop_enabled);
            OverlayUI::SliderFloatRow("急停保持(ms)", &config.auto_stop_hold_ms, 20.0f, 200.0f, "%.0f");
            OverlayUI::EndSection();
        }

        if (showTactical && OverlayUI::BeginSection("开火解锁Y轴", "mouse_section_unlock_y"))
        {
            OverlayUI::CheckboxRow("启用解锁Y轴", &config.unlock_y_enabled);
            OverlayUI::SliderFloatRow("解锁阈值(ms)", &config.unlock_y_threshold_ms, 0.0f, 500.0f, "%.0f");
            OverlayUI::SliderFloatRow("解锁强度", &config.unlock_y_strength, 0.0f, 1.0f, "%.2f");
            OverlayUI::EndSection();
        }

        if (showTactical && OverlayUI::BeginSection("射击修正", "mouse_section_fire_correction"))
        {
            OverlayUI::SliderFloatRow("修正强度", &config.fire_correction_strength, 0.0f, 3.0f, "%.2f");
            OverlayUI::EndSection();
        }
    }

    // ========== 参数预设（一键调参，仅配置页和全部页可见） ==========
    if (shouldDrawMousePage(page, MouseSettingsPage::Profiles))
    {
        if (OverlayUI::BeginSection("参数预设", "mouse_section_presets"))
    {
        const char* presetItems[] = { "自定义", "稳定", "均衡", "激进", "快速" };
        const char* presetKeys[]  = { "custom", "stable", "balanced", "aggressive", "fast" };
        int curIdx = 0;
        for (int i = 0; i < 5; ++i)
            if (config.preset_style == presetKeys[i]) { curIdx = i; break; }

        if (OverlayUI::ComboRow("预设风格", &curIdx, presetItems, 5))
        {
            config.preset_style = presetKeys[curIdx];
            MouseThread::applyPreset(presetKeys[curIdx]);
            OverlayConfig_MarkDirty();
        }

        OverlayUI::EndSection();
        }
    }

    // ========== 轨迹模拟设置 ==========
    // 模拟人手移动鼠标的自然轨迹，使鼠标运动更真实
    if (shouldDrawMousePage(page, MouseSettingsPage::Trajectory) &&
        OverlayUI::BeginSection("轨迹模拟", "mouse_section_wind_mouse"))
    {
        // 启用/禁用轨迹模拟算法
        if (OverlayUI::CheckboxRow("启用轨迹模拟", &config.wind_mouse_enabled))
        {
            OverlayConfig_MarkDirty();
        }

        if (!config.wind_mouse_enabled)
        {
            ImGui::BeginDisabled();
        }

        // G - 鼠标移速（越大移向目标越快越直）
        if (OverlayUI::SliderFloatRow("鼠标移速（越大移向目标越快越直）", &config.wind_G, 4.00f, 40.00f, "%.2f"))
        {
            OverlayConfig_MarkDirty();
        }

        // W - 轨迹摆动（越小移动路径越直）
        if (OverlayUI::SliderFloatRow("轨迹摆动（越小移动路径越直）", &config.wind_W, 1.00f, 40.00f, "%.2f"))
        {
            OverlayConfig_MarkDirty();
        }

        // M - 单步上限（每帧最大移动像素数）
        if (OverlayUI::SliderFloatRow("单步上限（每帧最大移动像素数）", &config.wind_M, 1.00f, 40.00f, "%.2f"))
        {
            OverlayConfig_MarkDirty();
        }

        // D - 微调距离（靠近目标后切精细微调模式的距离）
        if (OverlayUI::SliderFloatRow("微调距离（靠近目标后切精细模式的距离）", &config.wind_D, 1.00f, 40.00f, "%.2f"))
        {
            OverlayConfig_MarkDirty();
        }

        // 一键恢复轨迹模拟默认参数
        if (OverlayUI::ButtonRow("轨迹模拟", "恢复默认", "reset_wind_mouse_defaults"))
        {
            config.wind_G = 18.0f;
            config.wind_W = 15.0f;
            config.wind_M = 10.0f;
            config.wind_D = 8.0f;
            OverlayConfig_MarkDirty();
        }

        if (!config.wind_mouse_enabled)
        {
            ImGui::EndDisabled();
            ImGui::TextDisabled("启用轨迹模拟后才能编辑设置。");
        }

        OverlayUI::EndSection();
    }

    // ========== Bezier + EMA 设置 ==========
    if (shouldDrawMousePage(page, MouseSettingsPage::Trajectory) &&
        OverlayUI::BeginSection("Bezier / EMA", "mouse_section_bezier_ema"))
    {
        if (OverlayUI::CheckboxRow("启用 Bezier", &config.bezier_enabled))
            OverlayConfig_MarkDirty();
        OverlayUI::SliderFloatRow("Bezier 弧度", &config.bezier_strength, 0.0f, 1.0f, "%.2f");

        ImGui::Spacing();
        if (OverlayUI::CheckboxRow("启用 EMA 平滑", &config.move_ema_enabled))
            OverlayConfig_MarkDirty();
        OverlayUI::SliderFloatRow("EMA 平滑系数", &config.move_ema_alpha, 0.1f, 1.0f, "%.2f");

        OverlayUI::EndSection();
    }

    // ========== Input Method（输入法）设置 ==========
    // 支持多种鼠标输入方式：WIN32 / GHUB / RAZER / ARDUINO / RP2350 /
    // TEENSY41 / TEENSY41_HID / KMBOX_NET / KMBOX_A / MAKCU
    if (shouldDrawMousePage(page, MouseSettingsPage::Input) &&
        OverlayUI::BeginSection("输入方式", "mouse_section_input_method"))
    {
        std::vector<std::string> input_methods = { "WIN32", "GHUB", "RAZER", "ARDUINO", "RP2350", "TEENSY41", "TEENSY41_HID", "KMBOX_NET", "KMBOX_A", "MAKCU" };

        std::vector<const char*> method_items;
        method_items.reserve(input_methods.size());
        for (const auto& item : input_methods)
        {
            method_items.push_back(item.c_str());
        }

        // 查找当前输入法在列表中的索引
        int input_method_index = 0;
        for (size_t i = 0; i < input_methods.size(); ++i)
        {
            if (input_methods[i] == config.input_method)
            {
                input_method_index = static_cast<int>(i);
                break;
            }
        }

        // 输入法切换下拉框
        if (OverlayUI::ComboRow("鼠标输入方式", &input_method_index, method_items.data(), static_cast<int>(method_items.size())))
        {
            std::string new_input_method = input_methods[input_method_index];

            if (new_input_method != config.input_method)
            {
                config.input_method = new_input_method;
                OverlayConfig_MarkDirty();
                input_method_changed.store(true);
            }
        }

        // ===== ARDUINO / TEENSY41（串口方式）=====
        if (config.input_method == "ARDUINO" || config.input_method == "TEENSY41")
        {
            // 显示串口连接状态
            if (arduinoSerial)
            {
                if (arduinoSerial->isOpen())
                {
                    ImGui::TextColored(ImVec4(0, 255, 0, 255), config.input_method == "TEENSY41" ? "Teensy 4.1 已连接" : "Arduino 已连接");
                }
                else
                {
                    ImGui::TextColored(ImVec4(255, 0, 0, 255), config.input_method == "TEENSY41" ? "Teensy 4.1 未连接" : "Arduino 未连接");
                }
            }

            // 串口号 COM1 ~ COM30 选择
            std::vector<std::string> port_list;
            for (int i = 1; i <= 30; ++i)
            {
                port_list.push_back("COM" + std::to_string(i));
            }

            std::vector<const char*> port_items;
            port_items.reserve(port_list.size());
            for (const auto& port : port_list)
            {
                port_items.push_back(port.c_str());
            }

            int port_index = 0;
            for (size_t i = 0; i < port_list.size(); ++i)
            {
                if (port_list[i] == config.arduino_port)
                {
                    port_index = static_cast<int>(i);
                    break;
                }
            }

            if (OverlayUI::ComboRow(config.input_method == "TEENSY41" ? "Teensy端口" : "Arduino端口", &port_index, port_items.data(), static_cast<int>(port_items.size())))
            {
                config.arduino_port = port_list[port_index];
                OverlayConfig_MarkDirty();
                input_method_changed.store(true);
            }

            // 波特率选择（9600 ~ 115200）
            std::vector<int> baud_rate_list = { 9600, 19200, 38400, 57600, 115200 };
            std::vector<std::string> baud_rate_str_list;
            for (const auto& rate : baud_rate_list)
            {
                baud_rate_str_list.push_back(std::to_string(rate));
            }

            std::vector<const char*> baud_rate_items;
            baud_rate_items.reserve(baud_rate_str_list.size());
            for (const auto& rate_str : baud_rate_str_list)
            {
                baud_rate_items.push_back(rate_str.c_str());
            }

            int baud_rate_index = 0;
            for (size_t i = 0; i < baud_rate_list.size(); ++i)
            {
                if (baud_rate_list[i] == config.arduino_baudrate)
                {
                    baud_rate_index = static_cast<int>(i);
                    break;
                }
            }

            if (OverlayUI::ComboRow(config.input_method == "TEENSY41" ? "Teensy波特率" : "Arduino波特率", &baud_rate_index, baud_rate_items.data(), static_cast<int>(baud_rate_items.size())))
            {
                config.arduino_baudrate = baud_rate_list[baud_rate_index];
                OverlayConfig_MarkDirty();
                input_method_changed.store(true);
            }

            // Teensy41 串口模式和 Arduino 模式有不同的额外选项
            if (config.input_method == "TEENSY41")
            {
                ImGui::TextDisabled("使用 Teensy 4.1 串口鼠标桥协议。");
            }
            else
            {
                // Arduino 专用：16位移位模式、按键传递
                if (OverlayUI::CheckboxRow("Arduino 16位鼠标", &config.arduino_16_bit_mouse))
                {
                    OverlayConfig_MarkDirty();
                    input_method_changed.store(true);
                }
                if (OverlayUI::CheckboxRow("Arduino 启用按键", &config.arduino_enable_keys))
                {
                    OverlayConfig_MarkDirty();
                    input_method_changed.store(true);
                }
            }
        }
        // ===== RP2350（树莓派 Pico 串口）=====
        else if (config.input_method == "RP2350")
        {
            if (rp2350Serial)
            {
                if (rp2350Serial->isOpen())
                {
                    ImGui::TextColored(ImVec4(0, 255, 0, 255), "RP2350 已连接");
                }
                else
                {
                    ImGui::TextColored(ImVec4(255, 0, 0, 255), "RP2350 未连接");
                }
            }

            // COMn 端口选择
            std::vector<std::string> port_list;
            for (int i = 1; i <= 30; ++i)
            {
                port_list.push_back("COM" + std::to_string(i));
            }

            std::vector<const char*> port_items;
            port_items.reserve(port_list.size());
            for (const auto& port : port_list)
            {
                port_items.push_back(port.c_str());
            }

            int port_index = 0;
            for (size_t i = 0; i < port_list.size(); ++i)
            {
                if (port_list[i] == config.rp2350_port)
                {
                    port_index = static_cast<int>(i);
                    break;
                }
            }

            if (OverlayUI::ComboRow("RP2350端口", &port_index, port_items.data(), static_cast<int>(port_items.size())))
            {
                config.rp2350_port = port_list[port_index];
                OverlayConfig_MarkDirty();
                input_method_changed.store(true);
            }

            // RP2350 支持更高的波特率（最高 921600）
            std::vector<int> baud_rate_list = { 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600 };
            std::vector<std::string> baud_rate_str_list;
            for (const auto& rate : baud_rate_list)
            {
                baud_rate_str_list.push_back(std::to_string(rate));
            }

            std::vector<const char*> baud_rate_items;
            baud_rate_items.reserve(baud_rate_str_list.size());
            for (const auto& rate_str : baud_rate_str_list)
            {
                baud_rate_items.push_back(rate_str.c_str());
            }

            int baud_rate_index = 0;
            for (size_t i = 0; i < baud_rate_list.size(); ++i)
            {
                if (baud_rate_list[i] == config.rp2350_baudrate)
                {
                    baud_rate_index = static_cast<int>(i);
                    break;
                }
            }

            if (OverlayUI::ComboRow("RP2350波特率", &baud_rate_index, baud_rate_items.data(), static_cast<int>(baud_rate_items.size())))
            {
                config.rp2350_baudrate = baud_rate_list[baud_rate_index];
                OverlayConfig_MarkDirty();
                input_method_changed.store(true);
            }

            if (OverlayUI::CheckboxRow("RP2350 16位鼠标", &config.rp2350_16_bit_mouse))
            {
                OverlayConfig_MarkDirty();
                input_method_changed.store(true);
            }
            if (OverlayUI::CheckboxRow("RP2350 启用按键", &config.rp2350_enable_keys))
            {
                OverlayConfig_MarkDirty();
                input_method_changed.store(true);
            }
        }
        // ===== GHUB（罗技 G Hub 驱动注入）=====
        else if (config.input_method == "GHUB")
        {
            // 检查 GHUB 版本是否匹配
            if (ghub_version == "13.1.4")
            {
                std::string ghub_version_label = "已安装正确版本的GHUB: " + ghub_version;
                ImGui::Text(ghub_version_label.c_str());
            }
            else
            {
                ImGui::Text("安装的GHUB版本不正确或路径未设置。\n默认系统路径：C:\\Program Files\\LGHUB");
                if (OverlayUI::ButtonRow("GHub", "打开GHUB文档", "ghub_docs"))
                {
                    ShellExecute(0, 0, L"https://github.com/hpsazz1/Xen/blob/main/docs/guides.md#g-hub-input-method", 0, 0, SW_SHOW);
                }
            }

            ImGui::TextColored(ImVec4(255, 0, 0, 255), "风险自负，该方法在某些游戏中可能被检测。");
        }
        // ===== TEENSY41_HID（Teensy 4.1 RawHID 模式）=====
        else if (config.input_method == "TEENSY41_HID")
        {
            bool teensy41Connected = false;
            {
                std::lock_guard<std::mutex> lock(inputDevicesMutex);
                teensy41Connected = activeMouseInputOwner && activeMouseInputOwner->isOpen();
            }

            if (teensy41Connected)
            {
                ImGui::TextColored(ImVec4(0, 255, 0, 255), "Teensy 4.1 RawHID 已连接");
            }
            else
            {
                ImGui::TextColored(ImVec4(255, 0, 0, 255), "Teensy 4.1 RawHID 未连接");
            }

            // 本地静态缓存，非持久化；用于编辑后点击"Save & Reconnect"才写入配置
            static char serial[64] = "";
            static char vid[16] = "";
            static char pid[16] = "";
            static std::string last_serial;
            static std::string last_vid;
            static std::string last_pid;
            static int usage_page = 0;
            static int usage_id = 0;
            static int open_index = 0;
            static int timeout_ms = 0;
            static int reconnect_ms = 0;

            // 当配置持久化值与静态缓存不一致时重新同步
            if (last_serial != config.teensy_hid_serial ||
                last_vid != config.teensy_hid_vid_filter ||
                last_pid != config.teensy_hid_pid_filter ||
                usage_page != config.teensy_hid_usage_page ||
                usage_id != config.teensy_hid_usage_id ||
                open_index != config.teensy_hid_open_index ||
                timeout_ms != config.teensy_hid_packet_timeout_ms ||
                reconnect_ms != config.teensy_hid_reconnect_interval_ms)
            {
                strncpy(serial, config.teensy_hid_serial.c_str(), sizeof(serial));
                strncpy(vid, config.teensy_hid_vid_filter.c_str(), sizeof(vid));
                strncpy(pid, config.teensy_hid_pid_filter.c_str(), sizeof(pid));
                serial[sizeof(serial) - 1] = '\0';
                vid[sizeof(vid) - 1] = '\0';
                pid[sizeof(pid) - 1] = '\0';
                last_serial = config.teensy_hid_serial;
                last_vid = config.teensy_hid_vid_filter;
                last_pid = config.teensy_hid_pid_filter;
                usage_page = config.teensy_hid_usage_page;
                usage_id = config.teensy_hid_usage_id;
                open_index = config.teensy_hid_open_index;
                timeout_ms = config.teensy_hid_packet_timeout_ms;
                reconnect_ms = config.teensy_hid_reconnect_interval_ms;
            }

            // HID 参数编辑：序列号、VID/PID 过滤、Usage Page/ID、打开索引、超时与重连
            OverlayUI::InputTextRow("序列号", serial, sizeof(serial));
            OverlayUI::InputTextRow("VID过滤", vid, sizeof(vid));
            OverlayUI::InputTextRow("PID过滤", pid, sizeof(pid));
            OverlayUI::InputIntRow("用途页面", &usage_page);
            OverlayUI::InputIntRow("用途ID", &usage_id);
            OverlayUI::InputIntRow("打开索引", &open_index);
            OverlayUI::InputIntRow("数据包超时(毫秒)", &timeout_ms);
            OverlayUI::InputIntRow("重连间隔(毫秒)", &reconnect_ms);

            // 保存并重连按钮
            if (OverlayUI::ButtonRow("Teensy HID", "保存并重连", "teensy_hid_save_reconnect"))
            {
                config.teensy_hid_serial = serial;
                config.teensy_hid_vid_filter = vid;
                config.teensy_hid_pid_filter = pid;
                config.teensy_hid_usage_page = usage_page;
                config.teensy_hid_usage_id = usage_id;
                config.teensy_hid_open_index = open_index;
                config.teensy_hid_packet_timeout_ms = timeout_ms;
                config.teensy_hid_reconnect_interval_ms = reconnect_ms;
                last_serial = config.teensy_hid_serial;
                last_vid = config.teensy_hid_vid_filter;
                last_pid = config.teensy_hid_pid_filter;
                OverlayConfig_MarkDirty();
                input_method_changed.store(true);
            }
        }
        // ===== RAZER（雷蛇 rzctl 驱动注入）=====
        else if (config.input_method == "RAZER")
        {
            if (razerControl && razerControl->isOpen())
            {
                ImGui::TextColored(ImVec4(0, 255, 0, 255), "雷蛇 rzctl 已连接");
            }
            else
            {
                ImGui::TextColored(ImVec4(255, 0, 0, 255), "雷蛇 rzctl 未连接");
            }
            ImGui::Text("需要 rzctl.dll 与 Xen.exe 放在同一目录。");
            ImGui::TextColored(ImVec4(255, 0, 0, 255), "风险自负，该方法在某些游戏中可能被检测。");
        }
        // ===== WIN32（标准 Windows API 鼠标输入）=====
        else if (config.input_method == "WIN32")
        {
            ImGui::TextColored(ImVec4(255, 255, 255, 255), "这是标准鼠标输入方式，可能不适用于大多数游戏。请使用 GHUB、RAZER、ARDUINO、RP2350、TEENSY41 或 TEENSY41_HID。");
            ImGui::TextColored(ImVec4(255, 0, 0, 255), "风险自负，该方法在某些游戏中可能被检测。");
        }
        // ===== KMBOX_NET（网络版 KMBOX）=====
        else if (config.input_method == "KMBOX_NET")
        {
            static char ip[32] = "";
            static char port[8] = "";
            static char uuid[16] = "";
            static std::string last_ip;
            static std::string last_port;
            static std::string last_uuid;

            // 配置变更时同步到本地静态缓存
            if (last_ip != config.kmbox_net_ip || last_port != config.kmbox_net_port || last_uuid != config.kmbox_net_uuid)
            {
                strncpy(ip, config.kmbox_net_ip.c_str(), sizeof(ip));
                strncpy(port, config.kmbox_net_port.c_str(), sizeof(port));
                strncpy(uuid, config.kmbox_net_uuid.c_str(), sizeof(uuid));
                ip[sizeof(ip) - 1] = '\0';
                port[sizeof(port) - 1] = '\0';
                uuid[sizeof(uuid) - 1] = '\0';
                last_ip = config.kmbox_net_ip;
                last_port = config.kmbox_net_port;
                last_uuid = config.kmbox_net_uuid;
            }

            // IP、端口、UUID 编辑输入框
            OverlayUI::InputTextRow("IP地址", ip, sizeof(ip));
            OverlayUI::InputTextRow("端口", port, sizeof(port));
            OverlayUI::InputTextRow("UUID", uuid, sizeof(uuid));

            // 保存连接参数并断开重连
            if (OverlayUI::ButtonRow("kmboxNet", "保存并重连", "kmbox_net_save_reconnect"))
            {
                config.kmbox_net_ip = ip;
                config.kmbox_net_port = port;
                config.kmbox_net_uuid = uuid;
                last_ip = config.kmbox_net_ip;
                last_port = config.kmbox_net_port;
                last_uuid = config.kmbox_net_uuid;
                OverlayConfig_MarkDirty();
                input_method_changed.store(true);
            }

            bool kmboxNetConnected = false;
            {
                std::lock_guard<std::mutex> lock(inputDevicesMutex);
                KmboxNetConnection* device =
                    activeMouseInputOwner && std::string(activeMouseInputOwner->name()) == "KMBOX_NET"
                    ? activeMouseInputOwner->kmboxNet()
                    : nullptr;
                kmboxNetConnected = device && device->isOpen();
            }

            if (kmboxNetConnected)
            {
                ImGui::TextColored(ImVec4(0, 255, 0, 255), "kmboxNet 已连接");
            }
            else
            {
                ImGui::TextColored(ImVec4(255, 0, 0, 255), "kmboxNet 未连接");
            }

            // 以下操作仅在已连接时可用
            if (!kmboxNetConnected)
                ImGui::BeginDisabled();

            // 重启 KMBOX 盒子
            if (OverlayUI::ButtonRow("kmboxNet box", "重启盒子", "kmbox_net_reboot"))
            {
                std::lock_guard<std::mutex> lock(inputDevicesMutex);
                KmboxNetConnection* device =
                    activeMouseInputOwner && std::string(activeMouseInputOwner->name()) == "KMBOX_NET"
                    ? activeMouseInputOwner->kmboxNet()
                    : nullptr;
                if (device && device->isOpen())
                    device->reboot();
            }

            // 向 KMBOX 屏幕发送图片
            if (OverlayUI::ButtonRow("kmboxNet image", "更换图片", "kmbox_net_image"))
            {
                std::lock_guard<std::mutex> lock(inputDevicesMutex);
                KmboxNetConnection* device =
                    activeMouseInputOwner && std::string(activeMouseInputOwner->name()) == "KMBOX_NET"
                    ? activeMouseInputOwner->kmboxNet()
                    : nullptr;
                if (device && device->isOpen())
                {
                    device->lcdColor(0);
                    device->lcdPicture(gImage_128x160);
                }
            }

            if (!kmboxNetConnected)
                ImGui::EndDisabled();
        }
        // ===== KMBOX_A（USB 版 KMBOX）=====
        else if (config.input_method == "KMBOX_A")
        {
            static char pidvid[32] = "";
            static std::string last_pidvid;

            if (last_pidvid != config.kmbox_a_pidvid)
            {
                strncpy(pidvid, config.kmbox_a_pidvid.c_str(), sizeof(pidvid));
                pidvid[sizeof(pidvid) - 1] = '\0';
                last_pidvid = config.kmbox_a_pidvid;
            }

            // PID:VID 格式输入（PPPVVVV）
            OverlayUI::InputTextRow("PIDVID", pidvid, sizeof(pidvid));
            ImGui::TextDisabled("格式：PPPVVVV（单个字段）");

            if (OverlayUI::ButtonRow("kmboxA", "保存并重连", "kmbox_a_save_reconnect"))
            {
                config.kmbox_a_pidvid = pidvid;
                last_pidvid = config.kmbox_a_pidvid;
                OverlayConfig_MarkDirty();
                input_method_changed.store(true);
            }

            if (kmboxASerial && kmboxASerial->isOpen())
            {
                ImGui::TextColored(ImVec4(0, 255, 0, 255), "kmboxA 已连接");
            }
            else
            {
                ImGui::TextColored(ImVec4(255, 0, 0, 255), "kmboxA 未连接");
            }
        }
        // ===== MAKCU（串口鼠标桥）=====
        else if (config.input_method == "MAKCU")
        {
            std::vector<std::string> port_list;
            for (int i = 1; i <= 30; ++i)
            {
                port_list.push_back("COM" + std::to_string(i));
            }

            std::vector<const char*> port_items;
            port_items.reserve(port_list.size());
            for (const auto& port : port_list)
            {
                port_items.push_back(port.c_str());
            }

            int port_index = 0;
            for (size_t i = 0; i < port_list.size(); ++i)
            {
                if (port_list[i] == config.makcu_port)
                {
                    port_index = static_cast<int>(i);
                    break;
                }
            }

            if (OverlayUI::ComboRow("Makcu端口", &port_index, port_items.data(), static_cast<int>(port_items.size())))
            {
                config.makcu_port = port_list[port_index];
                OverlayConfig_MarkDirty();
                input_method_changed.store(true);
            }

            std::vector<int> baud_list = { 9600, 19200, 38400, 57600, 115200 };
            std::vector<std::string> baud_str_list;
            for (int b : baud_list) baud_str_list.push_back(std::to_string(b));

            std::vector<const char*> baud_items;
            baud_items.reserve(baud_list.size());
            for (const auto& baud : baud_str_list)
            {
                baud_items.push_back(baud.c_str());
            }

            int baud_index = 0;
            for (size_t i = 0; i < baud_list.size(); ++i)
            {
                if (baud_list[i] == config.makcu_baudrate)
                {
                    baud_index = static_cast<int>(i);
                    break;
                }
            }

            if (OverlayUI::ComboRow("Makcu波特率", &baud_index, baud_items.data(), static_cast<int>(baud_items.size())))
            {
                config.makcu_baudrate = baud_list[baud_index];
                OverlayConfig_MarkDirty();
                input_method_changed.store(true);
            }

            if (makcuSerial && makcuSerial->isOpen())
            {
                ImGui::TextColored(ImVec4(0, 255, 0, 255), "Makcu 已连接");
            }
            else
            {
                ImGui::TextColored(ImVec4(255, 0, 0, 255), "Makcu 未连接");
            }
        }

        OverlayUI::EndSection();
    }

    // ===== 脏检测块 1：FOV / 速度倍率 / 预测 / 卡尔曼 / 目标修正 =====
    // 检测上述配置项是否有变化，如有则同步 prev 变量、更新 mouseThread 并标记配置脏
    if (prev_fovX != config.fovX ||
        prev_fovY != config.fovY ||
        prev_minSpeedMultiplier != config.minSpeedMultiplier ||
        prev_maxSpeedMultiplier != config.maxSpeedMultiplier ||
        prev_predictionInterval != config.predictionInterval ||
        prev_kalman_enabled != config.kalman_enabled ||
        prev_kalman_process_noise_position != config.kalman_process_noise_position ||
        prev_kalman_process_noise_velocity != config.kalman_process_noise_velocity ||
        prev_kalman_measurement_noise != config.kalman_measurement_noise ||
        prev_kalman_velocity_damping != config.kalman_velocity_damping ||
        prev_kalman_max_velocity != config.kalman_max_velocity ||
        prev_kalman_warmup_frames != config.kalman_warmup_frames ||
        prev_kalman_compensate_detection_delay != config.kalman_compensate_detection_delay ||
        prev_kalman_additional_prediction_ms != config.kalman_additional_prediction_ms ||
        prev_kalman_reset_timeout_sec != config.kalman_reset_timeout_sec ||
        prev_snapRadius != config.snapRadius ||
        prev_nearRadius != config.nearRadius ||
        prev_speedCurveExponent != config.speedCurveExponent ||
        prev_snapBoostFactor != config.snapBoostFactor)
    {
        // 同步 FOV
        prev_fovX = config.fovX;
        prev_fovY = config.fovY;
        // 同步速度倍率
        prev_minSpeedMultiplier = config.minSpeedMultiplier;
        prev_maxSpeedMultiplier = config.maxSpeedMultiplier;
        // 同步预测
        prev_predictionInterval = config.predictionInterval;
        // 同步卡尔曼参数
        prev_kalman_enabled = config.kalman_enabled;
        prev_kalman_process_noise_position = config.kalman_process_noise_position;
        prev_kalman_process_noise_velocity = config.kalman_process_noise_velocity;
        prev_kalman_measurement_noise = config.kalman_measurement_noise;
        prev_kalman_velocity_damping = config.kalman_velocity_damping;
        prev_kalman_max_velocity = config.kalman_max_velocity;
        prev_kalman_warmup_frames = config.kalman_warmup_frames;
        prev_kalman_compensate_detection_delay = config.kalman_compensate_detection_delay;
        prev_kalman_additional_prediction_ms = config.kalman_additional_prediction_ms;
        prev_kalman_reset_timeout_sec = config.kalman_reset_timeout_sec;
        // 同步目标修正
        prev_snapRadius = config.snapRadius;
        prev_nearRadius = config.nearRadius;
        prev_speedCurveExponent = config.speedCurveExponent;
        prev_snapBoostFactor = config.snapBoostFactor;

        // 通知 mouseThread 重新加载配置
        globalMouseThread->updateConfig(
            config.detection_resolution,
            config.fovX,
            config.fovY,
            config.minSpeedMultiplier,
            config.maxSpeedMultiplier,
            config.predictionInterval,
            config.auto_shoot,
            config.bScope_multiplier);

        OverlayConfig_MarkDirty();
    }

    // ===== 脏检测块 2：轨迹模拟 + Bezier + EMA =====
    if (prev_wind_mouse_enabled != config.wind_mouse_enabled ||
        prev_wind_G != config.wind_G ||
        prev_wind_W != config.wind_W ||
        prev_wind_M != config.wind_M ||
        prev_wind_D != config.wind_D ||
        prev_bezier_enabled != config.bezier_enabled ||
        prev_bezier_strength != config.bezier_strength ||
        prev_move_ema_enabled != config.move_ema_enabled ||
        prev_move_ema_alpha != config.move_ema_alpha)
    {
        // 同步轨迹模拟参数
        prev_wind_mouse_enabled = config.wind_mouse_enabled;
        prev_wind_G = config.wind_G;
        prev_wind_W = config.wind_W;
        prev_wind_M = config.wind_M;
        prev_wind_D = config.wind_D;
        prev_bezier_enabled = config.bezier_enabled;
        prev_bezier_strength = config.bezier_strength;
        prev_move_ema_enabled = config.move_ema_enabled;
        prev_move_ema_alpha = config.move_ema_alpha;

        globalMouseThread->updateConfig(
            config.detection_resolution,
            config.fovX,
            config.fovY,
            config.minSpeedMultiplier,
            config.maxSpeedMultiplier,
            config.predictionInterval,
            config.auto_shoot,
            config.bScope_multiplier);

        OverlayConfig_MarkDirty();
    }

    // ===== 脏检测块 3：Auto Shoot + 开火拟人 + 急停 + 解锁Y + 射击修正 =====
    if (prev_auto_shoot != config.auto_shoot ||
        prev_bScope_multiplier != config.bScope_multiplier ||
        prev_trigger_stable_frames != config.trigger_stable_frames ||
        prev_trigger_random_delay_ms != config.trigger_random_delay_ms ||
        prev_trigger_delay_jitter_ms != config.trigger_delay_jitter_ms ||
        prev_trigger_hold_ms != config.trigger_hold_ms ||
        prev_trigger_hold_jitter_ms != config.trigger_hold_jitter_ms ||
        prev_trigger_shot_cooldown_ms != config.trigger_shot_cooldown_ms ||
        prev_auto_stop_enabled != config.auto_stop_enabled ||
        prev_auto_stop_hold_ms != config.auto_stop_hold_ms ||
        prev_unlock_y_enabled != config.unlock_y_enabled ||
        prev_unlock_y_threshold_ms != config.unlock_y_threshold_ms ||
        prev_unlock_y_strength != config.unlock_y_strength ||
        prev_fire_correction_strength != config.fire_correction_strength)
    {
        prev_auto_shoot = config.auto_shoot;
        prev_bScope_multiplier = config.bScope_multiplier;
        prev_trigger_stable_frames = config.trigger_stable_frames;
        prev_trigger_random_delay_ms = config.trigger_random_delay_ms;
        prev_trigger_delay_jitter_ms = config.trigger_delay_jitter_ms;
        prev_trigger_hold_ms = config.trigger_hold_ms;
        prev_trigger_hold_jitter_ms = config.trigger_hold_jitter_ms;
        prev_trigger_shot_cooldown_ms = config.trigger_shot_cooldown_ms;
        prev_auto_stop_enabled = config.auto_stop_enabled;
        prev_auto_stop_hold_ms = config.auto_stop_hold_ms;
        prev_unlock_y_enabled = config.unlock_y_enabled;
        prev_unlock_y_threshold_ms = config.unlock_y_threshold_ms;
        prev_unlock_y_strength = config.unlock_y_strength;
        prev_fire_correction_strength = config.fire_correction_strength;

        globalMouseThread->updateConfig(
            config.detection_resolution,
            config.fovX,
            config.fovY,
            config.minSpeedMultiplier,
            config.maxSpeedMultiplier,
            config.predictionInterval,
            config.auto_shoot,
            config.bScope_multiplier);

        OverlayConfig_MarkDirty();
    }
}

// ===== 公开包装函数 =====
// 以下函数供外部调用，每个函数对应一个子页面类别

// 绘制所有鼠标设置页面
void draw_mouse()
{
    draw_mouse_page(MouseSettingsPage::All);
}

// 仅绘制移动相关设置（FOV、速度倍率、目标修正、轨迹模拟）
void draw_mouse_movement()
{
    draw_mouse_page(MouseSettingsPage::Movement);
}

// 仅绘制预测设置（预测间隔 + 卡尔曼滤波）
void draw_mouse_prediction()
{
    draw_mouse_page(MouseSettingsPage::Prediction);
}

// 仅绘制辅助设置（Easy No Recoil + Auto Shoot）
void draw_mouse_assist()
{
    draw_mouse_page(MouseSettingsPage::Assist);
}

// 仅绘制轨迹设置（Wind Mouse + Bezier + EMA）
void draw_mouse_trajectory()
{
    draw_mouse_page(MouseSettingsPage::Trajectory);
}

// 仅绘制配置文件管理（Game Profile + Manage Profiles）
void draw_mouse_profiles()
{
    draw_mouse_page(MouseSettingsPage::Profiles);
}

// 仅绘制输入法设置（各类硬件/驱动输入方式）
void draw_mouse_input()
{
    draw_mouse_page(MouseSettingsPage::Input);
}
