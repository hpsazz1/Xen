#ifndef AUTO_STOP_DEBUG_RUNNER_H
#define AUTO_STOP_DEBUG_RUNNER_H
#include "config/config.h"
#include "mouse/mouse.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>

namespace auto_stop_probe_detail {
class CounterpulseHud;
class DebugRunFailure : public std::runtime_error {
public:
    DebugRunFailure(const std::string& reason, bool not_started)
        : std::runtime_error(reason), output_not_started(not_started) {}
    // 仅物理执行在任何订阅和执行之前的明确阶段失败可为true。
    const bool output_not_started;
};
enum class DebugRunMode { Counterpulse, ManualRecording, EvaluateManual, EvaluateCommands, DeriveDefaults, DeriveManualPlan };
struct DebugRunRequest {
    DebugRunMode mode = DebugRunMode::Counterpulse;
    nlohmann::json plan = nlohmann::json::object();
    nlohmann::json sampling_settings = nlohmann::json::object();
    std::filesystem::path input, output;
    AppConfig config;
    // 调用方持有唯一设备职责，核心不创建、不打开或关闭该设备。
    std::shared_ptr<IMouseController> device;
    bool allow_physical_output = false;
    std::string confirmation;
    std::size_t candidate_index = 0;
    int recording_duration_ms = 120000;
    std::shared_ptr<CounterpulseHud> hud;
};
struct DebugRunCallbacks {
    std::function<bool()> canceled;
    std::function<void(const nlohmann::json&)> publish;
};
// 同步冷路径，仅由任务后台线程调用；失败保留归档并抛出脱敏错误。
nlohmann::json run_debug(const DebugRunRequest&, const DebugRunCallbacks& = {});
nlohmann::json validate_debug_plan(const nlohmann::json&);
nlohmann::json make_fire_test_plan(const nlohmann::json&);
}
#endif
