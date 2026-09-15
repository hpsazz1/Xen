#ifndef DEBUG_SESSION_H
#define DEBUG_SESSION_H

#include "config/config.h"
#include "weapon/weapon_timing.h"
#include <nlohmann/json.hpp>
#include <functional>
#include <memory>
#include <string>

class Runtime;

namespace debug_session {
using Json = nlohmann::json;
enum class Mode { COUNTERPULSE, FIRE_TEST, MANUAL_RECORDING, EVALUATE_MANUAL,
    EVALUATE_COMMANDS, DERIVE_DEFAULTS, DERIVE_PLAN };
enum class Action { NONE, VALIDATE, SAVE_PLAN, PREPARE, START, CANCEL, REEVALUATE,
    HIDE_HUD, SHOW_HUD, LOAD_PLAN, LOAD_SAMPLING, LOAD_FIRE_SETTINGS,
    LOAD_WEAPON_TIMING, SAVE_WEAPON_TIMING, DERIVE_PLAN, DERIVE_DEFAULTS };
enum class State { IDLE, WORKING, PREPARED, RUNNING, STOPPING, COMPLETED,
    CANCELED, FAILED, CLEANUP_UNKNOWN };

// 草稿只携带非秘密实验参数；生产配置由App冻结后另行传入，不进显示快照。
struct Request {
    Mode mode = Mode::COUNTERPULSE;
    std::string plan_text, sampling_text, input_path, load_path;
    std::string weapon_id;
    std::string output_root = "cache/debug";
    int recording_duration_ms = 120000;
    int candidate_index = 0;
    int shot_hold_ms = 80, fire_interval_ms = 800;
    bool show_hud = true;
    bool override_sampling = false;
};
struct Snapshot {
    State state = State::IDLE;
    bool busy = false, physical = false, hud_visible = false, cleanup_unknown = false;
    bool repeat_ready = false;
    bool hud_requested = false;
    std::string hud_message;
    std::uint64_t generation = 0;
    std::string message, report_directory, prepared_id;
    Json plan, sampling;
    // 仅显式文件载入或派生发布草稿；运行历史快照不能反向改写编辑器。
    Json draft_plan, draft_sampling;
    Mode draft_plan_mode = Mode::COUNTERPULSE;
    std::uint64_t draft_plan_revision = 0, draft_sampling_revision = 0;
    std::shared_ptr<const Json> result, live;
    weapon::TimingCatalog timing_catalog;
    bool timing_catalog_valid = false;
};

// App生命周期owner提供准入事实；Session不创建第二设备、不停止或启动生产Runtime。
struct Context {
    AppConfig config;
    std::shared_ptr<IMouseController> device;
    bool runtime_idle = false, input_recording_idle = false, cleanup_known = false;
};

class Session {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    bool dispatch(Action action, const Request& request, const Context& context,
        const std::string& prepared_id = {}, bool allow_physical_output = false,
        const std::string& confirmation = {}) noexcept;
    void cancel(const std::string& reason = "用户停止") noexcept;
    std::shared_ptr<const Snapshot> snapshot() const noexcept;
    bool busy() const noexcept;
    // 仅显式启用的前台按下沿调用；繁忙不排队，每次从冻结模板创建独立Run。
    bool repeat(const Context& context) noexcept;
    void invalidate_repeat() noexcept;
    // 原始输入面板沿用Runtime的记录/回看接口；开始、停止与归档等待均不占UI线程。
    bool record_inputs(Runtime& runtime, const std::string& root,
        const Context& context) noexcept;
    bool load_inputs(Runtime& runtime, const std::string& directory) noexcept;
    // 仅完成线程的回收；UI可每帧调用，不等待仍在工作的任务。
    void poll() noexcept;
    void set_theme(UiTheme theme) noexcept;
    void request_shutdown() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
const char* state_name(State state) noexcept;
const char* physical_confirmation() noexcept;
}
#endif
