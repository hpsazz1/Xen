#pragma once

/** @brief 标记覆盖层配置为"脏"状态，提示需要保存 */
void OverlayConfig_MarkDirty();
/** @brief 尝试保存覆盖层配置（如果标记为脏） */
void OverlayConfig_TrySave(const char* filename = "config.ini");
/** @brief 立即保存覆盖层配置 */
void OverlayConfig_SaveNow(const char* filename = "config.ini");
