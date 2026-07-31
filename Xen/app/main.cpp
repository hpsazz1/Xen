#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "config/config.h"
#include "log/log.h"
#include "overlay/overlay.h"
#include "runtime/runtime.h"

#include <Windows.h>

#ifdef ERROR
#undef ERROR
#endif

#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    constexpr const char* kConfigPath = "config.ini";
    AppConfig config;
    std::string app_message;
    std::string config_error;
    if (!load_app_config(kConfigPath, config, config_error)) {
        app_message = config_error + "；请在配置页填写并保存。";
    }

    LogConfig log_config;
    Log::init(log_config);
    Log::register_module("app", LogLevel::INFO);

    Runtime runtime;
    Overlay overlay;
    if (!overlay.init(config.ui)) {
        LOG_ERROR("app", "Overlay 初始化失败");
        Log::shutdown();
        return 1;
    }

    while (overlay.pump_messages()) {
        runtime.poll_keyboard();
        const RuntimeSnapshot snapshot = runtime.snapshot();
        OverlayActions actions;
        if (!overlay.render(snapshot, config, app_message, actions)) {
            LOG_ERROR("app", "Overlay 渲染失败");
            break;
        }

        if (actions.start_requested) {
            app_message = runtime.start(config)
                ? "Runtime 已启动。"
                : "Runtime 启动失败。";
        }
        if (actions.stop_requested) {
            runtime.stop();
            app_message = "Runtime 已停止。";
        }
        for (const auto& intent : actions.runtime_intents) {
            if (!runtime.post_intent(intent)) {
                app_message = "意图被安全门拒绝。";
            }
        }
        if (actions.save_config_requested) {
            app_message = save_app_config(
                kConfigPath, config, config_error)
                ? "配置已保存。"
                : config_error;
        }
    }

    runtime.stop();
    overlay.shutdown();
    Log::shutdown();
    return 0;
}
