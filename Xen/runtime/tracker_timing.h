#pragma once

#include <algorithm>

/**
 * @brief 计算检测发布停顿多久后才完整清除跟踪状态。
 *
 * 跟踪器每收到一次检测结果发布才推进一步，因此过期时钟必须使用检测发布FPS，
 * 不能使用更快的捕获处理FPS。正常空检测结果仍会递增发布版本并驱动SOT滑行/
 * 掉锁；本超时只处理推理线程完全停止发布的情况，等待期间不会产生新瞄准命令。
 * 最短100 ms与基础预测器允许的最大真实观测间隔一致，低发布率时保留四个周期，
 * 避免一次正常调度抖动清空已经成熟的方向和积分状态。
 *
 * @param inferencePublishFps 检测结果实际发布FPS；未知时按60 FPS回退
 * @return 无检测发布时的完整状态清除门槛，范围100~250 ms
 */
inline int trackerStaleTimeoutMs(int inferencePublishFps)
{
    const int fps = std::clamp(
        inferencePublishFps > 0 ? inferencePublishFps : 60, 15, 500);
    return std::clamp(4000 / fps, 100, 250);
}
