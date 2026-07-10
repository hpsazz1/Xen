#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <atomic>
#include <memory>

/** @brief 覆盖层矩形结构 */
struct OverlayRect { float x, y, w, h; };
/** @brief 覆盖层线段结构 */
struct OverlayLine { float x1, y1, x2, y2; };
/** @brief 覆盖层圆形结构 */
struct OverlayCircle { float cx, cy, r; };

/** @brief 颜色类型（ARGB 格式：0xAARRGGBB） */
using OverlayColor = uint32_t;

/** @brief 从 ARGB 分量构造颜色值 */
inline constexpr OverlayColor ARGB(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return (OverlayColor(a) << 24) | (OverlayColor(r) << 16) | (OverlayColor(g) << 8) | OverlayColor(b);
}

/**
 * @brief 游戏覆盖层类
 *
 * 负责在游戏窗口之上绘制一个透明的覆盖层（Overlay），
 * 用于显示瞄准辅助信息、调试数据、UI 元素等。
 * 支持绘制线条、矩形、圆形、文本和图像。
 */
class Game_overlay
{
public:
    /** @brief 启动覆盖层 */
    bool Start();
    /** @brief 停止覆盖层 */
    void Stop();
    /** @brief 覆盖层是否正在运行 */
    bool IsRunning() const;

    /** @brief 设置覆盖层可见性 */
    void SetVisible(bool visible);
    /** @brief 获取覆盖层可见性 */
    bool GetVisible() const;
    /** @brief 设置是否排除在屏幕捕获之外 */
    void SetExcludeFromCapture(bool exclude);
    /** @brief 获取排除捕获状态 */
    bool GetExcludeFromCapture() const;

    /** @brief 开始新帧的绘制 */
    void BeginFrame();
    /** @brief 结束当前帧的绘制 */
    void EndFrame();

    /** @brief 添加线段 */
    void AddLine(const OverlayLine& line, OverlayColor color, float thickness = 1.0f);
    /** @brief 添加矩形边框 */
    void AddRect(const OverlayRect& rc, OverlayColor color, float thickness = 1.0f);
    /** @brief 填充矩形 */
    void FillRect(const OverlayRect& rc, OverlayColor color);
    /** @brief 添加圆形边框 */
    void AddCircle(const OverlayCircle& c, OverlayColor color, float thickness = 1.0f);
    /** @brief 填充圆形 */
    void FillCircle(const OverlayCircle& c, OverlayColor color);
    /** @brief 添加文本 */
    void AddText(float x, float y, const std::wstring& text, float sizePx,
        OverlayColor color, const std::wstring& font = L"Segoe UI");

    /** @brief 从文件加载图片，返回图片 ID */
    int  LoadImageFromFile(const std::wstring& path);
    /** @brief 卸载指定 ID 的图片 */
    void UnloadImage(int imageId);
    /** @brief 绘制图片 */
    void DrawImage(int imageId, float x, float y, float w, float h, float opacity = 1.0f);
    /** @brief 从 BGRA 数据更新图片内容 */
    int  UpdateImageFromBGRA(const void* data, int width, int height, int strideBytes, int imageId = 0);

    /** @brief 使用虚拟屏幕坐标 */
    void UseVirtualScreen();
    /** @brief 设置窗口边界 */
    void SetWindowBounds(int x, int y, int w, int h);

    /** @brief 设置最大帧率 */
    void SetMaxFPS(unsigned fps);

    Game_overlay();
    ~Game_overlay();

private:
    /** @brief 禁止拷贝 */
    Game_overlay(const Game_overlay&) = delete;
    Game_overlay& operator=(const Game_overlay&) = delete;

    struct Impl;                        ///< PIMPL 实现结构
    std::unique_ptr<Impl> impl_;        ///< 实现指针
};
