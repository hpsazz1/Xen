#include "debug_session/debug_session.h"
#include "auto_stop_probe/debug_runner.h"
#include "auto_stop_probe/counterpulse_internal.h"
#include "auto_stop_probe/counterpulse_hud.h"
#include "runtime/runtime.h"
#include "log/log.h"
#include "recoil/recoil_debug_run.h"
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

Json parse_document(const std::string& text) {
    if (text.size() > kDocumentLimit) throw std::runtime_error("文档超过1MiB限制");
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
Json read_document(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("无法打开所选文档");
    std::string text(kDocumentLimit + 1, '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(file.gcount()));
    if (text.starts_with("\xef\xbb\xbf")) text.erase(0, 3);
    return parse_document(text);
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
Json request_plan(const Request& request) {
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
bool physical_mode(Mode mode) { return mode == Mode::COUNTERPULSE || mode == Mode::FIRE_TEST || recoil_mode(mode); }
bool uses_device(Mode mode) { return physical_mode(mode) || mode == Mode::MANUAL_RECORDING; }
void admit(const Context& context, Mode mode) {
    if (!uses_device(mode)) return;
    if (!context.runtime_idle || !context.input_recording_idle || !context.cleanup_known)
        throw std::runtime_error("请先停止Runtime和记录，并确认设备清理完成");
    if (!context.device || !context.device->output_owner_exclusive() ||
        (context.device->status() != MouseStatus::READY && context.device->status() != MouseStatus::DISABLED))
        throw std::runtime_error("已有设备未就绪或未持有独占职责");
    if (context.config.mouse.backend != MouseBackend::KMBOX_NET)
        throw std::runtime_error("该测试仅支持已有KMBOX NET连接");
    if (context.device->left_button_faulted() || context.device->left_button_cleanup_required())
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
    std::atomic<std::shared_ptr<const Prepared>> repeat_plan;
    std::atomic_uint64_t repeat_revision{0};
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
    template<class F> bool launch(State state, bool physical, F&& fn) {
        if (busy()) return false;
        if (worker.valid()) {
            if (worker.wait_for(0ms) != std::future_status::ready) return false;
            worker.get();
        }
        canceled = false;
        const auto current = ++generation;
        update([&](Snapshot& s) { s.state = state; s.busy = true; s.physical = physical;
            // generation与结果同次发布，不能让新准备代次携带上一次完成结果。
            s.generation = current; s.message = "后台处理中"; s.live.reset(); s.result.reset(); });
        try {
            worker = std::async(std::launch::async, [this, current, physical, fn = std::forward<F>(fn)]() mutable {
                try { fn(); }
                catch (const DebugRunFailure& error) {
                    update([&](Snapshot& s) {
                        if (s.generation != current) return;
                        if (!error.output_not_started && physical) s.cleanup_unknown = true;
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
                    s.busy = false;
                    if (canceled && s.state == State::PREPARED) {
                        s.state = State::CANCELED; s.prepared_id.clear(); s.message = "准备已取消，已生成文件仅留档";
                    }
                    if (canceled || s.state == State::FAILED || s.cleanup_unknown) {
                        repeat_plan.store(nullptr); s.repeat_ready = false;
                        s.repeat_unavailable_reason = s.message;
                    }
                } });
            });
            return true;
        } catch (...) {
            update([](Snapshot& s) { s.state = State::FAILED; s.busy = false; s.message = "后台任务创建失败"; });
            return false;
        }
    }
    void run(Prepared work, bool allow, const std::string& confirmation) {
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
        write_document(work.directory / "result-index.json",index);
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
    if (impl_->busy()) return true;
    if (!impl_->shutting_down) return false;
    std::lock_guard lock(impl_->hud_mutex);
    return static_cast<bool>(impl_->hud.load());
}
void Session::poll() noexcept {
    try {
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
    ++impl_->repeat_revision;
    impl_->repeat_plan.store(nullptr);
    try { impl_->update([&](Snapshot& s) {
        s.repeat_ready = false;
        s.repeat_unavailable_reason = reason.empty() ? "测试模板已失效，请重新准备" : reason;
    }); } catch (...) {}
}
bool Session::repeat(const Context& context) noexcept {
    try {
        const auto reject = [&](const char* message) {
            impl_->update([&](Snapshot& s) { s.message = message; }); return false;
        };
        if (impl_->shutting_down) return reject("应用正在关闭，快捷键测试未执行");
        if (busy()) return reject("已有调试任务正在执行，本次快捷键忽略，不排队");
        if (!context.config.keyboard.debug_test_enabled) return reject("调试测试快捷键开关未启用");
        const auto source = impl_->repeat_plan.load();
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
        if (recoil_mode(source->request.mode) &&
            (context.config.keyboard.debug_test_virtual_keys != source->context.config.keyboard.debug_test_virtual_keys ||
             context.config.keyboard.emergency_virtual_keys != source->context.config.keyboard.emergency_virtual_keys))
            return reject("测试键或紧急停止键已改变，请重新准备");
        auto work = *source;
        if(single_use_recoil_mode(work.request.mode))invalidate_repeat("本次标定或采集许可已使用，请完成本组后重新准备");
        impl_->prepared.reset();
        return impl_->launch(State::RUNNING,true,[this,work = std::move(work)]() mutable {
            work.directory = new_directory(work.request.output_root);
            const auto parent_id = work.id;
            work.id = work.directory.filename().string();
            write_document(work.directory / "prepare.json",{{"schema_version",1},{"prepared_id",work.id},
                {"repeat_of",parent_id},{"trigger","USER_HOTKEY"},{"code_identity",code_identity()},
                {"plan",work.plan},{"sampling",work.sampling},{"physical_output",true}});
            write_document(work.directory / "plan.json",work.plan);
            write_document(work.directory / "sampling-settings.json",work.sampling);
            impl_->update([&](Snapshot& s) { s.prepared_id = work.id; s.report_directory = work.directory.string(); });
            impl_->run(work,true,physical_confirmation());
        });
    } catch (const std::exception& error) {
        try { impl_->update([&](Snapshot& s) { s.message = error.what(); }); } catch (...) {}
        return false;
    } catch (...) { return false; }
}

bool Session::dispatch(Action action, const Request& request, const Context& context,
    const std::string& prepared_id, bool allow_physical_output, const std::string& confirmation) noexcept {
    try {
        if (action == Action::CANCEL) { cancel(); return true; }
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
        impl_->prepared.reset();
        invalidate_repeat("新任务正在准备；旧测试模板已失效");
        const auto repeat_revision = impl_->repeat_revision.load();
        impl_->update([](Snapshot& s) { s.prepared_id.clear(); });
        return impl_->launch(State::WORKING,false,[this,action,request,context,repeat_revision] {
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
                const auto document = read_document(std::filesystem::u8path(request.load_path));
                const auto value = action == Action::LOAD_SAMPLING ? sampling_settings_json(parse_sampling_settings(document)) :
                    action == Action::LOAD_FIRE_SETTINGS ? make_fire_test_plan(document) : validate_debug_plan(document);
                impl_->update([&](Snapshot& s) { if (action == Action::LOAD_SAMPLING) s.sampling = value; else s.plan = value;
                    if (action == Action::LOAD_SAMPLING) { s.draft_sampling = value; ++s.draft_sampling_revision; }
                    else { s.draft_plan = value; s.draft_plan_mode = action == Action::LOAD_FIRE_SETTINGS ? Mode::FIRE_TEST : Mode::COUNTERPULSE;
                        ++s.draft_plan_revision; }
                    s.state = State::COMPLETED; s.message = "文档已载入，请检查草稿并重新准备"; });
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
                    std::filesystem::u8path(effective.recoil_calibration_path),context.config) : request_plan(effective);
            if(recoil_mode(effective.mode)) {
                if(effective.recoil_target_shots<1||effective.recoil_target_shots>50||
                    !std::isfinite(effective.recoil_locked_prefix_ms)||effective.recoil_locked_prefix_ms<0)
                    throw std::runtime_error("阶段目标须1–50发且锁定前缀时间有效");
                plan["target_shots"]=effective.recoil_target_shots;
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
                            effective.recoil_duration_ms,std::filesystem::u8path(effective.recoil_calibration_path),context.config);
                        plan["calibration"]=calibrated.at("calibration");
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
            if (!plan.empty()) write_document(directory / "plan.json",plan);
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
                if (training && training->status == input_training::Status::STOP_TIMEOUT) {
                    s.state = State::CLEANUP_UNKNOWN; s.cleanup_unknown = true;
                    s.message = "记录停止超时，不能开始新设备任务";
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
                if (training && training->status == input_training::Status::STOP_TIMEOUT) {
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
