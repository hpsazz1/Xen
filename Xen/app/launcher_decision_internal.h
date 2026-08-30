#ifndef APP_LAUNCHER_DECISION_INTERNAL_H
#define APP_LAUNCHER_DECISION_INTERNAL_H

#include "app/release_contract_internal.h"

#include <string>

namespace app::detail {

inline constexpr int kLauncherDecisionFailureExitCode = 1;

struct LauncherDecisionAdapter {
    void* context = nullptr;
    void (*show_error)(void* context, const std::string& message) = nullptr;
};

// 只负责 config.ini 到发布 Worker 的决策与失败展示；进程创建留在
// Launcher 外层，因此测试 adapter 不具备启动 Worker 的能力。
int resolve_launcher_runtime(
    const ReleaseManifest& manifest,
    const std::string& config_path,
    const LauncherDecisionAdapter& adapter,
    const ReleaseRuntimeEntry*& runtime) noexcept;

} // namespace app::detail

#endif // APP_LAUNCHER_DECISION_INTERNAL_H
