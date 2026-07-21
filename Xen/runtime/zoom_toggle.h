#pragma once

/**
 * @brief 将物理开镜键转换为运行时开镜状态。
 *
 * CS2单倍镜使用点击切换：只在按键上升沿翻转，按住期间不得重复触发。
 * 其他Profile继续使用按住即开镜的旧语义。切换模式变化时强制回到腰射，避免
 * 把另一个游戏或Profile的锁存状态带入当前控制坐标系。
 */
class ZoomToggleState
{
public:
    bool update(bool pressed, bool toggleMode) noexcept
    {
        if (toggleMode != toggleMode_)
        {
            toggleMode_ = toggleMode;
            previousPressed_ = false;
            zoomed_ = false;
        }

        if (toggleMode_)
        {
            if (pressed && !previousPressed_)
                zoomed_ = !zoomed_;
        }
        else
        {
            zoomed_ = pressed;
        }

        previousPressed_ = pressed;
        return zoomed_;
    }

private:
    bool toggleMode_ = false;
    bool previousPressed_ = false;
    bool zoomed_ = false;
};
