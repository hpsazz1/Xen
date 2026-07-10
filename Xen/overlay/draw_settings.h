#ifndef DRAW_SETTINGS_H
#define DRAW_SETTINGS_H

/** @brief 绘制捕获设置面板 */
void draw_capture_settings();
/** @brief 绘制捕获预览窗口 */
void draw_capture_preview();
/** @brief 绘制目标信息 */
void draw_target();
/** @brief 绘制跟踪器信息 */
void draw_tracker();
/** @brief 绘制鼠标配置 */
void draw_mouse();
/** @brief 绘制鼠标移动曲线 */
void draw_mouse_movement();
/** @brief 绘制鼠标轨迹设置 */
void draw_mouse_trajectory();
/** @brief 绘制鼠标预测信息 */
void draw_mouse_prediction();
/** @brief 绘制鼠标辅助功能 */
void draw_mouse_assist();
/** @brief 绘制鼠标配置文件 */
void draw_mouse_profiles();
/** @brief 绘制鼠标输入设备选择 */
void draw_mouse_input();
/** @brief 绘制 AI 检测设置 */
void draw_ai();
/** @brief 绘制功能按钮 */
void draw_buttons();
/** @brief 绘制覆盖层设置 */
void draw_overlay();
/** @brief 绘制统计信息 */
void draw_stats();
/** @brief 绘制调试信息 */
void draw_debug();
/** @brief 绘制深度信息 */
void draw_depth();
/** @brief 加载身体纹理 */
void load_body_texture();
/** @brief 释放身体纹理 */
void release_body_texture();
/** @brief 绘制游戏覆盖层配置 */
void draw_game_overlay_settings();
/** @brief 绘制游戏覆盖层常规设置 */
void draw_game_overlay_general();
/** @brief 绘制游戏覆盖层视觉设置 */
void draw_game_overlay_visuals();
/** @brief 绘制游戏覆盖层图标设置 */
void draw_game_overlay_icon();

#endif // DRAW_SETTINGS_H
