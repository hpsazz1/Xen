#pragma once

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

/** @brief 鼠标控制线程主函数 */
void mouseThreadFunction(MouseThread& mouseThread);
/** @brief 游戏覆盖层渲染循环 */
void gameOverlayRenderLoop();
