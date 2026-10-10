#ifndef APP_WORKSPACE_ACTIONS_INTERNAL_H
#define APP_WORKSPACE_ACTIONS_INTERNAL_H

#include "model_workspace/model_workspace.h"
#include "runtime/runtime.h"

namespace app::detail {
// 主循环与回归使用同一动作入口，模型重载与运行状态门禁保持一致。
template<class Resolve>
bool route_workspace_action(model_workspace::Workspace& workspace,
        model_workspace::Action action, model_workspace::Settings& settings,
        const RuntimeSnapshot& snapshot, const DetectorConfig& detector,
        Resolve&& resolve, std::string& message) {
    if (action == model_workspace::Action::NONE) return true;
    if (action == model_workspace::Action::START_COLLECTION &&
        snapshot.detector_reload_state == DetectorReloadState::LOADING) {
        message = "模型正在切换，请完成后再开始采集。";
        return false;
    }
    std::string active_model_path = snapshot.active_model_path;
    if (active_model_path.empty()) {
        DetectorConfig selection = detector;
        if (resolve(selection)) active_model_path = selection.model_path;
    }
    const bool busy_runtime = snapshot.state != RuntimeState::STOPPED &&
                              snapshot.state != RuntimeState::FAILED;
    return workspace.execute(action, settings, busy_runtime,
        snapshot.d3d11_cuda_interop || snapshot.d3d11_directml_interop, active_model_path);
}
}
#endif
