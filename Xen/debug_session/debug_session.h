#ifndef DEBUG_SESSION_H
#define DEBUG_SESSION_H

#include "config/config.h"
#include "weapon/weapon_timing.h"
#include <nlohmann/json.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class Runtime;

namespace debug_session {
using Json = nlohmann::json;
enum class Mode { COUNTERPULSE, FIRE_TEST, RECOIL_TEST, RECOIL_CALIBRATE, RECOIL_CAPTURE, MANUAL_RECORDING, EVALUATE_MANUAL,
    EVALUATE_COMMANDS, DERIVE_DEFAULTS, DERIVE_PLAN, MOVEMENT_TEST, GRENADE_TEST };
enum class Action { NONE, VALIDATE, SAVE_PLAN, PREPARE, START, CANCEL, REEVALUATE,
    HIDE_HUD, SHOW_HUD, LOAD_PLAN, LOAD_SAMPLING, LOAD_FIRE_SETTINGS,
    LOAD_WEAPON_TIMING, SAVE_WEAPON_TIMING, DERIVE_PLAN, DERIVE_DEFAULTS,
    ENABLE_MOVEMENT_TEST, RUN_MOVEMENT_TEST, LIST_MOVEMENT_PLANS,
    START_MOVEMENT_RECORDING, STOP_MOVEMENT_RECORDING, DELETE_PLAN };
enum class State { IDLE, WORKING, PREPARED, RUNNING, STOPPING, COMPLETED,
    CANCELED, FAILED, CLEANUP_UNKNOWN };

// 草稿只携带非秘密实验参数；生产配置由App冻结后另行传入，不进显示快照。
struct Request {
    Mode mode = Mode::COUNTERPULSE;
    std::string plan_text, sampling_text, input_path, load_path;
    std::string weapon_id;
    std::string recoil_profile_path, recoil_calibration_path;
    double recoil_x_strength = 1, recoil_y_strength = 1;
    int recoil_duration_ms = 1500;
    int recoil_target_shots = 5;
    bool recoil_follow_crosshair = false;
    double recoil_locked_prefix_ms = 0;
    std::string output_root = "cache/debug";
    int recording_duration_ms = 120000;
    int candidate_index = 0;
    int shot_hold_ms = 80, fire_interval_ms = 800;
    bool show_hud = true;
    bool override_sampling = false;
    bool movement_save_as = false;
    int movement_record_motion_mode = 1;
    std::string movement_library_root = "cache/movement/plans";
};
struct MovementSavedPlan { std::string path, name, map, voice_command, raw_name; };
struct Snapshot {
    State state = State::IDLE;
    bool busy = false, physical = false, hud_visible = false, cleanup_unknown = false;
    bool repeat_ready = false;
    bool movement_test_enabled = false;
    Mode movement_test_mode = Mode::MOVEMENT_TEST; // F11唯一执行域；启用另一页才切换。
    bool movement_recording = false;
    std::uint64_t movement_record_events = 0;
    std::string movement_recording_path;
    std::vector<MovementSavedPlan> movement_saved_plans;
    std::string movement_selected_path;
    std::uint64_t movement_catalog_revision = 0;
    Json movement_current, movement_baseline;
    std::vector<MovementSavedPlan> grenade_saved_plans;
    std::string grenade_selected_path;
    std::uint64_t grenade_catalog_revision = 0;
    Json grenade_current, grenade_baseline;
    // 每域独立载入发布槽，另一页后来的载入不能吞掉未显示页面的载入结果。
    Json movement_loaded_draft, grenade_loaded_draft;
    std::uint64_t movement_loaded_revision = 0, grenade_loaded_revision = 0;
    weapon::MapSnapshot movement_map;
    bool movement_voice_selected = false;
    std::uint64_t movement_voice_epoch = 0;
    // 主机提示独立于一次收音轮次；对齐、取消和投掷完成均隐藏，不触发声音或投掷。
    std::uint64_t grenade_guidance_revision = 0;
    std::string grenade_guidance_state = "hidden";
    std::string grenade_guidance_plan_id, grenade_guidance_reference_id, grenade_guidance_message;
    // 与普通进度消息分离，热键拒绝不能覆盖准备失败或失效原因。
    std::string repeat_unavailable_reason;
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

// App生命周期owner提供准入事实；运行态道具借用Runtime输出和原始ROI，不停启Runtime。
struct Context {
    AppConfig config;
    std::shared_ptr<IMouseController> device;
    Runtime* runtime = nullptr;
    // 仅worker持有Runtime借用时填写；每步时间线输出前检查。
    std::function<bool()> output_permitted;
    bool runtime_idle = false, input_recording_idle = false, cleanup_known = false;
    weapon::MapSnapshot map;
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
    // 仅同步已启用身法会话的最新草稿，无文件IO和设备输出；无效草稿撤下旧模板。
    bool update_movement_draft(const Request&, const Context&, bool manual = true) noexcept;
    void update_map(const weapon::MapSnapshot&) noexcept;
    // 文件解析异步完成，App 在本帧停止/热键处理之后复核再提交。
    bool select_voice(const Request&, const Context&) noexcept;
    // 手机选择明确切入道具域，只载入已保存方案，不执行物理动作。
    bool select_grenade(const Request&, const Context&) noexcept;
    std::optional<bool> finish_voice(const Context&, bool still_current) noexcept;
    bool align_grenade(const Context&) noexcept;
    bool throw_grenade(const Context&) noexcept;
    void cancel_grenade(const std::string& reason = "本次道具操作已取消") noexcept;
    // App每帧提供实际输入健康与运行许可；关闭设置或换上下文立即撤销旧对齐。
    void maintain_grenade(const Context&, bool input_healthy) noexcept;
    void invalidate_repeat(const std::string& reason = "测试模板已失效（参数或运行上下文变化），请重新准备") noexcept;
    // 原始输入面板沿用Runtime的记录/回看接口；开始、停止与归档等待均不占UI线程。
    bool record_inputs(Runtime& runtime, const std::string& root,
        const Context& context) noexcept;
    bool load_inputs(Runtime& runtime, const std::string& directory) noexcept;
    // 仅完成线程的回收；UI可每帧调用，不等待仍在工作的任务。
    void poll() noexcept;
    void set_theme(UiTheme theme) noexcept;
    void request_shutdown() noexcept;
private:
    bool run_repeat(const Context&, bool keyboard_trigger) noexcept;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
const char* state_name(State state) noexcept;
const char* physical_confirmation() noexcept;
}
#endif
