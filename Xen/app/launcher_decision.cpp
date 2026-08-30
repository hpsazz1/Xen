#include "app/launcher_decision_internal.h"

#include "config/config.h"

#include <filesystem>

namespace app::detail {
namespace {

void present_error(const LauncherDecisionAdapter& adapter,
                   const std::string& message) noexcept {
    if (!adapter.show_error) return;
    try {
        adapter.show_error(adapter.context, message);
    } catch (...) {
    }
}

} // namespace

int resolve_launcher_runtime(
        const ReleaseManifest& manifest,
        const std::string& config_path,
        const LauncherDecisionAdapter& adapter,
        const ReleaseRuntimeEntry*& runtime) noexcept {
    runtime = nullptr;
    try {
        AppConfig config;
        std::string config_error;
        if (!load_app_config(config_path, config, config_error)) {
            std::error_code filesystem_error;
            const bool config_exists =
                std::filesystem::exists(config_path, filesystem_error);
            if (filesystem_error) {
                present_error(
                    adapter,
                    "无法检查 Launcher 配置文件: " + config_path +
                        "；" + filesystem_error.message());
                return kLauncherDecisionFailureExitCode;
            }
            if (config_exists) {
                present_error(
                    adapter,
                    config_error.empty()
                        ? "Launcher 配置文件无效: " + config_path
                        : config_error);
                return kLauncherDecisionFailureExitCode;
            }
            // 发布包首次启动仍允许缺少 config.ini，并沿用 AppConfig 的
            // 安全默认 TensorRT 路由；只有确实缺失才能进入该分支。
        }

        runtime = find_runtime_for_backend(manifest, config.detector.backend);
        if (runtime) return 0;

        present_error(adapter, "配置请求的推理后端未被发布清单授权");
        return kLauncherDecisionFailureExitCode;
    } catch (...) {
        runtime = nullptr;
        present_error(adapter, "发布入口配置决策发生未知错误");
        return kLauncherDecisionFailureExitCode;
    }
}

} // namespace app::detail
