#include "runtime/thread_loops.h"
#include "Game_overlay.h"
#include <memory>

// 全局游戏覆盖层指针，用于在渲染线程中访问覆盖层实例
std::unique_ptr<Game_overlay> gameOverlayPtr;
// 原子退出标志，通知覆盖层线程安全退出
std::atomic<bool> gameOverlayShouldExit(false);

// 跟踪器调试数据互斥锁，保护调试信息的线程安全访问
std::mutex g_trackerDebugMutex;
// 当前帧的跟踪器调试轨迹列表，用于可视化调试
std::vector<TrackDebugInfo> g_trackerDebugTracks;
// 当前被锁定的跟踪目标ID，-1 表示无锁定目标
int g_trackerLockedId = -1;
