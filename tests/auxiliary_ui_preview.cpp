#include "overlay/overlay.h"

#include <iostream>
#include <string>
#include <string_view>

// 独立视觉验收入口：只创建日志和窗口；绝不实例化 Runtime 或设备。
// 所有运行、武装和输出意图均丢弃，保存只更新本进程中的展示参数。
int main(int argc, char** argv) {
    AppConfig config;
    config.mouse.allow_send_input = false;
    config.mouse.backend = MouseBackend::KMBOX_NET;
    config.auto_stop = {};
    config.log.enable_console = true;
    config.log.enable_file = false;
    config.log.enable_debug_file = false;
    config.ui.open_detached_preview_on_start = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--minimum") {
            config.ui.width = kMinimumUiWidth;
            config.ui.height = kMinimumUiHeight;
        } else if (argument == "--dark") {
            config.ui.theme = UiTheme::DARK;
        } else {
            std::cerr << "用法：auxiliary_ui_preview [--minimum] [--dark]\n";
            return 2;
        }
    }

    Log::init(config.log);
    Overlay overlay;
    if (!overlay.init(config.ui)) {
        std::cerr << overlay.last_error() << '\n';
        Log::shutdown();
        return 1;
    }
    RuntimeSnapshot snapshot;
    snapshot.state = RuntimeState::STOPPED;
    const OverlayModelCatalog models;
    const OverlayBackendCatalog backends;
    model_workspace::Settings workspace_settings;
    const model_workspace::Snapshot workspace_snapshot;
    std::string message = "界面预览：无设备连接；运行和输出操作均不执行，保存不写配置文件。";
    int result = 0;
    while (overlay.pump_messages()) {
        OverlayActions actions;
        if (!overlay.render(snapshot, {}, models, backends, config,
                workspace_settings, workspace_snapshot, message, actions)) {
            std::cerr << overlay.last_error() << '\n';
            result = 1;
            break;
        }
        // 即使在输入页更改总开关，也绝不创建输出消费者。
        config.mouse.allow_send_input = false;
        if (actions.save_config_requested) {
            message = "预览参数已保留在内存中；未写配置文件，未启动任何设备。";
        }
        // 不消费 runtime_intents、start/stop、reload 或 preview 订阅操作。
    }
    overlay.shutdown();
    Log::shutdown();
    return result;
}
