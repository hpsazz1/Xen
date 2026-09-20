#include "overlay/overlay.h"

#include <iostream>
#include <string>
#include <string_view>
#include <cmath>

// 独立视觉验收入口：只创建日志和窗口；绝不实例化 Runtime 或设备。
// 所有运行、武装和输出意图均丢弃，保存只更新本进程中的展示参数。
int main(int argc, char** argv) {
    bool input_training_preview = false;
    bool trigger_status_preview = false;
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
        } else if (argument == "--input-training") {
            input_training_preview = true;
        } else if (argument == "--trigger-status") {
            trigger_status_preview = true;
        } else {
            std::cerr << "用法：auxiliary_ui_preview [--minimum] [--dark] [--input-training] [--trigger-status]\n";
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
    if (input_training_preview) {
        auto training = std::make_shared<input_training::Snapshot>();
        training->status = input_training::Status::STOPPED;
        training->replay_source = true;
        training->directory = "合成界面样本：未采集真实设备";
        training->received_events = 1600;
        for (int i = 0; i < 40; ++i) {
            input_training::Timing timing;
            timing.delta_ns = (i % 9 - 4) * 4000000;
            const auto magnitude = std::abs(timing.delta_ns);
            timing.grade = magnitude == 0 ? input_training::Grade::UNCLASSIFIED :
                magnitude <= 2000000 ? input_training::Grade::PERFECT :
                magnitude <= 10000000 ? input_training::Grade::EXCELLENT :
                timing.delta_ns < 0 ? input_training::Grade::EARLY : input_training::Grade::LATE;
            timing.timing_uncertainty_known = true;
            timing.atomic_ambiguous = timing.grade == input_training::Grade::UNCLASSIFIED;
            training->timings.push_back(timing);
        }
        training->total_timings = training->timings.size();
        auto hold = std::make_shared<input_training::Hold>();
        hold->id = 1;
        hold->end = input_training::HoldEnd::RELEASED;
        hold->motion_available = true;
        hold->complete_received_stream = true;
        for (int i = 0; i < 1600; ++i) {
            input_training::Point point;
            point.raw_index = i;
            point.x = static_cast<std::int64_t>(180 * std::sin(i / 130.0));
            point.y = i / 3;
            point.event.motion_valid = true;
            point.event.left_down = i < 1599;
            point.event.received_at_ns = 1000000000ll + i * 1000000ll;
            point.event.epoch = 1;
            point.event.sequence = i + 1;
            hold->points.push_back(point);
        }
        training->total_holds = 1;
        training->holds.push_back(hold);
        snapshot.training = training;
    }
    const OverlayModelCatalog models;
    const OverlayBackendCatalog backends;
    model_workspace::Settings workspace_settings;
    const model_workspace::Snapshot workspace_snapshot;
    std::string message = "界面预览：无设备连接；运行和输出操作均不执行，保存不写配置文件。";
    int result = 0;
    unsigned int preview_frame = 0;
    while (overlay.pump_messages()) {
        if (trigger_status_preview) {
            // 重放现场两个待命原因逐帧交替；仅驱动展示快照，不创建控制器或设备。
            snapshot.trigger.phase = TriggerPhase::WAITING;
            snapshot.trigger.reason = (++preview_frame % 2) ?
                TriggerReason::NO_CANDIDATE : TriggerReason::RELEASED;
        }
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
