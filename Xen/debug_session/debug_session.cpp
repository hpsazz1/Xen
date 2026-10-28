#include "debug_session/debug_session.h"
#include "auto_stop_probe/debug_runner.h"
#include "auto_stop_probe/counterpulse_internal.h"
#include "auto_stop_probe/counterpulse_hud.h"
#include "runtime/runtime.h"
#include "log/log.h"
#include "recoil/recoil_debug_run.h"
#include "movement/timeline.h"
#include "movement/recording.h"
#include "debug_session/movement_library_internal.h"
#include "debug_session/grenade_posture_internal.h"
#include "source_context/source_context.h"
#include "grenade_alignment/grenade_alignment.h"
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <future>
#include <limits>
#include <mutex>
#include <set>
#include <thread>

namespace debug_session {
namespace {
using namespace auto_stop_probe_detail;
using namespace std::chrono_literals;
constexpr std::size_t kDocumentLimit = 1024 * 1024;
std::atomic_uint64_t next_id{0};

Json parse_document(const std::string& text, std::size_t limit = kDocumentLimit) {
    if (text.size() > limit) throw std::runtime_error("文档超过大小限制");
    std::vector<std::set<std::string>> keys;
    auto callback = [&](int depth, Json::parse_event_t event, Json& value) {
        if (depth > 32) throw std::runtime_error("文档嵌套过深");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
            throw std::runtime_error("文档包含重复字段");
        if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    };
    return Json::parse(text, callback, true, true);
}
Json code_identity() {
    return {{"commit",XEN_DEBUG_BUILD_COMMIT},{"dirty",XEN_DEBUG_BUILD_DIRTY},{"runtime",XEN_DEBUG_RUNTIME_ID}};
}
Json read_document(const std::filesystem::path& path, std::size_t limit = kDocumentLimit) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("无法打开所选文档");
    std::string text(limit + 1, '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(file.gcount()));
    if (text.starts_with("\xef\xbb\xbf")) text.erase(0, 3);
    return parse_document(text, limit);
}
void write_document(const std::filesystem::path& path, const Json& value) {
    auto temporary = path;
    temporary += ".partial";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file || !(file << value.dump(2) << '\n')) throw std::runtime_error("结果写入失败");
    file.close();
    if (!file) throw std::runtime_error("结果关闭失败");
    std::filesystem::rename(temporary, path);
}
std::filesystem::path new_directory(const std::string& root) {
    if (root.empty()) throw std::runtime_error("结果目录不能为空");
    const auto base = std::filesystem::absolute(std::filesystem::u8path(root));
    std::filesystem::create_directories(base);
    for (int retry = 0; retry < 8; ++retry) {
        const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
        auto path = base / ("debug-" + std::to_string(stamp) + "-" + std::to_string(++next_id));
        if (std::filesystem::create_directory(path)) return path;
    }
    throw std::runtime_error("不能建立独立结果目录");
}
bool timeline_mode(Mode mode) { return mode == Mode::MOVEMENT_TEST || mode == Mode::GRENADE_TEST; }
const char* timeline_name(Mode mode) { return mode == Mode::GRENADE_TEST ? "道具" : "身法"; }
const char* timeline_report(Mode mode) { return mode == Mode::GRENADE_TEST ? "grenade-run-plan.json" : mode == Mode::MOVEMENT_TEST ? "movement-run-plan.json" : "plan.json"; }
template<class T> auto& current_plan(T& s, Mode mode) { return mode == Mode::GRENADE_TEST ? s.grenade_current : s.movement_current; }
template<class T> auto& baseline_plan(T& s, Mode mode) { return mode == Mode::GRENADE_TEST ? s.grenade_baseline : s.movement_baseline; }
void publish_loaded_plan(Snapshot& s, Mode mode, const Json& plan) {
    auto& loaded = mode == Mode::GRENADE_TEST ? s.grenade_loaded_draft : s.movement_loaded_draft;
    auto& revision = mode == Mode::GRENADE_TEST ? s.grenade_loaded_revision : s.movement_loaded_revision;
    loaded = plan; ++revision;
}
std::string timeline_library_root(const Request& request) {
    return request.mode == Mode::GRENADE_TEST && request.movement_library_root == "cache/movement/plans"
        ? "cache/grenade/plans" : request.movement_library_root;
}
Json request_plan(const Request& request) {
    if (timeline_mode(request.mode)) {
        movement::timeline::Plan plan;
        std::string error;
        if (!movement::timeline::parse_plan(parse_document(request.plan_text,16*1024*1024),plan,error))
            throw std::runtime_error(error);
        return movement::timeline::plan_json(plan);
    }
    if (request.mode == Mode::FIRE_TEST)
        return make_fire_test_plan({{"shot_hold_ms",request.shot_hold_ms},{"fire_interval_ms",request.fire_interval_ms}});
    if (!request.plan_text.empty()) return validate_debug_plan(parse_document(request.plan_text));
    if (request.mode != Mode::COUNTERPULSE) return Json::object();
    return validate_debug_plan({{"schema_version",2},{"baseline","counter"},{"capture_enabled",false},
        {"shots",20},{"fire_delay_ms",0},{"fire_interval_ms",300},{"move_during_fire_delay",false},
        {"overlap_fire_interval",true},{"move_ms",500},{"counter_hold_ms",40},{"counter_delay_ms",0},
        {"shot_after_release_ms",18},{"shot_hold_ms",5},{"late_tolerance_ms",5},{"direction",2}});
}
Json request_sampling(const Request& request) {
    if (timeline_mode(request.mode)) return Json::object();
    if (request.mode == Mode::FIRE_TEST) {
        SamplingSettings settings; settings.hud_enabled = false;
        return sampling_settings_json(settings); // 原地点射不以其他页面的模型草稿作为准入条件。
    }
    if ((request.mode == Mode::EVALUATE_MANUAL || request.mode == Mode::EVALUATE_COMMANDS) && !request.override_sampling)
        return Json::object();
    if (request.sampling_text.empty() && (request.mode == Mode::EVALUATE_MANUAL ||
        request.mode == Mode::EVALUATE_COMMANDS || request.mode == Mode::DERIVE_DEFAULTS || request.mode == Mode::DERIVE_PLAN))
        return Json::object(); // 未覆盖才沿用原始Run参数，不能用默认值冒充其实际参数。
    auto settings = request.sampling_text.empty() ? SamplingSettings{} : parse_sampling_settings(parse_document(request.sampling_text));
    settings.hud_enabled = request.mode != Mode::FIRE_TEST && request.show_hud;
    return sampling_settings_json(settings);
}
DebugRunMode run_mode(Mode mode) {
    switch (mode) {
    case Mode::MANUAL_RECORDING: return DebugRunMode::ManualRecording;
    case Mode::EVALUATE_MANUAL: return DebugRunMode::EvaluateManual;
    case Mode::EVALUATE_COMMANDS: return DebugRunMode::EvaluateCommands;
    case Mode::DERIVE_DEFAULTS: return DebugRunMode::DeriveDefaults;
    case Mode::DERIVE_PLAN: return DebugRunMode::DeriveManualPlan;
    default: return DebugRunMode::Counterpulse;
    }
}
bool recoil_mode(Mode mode) { return mode == Mode::RECOIL_TEST || mode == Mode::RECOIL_CALIBRATE || mode == Mode::RECOIL_CAPTURE; }
bool single_use_recoil_mode(Mode mode) { return mode == Mode::RECOIL_CALIBRATE || mode == Mode::RECOIL_CAPTURE; }
bool physical_mode(Mode mode) { return mode == Mode::COUNTERPULSE || mode == Mode::FIRE_TEST || timeline_mode(mode) || recoil_mode(mode); }
bool uses_device(Mode mode) { return physical_mode(mode) || mode == Mode::MANUAL_RECORDING; }
bool runtime_available(const Context& context, Mode mode) {
    return context.runtime_idle || (mode == Mode::GRENADE_TEST && context.runtime);
}
std::shared_ptr<RuntimeGrenadeControl> borrow_grenade_control(const Context& context, std::atomic_bool& canceled) {
    if (!context.runtime) return {};
    std::string reason;
    auto control = context.runtime->acquire_grenade_control(canceled,reason);
    if (!control) throw DebugRunFailure(reason.empty()?"Runtime道具控制暂不可用":reason,true);
    return control;
}
grenade_alignment::SharedStream shared_stream(const std::shared_ptr<RuntimeGrenadeControl>& control) {
    if (!control) return {};
    return {[control](CapturedFrame& frame){return control->grab(frame);},
            [control]{return control->source_snapshot();}};
}
void admit(const Context& context, Mode mode) {
    if (!uses_device(mode)) return;
    if (!runtime_available(context,mode) || !context.input_recording_idle || !context.cleanup_known)
        throw std::runtime_error("请先停止Runtime和记录，并确认设备清理完成");
    if (!context.device || !context.device->output_owner_exclusive() ||
        (context.device->status() != MouseStatus::READY && context.device->status() != MouseStatus::DISABLED))
        throw std::runtime_error("已有设备未就绪或未持有独占职责");
    if (context.config.mouse.backend != MouseBackend::KMBOX_NET)
        throw std::runtime_error("该测试仅支持已有KMBOX NET连接");
    if (context.device->left_button_faulted() ||
        (context.device->left_button_cleanup_required() && !(mode==Mode::GRENADE_TEST && context.runtime)))
        throw std::runtime_error("左键清理尚未确认，不能开始新任务");
}
}

struct Session::Impl {
    std::atomic<std::shared_ptr<const Snapshot>> view{std::make_shared<const Snapshot>()};
    std::future<void> worker;
    std::atomic_bool canceled{false};
    std::atomic<std::shared_ptr<CounterpulseHud>> hud;
    std::mutex hud_mutex;
    std::atomic<bool> hud_requested{false};
    std::uint64_t observed_hud_close_sequence = 0;
    std::atomic<UiTheme> hud_theme{UiTheme::LIGHT};
    bool shutting_down = false;
    std::uint64_t generation = 0;
    struct Prepared {
        Request request;
        Context context;
        Json plan, sampling;
        std::string id;
        std::filesystem::path directory;
        std::uint64_t repeat_revision = 0;
    };
    std::optional<Prepared> prepared;
    std::optional<Context> movement_binding;
    std::atomic_bool movement_record_stop{false};
    std::atomic<std::shared_ptr<const Prepared>> repeat_plan;
    std::atomic_uint64_t repeat_revision{0};
    std::future<Json> voice_load;
    Request voice_request;
    std::uint64_t voice_revision = 0, voice_epoch = 0, voice_grenade_epoch = 0;
    std::atomic_uint64_t grenade_epoch{0};
    std::atomic_bool grenade_operation{false};
    std::atomic_bool grenade_assisted_operation{false};
    std::shared_ptr<std::atomic_bool> grenade_posture_fault = std::make_shared<std::atomic_bool>(false);
    struct AlignedGrenade {
        std::uint64_t epoch = 0, repeat_revision = 0;
        std::string reference_revision;
        Context context;
        std::shared_ptr<RuntimeGrenadeControl> control;
        std::shared_ptr<detail::GrenadePosture> posture;
    };
    std::atomic<std::shared_ptr<const AlignedGrenade>> aligned_grenade;
    void check_posture_cleanup() {
        if (!grenade_posture_fault->exchange(false)) return;
        canceled = true;
        update([](Snapshot& s) {
            s.cleanup_unknown = true; s.state = State::CLEANUP_UNKNOWN;
            s.repeat_ready = false; s.movement_test_enabled = false;
            s.message = "蹲姿释放未确认，其他输出保持暂停，请检查设备";
            s.grenade_guidance_state = "failed"; s.grenade_guidance_message = s.message;
        });
    }
    void guidance(const std::string& state, const std::string& message,
                  const std::string& path = {}, std::optional<std::uint64_t> expected_epoch = {}) {
        update([&](Snapshot& s) {
            if (expected_epoch && grenade_epoch.load() != *expected_epoch) return;
            s.grenade_guidance_state = state; s.grenade_guidance_message = message;
            if (!path.empty()) {
                const auto file = std::filesystem::u8path(path);
                const auto id = file.filename().u8string(), reference = file.stem().u8string();
                s.grenade_guidance_plan_id.assign(id.begin(), id.end());
                s.grenade_guidance_reference_id.assign(reference.begin(), reference.end());
            }
            ++s.grenade_guidance_revision;
        });
    }
    void invalidate_grenade(const std::string& reason) {
        ++grenade_epoch; aligned_grenade.store(nullptr);
        if (grenade_operation.load()) canceled = true;
        guidance("hidden", reason);
        check_posture_cleanup();
    }
    void invalidate_repeat(const std::string& reason) {
        ++repeat_revision;
        repeat_plan.store(nullptr);
        invalidate_grenade(reason);
        update([&](Snapshot& s) {
            s.repeat_ready = false; s.movement_test_enabled = false;
            s.movement_voice_selected = false;
            s.repeat_unavailable_reason = reason.empty() ? "测试模板已失效，请重新准备" : reason;
        });
    }
    void replace_hud(std::shared_ptr<CounterpulseHud> next = {}) {
        std::shared_ptr<CounterpulseHud> previous;
        { std::lock_guard lock(hud_mutex); previous = hud.exchange(std::move(next)); }
        // 最后引用只在调用本方法的后台线程释放；UI借用在同一短锁内结束。
    }

    std::shared_ptr<CounterpulseHud> ensure_hud() {
        std::lock_guard lock(hud_mutex);
        auto current = hud.load();
        if (!current) {
            current = std::make_shared<CounterpulseHud>(SamplingSettings{},false,true,hud_theme.load(),hud_requested.load());
            hud.store(current);
        }
        return current;
    }

    template<class F> void update(F&& fn) {
        auto prior = view.load();
        for (;;) {
            auto next = std::make_shared<Snapshot>(*prior);
            fn(*next);
            std::shared_ptr<const Snapshot> immutable = std::move(next);
            if (view.compare_exchange_weak(prior, immutable)) return;
        }
    }
    bool busy() const { return view.load()->busy || (worker.valid() && worker.wait_for(0ms) != std::future_status::ready); }
    template<class F> bool launch(State state, bool physical, F&& fn, bool retain_timeline = false,
                                 bool assisted_grenade = false) {
        if (busy()) return false;
        if (worker.valid()) {
            if (worker.wait_for(0ms) != std::future_status::ready) return false;
            worker.get();
        }
        canceled = false;
        grenade_operation = retain_timeline;
        grenade_assisted_operation = assisted_grenade;
        const auto current = ++generation;
        update([&](Snapshot& s) { s.state = state; s.busy = true; s.physical = physical;
            // generation与结果同次发布，不能让新准备代次携带上一次完成结果。
            s.generation = current; s.message = "后台处理中"; s.live.reset(); s.result.reset(); });
        try {
            worker = std::async(std::launch::async, [this, current, physical, retain_timeline, fn = std::forward<F>(fn)]() mutable {
                try { fn(); }
                catch (const DebugRunFailure& error) {
                    update([&](Snapshot& s) {
                        if (s.generation != current) return;
                        if (!error.output_not_started && !error.cleanup_confirmed && physical) s.cleanup_unknown = true;
                        s.state = s.cleanup_unknown ? State::CLEANUP_UNKNOWN : (canceled ? State::CANCELED : State::FAILED);
                        s.message = error.what();
                    });
                }
                catch (...) {
                    // 原始异常可能含外部文件内容，界面只发布固定原因；详细原生报告也须脱敏。
                    update([&](Snapshot& s) {
                        if (s.generation != current) return;
                        s.state = physical || s.cleanup_unknown ? State::CLEANUP_UNKNOWN : (canceled ? State::CANCELED : State::FAILED);
                        if (physical) s.cleanup_unknown = true;
                        s.message = physical ? "任务失败，设备释放状态不能确认；检查本组报告" : "任务未完成；请检查参数、来源与本组报告";
                    });
                }
                update([&](Snapshot& s) { if (s.generation == current) {
                    s.busy = false; s.movement_recording = false;
                    if (retain_timeline && canceled && !s.cleanup_unknown) s.state = State::CANCELED;
                    if (canceled && s.state == State::PREPARED) {
                        s.state = State::CANCELED; s.prepared_id.clear(); s.message = "准备已取消，已生成文件仅留档";
                    }
                    if (s.cleanup_unknown || (!retain_timeline &&
                        (canceled || s.state == State::CANCELED || s.state == State::FAILED))) {
                        repeat_plan.store(nullptr); s.repeat_ready = false; s.movement_test_enabled = false;
                        s.repeat_unavailable_reason = s.message;
                    }
                } });
                grenade_operation = false;
                grenade_assisted_operation = false;
            });
            return true;
        } catch (...) {
            grenade_operation = false;
            grenade_assisted_operation = false;
            update([](Snapshot& s) { s.state = State::FAILED; s.busy = false; s.message = "后台任务创建失败"; });
            return false;
        }
    }
    void record_movement(const Request& request, const Context& context) {
        using Clock = std::chrono::steady_clock;
        const auto now_ns = [] { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); };
        auto& device = *context.device;
        const auto directory = new_directory(request.output_root);
        const auto begin = now_ns();
        if (!device.begin_movement_trace()) throw std::runtime_error("录制未启动：设备不支持录制、已有录制或身法动作尚未释放，请在动作前开始");
        struct EndTrace { IMouseController& device; bool active=true; ~EndTrace(){if(active)device.end_movement_trace();} } guard{device};
        update([&](Snapshot& s) { s.movement_recording=true; s.movement_record_events=0;
            s.movement_recording_path.clear(); s.report_directory=directory.string();
            s.message="正在录制身法：执行人工操作或原Long Jump，动作结束后停止录制"; });
        std::vector<MovementTraceEvent> events;
        events.reserve(8192);
        bool gap=false;
        const auto consume = [&] {
            MovementTraceBatch batch;
            if (!device.read_movement_trace(batch)) { gap=true; return std::size_t{0}; }
            gap = gap || batch.gap;
            if (events.size()+batch.events.size()>131072) { gap=true; return std::size_t{0}; }
            events.insert(events.end(),batch.events.begin(),batch.events.end());
            return batch.events.size();
        };
        auto next_update=Clock::now();
        while(!movement_record_stop && !canceled && now_ns()-begin<60000000000LL) {
            consume();
            if(gap)break;
            if(device.status()!=MouseStatus::READY && device.status()!=MouseStatus::DISABLED){gap=true;break;}
            if(Clock::now()>=next_update) {
                update([&](Snapshot& s){s.movement_record_events=events.size();});
                next_update=Clock::now()+100ms;
            }
            std::this_thread::sleep_for(2ms);
        }
        device.end_movement_trace(); guard.active=false;
        const auto end=now_ns();
        // end等待在途命令退出后固定尾部，再排空已观察事实。
        while(consume()!=0) {}
        gap=gap || canceled || end-begin>60000000000LL;
        auto trace=movement::recording::trace_json(events,begin,end,gap,request.movement_record_motion_mode);
        trace["code_identity"]=code_identity();
        trace["sensitivity"]=context.config.movement.sensitivity;
        trace["yaw_degrees_per_count"]=context.config.movement.yaw_degrees_per_count;
        const auto path=directory/"movement-recording.json";
        write_document(path,trace);
        auto derived=movement::recording::derive(events,begin,end,gap,request.movement_record_motion_mode);
        Json plan;
        if(derived.complete) {
            Json previous = view.load()->movement_current;
            try { if (!request.plan_text.empty()) previous = request_plan(request); } catch (...) {}
            if (previous.is_object()) {
                derived.plan.map = previous.value("map",std::string{});
                derived.plan.voice_command = previous.value("voice_command",std::string{});
                derived.plan.name = previous.value("name",derived.plan.name);
                derived.plan.notes = previous.value("notes",derived.plan.notes);
                derived.plan.location = previous.value("location",derived.plan.location);
            }
            plan=movement::timeline::plan_json(derived.plan);
        }
        // 原始记录保留为证据；派生方案只替换当前草稿，显式保存才写入方案库。
        update([&](Snapshot& s) {
            const auto utf8=path.u8string();
            s.movement_recording_path.assign(reinterpret_cast<const char*>(utf8.data()),utf8.size());
            s.movement_record_events=events.size();
            s.state=canceled?State::CANCELED:State::COMPLETED;
            s.message=derived.complete?"录制已生成当前草稿，尚未保存方案；鼠标按所选解释派生，请核对后测试":derived.error;
            if(derived.complete) {
                s.draft_plan=plan;
                s.movement_current=plan; s.movement_voice_selected=false;
                s.draft_plan_mode=Mode::MOVEMENT_TEST; ++s.draft_plan_revision;
                publish_loaded_plan(s,Mode::MOVEMENT_TEST,plan);
            }
        });
    }
    void run_movement(const Prepared& work, const std::function<void()>& before_execute = {},
                      const std::function<bool()>& start_permitted = {}) {
        using namespace movement::timeline;
        Plan plan; std::string error;
        if (!parse_plan(work.plan,plan,error)) throw DebugRunFailure(error,true);
        auto& device = *work.context.device;
        const auto& config = work.context.config;
        const auto label = std::string(timeline_name(work.request.mode));
        if (!device.supports_wasd_keyboard() || config.keyboard.emergency_virtual_keys.empty() ||
            !valid_keyboard_config(config.keyboard))
            throw DebugRunFailure(label + "测试需要方向键能力和有效急停键，未开始输出",true);
        source_context::SourceContextClient source;
        auto source_config = config.source_context;
        char* token = nullptr; std::size_t token_size = 0;
        if (_dupenv_s(&token,&token_size,"XEN_SOURCE_CONTEXT_TOKEN") == 0 && token) {
            source_config.token = token; std::free(token);
        }
        if (!source_config.enabled || !source.start(source_config))
            throw DebugRunFailure("源焦点接收器未配置或启动失败，" + label + "未开始输出",true);
        const auto emergency = [&] {
            if (canceled.load()) return true;
            InputSnapshot input;
            if (!device.poll_input(input) || !input.state_valid) return false;
            for (int key : config.keyboard.emergency_virtual_keys)
                if (key > 0 && key < 256 && input.virtual_keys[static_cast<std::size_t>(key)]) {
                    // worker可能独自捕获很短的急停；不能依赖下一次UI轮询再次看到按住。
                    invalidate_repeat("急停键已撤销测试许可，请重新启用");
                    canceled = true; return true;
                }
            if (work.request.mode == Mode::GRENADE_TEST && config.keyboard.grenade_assist_enabled)
                for (int key : config.keyboard.grenade_cancel_virtual_keys)
                    if (key > 0 && key < 256 && input.virtual_keys[static_cast<std::size_t>(key)]) {
                        canceled = true; return true;
                    }
            return false;
        };
        update([&](Snapshot& s) { s.state = State::RUNNING;
            s.message = "等待源端游戏聚焦；" + label + "按当前方案执行，急停键可取消"; });
        source_context::SourceContextSnapshot initial;
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        bool ready = false;
        while (!emergency() && std::chrono::steady_clock::now() < deadline) {
            initial = source.snapshot(); InputSnapshot input;
            if (initial.available && initial.focused && device.poll_input(input) && input.state_valid) {
                ready = true; break;
            }
            std::this_thread::sleep_for(2ms);
        }
        if (!ready) throw DebugRunFailure(canceled.load() ? label + "测试已取消，未开始输出" :
            "等待源焦点或设备输入超时，未开始输出",true);
        // 在源焦点就绪后紧邻投掷复核新画面，不能在等待焦点前消费旧已对齐结果。
        if (before_execute) before_execute();
        Callbacks callbacks;
        callbacks.canceled = emergency;
        callbacks.first_output_permitted = start_permitted;
        callbacks.permitted = [&] {
            const auto focus = source.snapshot(); InputSnapshot input;
            return !canceled.load() && (!work.context.output_permitted || work.context.output_permitted()) &&
                focus.available && focus.focused &&
                focus.session_id == initial.session_id && focus.control_safety_epoch == initial.control_safety_epoch &&
                device.poll_input(input) && input.state_valid && config.mouse.allow_send_input;
        };
        int published_ms = -50;
        callbacks.progress = [&](const Progress& p) {
            if (p.elapsed_ms < published_ms + 25 && p.elapsed_ms < p.total_ms) return;
            published_ms = p.elapsed_ms;
            const auto value = std::make_shared<const Json>(Json{{"elapsed_ms",p.elapsed_ms},
                {"total_ms",p.total_ms},{"sent_dx",p.sent_dx}});
            update([&](Snapshot& s) { s.live = value; s.message = std::string(timeline_name(work.request.mode)) + "执行中：" +
                std::to_string(p.elapsed_ms) + "/" + std::to_string(p.total_ms) + " ms"; });
        };
        const auto result = run_plan(device,plan,config.movement,callbacks);
        Json index{{"schema_version",1},{"mode",work.request.mode == Mode::GRENADE_TEST ? "grenade" : "movement"},{"prepared_id",work.id},
            {"code_identity",code_identity()},{"plan",work.plan},
            {"sensitivity",config.movement.sensitivity},{"yaw_degrees_per_count",config.movement.yaw_degrees_per_count},
            {"success",result.outcome == Outcome::COMPLETED},{"cleanup_known",result.cleanup_known},
            {"canceled",result.outcome == Outcome::CANCELED},{"sent_dx",result.sent_dx},
            {"planned_duration_ms",duration_ms(plan)},{"actual_duration_with_cleanup_ms",result.elapsed_ms},
            {"error",result.error},{"physical_output",true},{"physical_validation_passed",false}};
        try { write_document(work.directory/"result-index.json",index); }
        catch (...) { throw DebugRunFailure(std::string(timeline_name(work.request.mode)) + "结果保存失败",false,result.cleanup_known); }
        auto completed = std::make_shared<const Json>(std::move(index));
        update([&](Snapshot& s) {
            s.result = completed; s.cleanup_unknown = !result.cleanup_known;
            s.state = s.cleanup_unknown ? State::CLEANUP_UNKNOWN : result.outcome == Outcome::COMPLETED ?
                State::COMPLETED : result.outcome == Outcome::CANCELED ? State::CANCELED : State::FAILED;
            const auto prefix = std::string(timeline_name(work.request.mode));
            s.message = s.cleanup_unknown ? prefix + "输入释放未确认，禁止新物理任务" :
                result.outcome == Outcome::COMPLETED ? prefix + "执行完成并已解除屏蔽；游戏效果需人工核对" :
                result.error.empty() ? prefix + "已取消并清理" : result.error;
        });
    }
    void run(Prepared work, bool allow, const std::string& confirmation) {
        if (timeline_mode(work.request.mode)) { run_movement(work); return; }
        if (recoil_mode(work.request.mode)) {
            update([&](Snapshot& s) { s.state=State::RUNNING;s.report_directory=(work.directory/"run").string();
                s.message="等待源端聚焦并松开键鼠，然后执行一组；紧急停止键可取消"; });
            auto result=run_recoil_debug(work.plan,work.context.config,work.context.device,work.directory/"run",canceled,{},
                [this](const std::string& message){
                    if(view.load()->message!=message)update([&](Snapshot& s){s.message=message;});
                });
            write_document(work.directory/"result-index.json",result);
            auto completed=std::make_shared<const Json>(std::move(result));
            update([&](Snapshot& s) {s.result=completed;s.cleanup_unknown=!completed->value("cleanup_known",false);
                s.state=s.cleanup_unknown?State::CLEANUP_UNKNOWN:canceled?State::CANCELED:
                    completed->value("success",false)?State::COMPLETED:State::FAILED;
                s.message=completed->value("message",std::string("本组弹道任务结束；采集与软件回执不代表真实校准通过"));});
            return;
        }
        DebugRunRequest task;
        task.mode = run_mode(work.request.mode);
        task.plan = work.plan; task.sampling_settings = work.sampling;
        task.config = work.context.config; task.device = uses_device(work.request.mode) ? work.context.device : nullptr;
        task.input = std::filesystem::u8path(work.request.input_path);
        task.output = work.directory / "run";
        task.recording_duration_ms = work.request.recording_duration_ms;
        task.candidate_index = static_cast<std::size_t>(work.request.candidate_index);
        task.allow_physical_output = allow; task.confirmation = confirmation;
        // GUI窗口独立于每组任务存活；新组只重置模型，绝不借换组销毁常驻窗口。
        if (uses_device(work.request.mode)) {
            std::uint64_t reset_close_sequence = 0;
            try {
                task.hud = ensure_hud();
                reset_close_sequence = task.hud->close_sequence();
                const bool model_enabled = work.request.mode != Mode::FIRE_TEST;
                if (!task.hud->begin_session(parse_sampling_settings(work.sampling),
                        work.request.mode == Mode::MANUAL_RECORDING,model_enabled))
                    throw std::runtime_error("HUD状态重置失败");
                task.hud->set_visible(hud_requested.load());
            } catch (...) {
                if (task.hud && task.hud->close_sequence() != reset_close_sequence) canceled = true;
                task.hud.reset();
                update([](Snapshot& s) { s.hud_message = "HUD不可用；物理测试仍按原有输入与时序检查执行"; });
                if (work.request.mode == Mode::MANUAL_RECORDING)
                    throw DebugRunFailure("人工模型录制需要可用的原生HUD反馈通道；未开始录制",true);
            }
        }
        struct EndHudSession {
            std::shared_ptr<CounterpulseHud> hud;
            ~EndHudSession() { if (hud) hud->end_session(); }
        } end_hud{task.hud};
        update([&](Snapshot& s) { s.state = State::RUNNING; s.report_directory = task.output.string();
            s.message = "任务执行中；可随时停止"; });
        Json result;
        try { result = run_debug(task, {[this] { return canceled.load(); }, [this](const Json& value) {
            // 原生端只发布阶段或有界事件；最终完整对象仅一次进入结果快照。
            auto published = std::make_shared<const Json>(value);
            update([&](Snapshot& s) {
                s.live = published;
                const auto stage = value.value("stage",std::string{});
                if (stage == "READINESS") {
                    const auto readiness = value.value("readiness",Json::object());
                    const auto reason = readiness.value("reason",std::string{});
                    const auto seconds = readiness.value("elapsed_ns",std::int64_t{0}) / 1000000000;
                    s.message = "等待就绪（" + std::to_string(seconds) + "/15秒）：";
                    if (reason == "SOURCE_UNAVAILABLE") s.message += "源焦点服务不可用，请检查源端入口与连接";
                    else if (reason == "SOURCE_NOT_FOCUSED") s.message += "请将源端游戏切到前台";
                    else if (reason == "PHYSICAL_KEYS_HELD") s.message += "请松开WASD和鼠标按键（含测试快捷键）";
                    else if (reason == "READY") s.message += "已就绪，即将执行本组";
                    else if (reason == "STABILIZING") s.message += "保持键鼠松开，正在确认连续就绪";
                    else s.message += "检查源端与设备输入状态：" + reason;
                } else if (!stage.empty()) s.message = "任务阶段：" + stage + "；可点击停止当前调试任务";
            });
        }}); } catch (const DebugRunFailure& error) {
            if (uses_device(work.request.mode) && !error.output_not_started)
                update([](Snapshot& s) { s.cleanup_unknown = true; });
            throw;
        } catch (...) {
            if (uses_device(work.request.mode)) update([](Snapshot& s) { s.cleanup_unknown = true; });
            throw;
        }
        const bool physical = physical_mode(work.request.mode);
        bool cleanup_ok = !physical;
        if (physical && result.contains("cleanup")) {
            const auto& cleanup = result.at("cleanup");
            cleanup_ok = cleanup.value("button_disposition",-1) == static_cast<int>(ButtonDisposition::ACKNOWLEDGED) &&
                cleanup.contains("keyboard") && cleanup.at("keyboard").value("disposition","") == "ACKNOWLEDGED" &&
                !task.device->left_button_cleanup_required();
        }
        const bool archive_ok = !result.contains("archive") || result.at("archive").value("success",false);
        const bool success = result.value("task_success", result.value("success",true)) && archive_ok;
        if (result.contains("archive") && result.at("archive").value("status","") == "STOP_TIMEOUT") cleanup_ok = false;
        // 唯一采集owner已经排空并返回后才结束订阅；未知归档不重置其冻结水位。
        if (uses_device(work.request.mode) && cleanup_ok && archive_ok)
            task.device->set_input_report_subscription(false);
        Json index{{"schema_version",1},{"prepared_id",work.id},{"generation",view.load()->generation},
            {"code_identity",code_identity()},
            {"parent_run",work.request.input_path},{"plan",work.plan},{"sampling",result.value("settings",work.sampling)},
            {"physical_output",physical},{"physical_validation_passed",false},{"cleanup_known",cleanup_ok},
            {"success",success},{"cancel_requested",canceled.load()},{"report_directory",task.output.string()}};
        try {
            write_document(work.directory / "result-index.json",index);
        } catch (...) {
            // 保存失败不改变先前校验的设备清理事实；任务仍失败并撤销重复执行许可。
            throw DebugRunFailure("结果索引保存失败；本组未完成，请检查原生报告", false, cleanup_ok);
        }
        auto completed = std::make_shared<const Json>(std::move(result));
        update([&](Snapshot& s) {
            s.result = completed;
            if (!cleanup_ok) { s.state = State::CLEANUP_UNKNOWN; s.cleanup_unknown = true; s.message = "释放未确认，禁止新物理任务"; }
            else if (canceled || !success) {
                s.state = canceled || completed->value("failure","") == "USER_STOP" ? State::CANCELED : State::FAILED;
                s.message = completed->value("failure","") == "MOVEMENT_WINDOW_UNAVAILABLE" ?
                    "武器间隔内已无移动余量；本组停止，请增大提交间隔或缩短按住时间后重新准备" : "任务已结束；请查看取消或失败记录";
            }
            else { s.state = State::COMPLETED; s.message = "任务完成；自动结果不代表真实停稳或子弹数"; }
            if (completed->contains("candidate_plan")) s.plan = completed->at("candidate_plan");
            if (completed->contains("plan")) s.plan = completed->at("plan");
            if (success && work.request.mode == Mode::DERIVE_PLAN && completed->contains("candidate_plan")) {
                s.draft_plan = completed->at("candidate_plan"); s.draft_plan_mode = Mode::COUNTERPULSE;
                ++s.draft_plan_revision;
            }
            if (completed->contains("sampling_settings")) s.sampling = completed->at("sampling_settings");
            if (completed->contains("settings")) s.sampling = completed->at("settings");
            if (completed->contains("sampling_analysis")) s.live = std::make_shared<const Json>(completed->at("sampling_analysis"));
        });
    }
};

Session::Session() : impl_(std::make_unique<Impl>()) { Log::register_module("debug_session",LogLevel::INFO); }
Session::~Session() {
    impl_->canceled = true;
    if (impl_->worker.valid()) impl_->worker.wait();
    impl_->hud.store(nullptr);
}
std::shared_ptr<const Snapshot> Session::snapshot() const noexcept { return impl_->view.load(); }
bool Session::busy() const noexcept {
    if (impl_->voice_load.valid()) return true;
    if (impl_->busy()) return true;
    if (!impl_->shutting_down) return false;
    std::lock_guard lock(impl_->hud_mutex);
    return static_cast<bool>(impl_->hud.load());
}
void Session::poll() noexcept {
    try {
        impl_->check_posture_cleanup();
        if (impl_->shutting_down) finish_voice(Context{},false);
        if (impl_->worker.valid() && impl_->worker.wait_for(0ms) == std::future_status::ready) impl_->worker.get();
        {
            std::lock_guard lock(impl_->hud_mutex);
            if (auto hud = impl_->hud.load()) {
                const auto close_sequence = hud->close_sequence();
                if (close_sequence != impl_->observed_hud_close_sequence) {
                    impl_->observed_hud_close_sequence = close_sequence;
                    if (snapshot()->busy) impl_->canceled = true;
                }
                if (hud->closed()) impl_->hud_requested = false;
                const bool requested = impl_->hud_requested.load();
                const bool visible = hud->visible();
                const std::string feedback = hud->failed() ? "HUD窗口创建或显示失败；显示故障不替代设备执行结果" :
                    hud->closed() ? "HUD已关闭；运行中的任务正在响应停止请求" : visible ? "HUD已显示；结束后保留" :
                    requested ? "正在显示HUD" : "HUD已隐藏；隐藏不会停止任务";
                if (snapshot()->hud_visible != visible || snapshot()->hud_requested != requested || snapshot()->hud_message != feedback)
                    impl_->update([&](Snapshot& s) { s.hud_visible = visible; s.hud_requested = requested; s.hud_message = feedback; });
                if (snapshot()->busy && hud->task_active())
                    if (auto live = hud->latest_analysis(); live && live != snapshot()->live)
                        impl_->update([&](Snapshot& s) { s.live = live; });
            }
        }
        if (impl_->shutting_down && !impl_->busy() && impl_->hud.load()) {
            impl_->launch(State::STOPPING,false,[this] {
                impl_->replace_hud();
                impl_->update([](Snapshot& s) { s.state = State::CANCELED; s.hud_visible = false; s.message = "后台资源已回收"; });
            });
        }
    } catch (...) {}
}
void Session::set_theme(UiTheme theme) noexcept {
    impl_->hud_theme = theme;
    std::lock_guard lock(impl_->hud_mutex);
    if (auto hud = impl_->hud.load()) hud->set_theme(theme);
}
void Session::request_shutdown() noexcept { impl_->shutting_down = true; cancel("应用关闭，正在清理"); poll(); }
void Session::cancel(const std::string& reason) noexcept {
    impl_->canceled = true;
    invalidate_repeat("测试模板已取消：" + reason);
    try {
        impl_->update([&](Snapshot& s) {
            if (s.busy) { s.state = State::STOPPING; s.message = reason; }
            else if (s.state == State::PREPARED) { s.state = State::CANCELED; s.prepared_id.clear(); s.message = "准备已取消"; }
        });
    } catch (...) {}
}

void Session::invalidate_repeat(const std::string& reason) noexcept {
    try { impl_->invalidate_repeat(reason); } catch (...) {}
}
bool Session::update_movement_draft(const Request& request, const Context& context, bool manual) noexcept {
    if (impl_->shutting_down || !timeline_mode(request.mode)) return false;
    const auto owns_test = [&] { const auto s = snapshot(); return s->movement_test_enabled && s->movement_test_mode == request.mode; };
    try {
        const auto plan = request_plan(request);
        const bool changed = current_plan(*snapshot(),request.mode) != plan;
        if (changed && request.mode == Mode::GRENADE_TEST) impl_->invalidate_grenade("道具内容已改变，请保存后重新选择与对齐");
        if (manual && changed && owns_test()) ++impl_->repeat_revision;
        impl_->update([&](Snapshot& s) {
            current_plan(s,request.mode) = plan;
            if (manual && changed && s.movement_test_mode == request.mode) s.movement_voice_selected = false;
        });
        // 编辑另一页只更新该页草稿，不能接管或撤销当前F11模板。
        if (!owns_test()) return false;
        const auto& binding = impl_->movement_binding;
        if (!binding || binding->device != context.device || !runtime_available(context,request.mode) || !context.input_recording_idle ||
            !context.cleanup_known || !context.config.mouse.allow_send_input ||
            binding->config.keyboard.debug_test_virtual_keys != context.config.keyboard.debug_test_virtual_keys ||
            binding->config.keyboard.emergency_virtual_keys != context.config.keyboard.emergency_virtual_keys) {
            invalidate_repeat("测试运行环境已改变，请重新启用"); return false;
        }
        // UI回填同一份已载入草稿不能撤销刚完成的对齐，也不创建新模板版本。
        if (const auto prior = impl_->repeat_plan.load(); request.mode == Mode::GRENADE_TEST && !changed && prior &&
            prior->request.mode == request.mode && prior->repeat_revision == impl_->repeat_revision.load() &&
            prior->plan == plan) return true;
        const auto revision = ++impl_->repeat_revision;
        impl_->repeat_plan.store(nullptr);
        impl_->update([](Snapshot& s) { s.repeat_ready = false; s.repeat_unavailable_reason = "当前草稿无效，请修正参数"; });
        Impl::Prepared work{request,context,plan,Json::object(),"timeline-session-" + std::to_string(revision),{}};
        work.repeat_revision = revision;
        impl_->repeat_plan.store(std::make_shared<const Impl::Prepared>(std::move(work)));
        impl_->update([&](Snapshot& s) {
            if (s.movement_test_enabled && s.movement_test_mode == request.mode) {
                s.repeat_ready = true; s.repeat_unavailable_reason.clear();
                if (!s.busy) s.message = std::string(timeline_name(request.mode)) + "测试已启用，修改参数后可直接测试";
            } else impl_->repeat_plan.store(nullptr);
        });
        return owns_test();
    } catch (...) {
        const bool owner = owns_test();
        if (owner) { ++impl_->repeat_revision; impl_->repeat_plan.store(nullptr); }
        try { impl_->update([&](Snapshot& s) {
            current_plan(s,request.mode) = Json();
            if (s.movement_test_mode == request.mode) s.movement_voice_selected = false;
            if (owner) { s.repeat_ready = false; s.repeat_unavailable_reason = "当前草稿无效，请修正参数"; }
        }); } catch (...) {}
        return false;
    }
}
void Session::update_map(const weapon::MapSnapshot& map) noexcept {
    try {
        const auto prior = snapshot();
        if (prior->movement_map.context_epoch == map.context_epoch && prior->movement_map.valid == map.valid) return;
        // 地图刷新只撤销已有图像引导，不连带打断人工F11时间线。
        if (prior->grenade_guidance_state != "hidden" || impl_->grenade_assisted_operation.load())
            impl_->invalidate_grenade("地图上下文已变化，请重新选择道具");
        if (prior->movement_voice_selected && (!map.valid || prior->movement_voice_epoch != map.context_epoch)) {
            ++impl_->repeat_revision; impl_->repeat_plan.store(nullptr);
            impl_->update([](Snapshot& s) { s.repeat_ready = false; s.repeat_unavailable_reason = "语音方案的地图已失效，请重新选择"; });
        }
        impl_->update([&](Snapshot& s) { s.movement_map = map; });
    } catch (...) {}
}
bool Session::select_voice(const Request& request, const Context& context) noexcept {
    try {
        const auto s = snapshot();
        if (busy() || impl_->voice_load.valid() || impl_->shutting_down || !timeline_mode(request.mode) ||
            s->movement_test_mode != request.mode || !s->movement_test_enabled || !context.map.valid ||
            current_plan(*s,request.mode) != baseline_plan(*s,request.mode) || !runtime_available(context,request.mode) || !context.input_recording_idle ||
            !context.cleanup_known || !context.config.mouse.allow_send_input) return false;
        admit(context, request.mode);
        if (request.mode == Mode::GRENADE_TEST) impl_->invalidate_grenade("正在选择新的道具方案");
        impl_->voice_request = request;
        impl_->voice_revision = impl_->repeat_revision.load();
        impl_->voice_epoch = context.map.context_epoch;
        impl_->voice_grenade_epoch = impl_->grenade_epoch.load();
        impl_->voice_load = std::async(std::launch::async, [request] {
            auto r = request;
            r.plan_text = read_document(std::filesystem::u8path(request.load_path),16*1024*1024).dump();
            return request_plan(r);
        });
        return true;
    } catch (...) { return false; }
}
bool Session::select_grenade(const Request& request, const Context& context) noexcept {
    try {
        if (request.mode!=Mode::GRENADE_TEST || busy() || impl_->voice_load.valid() || impl_->shutting_down ||
            !context.config.keyboard.grenade_assist_enabled || !context.map.valid ||
            !context.config.mouse.allow_send_input) return false;
        admit(context,Mode::GRENADE_TEST);
        const auto prior=snapshot();
        if (prior->grenade_current!=prior->grenade_baseline) return false;
        impl_->invalidate_repeat("手机正在选择道具方案");
        impl_->movement_binding=context;
        impl_->update([](Snapshot& s){s.movement_test_enabled=true;s.movement_test_mode=Mode::GRENADE_TEST;});
        return select_voice(request,context);
    } catch (...) { return false; }
}
std::optional<bool> Session::finish_voice(const Context& context, bool still_current) noexcept {
    if (!impl_->voice_load.valid() || impl_->voice_load.wait_for(0ms) != std::future_status::ready) return {};
    try {
        const auto plan = impl_->voice_load.get();
        const auto s = snapshot();
        const auto& binding = impl_->movement_binding;
        const auto mode = impl_->voice_request.mode;
        if (!still_current || impl_->shutting_down || !s->movement_test_enabled || s->movement_test_mode != mode ||
            (mode == Mode::GRENADE_TEST && impl_->voice_grenade_epoch != impl_->grenade_epoch.load()) ||
            impl_->voice_revision != impl_->repeat_revision.load() || !context.map.valid ||
            impl_->voice_epoch != context.map.context_epoch || plan.value("map",std::string{}) != context.map.map_name ||
            !runtime_available(context,mode) || !context.input_recording_idle || !context.cleanup_known ||
            !context.config.mouse.allow_send_input || !binding || binding->device != context.device ||
            binding->config.keyboard.debug_test_virtual_keys != context.config.keyboard.debug_test_virtual_keys ||
            binding->config.keyboard.emergency_virtual_keys != context.config.keyboard.emergency_virtual_keys ||
            current_plan(*s,mode) != baseline_plan(*s,mode)) return false;
        admit(context, mode);
        auto request = impl_->voice_request; request.plan_text = plan.dump();
        if (!update_movement_draft(request,context,false)) return false;
        impl_->update([&](Snapshot& next) {
            next.draft_plan = plan; next.draft_plan_mode = mode; ++next.draft_plan_revision;
            publish_loaded_plan(next,mode,plan);
            baseline_plan(next,mode) = plan; current_plan(next,mode) = plan;
            auto& path = mode == Mode::GRENADE_TEST ? next.grenade_selected_path : next.movement_selected_path;
            auto& revision = mode == Mode::GRENADE_TEST ? next.grenade_catalog_revision : next.movement_catalog_revision;
            path = request.load_path; ++revision;
            next.movement_voice_selected = true; next.movement_voice_epoch = context.map.context_epoch;
            next.message = "语音方案已载入，下一次测试使用当前方案";
        });
        if (mode == Mode::GRENADE_TEST) impl_->guidance("selected","请按站位图定位，拿出道具后对齐",request.load_path);
        return true;
    } catch (...) { return false; }
}
void Session::cancel_grenade(const std::string& reason) noexcept {
    try {
        const auto s = snapshot();
        if (s->movement_test_mode != Mode::GRENADE_TEST) return;
        const bool owns_worker = impl_->grenade_operation.load();
        if (s->busy && s->physical && !owns_worker) return;
        impl_->invalidate_grenade(reason);
        // 局部取消只结束道具任务，保留当前已启用方案；全局cancel另行撤销许可。
        impl_->update([&](Snapshot& next) {
            if (!next.cleanup_unknown) next.message = reason;
            if (next.busy && owns_worker) next.state = State::STOPPING;
        });
    } catch (...) {}
}
void Session::maintain_grenade(const Context& context, bool input_healthy) noexcept {
    try {
        impl_->check_posture_cleanup();
        const auto s = snapshot();
        // F11人工试投先隐藏引导，不依赖图像辅助开关、GSI或参考图。
        if (s->grenade_guidance_state == "hidden" && !impl_->grenade_assisted_operation.load() &&
            !impl_->aligned_grenade.load()) return;
        if (!s->movement_test_enabled || s->movement_test_mode != Mode::GRENADE_TEST ||
            !context.config.keyboard.grenade_assist_enabled || !input_healthy || !runtime_available(context,Mode::GRENADE_TEST) ||
            !context.input_recording_idle || !context.cleanup_known || !context.config.mouse.allow_send_input ||
            !context.map.valid || s->grenade_current.value("map",std::string{}) != context.map.map_name ||
            !impl_->movement_binding || impl_->movement_binding->device != context.device) {
            cancel_grenade("道具辅助上下文已失效，请重新选择与对齐");
            return;
        }
        const auto ready = impl_->aligned_grenade.load();
        if (ready && (ready->epoch != impl_->grenade_epoch.load() ||
             ready->context.map.context_epoch != context.map.context_epoch ||
             ready->repeat_revision != impl_->repeat_revision.load() ||
             ready->context.config.aim.counts_per_pixel_x != context.config.aim.counts_per_pixel_x ||
             ready->context.config.aim.counts_per_pixel_y != context.config.aim.counts_per_pixel_y)) {
            cancel_grenade("对齐参数或地图已改变，请重新对齐");
            return;
        }
        if (ready && ready->posture && !ready->posture->valid()) {
            cancel_grenade("源端焦点或蹲姿控制已失效，已释放蹲姿，请重新找点"); return;
        }
        // 只在等待下键时响应人工移动；执行中的W/SPACE由同一时间线管理，不能误判冲突。
        if (ready && ready->posture && !impl_->grenade_operation.load()) {
            InputSnapshot input;
            if (!context.device->poll_input(input) || !input.state_valid) {
                cancel_grenade("等待投掷时设备输入失效，已释放蹲姿"); return;
            }
            for (int key : {0x57,0x41,0x53,0x44,0x20,0x01,0x02})
                if (input.virtual_keys[static_cast<std::size_t>(key)]) {
                    cancel_grenade("人工移动或按钮输入取消等待，已释放蹲姿"); return;
                }
        }
    } catch (...) { cancel_grenade("道具辅助上下文不可用"); }
}
bool Session::align_grenade(const Context& context) noexcept {
    try {
        const auto s = snapshot();
        const auto source = impl_->repeat_plan.load();
        if (busy() || impl_->shutting_down) return false;
        if (!context.config.keyboard.grenade_assist_enabled || !s->movement_test_enabled ||
            s->movement_test_mode != Mode::GRENADE_TEST || !source ||
            source->request.mode != Mode::GRENADE_TEST || source->repeat_revision != impl_->repeat_revision.load() ||
            s->grenade_selected_path.empty() || s->grenade_current != s->grenade_baseline ||
            !context.map.valid || s->grenade_current.value("map",std::string{}) != context.map.map_name)
            throw std::runtime_error("请先启用道具辅助，保存并选择当前地图方案");
        admit(context,Mode::GRENADE_TEST);
        if (!context.config.mouse.allow_send_input || !source->context.config.mouse.allow_send_input ||
            source->context.device != context.device) throw std::runtime_error("道具输出许可或设备已变化，请重新启用");
        impl_->aligned_grenade.store(nullptr);
        impl_->check_posture_cleanup();
        if (snapshot()->cleanup_unknown) throw std::runtime_error("蹲姿释放未确认，不能开始新的对齐");
        const auto epoch = ++impl_->grenade_epoch;
        const auto revision = impl_->repeat_revision.load();
        const auto path = s->grenade_selected_path;
        impl_->guidance("aligning","正在对齐；左键取消",path);
        return impl_->launch(State::RUNNING,true,[this,context,source,epoch,revision,path] {
            std::shared_ptr<RuntimeGrenadeControl> control;
            std::shared_ptr<detail::GrenadePosture> posture;
            const auto permitted = [this,epoch,revision,&control,&posture] {
                return !impl_->canceled.load() && impl_->grenade_epoch.load() == epoch &&
                    impl_->repeat_revision.load() == revision && (!control || control->valid()) &&
                    (!posture || posture->valid());
            };
            try {
                const auto reference = grenade_alignment::load_reference(std::filesystem::u8path(path),source->plan);
                control=borrow_grenade_control(context,impl_->canceled);
                if (!permitted()) return;
                const bool crouch = source->plan.value("crouch",false);
                if (crouch) {
                    if (!control) throw std::runtime_error("蹲姿自动找点需要先启动Runtime，以保持同一输入控制链");
                    const auto fault = impl_->grenade_posture_fault;
                    posture = std::make_shared<detail::GrenadePosture>(context.device,
                        [control]{return control->source_snapshot();},[control]{return control->valid();},
                        [control,fault](bool known){if(!known) fault->store(true);control->finish(known);});
                    if (!posture->start()) throw std::runtime_error("未能建立可信蹲姿，请确认游戏聚焦和设备状态");
                }
                const auto directory = new_directory(source->request.output_root);
                impl_->update([&](Snapshot& next){next.report_directory=directory.string();});
                auto stream = shared_stream(control);
                stream.settle_before_alignment = crouch;
                const auto result = grenade_alignment::run(reference,context.config,context.device,impl_->canceled,false,permitted,std::move(stream));
                if (result.emergency_stop) cancel("急停键已取消对齐并撤销测试许可");
                auto evidence = result.evidence;
                evidence["code_identity"] = code_identity(); evidence["plan"] = source->plan;
                evidence["reference_revision"] = reference.revision;
                evidence["aligned"] = result.aligned; evidence["canceled"] = result.canceled;
                evidence["error"] = result.error; evidence["physical_validation_passed"] = false;
                write_document(directory/"grenade-alignment.json",evidence);
                if (impl_->grenade_epoch.load() != epoch) return;
                const bool aligned = result.aligned && permitted();
                if (aligned) {
                    auto published = std::make_shared<const Impl::AlignedGrenade>(
                        Impl::AlignedGrenade{epoch,revision,reference.revision,context,posture?control:nullptr,posture});
                    impl_->aligned_grenade.store(published);
                    // 取消可穿过最后一次许可检查；发布后再核对，只撤销本次持蹲所有权。
                    if (!permitted()) {
                        impl_->aligned_grenade.compare_exchange_strong(published,{});
                        return;
                    }
                }
                impl_->guidance(aligned?"aligned":result.canceled?"hidden":"failed",
                    aligned?"对齐完成，按下键投掷":result.error,{},epoch);
                impl_->update([&](Snapshot& next) {
                    if (impl_->grenade_epoch.load() != epoch) return;
                    next.state = result.canceled ? State::CANCELED : aligned ? State::COMPLETED : State::FAILED;
                    next.message = aligned ? "对齐完成，等待下键；未执行投掷" : result.error;
                    next.result = std::make_shared<const Json>(evidence);
                });
            } catch (const std::exception& error) {
                if (impl_->grenade_epoch.load() != epoch) return;
                impl_->guidance("failed",error.what(),{},epoch);
                impl_->update([&](Snapshot& next){if(impl_->grenade_epoch.load()==epoch){next.state=State::FAILED;next.message=error.what();}});
            }
        },true,true);
    } catch (const std::exception& error) {
        try {
            impl_->invalidate_grenade(error.what());
            if (snapshot()->cleanup_unknown) return false;
            impl_->guidance("failed",error.what(),snapshot()->grenade_selected_path);
            impl_->update([&](Snapshot& s){s.message=error.what();});
        } catch (...) {}
        return false;
    } catch (...) { return false; }
}
bool Session::throw_grenade(const Context& context) noexcept {
    try {
        const auto ready = impl_->aligned_grenade.load();
        const auto source = impl_->repeat_plan.load();
        const auto s = snapshot();
        if (busy() || impl_->shutting_down) return false;
        if (!ready || !source || !context.config.keyboard.grenade_assist_enabled ||
            !s->movement_test_enabled || s->movement_test_mode != Mode::GRENADE_TEST ||
            s->grenade_guidance_state != "aligned" || ready->epoch != impl_->grenade_epoch.load() ||
            ready->repeat_revision != impl_->repeat_revision.load() ||
            !context.map.valid || ready->context.map.context_epoch != context.map.context_epoch ||
            source->plan.value("map",std::string{}) != context.map.map_name)
            throw std::runtime_error("尚未完成当前道具对齐，请先按上键");
        admit(context,Mode::GRENADE_TEST);
        if (!context.config.mouse.allow_send_input || context.device != ready->context.device)
            throw std::runtime_error("输出许可或设备已改变，请重新对齐");
        const auto path = s->grenade_selected_path;
        impl_->aligned_grenade.store(nullptr); // 下键只消费一次，重复按键不排队。
        impl_->guidance("hidden","正在复核并投掷");
        auto work = *source; work.context = context;
        return impl_->launch(State::RUNNING,true,[this,ready,path,work=std::move(work)]() mutable {
            std::shared_ptr<RuntimeGrenadeControl> control;
            struct FinishPosture {
                std::shared_ptr<detail::GrenadePosture> posture;
                ~FinishPosture(){if(posture) posture->finish();}
            } finish_posture{ready->posture};
            const auto permitted = [this,ready,&control] {
                return !impl_->canceled.load() && impl_->grenade_epoch.load()==ready->epoch &&
                    impl_->repeat_revision.load()==ready->repeat_revision && (!control || control->valid()) &&
                    (!ready->posture || ready->posture->valid());
            };
            Json check_evidence;
            auto checked_at = std::chrono::steady_clock::time_point{};
            bool entered_execution = false;
            bool execution_returned = false;
            try {
                control=ready->posture ? ready->control : borrow_grenade_control(work.context,impl_->canceled);
                if (!permitted()) throw DebugRunFailure("蹲姿或道具上下文已失效，请重新找点",true);
                if (control) work.context.output_permitted=[control]{return control->valid();};
                work.directory = new_directory(work.request.output_root);
                const auto parent_id = work.id;
                work.id = work.directory.filename().string();
                write_document(work.directory/"prepare.json",{{"schema_version",1},{"prepared_id",work.id},
                    {"repeat_of",parent_id},{"trigger","GRENADE_ALIGN_THROW"},{"code_identity",code_identity()},
                    {"plan",work.plan},{"sampling",work.sampling},{"physical_output",true}});
                write_document(work.directory/timeline_report(work.request.mode),work.plan);
                write_document(work.directory/"sampling-settings.json",work.sampling);
                impl_->update([&](Snapshot& next){next.report_directory=work.directory.string();next.prepared_id=work.id;});
                entered_execution = true;
                impl_->run_movement(work,[&] {
                    const auto reference = grenade_alignment::load_reference(std::filesystem::u8path(path),work.plan);
                    if (reference.revision != ready->reference_revision || !permitted())
                        throw DebugRunFailure("参考资源或上下文已改变，请重新对齐",true);
                    const auto check = grenade_alignment::run(reference,work.context.config,work.context.device,impl_->canceled,true,permitted,shared_stream(control));
                    if (check.emergency_stop) cancel("急停键已取消投掷并撤销测试许可");
                    check_evidence = check.evidence;
                    if (!check.aligned || !permitted()) throw DebugRunFailure(
                        check.error.empty()?"当前画面已偏离瞄点，请重新对齐":check.error,true);
                    checked_at = check.verified_at;
                },[&] {
                    return permitted() && checked_at != std::chrono::steady_clock::time_point{} &&
                        std::chrono::steady_clock::now()-checked_at < 200ms;
                });
                execution_returned = true;
                if (control && snapshot()->cleanup_unknown) control->finish(false);
                if (ready->posture) ready->posture->finish();
                // 最后一帧判定与动作开始之间不做文件IO；报告在动作清理之后落盘。
                write_document(work.directory/"grenade-prethrow-check.json",check_evidence);
                const auto result = snapshot();
                if (control && result->cleanup_unknown) control->finish(false);
                if (result->state == State::FAILED || result->state == State::CLEANUP_UNKNOWN)
                    impl_->guidance("failed",result->message,{},ready->epoch);
                // 完成后持续隐藏；轮询端即使错过投掷中间态，也不能恢复已消费的提示。
                else impl_->guidance("hidden",
                    result->state==State::CANCELED?"本次投掷已取消":"投掷动作已完成；下次先按上键找点",{},ready->epoch);
            } catch (const DebugRunFailure& error) {
                if (control && !error.output_not_started && !error.cleanup_confirmed) control->finish(false);
                if (!check_evidence.is_null()) { try { write_document(work.directory/"grenade-prethrow-check.json",check_evidence); } catch (...) {} }
                impl_->guidance("failed",error.what(),{},ready->epoch);
                throw;
            } catch (const std::exception& error) {
                if (control && entered_execution && !(execution_returned && !snapshot()->cleanup_unknown)) control->finish(false);
                impl_->guidance("failed",error.what(),{},ready->epoch);
                // 执行后的写报告异常不得伪装成零输出；只继承已经确认的清理事实。
                throw DebugRunFailure(error.what(),!entered_execution,
                    execution_returned && !snapshot()->cleanup_unknown);
            }
        },true,true);
    } catch (const std::exception& error) {
        try {
            impl_->invalidate_grenade(error.what());
            if (snapshot()->cleanup_unknown) return false;
            impl_->guidance("failed",error.what(),snapshot()->grenade_selected_path);
            impl_->update([&](Snapshot& s){s.message=error.what();});
        } catch (...) {}
        return false;
    } catch (...) { return false; }
}
bool Session::repeat(const Context& context) noexcept { return run_repeat(context,true); }
bool Session::run_repeat(const Context& context, bool keyboard_trigger) noexcept {
    try {
        const auto reject = [&](const char* message) {
            impl_->update([&](Snapshot& s) { s.message = message; }); return false;
        };
        if (impl_->shutting_down) return reject("应用正在关闭，快捷键测试未执行");
        if (busy()) return reject("已有调试任务正在执行，本次快捷键忽略，不排队");
        if (snapshot()->movement_voice_selected && (!context.map.valid ||
            context.map.context_epoch != snapshot()->movement_voice_epoch))
            return reject("语音方案地图已失效，请重新选择");
        if (keyboard_trigger && !context.config.keyboard.debug_test_enabled) return reject("调试测试快捷键开关未启用");
        const auto source = impl_->repeat_plan.load();
        if (!keyboard_trigger && (!source || !timeline_mode(source->request.mode) || !snapshot()->movement_test_enabled ||
            snapshot()->movement_test_mode != source->request.mode))
            return reject("请先启用当前页面测试并修正参数");
        if (!source || source->repeat_revision != impl_->repeat_revision.load() || !physical_mode(source->request.mode)) {
            const auto current=snapshot();
            // 无许可重按不能覆盖上组失败；保留原始原因供用户决定如何重新准备。
            if(current->state==State::FAILED || current->state==State::CLEANUP_UNKNOWN ||
                current->state==State::CANCELED) return false;
            if(current->state==State::COMPLETED && current->result &&
                current->result->value("mode",std::string{})=="capture" &&
                !current->result->value("candidate_path",std::string{}).empty())
                return reject("初始候选已生成；请回压枪页核对并保存候选，再测试或推进阶段，无需重复采集");
            if (!current->repeat_unavailable_reason.empty())
                return reject(current->repeat_unavailable_reason.c_str());
            return reject("没有有效测试模板，请先在调试页重新准备");
        }
        admit(context,source->request.mode);
        if (snapshot()->cleanup_unknown || context.device != source->context.device)
            return reject("设备或清理状态变化，请重新准备");
        if (!context.config.mouse.allow_send_input || !source->context.config.mouse.allow_send_input)
            return reject("设置中的物理输出未允许，请保存后重新准备");
        if ((recoil_mode(source->request.mode) || timeline_mode(source->request.mode)) &&
            (context.config.keyboard.debug_test_virtual_keys != source->context.config.keyboard.debug_test_virtual_keys ||
             context.config.keyboard.emergency_virtual_keys != source->context.config.keyboard.emergency_virtual_keys))
            return reject("测试键或紧急停止键已改变，请重新准备");
        auto work = *source;
        if (work.request.mode == Mode::GRENADE_TEST) work.context=context;
        if (work.request.mode == Mode::GRENADE_TEST) impl_->invalidate_grenade("本次人工投掷消耗旧对齐结果");
        if(single_use_recoil_mode(work.request.mode))invalidate_repeat("本次标定或采集许可已使用，请完成本组后重新准备");
        impl_->prepared.reset();
        const bool retain_grenade = work.request.mode == Mode::GRENADE_TEST;
        return impl_->launch(State::RUNNING,true,[this,work = std::move(work),keyboard_trigger]() mutable {
            std::shared_ptr<RuntimeGrenadeControl> control;
            if (work.request.mode==Mode::GRENADE_TEST) {
                control=borrow_grenade_control(work.context,impl_->canceled);
                if (control) work.context.output_permitted=[control]{return control->valid();};
            }
            work.directory = new_directory(work.request.output_root);
            const auto parent_id = work.id;
            work.id = work.directory.filename().string();
            write_document(work.directory / "prepare.json",{{"schema_version",1},{"prepared_id",work.id},
                {"repeat_of",parent_id},{"trigger",keyboard_trigger ? "USER_HOTKEY" : "USER_BUTTON"},{"code_identity",code_identity()},
                {"plan",work.plan},{"sampling",work.sampling},{"physical_output",true}});
            write_document(work.directory / timeline_report(work.request.mode),work.plan);
            write_document(work.directory / "sampling-settings.json",work.sampling);
            impl_->update([&](Snapshot& s) { s.prepared_id = work.id; s.report_directory = work.directory.string(); });
            try {
                impl_->run(work,true,physical_confirmation());
                if (control && snapshot()->cleanup_unknown) control->finish(false);
            } catch (const DebugRunFailure& error) {
                if (control && !error.output_not_started && !error.cleanup_confirmed) control->finish(false);
                throw;
            } catch (...) { if (control) control->finish(false);throw; }
        },retain_grenade);
    } catch (const std::exception& error) {
        try { impl_->update([&](Snapshot& s) { s.message = error.what(); }); } catch (...) {}
        return false;
    } catch (...) { return false; }
}

bool Session::dispatch(Action action, const Request& request, const Context& context,
    const std::string& prepared_id, bool allow_physical_output, const std::string& confirmation) noexcept {
    try {
        if (action == Action::CANCEL) { cancel(); return true; }
        if (action == Action::STOP_MOVEMENT_RECORDING) {
            if (!snapshot()->movement_recording) return false;
            impl_->movement_record_stop=true; return true;
        }
        if (action == Action::SHOW_HUD || action == Action::HIDE_HUD) {
            if (impl_->shutting_down) return false;
            const bool desired = action == Action::SHOW_HUD;
            impl_->hud_requested = desired;
            set_theme(context.config.ui.theme);
            if (desired) impl_->ensure_hud()->set_visible(true);
            else { std::lock_guard lock(impl_->hud_mutex); if (auto hud = impl_->hud.load()) hud->set_visible(false); }
            impl_->update([&](Snapshot& s) { s.hud_requested = desired;
                s.hud_message = desired ? "正在显示HUD；显示不会连接设备或开始任务" : "HUD已隐藏；任务继续"; });
            return true;
        }
        const auto reject = [&](const char* message) {
            impl_->update([&](Snapshot& s) { s.message = message; }); return false;
        };
        if (action == Action::NONE) return false;
        if (impl_->shutting_down || busy()) return reject("已有任务执行或正在关闭，本次请求未执行");
        poll();
        if (action == Action::START_MOVEMENT_RECORDING) {
            if (request.mode!=Mode::MOVEMENT_TEST || request.movement_record_motion_mode<0 || request.movement_record_motion_mode>2)
                return reject("身法录制参数无效");
            if (!context.input_recording_idle || !context.device || !context.device->output_owner_exclusive() ||
                context.config.mouse.backend!=MouseBackend::KMBOX_NET ||
                (context.device->status()!=MouseStatus::READY && context.device->status()!=MouseStatus::DISABLED))
                return reject("请连接KMBOX NET并结束其他输入录制");
            invalidate_repeat("身法录制中，回放测试已关闭");
            impl_->prepared.reset(); impl_->movement_record_stop=false;
            return impl_->launch(State::RUNNING,false,[this,request,context]{impl_->record_movement(request,context);});
        }
        if (action == Action::ENABLE_MOVEMENT_TEST) {
            if (!timeline_mode(request.mode)) return reject("只有身法或道具页面可以启用动作测试");
            admit(context,request.mode);
            if (snapshot()->cleanup_unknown || !context.config.mouse.allow_send_input)
                return reject("设备清理未确认或设置中的物理输出未允许");
            invalidate_repeat("已切换测试执行域");
            impl_->prepared.reset(); impl_->movement_binding = context;
            impl_->update([&](Snapshot& s) { s.movement_test_enabled = true; s.movement_test_mode = request.mode; });
            update_movement_draft(request,context);
            if (request.mode == Mode::GRENADE_TEST && context.config.keyboard.grenade_assist_enabled &&
                !snapshot()->grenade_selected_path.empty())
                impl_->guidance("selected","请按站位图定位，拿出道具后对齐",snapshot()->grenade_selected_path);
            return true;
        }
        if (action == Action::RUN_MOVEMENT_TEST) {
            if (snapshot()->movement_test_mode != request.mode) return reject("请先启用本页测试；另一页仍持有F11");
            if (!update_movement_draft(request,context)) return reject("请启用当前页面测试并修正草稿");
            return run_repeat(context,false);
        }
        if (action == Action::START) {
            if (!impl_->prepared || snapshot()->state != State::PREPARED || prepared_id != impl_->prepared->id)
                return reject("准备身份已失效，请重新准备后启动");
            auto work = *impl_->prepared;
            const bool physical = physical_mode(work.request.mode);
            if (snapshot()->cleanup_unknown && uses_device(work.request.mode)) return reject("设备清理未知，不能启动新任务");
            admit(context,work.request.mode);
            if (uses_device(work.request.mode) && context.device != work.context.device) return reject("设备连接已改变，请重新准备");
            if (physical && (!allow_physical_output || confirmation != physical_confirmation())) return reject("请勾选允许本次真实物理输出后点击启动");
            if (physical && (!work.context.config.mouse.allow_send_input || !context.config.mouse.allow_send_input)) return reject("设置中的物理输出未允许，请保存后重新准备");
            if (!physical && (allow_physical_output || !confirmation.empty())) return reject("离线及录制任务不接受物理输出授权");
            if(single_use_recoil_mode(work.request.mode))invalidate_repeat("本次标定或采集许可已使用，请完成本组后重新准备");
            impl_->prepared.reset();
            return impl_->launch(State::RUNNING,physical,[this,work,allow_physical_output,confirmation] {
                impl_->run(work,allow_physical_output,confirmation);
            });
        }
        if (snapshot()->cleanup_unknown && uses_device(request.mode)) return false;
        const bool movement_document = timeline_mode(request.mode) &&
            (action == Action::LIST_MOVEMENT_PLANS || action == Action::LOAD_PLAN ||
             action == Action::SAVE_PLAN || action == Action::DELETE_PLAN || action == Action::VALIDATE);
        if (action == Action::DELETE_PLAN) {
            if (request.mode != Mode::GRENADE_TEST || request.load_path.empty() ||
                request.load_path != snapshot()->grenade_selected_path)
                return reject("请先载入要删除的已保存道具方案");
            if (snapshot()->movement_test_mode == Mode::GRENADE_TEST)
                impl_->invalidate_repeat("当前道具正在归档，旧测试和蹲姿已取消");
            else impl_->invalidate_grenade("当前道具正在归档");
        }
        if (!movement_document) {
            impl_->prepared.reset();
            invalidate_repeat("新任务正在准备；旧测试模板已失效");
        } else if (action == Action::LOAD_PLAN && snapshot()->movement_test_mode == request.mode) {
            // 载入期间不留旧参数兜底；页面接收载入草稿后更新下一次测试。
            ++impl_->repeat_revision; impl_->repeat_plan.store(nullptr);
            if (request.mode == Mode::GRENADE_TEST) impl_->invalidate_grenade("正在载入新的道具方案");
            impl_->update([](Snapshot& s) { s.repeat_ready = false; s.repeat_unavailable_reason = "等待载入当前方案"; });
        }
        const auto repeat_revision = impl_->repeat_revision.load();
        if (timeline_mode(request.mode) && action == Action::SAVE_PLAN) {
            const auto current = request_plan(request);
            impl_->update([&](Snapshot& s) { current_plan(s,request.mode) = current; });
            if (request.mode == Mode::GRENADE_TEST) impl_->invalidate_grenade("道具保存后请重新对齐");
        }
        const auto grenade_epoch = impl_->grenade_epoch.load();
        impl_->update([](Snapshot& s) { s.prepared_id.clear(); });
        return impl_->launch(State::WORKING,false,[this,action,request,context,repeat_revision,grenade_epoch] {
            const auto refresh_movement_library = [&](const std::string& selected) {
                const auto entries = detail::list_movement_plans(timeline_library_root(request),request.output_root);
                impl_->update([&](Snapshot& s) {
                    auto& catalog = request.mode == Mode::GRENADE_TEST ? s.grenade_saved_plans : s.movement_saved_plans;
                    auto& path = request.mode == Mode::GRENADE_TEST ? s.grenade_selected_path : s.movement_selected_path;
                    auto& revision = request.mode == Mode::GRENADE_TEST ? s.grenade_catalog_revision : s.movement_catalog_revision;
                    catalog.clear();
                    for (const auto& entry : entries) catalog.push_back({entry.path,entry.name,entry.map,entry.voice_command,entry.raw_name});
                    path = selected; ++revision;
                });
            };
            if (timeline_mode(request.mode) && action == Action::LIST_MOVEMENT_PLANS) {
                refresh_movement_library(request.load_path);
                impl_->update([&](Snapshot& s) { s.state = State::COMPLETED; s.message = std::string("已刷新") + timeline_name(request.mode) + "方案列表"; });
                return;
            }
            if (action == Action::DELETE_PLAN) {
                std::string backup;
                try { backup = detail::archive_movement_plan(request.load_path,timeline_library_root(request)); }
                catch (const std::exception& error) {
                    impl_->update([&](Snapshot& s) {
                        // 文档归档失败不撤销另一页的测试许可；沿用保存失败的非物理反馈语义。
                        s.state = s.cleanup_unknown ? State::CLEANUP_UNKNOWN : State::COMPLETED;
                        s.message = error.what();
                    });
                    return;
                }
                refresh_movement_library({});
                movement::timeline::Plan reset;
                reset.name = "新道具方案";
                if (context.map.valid) {
                    reset.map = context.map.map_name;
                    if (context.map.local_team != weapon::Team::UNKNOWN)
                        reset.team = weapon::team_name(context.map.local_team);
                }
                movement::timeline::Action button; button.kind = movement::timeline::ActionKind::BUTTON;
                button.duration_ms = 1000; reset.actions.push_back(button);
                const auto draft = movement::timeline::plan_json(reset);
                impl_->update([&](Snapshot& s) {
                    s.grenade_current = draft; s.grenade_baseline = nullptr;
                    publish_loaded_plan(s,Mode::GRENADE_TEST,draft);
                    s.grenade_guidance_state = "hidden";
                    s.grenade_guidance_plan_id.clear(); s.grenade_guidance_reference_id.clear();
                    s.state = s.cleanup_unknown ? State::CLEANUP_UNKNOWN : State::COMPLETED;
                    s.message = "道具已删除并可恢复保留于：" + backup;
                    if (s.cleanup_unknown) s.message += "；蹲姿释放未确认，其他输出保持暂停";
                });
                return;
            }
            if (timeline_mode(request.mode) && action == Action::SAVE_PLAN) {
                const auto plan = request_plan(request);
                std::string path;
                try { path = detail::save_movement_plan(plan,request.load_path,request.movement_save_as,timeline_library_root(request)); }
                catch (const std::exception& error) {
                    impl_->update([&](Snapshot& s) { s.state = State::COMPLETED; s.message = error.what(); });
                    return;
                }
                refresh_movement_library(path);
                impl_->update([&](Snapshot& s) { baseline_plan(s,request.mode) = plan;
                    s.state = State::COMPLETED; s.message = std::string(timeline_name(request.mode)) + "方案已保存，可从下拉框重新选择"; });
                if (request.mode == Mode::GRENADE_TEST && snapshot()->movement_test_enabled &&
                    snapshot()->movement_test_mode == request.mode) {
                    impl_->guidance("selected","方案已保存，请核对参考图是否匹配当前版本",path,grenade_epoch);
                }
                return;
            }
            if (action == Action::SAVE_WEAPON_TIMING) {
                auto catalog = weapon::default_timing_catalog();
                std::string error;
                const auto& configured_path = context.config.weapon_timing_file;
                const auto path = std::filesystem::u8path(configured_path);
                const bool default_missing = configured_path == "cache/recoil/weapon-timing.json" && !std::filesystem::exists(path);
                if (!default_missing && !weapon::load_timing_catalog(path,catalog,error))
                    throw std::runtime_error("保存前读取共享武器资料失败：" + error);
                const auto* selected = weapon::find_timing(catalog,request.weapon_id);
                if (!selected) throw std::runtime_error("未选择有效武器，未保存");
                if (!selected->enabled) throw std::runtime_error("所选武器已禁用，未保存");
                if (catalog.revision == (std::numeric_limits<std::uint64_t>::max)())
                    throw std::runtime_error("武器资料版本已达上限，未保存");
                // 每次从磁盘最新目录修改两字段，避免旧UI快照覆盖其他武器的修改。
                auto& profile = catalog.profiles[static_cast<std::size_t>(selected - catalog.profiles.data())];
                profile.shot_hold_ms = request.shot_hold_ms;
                profile.fire_interval_ms = request.fire_interval_ms;
                if (!weapon::valid_timing_catalog(catalog))
                    throw std::runtime_error("点射按住须为1–500ms，射击间隔须大于按住且不超过2000ms，未保存");
                ++catalog.revision;
                if (!weapon::save_timing_catalog(path,catalog,error)) throw std::runtime_error(error);
                impl_->update([&](Snapshot& s) { s.timing_catalog = catalog; s.timing_catalog_valid = true;
                    s.state = State::COMPLETED; s.message = "所选武器的点射按住和射击间隔已保存；请重新准备测试"; });
                return;
            }
            if (action == Action::LOAD_WEAPON_TIMING) {
                auto catalog = weapon::default_timing_catalog();
                std::string error;
                auto path = std::filesystem::u8path(request.load_path);
                const bool default_missing = request.load_path == "cache/recoil/weapon-timing.json" && !std::filesystem::exists(path);
                if (!default_missing && !weapon::load_timing_catalog(path,catalog,error)) throw std::runtime_error("点射资料读取失败");
                impl_->update([&](Snapshot& s) { s.timing_catalog = catalog; s.timing_catalog_valid = true;
                    s.state = State::COMPLETED; s.message = "共享资料已载入；带入草稿不会热改生产配置"; });
                return;
            }
            if (action == Action::LOAD_PLAN || action == Action::LOAD_SAMPLING || action == Action::LOAD_FIRE_SETTINGS) {
                const auto document = read_document(std::filesystem::u8path(request.load_path),
                    timeline_mode(request.mode) ? 16*1024*1024 : kDocumentLimit);
                const auto value = action == Action::LOAD_SAMPLING ? sampling_settings_json(parse_sampling_settings(document)) :
                    action == Action::LOAD_FIRE_SETTINGS ? make_fire_test_plan(document) :
                    timeline_mode(request.mode) ? request_plan([&] { auto r=request; r.plan_text=document.dump(); return r; }()) : validate_debug_plan(document);
                impl_->update([&](Snapshot& s) { if (action == Action::LOAD_SAMPLING) s.sampling = value; else s.plan = value;
                    if (action == Action::LOAD_SAMPLING) { s.draft_sampling = value; ++s.draft_sampling_revision; }
                    else { s.draft_plan = value; s.draft_plan_mode = action == Action::LOAD_FIRE_SETTINGS ? Mode::FIRE_TEST :
                        timeline_mode(request.mode) ? request.mode : Mode::COUNTERPULSE;
                        ++s.draft_plan_revision;
                        if (timeline_mode(request.mode)) publish_loaded_plan(s,request.mode,value);
                    }
                    s.state = State::COMPLETED; s.message = "文档已载入，请检查草稿并重新准备"; });
                if (timeline_mode(request.mode)) {
                    impl_->update([&](Snapshot& s) { baseline_plan(s,request.mode) = value; current_plan(s,request.mode) = value;
                        if (s.movement_test_mode == request.mode) s.movement_voice_selected = false; });
                    auto loaded = request; loaded.plan_text = value.dump();
                    update_movement_draft(loaded,context,false);
                    refresh_movement_library(request.load_path);
                    if (request.mode == Mode::GRENADE_TEST && snapshot()->movement_test_enabled &&
                        snapshot()->movement_test_mode == request.mode && !impl_->canceled.load())
                        impl_->guidance("selected","请按站位图定位，拿出道具后对齐",request.load_path,grenade_epoch);
                    impl_->update([&](Snapshot& s) { s.message = std::string(timeline_name(request.mode)) + "方案已载入，修改后可直接测试或保存"; });
                }
                return;
            }
            auto effective = request;
            if (action == Action::DERIVE_DEFAULTS) effective.mode = Mode::DERIVE_DEFAULTS;
            if (action == Action::DERIVE_PLAN) effective.mode = Mode::DERIVE_PLAN;
            if (action == Action::REEVALUATE && physical_mode(effective.mode)) throw std::runtime_error("离线重评拒绝物理模式");
            auto plan = effective.mode==Mode::RECOIL_TEST ?
                prepare_recoil_debug_plan(std::filesystem::u8path(effective.recoil_profile_path),context.config,
                    effective.recoil_x_strength,effective.recoil_y_strength) :
                (effective.mode==Mode::RECOIL_CAPTURE||effective.mode==Mode::RECOIL_CALIBRATE) ?
                prepare_wall_debug_plan(effective.mode==Mode::RECOIL_CALIBRATE,effective.weapon_id,effective.recoil_duration_ms,
                    std::filesystem::u8path(effective.recoil_calibration_path),context.config,effective.recoil_follow_crosshair) : request_plan(effective);
            if(recoil_mode(effective.mode)) {
                if(effective.recoil_target_shots<1||effective.recoil_target_shots>50||
                    !std::isfinite(effective.recoil_locked_prefix_ms)||effective.recoil_locked_prefix_ms<0)
                    throw std::runtime_error("阶段目标须1–50发且锁定前缀时间有效");
                plan["target_shots"]=effective.recoil_target_shots;
                if(effective.recoil_follow_crosshair && effective.mode==Mode::RECOIL_CAPTURE && effective.recoil_target_shots>5)
                    throw std::runtime_error("跟随后坐力准星采集最多5发，请减少本次发数");
                plan["locked_prefix_ms"]=effective.recoil_locked_prefix_ms;
                if(effective.mode==Mode::RECOIL_TEST) {
                    if(effective.recoil_duration_ms<100||effective.recoil_duration_ms>10000)
                        throw std::runtime_error("测试最长时长须100–10000ms");
                    plan["duration_ms"]=effective.recoil_duration_ms;
                    // 阶段延长只扩等待时间，旧曲线尾部不外推，counts额度仍由冻结曲线决定。
                    const int firing_limit=std::min(60000,effective.recoil_duration_ms+100);
                    plan["limits"]["firing_ms"]=std::max(plan["limits"]["firing_ms"].get<int>(),firing_limit);
                    plan["limits"]["session_ms"]=std::min(600000,plan["limits"]["firing_ms"].get<int>()+30000);
                    plan["measurement_enabled"]=!effective.recoil_calibration_path.empty();
                    if(!effective.recoil_calibration_path.empty()){
                        auto calibrated=prepare_wall_debug_plan(false,plan.at("profile").at("weapon_id"),
                            effective.recoil_duration_ms,std::filesystem::u8path(effective.recoil_calibration_path),context.config,effective.recoil_follow_crosshair);
                        plan["calibration"]=calibrated.at("calibration");
                        plan["follow_recoil"]=effective.recoil_follow_crosshair;
                    }
                }
            }
            const auto sampling = recoil_mode(effective.mode)?Json::object():request_sampling(effective);
            impl_->update([&](Snapshot& s) { s.plan = plan; s.sampling = sampling; s.result.reset(); });
            if (impl_->canceled) { impl_->update([](Snapshot& s) { s.state = State::CANCELED; }); return; }
            if (action == Action::VALIDATE) {
                impl_->update([](Snapshot& s) { s.state = State::COMPLETED; s.message = "参数校验通过；未创建或启动实际测试"; });
                return;
            }
            const auto directory = new_directory(effective.output_root);
            Impl::Prepared work{effective,context,plan,sampling,directory.filename().string(),directory};
            work.repeat_revision = repeat_revision;
            Json task{{"schema_version",1},{"prepared_id",work.id},{"plan",plan},{"sampling",sampling},
                {"code_identity",code_identity()},
                {"parent_run",effective.input_path},{"physical_output",false},{"state","PREPARED_NOT_LAUNCHED"}};
            write_document(directory / "prepare.json",task);
            if (!plan.empty()) write_document(directory / timeline_report(effective.mode),plan);
            write_document(directory / "sampling-settings.json",sampling);
            impl_->update([&](Snapshot& s) { s.report_directory = directory.string(); });
            if (action == Action::SAVE_PLAN) {
                impl_->update([](Snapshot& s) { s.state = State::COMPLETED; s.message = "计划已保存到独立目录"; }); return;
            }
            if (action == Action::PREPARE) {
                // Prepare不探测网络、不发送命令，只冻结已有连接与配置身份。
                if (effective.recording_duration_ms < 1000 || effective.recording_duration_ms > 120000 || effective.candidate_index < 0)
                    throw std::runtime_error("记录时长或候选索引越界");
                impl_->prepared = work;
                std::string admission;
                if (uses_device(effective.mode)) {
                    try { admit(context,effective.mode);
                        if (physical_mode(effective.mode) && !context.config.mouse.allow_send_input)
                            admission = "设置中的物理输出未允许，请保存后重新准备";
                    } catch (const std::exception& error) { admission = error.what(); }
                }
                if (!impl_->canceled && repeat_revision == impl_->repeat_revision.load() && physical_mode(effective.mode) && admission.empty())
                    impl_->repeat_plan.store(std::make_shared<const Impl::Prepared>(work));
                impl_->update([&](Snapshot& s) { s.state = State::PREPARED; s.prepared_id = work.id;
                    s.repeat_ready = repeat_revision == impl_->repeat_revision.load() && static_cast<bool>(impl_->repeat_plan.load());
                    if (s.repeat_ready) s.repeat_unavailable_reason.clear();
                    else if (repeat_revision == impl_->repeat_revision.load() && !impl_->canceled)
                        s.repeat_unavailable_reason = admission.empty() ? "当前为离线计划，不提供物理测试快捷键模板" : admission;
                    // revision变化时保留实际编辑/取消原因，后台完成不能用“已准备”掩盖它。
                    s.physical = physical_mode(effective.mode);
                    s.message = !s.repeat_ready && physical_mode(effective.mode) ?
                        "计划已保存，但不能启动：" + s.repeat_unavailable_reason : "已准备，等待本次前台启动；参数已冻结";
                });
                const auto readiness = snapshot();
                // 仅记录准备阶段事实，后续编辑不覆写原始准备证据，也不保存配置或设备参数。
                write_document(directory / "readiness.json",{{"schema_version",1},{"prepared_id",work.id},
                    {"scope","prepare_snapshot_not_live_permission"},{"ready",readiness->repeat_ready},
                    {"reason",readiness->repeat_unavailable_reason}});
            } else impl_->run(work,false,{});
        });
    } catch (const std::exception& error) {
        try { impl_->update([&](Snapshot& s) { s.message = error.what(); }); } catch (...) {}
        return false;
    } catch (...) { return false; }
}

bool Session::record_inputs(Runtime& runtime, const std::string& root, const Context& context) noexcept {
    try {
        if (impl_->shutting_down || busy() || snapshot()->cleanup_unknown || !context.input_recording_idle || !context.device) return false;
        return impl_->launch(State::RUNNING,false,[this,&runtime,root,context] {
            const auto directory = new_directory(root);
            if (!runtime.start_input_training(directory / "input",context.device)) throw std::runtime_error("输入记录未启动");
            impl_->update([&](Snapshot& s) { s.report_directory = (directory / "input").string(); s.message = "正在记录原始输入，停止后后台保存"; });
            while (!impl_->canceled) {
                auto training = runtime.snapshot().training;
                if (!training || training->status != input_training::Status::RECORDING) break;
                std::this_thread::sleep_for(20ms);
            }
            runtime.stop_input_training();
            auto training = runtime.snapshot().training;
            impl_->update([&](Snapshot& s) {
                if (training && input_training::stop_is_unconfirmed(training->status)) {
                    s.state = State::CLEANUP_UNKNOWN; s.cleanup_unknown = true;
                    s.message = "记录收尾尚未确认，不能开始新设备任务";
                } else if (training && training->status == input_training::Status::STOPPED) {
                    s.state = State::COMPLETED;
                    s.message = "输入记录结束；完整性以归档报告为准";
                } else {
                    s.state = State::FAILED;
                    s.message = training && training->status == input_training::Status::LIMIT ?
                        "输入记录达到预算，仅保留有限档案，不能认定完整记录" : "输入记录未成功归档；检查本组记录状态";
                }
            });
        });
    } catch (...) { return false; }
}
bool Session::load_inputs(Runtime& runtime, const std::string& directory) noexcept {
    try {
        if (impl_->shutting_down || busy()) return false;
        return impl_->launch(State::WORKING,false,[this,&runtime,directory] {
            if (!runtime.load_input_training(std::filesystem::u8path(directory))) throw std::runtime_error("离线记录未加载");
            while (!impl_->canceled) {
                auto training = runtime.snapshot().training;
                if (!training || training->status != input_training::Status::REPLAYING) break;
                std::this_thread::sleep_for(20ms);
            }
            if (impl_->canceled) runtime.stop_input_training();
            const auto training = runtime.snapshot().training;
            impl_->update([&](Snapshot& s) {
                s.report_directory = directory;
                if (training && input_training::stop_is_unconfirmed(training->status)) {
                    s.state = State::CLEANUP_UNKNOWN; s.cleanup_unknown = true;
                    s.message = "离线读取停止超时，后台资源尚未确认退出";
                } else if (training && training->status == input_training::Status::STOPPED) {
                    s.state = impl_->canceled ? State::CANCELED : State::COMPLETED;
                    s.message = impl_->canceled ? "离线输入读取已取消；未连接设备" : "离线输入读取已结束；未连接设备";
                } else {
                    s.state = State::FAILED;
                    s.message = training && training->status == input_training::Status::LIMIT ?
                        "离线档案包含预算截断，不能认定完整回看" : "离线输入读取失败；检查档案清单与原始文件";
                }
            });
        });
    } catch (...) { return false; }
}
const char* state_name(State state) noexcept {
    switch (state) {
    case State::IDLE:return "空闲"; case State::WORKING:return "后台处理中";
    case State::PREPARED:return "已准备，未启动"; case State::RUNNING:return "执行中";
    case State::STOPPING:return "停止与清理中"; case State::COMPLETED:return "已完成";
    case State::CANCELED:return "已取消"; case State::FAILED:return "失败";
    case State::CLEANUP_UNKNOWN:return "清理未确认";
    } return "未知";
}
const char* physical_confirmation() noexcept { return "AUTO_STOP_COUNTERPULSE"; }
}
