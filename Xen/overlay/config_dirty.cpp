// config_dirty.cpp
// 配置脏标记管理模块
// 负责追踪用户界面中的配置是否被修改（标记为"脏"），
// 并在合适的时机自动将配置保存到文件。

#include "overlay/config_dirty.h"

#include "config.h"
#include "imgui/imgui.h"

extern Config config;

namespace
{
    // 配置脏标记，true 表示配置已被修改但尚未保存
    bool cfgDirty = false;
    // 配置被标记为脏时的时间戳（ImGui 时间，秒）
    double cfgDirtyAt = 0.0;
    // 自动保存延迟时间（秒），避免频繁写入磁盘
    constexpr double kSaveDelaySec = 0.35;
}

// 将配置标记为脏（已修改未保存）
// 记录当前时间戳，用于后续的延迟自动保存判断
void OverlayConfig_MarkDirty()
{
    cfgDirty = true;
    cfgDirtyAt = ImGui::GetTime();
}

// 尝试保存配置（带延迟的自动保存）
// 仅在配置被标记为脏且经过延迟时间后才执行实际保存。
// 如果当前有 ImGui 控件处于活动状态（用户正在编辑），则暂缓保存。
// filename: 保存的文件名，nullptr 则使用默认 "config.ini"
void OverlayConfig_TrySave(const char* filename)
{
    if (!cfgDirty)
        return;

    const double now = ImGui::GetTime();
    if ((now - cfgDirtyAt) < kSaveDelaySec)
        return;

    if (ImGui::IsAnyItemActive())
        return;

    config.saveConfig(filename ? filename : "config.ini");
    cfgDirty = false;
}

// 立即保存配置（忽略延迟时间）
// filename: 保存的文件名，nullptr 则使用默认 "config.ini"
void OverlayConfig_SaveNow(const char* filename)
{
    if (!cfgDirty)
        return;

    config.saveConfig(filename ? filename : "config.ini");
    cfgDirty = false;
}
