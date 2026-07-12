#pragma once

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "AimbotTarget.h"

class MouseThread;
class Game_overlay;

extern std::unique_ptr<Game_overlay> gameOverlayPtr;  ///< 全局游戏覆盖层指针
extern std::thread gameOverlayThread;            ///< 覆盖层渲染线程
extern std::atomic<bool> gameOverlayShouldExit;  ///< 覆盖层线程退出标志

extern std::mutex g_trackerDebugMutex;           ///< 跟踪器调试信息互斥锁
extern std::vector<TrackDebugInfo> g_trackerDebugTracks;  ///< 跟踪器调试轨道列表
extern int g_trackerLockedId;                    ///< 当前锁定目标的跟踪 ID

/**
 * @brief 计算帧间隔时间（秒），统一 FPS→dt 转换
 *
 * 将 FPS 值钳位在 [15, 500]，返回每帧时间间隔。
 * 由 mouse.cpp 和 mouse_thread_loop.cpp 共享使用。
 *
 * @param captureFpsValue 当前捕获帧率（<=0 时回退到 60）
 * @return 每帧间隔（秒）
 */
inline double frameIntervalSec(int captureFpsValue)
{
    double fps = static_cast<double>((captureFpsValue > 0) ? captureFpsValue : 60);
    fps = std::clamp(fps, 15.0, 500.0);
    return 1.0 / fps;
}

/** @brief 鼠标控制线程主函数 */
void mouseThreadFunction(MouseThread& mouseThread);
/** @brief 游戏覆盖层渲染循环 */
void gameOverlayRenderLoop();
