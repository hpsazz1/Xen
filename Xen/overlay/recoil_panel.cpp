#include "overlay/recoil_panel.h"
#include "overlay/overlay.h"
#include "recoil/recoil_store.h"
#include "recoil/recoil_calibration_io.h"
#include "recoil_tuner/recoil_tuner.h"
#include "recoil_tuner/wall_capture_analysis.h"
#include "weapon/weapon_timing.h"
#include "weapon/weapon_catalog.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include "recoil/recoil_editor_job_internal.h"
#include <functional>
#include <limits>
#include <sstream>
#include <stdexcept>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

namespace {
struct PickerCancellation {
    IFileDialog* dialog;
    const std::atomic<bool>* canceled;
    UINT_PTR timer = 0;
};
thread_local PickerCancellation* active_picker = nullptr;
void CALLBACK cancel_picker_timer(HWND, UINT, UINT_PTR timer, DWORD) noexcept {
    // 模态窗口消息循环在创建 COM 对象的同一 STA 调用此回调。
    if (active_picker && active_picker->timer == timer && active_picker->canceled->load())
        active_picker->dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
}
std::string choose_recoil_profile(HWND owner, const std::shared_ptr<std::atomic<bool>>& canceled) {
    if(canceled->load())return {};
    const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE);
    if(FAILED(initialized)) throw std::runtime_error("无法初始化文件选择窗口");
    struct Uninitialize { ~Uninitialize(){CoUninitialize();} } uninitialize;
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    if(FAILED(CoCreateInstance(__uuidof(FileOpenDialog),nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog))))
        throw std::runtime_error("无法打开文件选择窗口");
    FILEOPENDIALOGOPTIONS options{};
    const COMDLG_FILTERSPEC filters[]{{L"弹道曲线 JSON",L"*.json"},{L"所有文件",L"*.*"}};
    if(FAILED(dialog->GetOptions(&options)) ||
        FAILED(dialog->SetOptions(options|FOS_FORCEFILESYSTEM|FOS_FILEMUSTEXIST|FOS_PATHMUSTEXIST|FOS_NOCHANGEDIR)) ||
        FAILED(dialog->SetFileTypes(2,filters)) || FAILED(dialog->SetTitle(L"选择弹道曲线 JSON")))
        throw std::runtime_error("文件选择窗口配置失败");
    PickerCancellation cancellation{dialog.Get(), canceled.get()};
    struct TimerScope {
        PickerCancellation* previous;
        PickerCancellation& current;
        ~TimerScope(){KillTimer(nullptr,current.timer);active_picker=previous;}
    } timer_scope{active_picker,cancellation};
    active_picker=&cancellation;
    cancellation.timer=SetTimer(nullptr,0,20,cancel_picker_timer);
    if(!cancellation.timer)throw std::runtime_error("无法建立文件选择取消响应");
    if(canceled->load())return {};
    const auto shown=dialog->Show(owner);
    if(canceled->load() || shown==HRESULT_FROM_WIN32(ERROR_CANCELLED))return {};
    if(FAILED(shown))throw std::runtime_error("文件选择失败");
    Microsoft::WRL::ComPtr<IShellItem> item; PWSTR name=nullptr;
    if(FAILED(dialog->GetResult(&item))||FAILED(item->GetDisplayName(SIGDN_FILESYSPATH,&name)))
        throw std::runtime_error("无法取得选中文件路径");
    struct FreePath { PWSTR value; ~FreePath(){CoTaskMemFree(value);} } free_path{name};
    const auto utf8=std::filesystem::path(name).u8string();
    if(canceled->load())return {};
    return {reinterpret_cast<const char*>(utf8.data()),utf8.size()};
}
struct DisabledScope { explicit DisabledScope(bool disabled) { ImGui::BeginDisabled(disabled); } ~DisabledScope() { ImGui::EndDisabled(); } };
const char* reason_text(RecoilReason reason) {
    switch (reason) {
    case RecoilReason::NONE: return "可用";
    case RecoilReason::DISABLED: return "已关闭";
    case RecoilReason::INVALID_PROFILE: return "曲线无效";
    case RecoilReason::UNCALIBRATED: return "缺少校准";
    case RecoilReason::CONTEXT: return "上下文门禁未满足";
    case RecoilReason::WAIT_RELEASE: return "等待松键";
    case RecoilReason::RESET_UNVERIFIED: return "恢复尚未证实";
    case RecoilReason::PROFILE_CHANGED: return "曲线已切换";
    case RecoilReason::RELEASED: return "已松开";
    case RecoilReason::CANCELED: return "已取消";
    case RecoilReason::LATE: return "相位超时";
    case RecoilReason::EXHAUSTED: return "曲线结束";
    case RecoilReason::NOT_SENT: return "命令未发送";
    case RecoilReason::UNKNOWN_RECEIPT: return "回执未知";
    case RecoilReason::COMMAND_PENDING: return "等待命令确认";
    case RecoilReason::INVALID_TIME: return "时间无效";
    case RecoilReason::LIMIT: return "超过预算";
    } return "未知状态";
}
void help(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

std::string new_output_directory(const char* category) {
    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return std::string("cache/recoil/") + category + "/run-" +
        std::to_string(stamp) + "-" + std::to_string(GetCurrentProcessId());
}

void prepare_output_parent(const std::string& directory) {
    const auto parent = std::filesystem::u8path(directory).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
}
std::string read_workflow_file(const std::string& path) {
    const auto file = std::filesystem::u8path(path);
    if (!std::filesystem::is_regular_file(file) || std::filesystem::file_size(file) > 16 * 1024 * 1024)
        throw std::runtime_error("采集文件不存在或过大");
    std::ifstream stream(file, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(stream)), {});
    if (stream.bad()) throw std::runtime_error("采集文件读取失败");
    return bytes;
}
void row(const char* label, const char* description) {
    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(label); help(description);
    ImGui::TableSetColumnIndex(1); ImGui::SetNextItemWidth(-1);
}
bool form(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) return false;
    ImGui::TableSetupColumn("标签", ImGuiTableColumnFlags_WidthFixed, 150);
    ImGui::TableSetupColumn("值", ImGuiTableColumnFlags_WidthStretch);
    return true;
}
void chart(const char* id, const RecoilProfile& base, const RecoilProfile& candidate, bool x_axis) {
    if (candidate.points.empty() || base.points.empty()) return;
    std::array<float, 192> a{}, b{};
    const double duration = std::max(base.points.back().time_ms, candidate.points.back().time_ms);
    float low = 0, high = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto pa = sample_recoil_profile(base, duration * i / (a.size() - 1));
        const auto pb = sample_recoil_profile(candidate, duration * i / (a.size() - 1));
        a[i] = static_cast<float>(x_axis ? pa.x_counts : pa.y_counts); b[i] = static_cast<float>(x_axis ? pb.x_counts : pb.y_counts);
        low = std::min({low, a[i], b[i]}); high = std::max({high, a[i], b[i]});
    }
    const float width = std::max(100.0f, ImGui::GetContentRegionAvail().x), height = 120;
    const auto origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, {width, height});
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, {origin.x + width, origin.y + height}, IM_COL32(235, 240, 246, 255), 5);
    const float range = std::max(1.0f, high - low);
    auto plot = [&](const auto& values, ImU32 color) {
        ImVec2 previous;
        for (std::size_t i = 0; i < values.size(); ++i) {
            const ImVec2 point{origin.x + 8 + (width - 16) * i / (values.size() - 1), origin.y + height - 8 - (height - 16) * (values[i] - low) / range};
            if (i) draw->AddLine(previous, point, color, 1.5f);
            previous = point;
        }
    };
    plot(a, IM_COL32(120, 125, 135, 255)); plot(b, IM_COL32(20, 125, 230, 255));
    ImGui::Text("%s / counts：%.2f 至 %.2f；时间 0 至 %.1f ms（灰：基线，蓝：草稿）", x_axis ? "X" : "Y", low, high, duration);
}
}

struct RecoilPanel::Impl {
    std::vector<RecoilStoredProfile> files;
    std::string selected_file, manual_file, status, dataset_path;
    std::string result_directory = new_output_directory("tuning");
    RecoilProfile base, draft, preview;
    RecoilTuning tuning;
    struct Edit { RecoilProfile draft; RecoilTuning tuning; };
    Edit checkpoint;
    std::vector<Edit> undo, redo;
    bool loaded = false, preview_valid = false, show_editor = false, calibration_confirmed = false;
    std::uint64_t save_revision = 1;
    RecoilCalibration calibration;
    double phase_ms = 0, recovery_ms = 0;
    recoil_tuner::Dataset dataset;
    bool dataset_loaded = false;
    recoil_tuner::Request request;
    std::optional<recoil_tuner::Report> report;
    using Job = recoil_editor_detail::Job<std::shared_ptr<Impl>>;
    std::shared_ptr<Job> job;
    bool working_copy = false;
    bool cancel_requested = false;
    std::shared_ptr<std::atomic<bool>> job_cancellation;

    std::function<void(Impl&)> pending_action;
    void launch(std::function<void(Impl&)> action) {
        if (job || pending_action) return;
        pending_action = std::move(action);
    }
    void poll() {
        if (pending_action) {
            auto action = std::exchange(pending_action, {});
            job_cancellation=std::make_shared<std::atomic<bool>>(false);
            // 在绘制调用之外转移整份编辑状态，不在 UI 线程复制大型数据集和撤销历史。
            auto copy = std::make_shared<Impl>(std::move(*this));
            copy->working_copy = true;
            job_cancellation=copy->job_cancellation;
            cancel_requested = false;
            try { job = Job::start(copy, [action = std::move(action)](auto& state) {
                try { action(*state); }
                catch (...) { state->status = "后台操作失败；请核对文件与数据，已有保存结果不会自动回滚。"; }
            }); } catch (...) {
                *this = std::move(*copy);
                working_copy = false;
                status = "无法启动后台任务，编辑状态已保留。";
            }
            return;
        }
        if (!job || !job->ready()) return;
        auto finished = std::move(job);
        const bool canceled = cancel_requested;
        const bool failed = finished->failed();
        *this = std::move(*finished->value);
        working_copy = false;
        cancel_requested = false;
        job_cancellation.reset();
        if (failed) status = "后台任务异常结束；编辑状态已保留。";
        if (canceled) {
            workflow_after_calibration.reset(); workflow_prepare_next.reset();
            status += " 已处理取消请求；已经开始的同步操作完成后退出，已保存结果不回滚。";
        }
    }
    RecoilCalibrationPrepareRequest calibration_request;
    std::string calibration_config = "config.ini";
    std::string calibration_output = new_output_directory("calibration");
    std::string calibration_request_file, calibration_command;
    std::string calibration_prepared_identity;
    std::string workflow_weapon = "ak47", workflow_calibration_path, workflow_run;
    int workflow_duration_ms = 3000, workflow_target_shots = 5, workflow_group_shots = 30, workflow_step_shots = 5;
    double workflow_locked_prefix_ms = 0;
    std::uint64_t workflow_result_generation = 0;
    std::uint64_t workflow_abort_generation = std::numeric_limits<std::uint64_t>::max();
    std::string workflow_preview_path, workflow_import_path, workflow_candidate_path;
    recoil_tuner::WallCaptureReport workflow_measurement;
    bool workflow_measurement_ready = false, workflow_confirmed = false, workflow_count_ok = false;
    int workflow_observed_shots = -1, workflow_requested_shots = 0;
    std::string workflow_executed_profile;
    struct WallSample {
        recoil_tuner::WallCaptureReport measurement;
        std::string source_run, executed_profile;
    };
    std::vector<WallSample> workflow_samples;
    nlohmann::json workflow_weapon_settings = nlohmann::json::object();
    bool workflow_settings_loaded = false, workflow_settings_dirty = false;
    bool workflow_record_measurement = false;
    std::optional<debug_session::Mode> workflow_after_calibration, workflow_prepare_next;
    std::string workflow_profile_import, workflow_profiles_directory;
    void remember_workflow_weapon() {
        workflow_weapon_settings[workflow_weapon] = {{"target_shots",workflow_target_shots},{"step_shots",workflow_step_shots},
            {"group_shots",workflow_group_shots},{"duration_ms",workflow_duration_ms},
            {"calibration_path",workflow_calibration_path},
            {"selected_file",loaded && base.weapon_id != workflow_weapon ? std::string{} : selected_file}};
        workflow_settings_dirty = true;
    }
    void select_workflow_weapon(const std::string& weapon) {
        remember_workflow_weapon(); workflow_weapon = weapon;
        const auto value = workflow_weapon_settings.value(weapon, nlohmann::json::object());
        workflow_group_shots = std::clamp(value.value("group_shots",30),1,50);
        workflow_target_shots = std::clamp(value.value("target_shots",5),1,workflow_group_shots);
        workflow_step_shots = std::clamp(value.value("step_shots",5),1,50);
        workflow_duration_ms = std::clamp(value.value("duration_ms",3000),500,10000);
        workflow_calibration_path = value.value("calibration_path",std::string{});
        selected_file = value.value("selected_file",std::string{}); loaded = false;
        workflow_record_measurement = false; workflow_after_calibration.reset(); workflow_prepare_next.reset();
        workflow_locked_prefix_ms = 0; workflow_samples.clear(); workflow_measurement_ready = false;
    }

    void prepare_workflow(debug_session::Mode mode, const AppConfig& config, OverlayActions& actions) {
        actions.debug_plan_edited = true;
        actions.debug_action = debug_session::Action::PREPARE;
        auto& r = actions.debug_request;
        r.mode = mode; r.weapon_id = workflow_weapon; r.output_root = "cache/recoil/workflow";
        r.recoil_duration_ms = workflow_duration_ms; r.recoil_target_shots = workflow_target_shots;
        r.recoil_locked_prefix_ms = workflow_locked_prefix_ms;
        r.recoil_calibration_path = mode == debug_session::Mode::RECOIL_TEST && !workflow_record_measurement
            ? std::string{} : workflow_calibration_path;
        r.recoil_profile_path = selected_file.empty() ? "" : (std::filesystem::u8path(config.recoil.profile_directory) /
            std::filesystem::u8path(selected_file)).string();
        r.recoil_x_strength = tuning.x_strength; r.recoil_y_strength = tuning.y_strength; r.show_hud = false;
    }

    void import_workflow_result(const nlohmann::json& result, bool advance = false) {
        if (result.value("weapon_id",std::string{}) != workflow_weapon) {
            status = "结果武器与当前选择不一致，未带入标定或训练数据。"; return;
        }
        workflow_confirmed = false; workflow_measurement_ready = false;
        workflow_candidate_path = result.value("candidate_path", std::string{});
        workflow_preview_path = result.value("preview_path", std::string{});
        workflow_run = result.value("capture_path", std::string{});
        const auto calibration_path = result.value("calibration_path", std::string{});
        if (!calibration_path.empty() && result.value("success",false) && result.value("completed",false) && result.value("cleanup_known",false)) {
            workflow_calibration_path = calibration_path; remember_workflow_weapon();
            if (advance && workflow_after_calibration && result.value("weapon_id",std::string{}) == workflow_weapon) {
                workflow_prepare_next = workflow_after_calibration; workflow_after_calibration.reset();
            }
        }
        const auto measurement_path = result.value("measurement_path", std::string{});
        if (!measurement_path.empty())
            workflow_measurement_ready = recoil_tuner::load_wall_report(std::filesystem::u8path(measurement_path), workflow_measurement, status);
        const auto executed = result.value("base_profile_path", std::string{});
        workflow_executed_profile = executed.empty() ? "" : read_workflow_file(executed);
        workflow_requested_shots = result.value("requested_shots",0);
        workflow_observed_shots = result.contains("observed_ammo_delta") && result.at("observed_ammo_delta").is_number_integer()
            ? result.at("observed_ammo_delta").get<int>() : -1;
        workflow_count_ok = result.value("completed", false) && result.value("success", false) &&
            result.value("cleanup_known", false) && result.value("archive_complete", true) &&
            result.value("training_eligible", false) && result.contains("observed_ammo_delta") &&
            result.at("observed_ammo_delta").is_number_integer() &&
            result.at("observed_ammo_delta").get<int>() == result.value("requested_shots", -2) &&
            result.value("requested_shots", 0) == workflow_target_shots;
        if (workflow_measurement_ready) status = workflow_measurement.message;
        else if (!calibration_path.empty() && result.value("success",false) && result.value("completed",false) &&
            result.value("cleanup_known",false)) status = "画面标定已带入；下一步准备采集或测试。";
        else status = result.value("message",std::string{"本组已结束，请人工核对效果。"});
    }

    void workflow_results(AppConfig& config, OverlayActions& actions, const debug_session::Snapshot* debug) {
        if (!pending_action && !job && debug && !debug->busy && debug->result && debug->generation != workflow_result_generation &&
            (debug->result->contains("capture_path") || debug->result->contains("calibration_path"))) {
            workflow_result_generation = debug->generation;
            if (debug->state != debug_session::State::COMPLETED) {
                workflow_measurement_ready = false; workflow_count_ok = false; workflow_confirmed = false;
                workflow_candidate_path.clear(); workflow_executed_profile.clear();
                workflow_preview_path = debug->result->value("preview_path",std::string{});
                status = debug->message;
                return;
            }
            const auto result = *debug->result;
            launch([result](Impl& s) { s.import_workflow_result(result, true); });
            return;
        }
        if (!workflow_preview_path.empty() && ImGui::Button("查看本组画面")) {
            const auto path = std::filesystem::u8path(workflow_preview_path);
            ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        help("打开本组保存的画面对照，核对固定靶点、遮挡与是否有人工移动；不会执行设备动作。");
        if (workflow_measurement_ready) {
            ImGui::TextWrapped("本组：%s", workflow_measurement.message.c_str());
            if (workflow_observed_shots >= 0) ImGui::Text("目标 %d 发；GSI观测 %d 发（非逐发精确计数）",workflow_requested_shots,workflow_observed_shots);
            if (!workflow_count_ok) ImGui::TextWrapped("实际发数未与本阶段目标一致；保留原始数据，不自动纳入阶段训练。");
            ImGui::BeginDisabled(!workflow_measurement.valid || !workflow_count_ok);
            if (!workflow_candidate_path.empty() && ImGui::Button("画面已核对：保存候选并准备测试")) {
                workflow_confirmed = true; actions.debug_plan_edited = true;
                const auto settings = config.recoil;
                launch([settings](Impl& s) {
                    RecoilProfile candidate;
                    if (!load_recoil_profile(read_workflow_file(s.workflow_candidate_path), candidate, s.status)) return;
                    RecoilStore store(std::filesystem::u8path(settings.profile_directory));
                    if (store.save_new(candidate, s.selected_file, s.status)) {
                        s.load_file(settings, s.selected_file); s.refresh(settings);
                        s.workflow_samples.clear(); s.workflow_locked_prefix_ms = 0;
                        s.workflow_record_measurement = true; s.workflow_prepare_next = debug_session::Mode::RECOIL_TEST;
                        s.workflow_candidate_path.clear(); s.workflow_measurement_ready = false;
                        s.status = "采集候选已载入，正在准备测试；仍须回游戏重新按测试键。";
                    }
                });
            }
            if (!workflow_executed_profile.empty() && ImGui::Button("画面已核对：加入优化数据")) {
                workflow_confirmed = true;
                const auto duplicate = std::any_of(workflow_samples.begin(), workflow_samples.end(), [&](const auto& item) { return item.source_run == workflow_run; });
                if (duplicate) status = "本组已加入，不能重复计数。";
                else if (workflow_samples.size() >= 5) status = "本轮已有5组，先优化或清空后重新采集。";
                else if (!workflow_samples.empty() && workflow_samples.front().executed_profile != workflow_executed_profile)
                    status = "本组执行曲线不同；请先清空旧阶段数据，不能混合优化。";
                else { workflow_samples.push_back({workflow_measurement, workflow_run, workflow_executed_profile}); status = "已加入独立测试数据。"; }
            }
            ImGui::EndDisabled();
            help("点击即人工确认固定靶点、画面可用且没有人物或手动视角移动，并执行对应保存/加入操作；程序不会代替确认，不代表物理验收通过。");
        }
        if (workflow_record_measurement || !workflow_samples.empty())
            ImGui::Text("本轮优化数据：训练 %d/3，验证 %d/2", static_cast<int>(std::min<std::size_t>(3,workflow_samples.size())),
                static_cast<int>(workflow_samples.size() > 3 ? workflow_samples.size() - 3 : 0));
        if (ImGui::TreeNode("优化与阶段推进")) {
        if (ImGui::SmallButton("清空本轮待用数据")) workflow_samples.clear();
        help("只清除本轮选择，不删除原始采集，也不撤销已使用验证数据的记录。");
        ImGui::BeginDisabled(workflow_samples.size() != 5);
        if (ImGui::Button("优化本阶段并载入候选")) {
            actions.debug_plan_edited = true;
            const auto settings = config.recoil;
            launch([settings](Impl& s) {
                RecoilProfile executed;
                if (!load_recoil_profile(s.workflow_samples.front().executed_profile, executed, s.status)) return;
                std::vector<recoil_tuner::WallTrial> trials;
                for (std::size_t i=0; i<s.workflow_samples.size(); ++i) {
                    const auto& sample=s.workflow_samples[i];
                    trials.push_back({sample.measurement,sample.source_run,sample.executed_profile,true,
                        i<3 ? recoil_tuner::TrialUse::FIT : recoil_tuner::TrialUse::HOLDOUT});
                }
                recoil_tuner::WallOptimizationRequest request;
                request.measurements_confirmed = true; request.locked_prefix_ms = s.workflow_locked_prefix_ms;
                auto result = recoil_tuner::optimize_wall_trials_recorded(executed,trials,request,
                    std::filesystem::u8path("cache/recoil/wall-usage"));
                s.status = result.message;
                if (!result.valid || !result.candidate) return;
                RecoilCandidateReplayReport replay;
                if (!validate_recoil_candidate_execution(*result.candidate,{},replay,s.status)) return;
                RecoilStore store(std::filesystem::u8path(settings.profile_directory));
                if (!store.save_new(*result.candidate,s.selected_file,s.status)) return;
                const auto directory = new_output_directory("tuning");
                std::filesystem::create_directories(std::filesystem::u8path(directory));
                if (!recoil_tuner::save_wall_report(std::filesystem::u8path(directory)/"analysis.json",result,s.status)) return;
                s.load_file(settings,s.selected_file); s.refresh(settings); s.workflow_samples.clear();
                s.workflow_measurement_ready = false;
                s.workflow_record_measurement = true; s.workflow_prepare_next = debug_session::Mode::RECOIL_TEST;
                s.status = "优化候选已载入，正在准备复测；仍须重新按测试键，实测变好后再推进阶段。";
            });
        }
        help("使用三组训练、两组独立验证画面，限定修正量并锁定已验收前段；只生成待复测候选，验证数据不会重复使用。");
        ImGui::EndDisabled();
        if (ImGui::TreeNode("载入以前的采集")) {
            ImGui::InputText("本组结果 JSON", &workflow_import_path);
            if (ImGui::Button("载入采集结果")) launch([](Impl& s) {
                s.import_workflow_result(nlohmann::json::parse(read_workflow_file(s.workflow_import_path)));
            });
            help("载入已完成会话的结果，重新核对后加入本轮；不连接设备。");
            ImGui::TreePop();
        }
        ImGui::BeginDisabled(!loaded || base.weapon_id != workflow_weapon || !workflow_samples.empty());
        if (ImGui::Button("已复测本阶段：锁定前段并追加")) {
            workflow_locked_prefix_ms = base.points.empty() ? 0 : base.points.back().time_ms;
            tuning = {};
            workflow_target_shots = std::min(workflow_group_shots, workflow_target_shots + workflow_step_shots);
            remember_workflow_weapon();
            actions.debug_plan_edited = true; workflow_measurement_ready = false;
        }
        help("由你完成实际复测后推进。锁定当前曲线已有时间段，仅优化后续新增段；不把图像时间当精确第N发时刻。");
        if (ImGui::Button("整组微调：解除前段锁定")) {
            workflow_locked_prefix_ms = 0; workflow_target_shots = workflow_group_shots;
            remember_workflow_weapon();
            actions.debug_plan_edited = true; workflow_measurement_ready = false;
        }
        help("整组分段完成并复测后使用；下一轮允许优化全曲线，旧版本保留。");
        ImGui::EndDisabled();
        ImGui::TreePop();
        }
    }

    void workflow(const RuntimeSnapshot& snapshot, AppConfig& config, OverlayActions& actions,
                  const debug_session::Snapshot* debug, bool can_edit) {
        const bool newly_aborted = debug && workflow_abort_generation != debug->generation &&
            (debug->state == debug_session::State::CANCELED || debug->state == debug_session::State::FAILED ||
                debug->state == debug_session::State::CLEANUP_UNKNOWN);
        if (newly_aborted) workflow_abort_generation = debug->generation;
        if (actions.debug_plan_edited || actions.debug_action == debug_session::Action::CANCEL || newly_aborted) {
            workflow_after_calibration.reset(); workflow_prepare_next.reset();
        }
        if (!workflow_settings_loaded) {
            workflow_settings_loaded = true;
            const auto settings = config.recoil;
            launch([settings](Impl& s) {
                const auto path = std::filesystem::path("cache/recoil/workflow-settings.json");
                if (std::filesystem::exists(path)) {
                    s.workflow_weapon_settings = nlohmann::json::parse(read_workflow_file(path.string()));
                    if (!s.workflow_weapon_settings.is_object()) throw std::runtime_error("阶段设置无效");
                    const auto value=s.workflow_weapon_settings.value(s.workflow_weapon,nlohmann::json::object());
                    s.workflow_group_shots=std::clamp(value.value("group_shots",30),1,50);
                    s.workflow_target_shots=std::clamp(value.value("target_shots",5),1,s.workflow_group_shots);
                    s.workflow_step_shots=std::clamp(value.value("step_shots",5),1,50);
                    s.workflow_duration_ms=std::clamp(value.value("duration_ms",3000),500,10000);
                    s.workflow_calibration_path=value.value("calibration_path",std::string{});
                    s.selected_file=value.value("selected_file",std::string{});
                }
                s.refresh_workflow_selection(settings); s.workflow_profiles_directory = settings.profile_directory;
            });
            return;
        }
        if (workflow_profiles_directory != config.recoil.profile_directory) {
            workflow_profiles_directory = config.recoil.profile_directory;
            selected_file.clear(); loaded = false; remember_workflow_weapon();
            workflow_after_calibration.reset(); workflow_prepare_next.reset();
            refresh_workflow_selection(config.recoil); return;
        }
        ImGui::TextWrapped("对准固定靶点，准备后回游戏按顶部测试键。每次只执行一组；标定只移动，采集和测试会自动射击。");
        bool changed = false;
        if (ImGui::BeginCombo("武器", weapon::display_name(workflow_weapon).data())) {
            for (const auto& item : weapon::kWeaponNames) if (ImGui::Selectable(item.display_name.data(), workflow_weapon == item.canonical_id)) {
                select_workflow_weapon(std::string(item.canonical_id));
                refresh_workflow_selection(config.recoil);
                changed = true;
            }
            ImGui::EndCombo();
        }
        help("选择本组实际持有的武器；运行时由GSI核对。没有已知曲线也可以采集。");
        if (!snapshot.weapon_snapshot.canonical_id.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("使用当前武器")) {
                select_workflow_weapon(snapshot.weapon_snapshot.canonical_id);
                refresh_workflow_selection(config.recoil);
                changed = true;
            }
            help("带入GSI最后识别的武器；不生成任何弹道数据。");
        }
        changed |= ImGui::SliderInt("本次发数", &workflow_target_shots, 1, workflow_group_shots);
        help("按武器和后坐力调整，可从1至2发开始。GSI有延迟，超发或未知数据不能自动用于本阶段优化。");
        if (ImGui::TreeNode("阶段与采集设置")) {
            changed |= ImGui::SliderInt("每阶段追加发数", &workflow_step_shots, 1, workflow_group_shots);
            changed |= ImGui::SliderInt("整组发数", &workflow_group_shots, 1, 50);
            workflow_target_shots = std::min(workflow_target_shots, workflow_group_shots);
            changed |= ImGui::SliderInt("最长扫射 / ms", &workflow_duration_ms, 500, 10000);
            help("无论弹药更新是否及时，到达此时长都停止。它是兜底上限，不代替实际发数。");
            changed |= ImGui::InputText("画面标定文件", &workflow_calibration_path);
            help("标定完成自动带入；也可载入此前相同灵敏度和画面配置的标定文件。");
            if (ImGui::Button("重新准备画面标定")) { workflow_after_calibration.reset(); workflow_prepare_next = debug_session::Mode::RECOIL_CALIBRATE; }
            help("画面或灵敏度变化后重新标定；仅准备，仍须回游戏按测试键才移动，不开枪。");
            ImGui::TreePop();
        }
        const bool has_saved_curve = std::any_of(files.begin(),files.end(),[&](const auto& file) { return file.profile->weapon_id == workflow_weapon; });
        if (ImGui::BeginCombo("测试曲线", selected_file.empty() ? (has_saved_curve ? "请选择已有曲线" : "暂无曲线：可导入或采集") : selected_file.c_str())) {
            for (const auto& file : files) if (file.profile->weapon_id == workflow_weapon &&
                ImGui::Selectable(file.file.c_str(), selected_file == file.file)) {
                load_file(config.recoil, file.file); changed = true;
            }
            ImGui::EndCombo();
        }
        help("测试使用已保存文件；新武器无需先加载曲线即可采集。");
        if (ImGui::TreeNode("导入已有曲线 JSON")) {
            if(ImGui::Button("选择曲线文件")) {
                workflow_after_calibration.reset(); workflow_prepare_next.reset();
                const auto settings=config.recoil;
                const auto owner=static_cast<HWND>(ImGui::GetMainViewport()->PlatformHandleRaw);
                launch([settings,owner](Impl& s) {
                    const auto path=choose_recoil_profile(owner,s.job_cancellation);
                    if(path.empty() || s.job_cancellation->load()){s.status="已取消文件选择，当前曲线保持不变。";return;}
                    s.workflow_profile_import=path;
                    s.import_workflow_profile(settings);
                });
            }
            help("打开Windows文件选择窗口，选择JSON后直接导入并准备验证；取消不会更改当前曲线。");
            ImGui::InputText("已有曲线文件", &workflow_profile_import);
            ImGui::BeginDisabled(workflow_profile_import.empty());
            if (ImGui::Button("导入并准备验证")) {
                actions.debug_plan_edited = true;
                workflow_after_calibration.reset(); workflow_prepare_next.reset();
                const auto settings = config.recoil;
                launch([settings](Impl& s) {s.import_workflow_profile(settings);});
            }
            ImGui::EndDisabled();
            help("导入结构有效的曲线另存为候选，保留原文件，不要求先采集或标定，不自动激活或射击。");
            ImGui::TreePop();
        }
        if (loaded && base.weapon_id == workflow_weapon && ImGui::TreeNode("曲线强度")) {
            ImGui::BeginDisabled(workflow_locked_prefix_ms > 0);
            changed |= strength_controls();
            ImGui::EndDisabled();
            if (workflow_locked_prefix_ms > 0) ImGui::TextWrapped("前段已锁定：保留现有力度，优化器只调整新增段；整组微调时可解除锁定。");
            ImGui::TreePop();
        }
        changed |= ImGui::Checkbox("记录优化数据", &workflow_record_measurement);
        help("已有曲线验证默认关闭，无需画面标定；开启后记录残差用于优化，缺少标定会先准备标定。");
        if (changed) {
            actions.debug_plan_edited = true; remember_workflow_weapon();
            workflow_samples.clear(); workflow_confirmed = false; workflow_count_ok = false;
            workflow_after_calibration.reset(); workflow_prepare_next.reset();
        }
        ImGui::BeginDisabled(pending_action != nullptr || (debug && debug->busy));
        if (ImGui::Button("采集新弹道")) {
            workflow_record_measurement = true;
            workflow_after_calibration = workflow_calibration_path.empty() ? std::optional{debug_session::Mode::RECOIL_CAPTURE} : std::nullopt;
            workflow_prepare_next = workflow_after_calibration ? debug_session::Mode::RECOIL_CALIBRATE : debug_session::Mode::RECOIL_CAPTURE;
        }
        help("仅准备本次采集；没有标定时先准备标定。每一步都必须回游戏重新按测试键，不连续自动执行。");
        ImGui::SameLine();
        ImGui::BeginDisabled(selected_file.empty() || !loaded || base.weapon_id != workflow_weapon);
        if (ImGui::Button("验证已有弹道")) {
            workflow_after_calibration = workflow_record_measurement && workflow_calibration_path.empty()
                ? std::optional{debug_session::Mode::RECOIL_TEST} : std::nullopt;
            workflow_prepare_next = workflow_after_calibration ? debug_session::Mode::RECOIL_CALIBRATE : debug_session::Mode::RECOIL_TEST;
        }
        help("冻结当前曲线并准备有界测试，不要求先采集；仍须回游戏按测试键执行，不代表已校准发布。");
        ImGui::EndDisabled(); ImGui::EndDisabled();
        ImGui::TextWrapped("当前步骤：%s", workflow_after_calibration ? "画面标定；完成后只准备下一步" :
            workflow_prepare_next ? "正在准备下一步" : debug && debug->busy ? "本组处理中" :
            debug && debug->repeat_ready ? "已准备，等待你按测试键" : "选择采集或验证");
        if(debug&&debug->busy&&debug->plan.is_object()&&
            debug->plan.value("kind",std::string{}).starts_with("recoil_")&&!debug->message.empty())
            ImGui::TextWrapped("本组状态：%s",debug->message.c_str());
        if (debug && debug->repeat_ready) ImGui::TextWrapped("已准备：回游戏对准靶点，按一下测试键后松开，等待本组完成。修改参数后请重新准备。");
        ImGui::TextWrapped("采集的是视角/靶点运动候选，不把准星动画当弹着点。固定位置和姿态；每组对准新的干净靶面。");
        workflow_results(config, actions, debug);
        if (workflow_settings_dirty && !ImGui::IsAnyItemActive() && actions.debug_action == debug_session::Action::NONE && !pending_action) {
            workflow_settings_dirty = false;
            launch([](Impl& s) {
                std::filesystem::create_directories("cache/recoil");
                const auto temp=std::filesystem::path("cache/recoil/workflow-settings.json.partial-"+std::to_string(GetCurrentProcessId()));
                { std::ofstream out(temp,std::ios::binary); out << s.workflow_weapon_settings.dump(2); out.close();
                    if(!out) throw std::runtime_error("阶段设置保存失败"); }
                if(!MoveFileExW(temp.c_str(),L"cache/recoil/workflow-settings.json",MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
                    throw std::runtime_error("阶段设置替换失败");
            });
        }
        // 文件与设置先落盘，避免同帧后台任务导致 App 丢弃 PREPARE；这里从不发送 START。
        if (can_edit && workflow_prepare_next && !pending_action && !job && (!debug || !debug->busy) &&
            actions.debug_action == debug_session::Action::NONE) {
            const auto mode = *workflow_prepare_next; workflow_prepare_next.reset();
            if (mode != debug_session::Mode::RECOIL_TEST || (loaded && base.weapon_id == workflow_weapon))
                prepare_workflow(mode,config,actions);
        }
    }

    weapon::TimingCatalog timing_catalog = weapon::default_timing_catalog();
    std::string timing_loaded_path, timing_path_draft, timing_status;
    std::size_t timing_selected = 0;
    bool timing_loaded = false, timing_valid = false, timing_dirty = false;

    void load_timing(const std::string& path) {
        if (!working_copy) { launch([path](Impl& state) { state.load_timing(path); }); return; }
        // 仅首次展开、应用路径或显式重载访问磁盘；失败也缓存，避免每帧重试。
        timing_loaded = true;
        timing_loaded_path = timing_path_draft = path;
        timing_valid = false;
        timing_dirty = false;
        std::error_code ec;
        const auto file = std::filesystem::u8path(path);
        const bool exists = std::filesystem::exists(file, ec);
        if (path.empty() || ec) { timing_status = "资料路径不可用，请修正路径后重新载入。"; return; }
        if (!exists) {
            timing_catalog = weapon::default_timing_catalog();
            timing_valid = true;
            timing_status = path == "cache/recoil/weapon-timing.json" ?
                "默认资料文件尚不存在，运行可直接使用33项内置实测资料；保存可生成独立资料文件。" :
                "自定义资料文件不存在，以下仅为内置草稿。必须先保存资料再启动运行，否则启动将拒绝。";
        } else {
            timing_valid = weapon::load_timing_catalog(file, timing_catalog, timing_status);
            if (timing_valid) timing_status = "已载入资料；界面编辑不会热改当前运行快照。";
        }
    }

    void timing_settings(const RuntimeSnapshot& snapshot, AppConfig& config, bool can_edit) {
        if (!ImGui::CollapsingHeader("武器点射资料", ImGuiTreeNodeFlags_DefaultOpen)) return;
        if (!timing_loaded || timing_loaded_path != config.weapon_timing_file) {
            load_timing(config.weapon_timing_file);
            ImGui::TextUnformatted("正在后台载入武器资料。");
            return;
        }
        ImGui::TextWrapped("自动扳机使用共享GSI识别到的武器点射资料，独立于压枪开关。修改资料在下一次启动运行时生效。");
        ImGui::TextWrapped("当前GSI武器：%s（%s）", weapon::display_name(snapshot.weapon_snapshot.canonical_id).data(),
            weapon::status_name(snapshot.weapon_snapshot.status));
        const auto& active_timing = snapshot.trigger.context;
        if (snapshot.state == RuntimeState::RUNNING && snapshot.trigger_telemetry_available &&
            active_timing.timing_required && active_timing.timing_valid && active_timing.valid &&
            active_timing.generation != 0 && !active_timing.timing_weapon_id.empty()) {
            ImGui::TextWrapped("运行实际点射资料：%s / r%llu；按住 %dms，按下间隔 %dms。",
                weapon::display_name(active_timing.timing_weapon_id).data(),
                static_cast<unsigned long long>(active_timing.timing_catalog_revision),
                active_timing.shot_hold_ms, active_timing.fire_interval_ms);
        } else ImGui::TextWrapped("运行点射资料：当前未启用或上下文无效，不可作为开火依据；下方为配置草稿。");
        {
            DisabledScope disabled(!can_edit);
            if (form("weapon_timing_settings")) {
                row("资料文件", "回车应用路径并加载资料；只在首次、路径应用和手动重载时读盘。默认cache/recoil/weapon-timing.json缺失可直接使用内置33项，自定义路径缺失必须先保存。损坏文件不静默回退。");
                if (ImGui::InputText("##weapon_timing_file", &timing_path_draft, ImGuiInputTextFlags_EnterReturnsTrue)) {
                    config.weapon_timing_file = timing_path_draft;
                    load_timing(config.weapon_timing_file);
                }
                ImGui::EndTable();
            }
            if (ImGui::Button("重新载入武器资料")) load_timing(config.weapon_timing_file);
            help("丢弃本区未保存的参数编辑并重读当前文件；运行中禁用，不更改已有报告。");
        }
        if (!timing_status.empty()) ImGui::TextWrapped("%s", timing_status.c_str());
        if (!timing_valid) return;
        ImGui::Text("资料版本：%llu；来源：%s%s", static_cast<unsigned long long>(timing_catalog.revision),
            weapon::timing_catalog_source().data(), timing_dirty ? "；有未保存编辑" : "");
        if (ImGui::BeginCombo("查看/编辑武器", weapon::display_name(timing_catalog.profiles[timing_selected].canonical_id).data())) {
            for (std::size_t i = 0; i < timing_catalog.profiles.size(); ++i)
                if (ImGui::Selectable(weapon::display_name(timing_catalog.profiles[i].canonical_id).data(), timing_selected == i)) timing_selected = i;
            ImGui::EndCombo();
        }
        help("只切换下面的资料编辑对象，不改变自动扳机实际选用的武器。运行中仍可查看各武器资料。");
        auto& profile = timing_catalog.profiles[timing_selected];
        {
            DisabledScope disabled(!can_edit);
            if (form("weapon_timing_profile")) {
                row("按住时长 / ms", "范围1至500ms。左键DOWN确认到请求UP的目标时长；取消和清理优先。该数值不是人类反应时间或游戏实际击发时刻。");
                timing_dirty |= ImGui::InputInt("##weapon_timing_hold", &profile.shot_hold_ms);
                row("按下间隔 / ms", "须大于按住时长且不超过2000ms。两次左键DOWN提交之间的最小间隔；与急停和目标资格等待并行取最晚到期，不在松开后再等完整间隔。实际值可因调度延后。");
                timing_dirty |= ImGui::InputInt("##weapon_timing_interval", &profile.fire_interval_ms);
                ImGui::EndTable();
            }
            const bool valid = weapon::valid_timing_catalog(timing_catalog);
            if (!valid) ImGui::TextWrapped("参数无效：按住须为1至500ms，间隔须大于按住且不超过2000ms。保存前须修正。");
            DisabledScope invalid(!valid || timing_catalog.revision == std::numeric_limits<std::uint64_t>::max());
            if (ImGui::Button("保存武器点射资料")) {
                auto saved = timing_catalog;
                ++saved.revision;
                const auto path = config.weapon_timing_file;
                launch([saved, path](Impl& state) {
                    if (weapon::save_timing_catalog(std::filesystem::u8path(path), saved, state.timing_status)) {
                        state.timing_catalog = saved;
                        state.timing_dirty = false;
                        state.timing_status = "资料已原子保存；下次启动运行读取新版本。启用、路径和手选项请使用全局保存配置。";
                    }
                });
            }
            help("校验后原子替换资料文件并递增版本；失败保留原文件。只保存共享点射参数，不发布或修改压枪弹道。");
        }
        if (profile.canonical_id == "revolver") ImGui::TextWrapped("R8仅保留原始数据，延迟击发模型未启用；不能选择为活动武器。");
        ImGui::TextWrapped("这些值是人工测试的点射节奏，不代表游戏循环射速、后坐力已复位或真实停稳证明；随机波动仍未启用。");
    }

    void prepare_panel(const AppConfig& config) {
        const bool show_calibration = ImGui::TreeNode("校准此版本");
        help("为磁盘上已保存的版本准备独立校准会话；只有用户执行前台命令才会连接设备。");
        if (!show_calibration) return;
        ImGui::TextWrapped("对象：%s。准备使用磁盘上的已保存候选，不包含未保存草稿。", selected_file.c_str());
        ImGui::TextWrapped("只准备一次弹序的独立会话。退出应用释放设备后，由你在前台执行生成的命令；结果不会自动发布。");
        ImGui::InputText("已保存应用配置", &calibration_config);
        help("读取并绑定这份已保存INI的身份；当前界面尚未保存的更改不会进入校准会话。");
        ImGui::InputText("新的校准目录", &calibration_output);
        help("保存此次请求、候选身份和前台命令。默认归入cache/recoil/calibration；必须使用新目录，旧会话不会覆盖。");
        if (ImGui::Button("带入当前武器与灵敏度")) {
            calibration_request.environment = {base.weapon_id, "", "kmbox_net", "", config.recoil.sensitivity};
            calibration_request.hold_virtual_key = config.recoil.hold_virtual_key;
        }
        help("复制当前曲线的武器和游戏灵敏度；输入路径固定为KMBOX NET，不产生校准结果。");
        if (ImGui::TreeNode("环境与一次性预算")) {
            auto& e = calibration_request.environment; auto& l = calibration_request.limits;
            ImGui::TextWrapped("预算必须按本次试验明确填写；命令相位预算仅约束发送时限，不是已验证的物理相位容差。");
            ImGui::InputText("武器标识", &e.weapon_id); help("必须与所选磁盘曲线的武器标识相同；不会根据此字段生成新弹道。");
            e.input_path = "kmbox_net";
            ImGui::InputDouble("灵敏度", &e.sensitivity);
            help("本次游戏使用的正值灵敏度；改变它会改变候选适用环境，不是草稿强度滑块。");
            ImGui::InputInt("保持键 / VK", &calibration_request.hold_virtual_key);
            help("本次校准保持键的Windows虚拟键码；必须完整松开后重新按下，退出时仍执行清理。");
            ImGui::InputInt("取消键 / VK", &calibration_request.cancel_virtual_key);
            help("本次校准取消键的Windows虚拟键码；按下后结束当前一次会话，不能复用已消费许可。");
            ImGui::InputInt("会话总时长上限 / ms", &l.max_session_duration_ms);
            help("从武装开始计的整场校准时间上限，单位毫秒；到期不再接受新补偿命令。");
            ImGui::InputInt("单次弹序最长 / ms", &l.max_firing_duration_ms);
            help("一次射击弹序允许的最长补偿时间，单位毫秒；不得超过本次会话总预算。");
            ImGui::InputScalar("累计绝对位移上限 / counts", ImGuiDataType_U64, &l.max_sent_l1_counts);
            help("本次会话所有已发送命令的|X|+|Y|累计上限，单位设备counts；未知回执不退还额度。");
            ImGui::InputInt("单命令绝对位移上限 / counts", &l.max_command_l1_counts);
            help("单次补偿命令的|X|+|Y|上限，单位设备counts；超过预算的动作被拒绝，不拆成追补动作。");
            ImGui::InputInt("滚动窗口 / ms", &l.rolling_window_ms);
            help("累计窗口采用真实经过的毫秒，调度tick不会使额度提前补满。");
            ImGui::InputDouble("窗口位移上限 / counts", &l.rolling_window_counts);
            help("上述时间窗口内允许的累计绝对位移，单位设备counts；与总预算和单命令预算同时生效。");
            ImGui::InputDouble("命令相位预算 / ms", &l.command_phase_budget_ms);
            help("只约束本次校准命令计划和完成的时限，单位毫秒；它不表示候选已有实测相位容差。");
            ImGui::InputText("可复用请求 JSON", &calibration_request_file);
            help("读取已有请求中的环境和预算；不会复用旧会话身份、消费标记或真实输出许可。");
            if (ImGui::Button("载入已有环境与预算")) {
                launch([](Impl& state) {
                    RecoilCalibrationPrepareRequest loaded_request;
                    if (load_recoil_calibration_request(state.calibration_request_file, loaded_request, state.status))
                        state.calibration_request = std::move(loaded_request);
                });
            }
            help("从上面的请求文件回填环境和预算，当前所选曲线与新的输出目录仍需单独核对。");
            ImGui::TreePop();
        }
        ImGui::BeginDisabled(selected_file.empty() || calibration_output.empty());
        if (ImGui::Button("准备独立校准会话")) {
            const auto directory = config.recoil.profile_directory;
            launch([directory](Impl& state) {
                state.calibration_command.clear();
                wchar_t executable[32768]{};
                const auto length = GetModuleFileNameW(nullptr, executable, 32768);
                if (length == 0 || length == 32768) state.status = "无法定位独立校准工具。";
                else {
                    auto request = state.calibration_request;
                    request.profile_path = std::filesystem::u8path(directory) / std::filesystem::u8path(state.selected_file);
                    request.config_path = std::filesystem::u8path(state.calibration_config);
                    request.output_directory = std::filesystem::u8path(state.calibration_output);
                    request.executable_path = std::filesystem::path(executable).parent_path() / "xen_recoil_calibration.exe";
                    RecoilCalibrationPrepared prepared;
                    prepare_output_parent(state.calibration_output);
                    if (prepare_recoil_calibration(request, prepared, state.status)) {
                        state.calibration_command = prepared.launch_command;
                        state.calibration_prepared_identity = state.selected_file + " / " + prepared.manifest.session_id +
                            " / " + prepared.manifest.profile_file_sha256;
                        state.status = "校准会话已准备；尚未启动设备，也未声明校准通过。";
                    }
                }
            });
        }
        help("仅生成绑定磁盘候选与配置身份的新校准会话。缺文件、身份不符或目录已存在会失败；不会启动设备。");
        ImGui::EndDisabled();
        if (!calibration_command.empty()) {
            ImGui::TextWrapped("以下命令固定绑定已准备对象：%s。修改上方选项不会改变它，需要重新准备。", calibration_prepared_identity.c_str());
            ImGui::InputTextMultiline("前台命令", &calibration_command, {-1, 90}, ImGuiInputTextFlags_ReadOnly);
            help("此命令绑定已准备的版本、配置与会话；更改上方输入后必须重新准备，不能把旧命令当成新参数。");
            if (ImGui::Button("复制前台命令")) ImGui::SetClipboardText(calibration_command.c_str());
            help("只复制命令。请先退出应用释放设备，再由你在前台明确执行。");
        }
        ImGui::TreePop();
    }

    void set_draft(const RecoilProfile& profile) {
        report.reset();
        base = draft = profile; tuning = {}; checkpoint = {draft, tuning}; undo.clear(); redo.clear();
        loaded = true; calibration_confirmed = false; calibration = profile.calibration;
        phase_ms = profile.phase_tolerance_ms.value_or(0); recovery_ms = profile.recovery_ms.value_or(0);
        save_revision = profile.revision == std::numeric_limits<std::uint64_t>::max() ? profile.revision : profile.revision + 1;
        compile();
    }
    void compile() {
        if (!working_copy) {
            preview_valid = false;
            launch([](Impl& state) { state.compile(); });
            return;
        }
        preview_valid = compile_recoil_profile(draft, tuning, preview, status);
    }
    void edited() {
        report.reset();
        if (undo.size() >= 20) undo.erase(undo.begin());
        undo.push_back(checkpoint); redo.clear(); checkpoint = {draft, tuning}; calibration_confirmed = false; compile();
    }
    void import_workflow_profile(const RecoilConfig& settings) {
        if(job_cancellation && job_cancellation->load())return;
        RecoilProfile candidate;
        if(!load_recoil_profile(read_workflow_file(workflow_profile_import),candidate,status))return;
        RecoilStore store(std::filesystem::u8path(settings.profile_directory));
        std::string file;
        // 用户常直接选择曲线目录内的现有文件；相同内容直接选中，避免重复另存同一曲线。
        refresh(settings);
        for(const auto& saved:files)
            if(serialize_recoil_profile(*saved.profile)==serialize_recoil_profile(candidate)){file=saved.file;break;}
        if(job_cancellation && job_cancellation->load())return;
        if(file.empty()&&!store.save_new(candidate,file,status))return;
        select_workflow_weapon(candidate.weapon_id);
        workflow_after_calibration.reset(); workflow_prepare_next.reset();
        load_file(settings,file); refresh(settings);
        if(!loaded||base.weapon_id!=workflow_weapon)return;
        workflow_record_measurement=false; workflow_prepare_next=debug_session::Mode::RECOIL_TEST;
        status="曲线已选中并准备验证；回游戏按测试键即可开始本组。";
    }
    void refresh(const RecoilConfig& config) {
        if (!working_copy) { launch([config](Impl& state) { state.refresh(config); }); return; }
        try { RecoilStore store(std::filesystem::u8path(config.profile_directory)); store.list(files, status); } catch (...) { status = "曲线目录无效。"; }
    }
    void refresh_workflow_selection(const RecoilConfig& config) {
        if (!working_copy) { launch([config](Impl& state) { state.refresh_workflow_selection(config); }); return; }
        refresh(config);
        if (!selected_file.empty()) {
            const auto remembered = selected_file;
            load_file(config,remembered);
            if (loaded && base.weapon_id == workflow_weapon) return;
            selected_file.clear(); loaded = false;
        }
        const RecoilStoredProfile* only = nullptr;
        for (const auto& file : files) {
            if (file.profile->weapon_id != workflow_weapon) continue;
            if (only) return;
            only = &file;
        }
        if (only) load_file(config,only->file);
    }
    void load_file(const RecoilConfig& config, const std::string& file) {
        if (!working_copy) { launch([config, file](Impl& state) { state.load_file(config, file); }); return; }
        loaded = false;
        try { RecoilStore store(std::filesystem::u8path(config.profile_directory)); RecoilProfile p;
        if (store.load(file, p, status)) { selected_file = file; set_draft(p);
            if (workflow_settings_loaded && base.weapon_id == workflow_weapon) remember_workflow_weapon();
            status = "已加载独立草稿；活动曲线未改变。"; } } catch (...) { status = "曲线文件或目录无效。"; }
    }
    bool strength_controls() {
        bool changed = false;
        if (form("recoil_tuning")) {
            row("垂直强度", "只改变草稿Y增量；100%保持基线，正比例保留原方向。不会热改正在执行的版本。");
            double vertical = tuning.y_strength * 100;
            if (ImGui::SliderScalar("##recoil_y_strength", ImGuiDataType_Double, &vertical, &kZero, &kTwoHundred, "%.0f%%")) { tuning.y_strength = vertical / 100; changed = true; }
            row("水平强度", "只改变草稿X增量；原X为0时请使用节点或局部分段修正。");
            double horizontal = tuning.x_strength * 100;
            if (ImGui::SliderScalar("##recoil_x_strength", ImGuiDataType_Double, &horizontal, &kZero, &kTwoHundred, "%.0f%%")) { tuning.x_strength = horizontal / 100; changed = true; }
            ImGui::EndTable();
        }
        return changed;
    }
    void settings(const RuntimeSnapshot& snapshot, AppConfig& config) {
        auto& c = config.recoil;
        if (form("recoil_settings")) {
            row("启用压枪", "只在真实射击事实、源端焦点、GSI武器与匹配校准配置有效时补偿；启用本身不移动。");
            ImGui::Checkbox("##recoil_enabled", &c.enabled);
            row("Aim混合模式", "关闭时选择独立压枪阶段能力；混合需外部运动账本和对应物理验收，不改变Aim参数。");
            ImGui::Checkbox("##recoil_mixed", &c.mixed_aim);
            ImGui::EndTable();
        }
        std::string missing;
        const auto require = [&missing](bool valid, const char* name) {
            if (valid) return;
            if (!missing.empty()) missing += "、";
            missing += name;
        };
        require(config.mouse.backend == MouseBackend::KMBOX_NET, "KMBOX NET 后端（设置）");
        require(config.gsi.enabled, "启用 GSI（设置 / GSI自动武器识别配置）");
        require(config.source_context.enabled, "启用源端焦点（设置）");
        require(std::isfinite(c.sensitivity) && c.sensitivity > 0, "游戏灵敏度");
        require(!c.profile_directory.empty(), "曲线目录");
        if (!missing.empty()) ImGui::TextWrapped("配置缺项：%s。校准配置见调试 / 弹道工具；连接设置见括号位置。", missing.c_str());
        ImGui::TextWrapped("校准配置、曲线编辑和活动版本选择见调试 / 弹道工具。GSI 自动识别武器，执行仍需已校准曲线。");
        ImGui::TextWrapped("武器：%s（%s）", weapon::display_name(snapshot.weapon_snapshot.canonical_id).data(),
            weapon::status_name(snapshot.weapon_snapshot.status));
        if (!snapshot.recoil_profile_status.empty()) ImGui::TextWrapped("曲线匹配：%s", snapshot.recoil_profile_status.c_str());
        if (snapshot.recoil_telemetry_available) {
            ImGui::TextWrapped("压枪状态：%s；已确认 X %.2f / Y %.2f counts", reason_text(snapshot.recoil.reason),
                snapshot.recoil.confirmed_x, snapshot.recoil.confirmed_y);
        } else ImGui::TextUnformatted("本会话暂无压枪执行记录。");
    }
    void calibration_settings(AppConfig& config) {
        auto& c = config.recoil;
        if (!ImGui::CollapsingHeader("压枪校准配置")) return;
        if (form("recoil_calibration_settings")) {
            row("曲线目录", "新配置默认使用cache/recoil/profiles；旧配置中的自定义目录保留。按武器加载明确选定的活动版本，新文件不会自动激活。");
            ImGui::InputText("##recoil_directory", &c.profile_directory);
            row("游戏灵敏度", "与实际游戏设置一致；不能把灵敏度当某一把武器的力度滑块。"); ImGui::InputDouble("##recoil_sensitivity", &c.sensitivity, 0, 0, "%.4f");
            ImGui::EndTable();
        }
    }
    void connections(AppConfig& config) {
        if (ImGui::CollapsingHeader("GSI自动武器识别配置")) {
            auto& g = config.gsi;
            if (form("gsi_settings")) {
                row("启用GSI", "武器上下文来源，不是逐发、前台或停稳证据。"); ImGui::Checkbox("##gsi_enabled", &g.enabled);
                row("Xen接收地址", "填写运行Xen的电脑地址；双机时为辅机IP，游戏机向此地址发送GSI。127.0.0.1只适用于游戏与Xen在同一台电脑。"); ImGui::InputText("##gsi_bind", &g.bind_address);
                row("接收端口", "游戏GSI配置需指向此HTTP接收端口。"); int port = g.port;
                if (ImGui::InputInt("##gsi_port", &port)) g.port = static_cast<std::uint16_t>(std::clamp(port, 1, 65535));
                row("游戏机IPv4", "填写运行游戏的主机IP，只接受这台电脑发送的GSI；这里不是辅机地址。"); ImGui::InputText("##gsi_peer", &g.allowed_peer_ipv4);
                row("上下文有效期 / ms", "同一源时间的重复包不续命；不是逐发时间精度。"); ImGui::InputInt("##gsi_ttl", &g.ttl_ms);
                row("请求超时 / ms", "限制HTTP接收时间，不在设备实时线程解析请求。"); ImGui::InputInt("##gsi_timeout", &g.request_timeout_ms);
                ImGui::EndTable();
            }
            if (g.bind_address != "0.0.0.0" && g.bind_address != "127.0.0.1" && !g.bind_address.empty())
                ImGui::TextWrapped("游戏端GSI URI：http://%s:%u/gsi", g.bind_address.c_str(), static_cast<unsigned>(g.port));
            else ImGui::TextWrapped("双机使用时，游戏端URI须填写辅机实际IP，不能填写127.0.0.1或0.0.0.0。");
            ImGui::TextWrapped("玩家身份自动跟随游戏客户端，观战对象身份不一致时暂停识别。仅接收所配置游戏机IP的数据，无需认证环境变量。");
        }
    }

    void editor(const RuntimeSnapshot& snapshot, AppConfig& config) {
        if (ImGui::Button("刷新曲线目录")) refresh(config.recoil);
        help("重新列出所配置目录中的已保存曲线；不会挑选最新文件或自动切换活动版本。");
        ImGui::SameLine();
        if (ImGui::Button("加载当前武器活动版本")) {
            auto lookup = config.recoil;
            const auto weapon_id = snapshot.weapon_snapshot.canonical_id;
            launch([lookup, weapon_id](Impl& state) {
                RecoilStore store(std::filesystem::u8path(lookup.profile_directory));
                auto p = store.resolve(lookup, weapon_id, state.status);
                if (p) { state.selected_file.clear(); state.set_draft(*p); }
            });
        }
        help("按当前GSI武器读取已校准且灵敏度匹配的活动版本作为草稿；不会修改正在执行的版本。");
        if (ImGui::BeginCombo("已保存版本", selected_file.empty() ? "选择独立版本" : selected_file.c_str())) {
            for (const auto& file : files) if (ImGui::Selectable(file.file.c_str(), selected_file == file.file)) load_file(config.recoil, file.file);
            ImGui::EndCombo();
        }
        help("选择要编辑的磁盘版本；选择行为只加载草稿，不会发布或激活。");
        ImGui::InputText("文件名", &manual_file); help("填写曲线目录内的文件名；目录外路径和不合规曲线会被拒绝。"); ImGui::SameLine();
        if (ImGui::Button("加载文件")) load_file(config.recoil, manual_file);
        help("从曲线目录读取上面的文件到编辑草稿，保留活动索引。");
        if (!loaded) { ImGui::TextWrapped("先加载已有曲线，再编辑草稿；不会自动创建虚构弹道。"); return; }
        ImGui::TextWrapped("编辑对象固定：%s / %s / 基线 %llu；GSI换枪不会切走当前草稿。", weapon::display_name(base.weapon_id).data(), base.id.c_str(),
            static_cast<unsigned long long>(base.revision));
        bool changed = false;
        changed |= strength_controls();
        if (ImGui::TreeNode("时序（高级）")) {
            changed |= ImGui::InputDouble("起压偏移 / ms", &tuning.start_offset_ms, 0.5, 1, "%.2f"); help("相对基线首次补偿时刻偏移；不允许跨到射击事件之前。0.5ms是编辑步长，不是硬实时保证。");
            changed |= ImGui::InputDouble("后续时间比例", &tuning.time_scale, 0.05, 0.1, "%.3f"); help("只伸缩首次非零节点后的时间，保持总位移；不改变扳机间隔或游戏射速。");
            ImGui::TreePop();
        }
        if (ImGui::Button("还原草稿")) { draft = base; tuning = {}; changed = true; }
        help("将内存草稿恢复到本次加载的基线并清除强度/时序调整；磁盘文件和活动索引不变。");
        ImGui::SameLine();
        ImGui::BeginDisabled(undo.empty());
        if (ImGui::Button("撤销")) { redo.push_back({draft, tuning}); const auto previous = undo.back(); undo.pop_back(); draft = previous.draft; tuning = previous.tuning; checkpoint = {draft, tuning}; calibration_confirmed = false; report.reset(); compile(); }
        help("撤销本次编辑历史中的一步；没有可撤销步骤时禁用，不会回退活动版本。");
        ImGui::EndDisabled(); ImGui::SameLine(); ImGui::BeginDisabled(redo.empty());
        if (ImGui::Button("重做")) { undo.push_back({draft, tuning}); const auto next = redo.back(); redo.pop_back(); draft = next.draft; tuning = next.tuning; checkpoint = {draft, tuning}; calibration_confirmed = false; report.reset(); compile(); }
        help("恢复最近撤销的草稿操作；改变草稿后需重新核对校准声明。");
        ImGui::EndDisabled();
        if (ImGui::TreeNode("曲线节点（累计counts，非逐发编号）")) {
            if (ImGui::BeginTable("recoil_nodes", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, {0, 210})) {
                for (const char* column : {"节点", "时间/ms", "累计X", "累计Y"}) ImGui::TableSetupColumn(column);
                ImGui::TableHeadersRow();
                ImGuiListClipper clipper; clipper.Begin(static_cast<int>(draft.points.size()));
                while (clipper.Step()) for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    auto& p = draft.points[i]; ImGui::PushID(i); ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Text("%d", i);
                    ImGui::BeginDisabled(i == 0);
                    ImGui::TableSetColumnIndex(1); ImGui::SetNextItemWidth(-1); changed |= ImGui::InputDouble("##node_t", &p.time_ms, 0, 0, "%.2f");
                    help("相对弹序起点的毫秒数，时间必须严格递增且不超过60000ms；起点固定为0。");
                    ImGui::TableSetColumnIndex(2); ImGui::SetNextItemWidth(-1); changed |= ImGui::InputDouble("##node_x", &p.x_counts, 0, 0, "%.3f");
                    help("到此时刻的累计X设备counts，允许正负值；不是此节点的单次增量，起点固定为0。");
                    ImGui::TableSetColumnIndex(3); ImGui::SetNextItemWidth(-1); changed |= ImGui::InputDouble("##node_y", &p.y_counts, 0, 0, "%.3f");
                    help("到此时刻的累计Y设备counts，允许正负值；编辑后会重新校验曲线，不能沿用旧实测声明。");
                    ImGui::EndDisabled(); ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (ImGui::Button("追加末端节点") && draft.points.size() < 100000) { const auto tail = draft.points.back(); draft.points.push_back({tail.time_ms + 10, tail.x_counts, tail.y_counts}); changed = true; }
            help("在当前末端后10ms追加相同累计位移的节点；只延长零增量尾部，仍受节点数和总时长校验。");
            ImGui::SameLine(); ImGui::BeginDisabled(draft.points.size() <= 2);
            if (ImGui::Button("删除末端节点")) { draft.points.pop_back(); changed = true; }
            help("删除草稿最后一个节点；至少保留起点与一个末端节点，磁盘版本不变。");
            ImGui::EndDisabled(); ImGui::TreePop();
        }
        if (changed) edited();
        if (preview_valid) { chart("recoil_x_chart", base, preview, true); chart("recoil_y_chart", base, preview, false); }
        else ImGui::TextWrapped("草稿未通过生产编译：%s", status.c_str());
        ImGui::InputScalar("另存版本号", ImGuiDataType_U64, &save_revision);
        help("必须大于已加载基线的版本号，且目标文件不能已存在；另存不会覆盖旧版本。");
        ImGui::BeginDisabled(!preview_valid || save_revision <= base.revision);
        if (ImGui::Button("另存候选版本")) {
            auto candidate = preview; candidate.revision = save_revision;
            const auto settings = config.recoil;
            launch([candidate, settings](Impl& state) {
                RecoilStore store(std::filesystem::u8path(settings.profile_directory));
                if (store.save_new(candidate, state.selected_file, state.status)) { state.refresh(settings); state.status = "候选已另存；未校准、未激活。"; }
            });
        }
        help("将当前通过编译的草稿另存为未校准候选；此操作不授予物理输出资格，也不更新活动索引。");
        ImGui::EndDisabled();
        prepare_panel(config);
        if (ImGui::TreeNode("人工校准证据与版本发布")) {
            ImGui::TextWrapped("只填写已完成实机的真实证据；输入字段不会产生实测，修改草稿后需重新确认。");
            if (calibration.input_path.empty()) calibration.input_path = "kmbox_net";
            ImGui::InputText("真实Run证据路径", &calibration.evidence); help("指向当前草稿对应的真实Run记录；必须由你核对曲线、配置、环境及结果，路径存在本身不证明校准通过。");
            double sensitivity = calibration.sensitivity.value_or(0);
            if (ImGui::InputDouble("校准灵敏度", &sensitivity, 0, 0, "%.4f")) calibration.sensitivity = sensitivity;
            help("填写真实校准时的正值游戏灵敏度；不能从曲线强度或模拟回放推断。");
            ImGui::InputDouble("已验证相位容差 / ms", &phase_ms); help("填写真实测量支持的命令相位容差，单位毫秒；准备时的软件预算不能替代此值。");
            ImGui::InputDouble("已验证恢复时间 / ms", &recovery_ms); help("填写已实测支持的松键后恢复时间，单位毫秒；它决定后续弹序何时可重新开始。");
            ImGui::Checkbox("我已人工完成当前曲线的校准与证据核对", &calibration_confirmed);
            help("由你确认当前草稿与真实Run证据、配置和环境一致；更改草稿后确认会失效，程序不会代替人工判定效果。");
            ImGui::BeginDisabled(!preview_valid || !calibration_confirmed || save_revision <= base.revision);
            if (ImGui::Button("另存人工校准版本")) {
                const auto settings = config.recoil;
                launch([settings](Impl& state) {
                    RecoilStore store(std::filesystem::u8path(settings.profile_directory));
                    std::error_code ec;
                    if (!std::filesystem::exists(std::filesystem::u8path(state.calibration.evidence), ec) || ec) state.status = "校准证据路径不存在，不能保存已校准声明。";
                    else {
                        auto candidate = state.preview; candidate.revision = state.save_revision; candidate.state = RecoilProfileState::CALIBRATED;
                        candidate.calibration = state.calibration; candidate.phase_tolerance_ms = state.phase_ms; candidate.recovery_ms = state.recovery_ms;
                        if (store.save_new(candidate, state.selected_file, state.status, true)) { state.refresh(settings); state.status = "已保存人工声明的校准版本；仍未自动激活。"; }
                    }
                });
            }
            help("另存包含人工校准声明的新版本；须有有效草稿、人工确认、新版本号和存在的证据路径，保存后仍不自动激活。");
            ImGui::EndDisabled();
            ImGui::TextWrapped("发布对象：%s", selected_file.empty() ? "尚未选择已保存文件" : selected_file.c_str());
            ImGui::BeginDisabled(selected_file.empty());
            if (ImGui::Button("发布已选保存版本")) {
                const auto directory = config.recoil.profile_directory;
                launch([directory](Impl& state) {
                    RecoilStore store(std::filesystem::u8path(directory));
                    RecoilProfile saved;
                    if (store.load(state.selected_file, saved, state.status) && store.set_active(saved.weapon_id, state.selected_file, state.status))
                        state.status = "已更新活动索引；停止状态发布，下次新会话生效。";
                });
            }
            help("发布上面显示的磁盘文件，更新该武器的活动索引；不会发布未保存草稿，下一会话使用新选择。");
            ImGui::EndDisabled();
            if (ImGui::Button("回退该武器活动版本")) {
                const auto directory = config.recoil.profile_directory;
                launch([directory](Impl& state) {
                    RecoilStore store(std::filesystem::u8path(directory));
                    if (store.rollback(state.base.weapon_id, state.status)) state.status = "已回退活动索引；下一会话生效。";
                });
            }
            help("回退当前编辑武器的活动索引到之前的已保存版本，下一会话生效；不会删除候选。");
            ImGui::TreePop();
        }
        tuner();
    }
    void tuner() {
        if (!ImGui::CollapsingHeader("独立弹道自动优化器")) return;
        ImGui::TextWrapped("读取已完成独立压枪数据，批次间生成候选。GSI不提供逐发残差；缺响应H或独立留出将明确拒绝。");
        ImGui::InputText("Trial数据集JSON", &dataset_path);
        help("选择已完成、已核对基线与响应H的独立试验数据集；不接受把GSI包当作逐发测量。");
        if (ImGui::Button("导入数据集")) launch([](Impl& state) {
            state.dataset_loaded = recoil_tuner::load_dataset(std::filesystem::u8path(state.dataset_path), state.dataset, state.status);
            state.report.reset();
        });
        help("读取并验证数据集，清除旧分析显示；不会开始采集、训练或设备操作。");
        ImGui::InputScalar("优化代际", ImGuiDataType_U64, &request.generation);
        help("本次优化的正整数代际，用于数据与留出使用记录；重复消费同一留出会被拒绝。");
        ImGui::InputText("候选版本标识", &request.candidate_revision);
        help("为此次候选提供独立标识；保存为生产曲线时仍需另存大于基线的数值版本，不自动覆盖活动版本。");
        ImGui::InputDouble("每轴修改预算 / counts", &request.max_axis_correction_counts);
        help("限制本次优化单轴曲线修正量，单位设备counts；不是鼠标发送预算或物理校准结论。");
        ImGui::InputDouble("总修改预算 / counts", &request.max_total_correction_counts);
        help("限制本次优化的总修正量；提高预算不会补齐缺失的响应H、留出或真实试验。");
        ImGui::BeginDisabled(!dataset_loaded || !preview_valid || (job || pending_action));
        if (ImGui::Button("分析并生成独立候选")) {
            launch([](Impl& state) {
                const auto& dataset_copy = state.dataset;
                const auto& request_copy = state.request;
                const auto& base_copy = state.base;
                bool same = dataset_copy.base_profile_revision == std::to_string(base_copy.revision) && dataset_copy.base_curve.size() == base_copy.points.size();
                if (same) for (std::size_t i = 0; i < base_copy.points.size(); ++i) same &= dataset_copy.base_curve[i].time_ms == base_copy.points[i].time_ms &&
                    dataset_copy.base_curve[i].x_counts == base_copy.points[i].x_counts && dataset_copy.base_curve[i].y_counts == base_copy.points[i].y_counts;
                if (!same) { state.status = "数据集基线与加载的执行版本不一致，拒绝用草稿回填旧Run。"; return; }
                RecoilCandidateReplayReport replay;
                bool replay_attempted = false;
                auto result = recoil_tuner::optimize_profile_recorded(dataset_copy, request_copy, base_copy, [&](const auto& points, std::string& error) {
                    auto candidate = base_copy; candidate.state = RecoilProfileState::SCHEMA_VALID; candidate.points.clear();
                    candidate.phase_tolerance_ms.reset(); candidate.recovery_ms.reset(); candidate.calibration.evidence.clear();
                    for (const auto& p : points) candidate.points.push_back({p.time_ms, p.x_counts, p.y_counts});
                    replay_attempted = true;
                    return validate_recoil_candidate_execution(candidate, {}, replay, error);
                });
                if (replay_attempted) {
                    const auto checked = [&](bool value) {
                        return value ? "通过" : replay.validated && !replay.acknowledged_commands ? "不适用（无非零意图）" : "未完成";
                    };
                    std::ostringstream summary;
                    summary << "执行器软件回放" << (replay.validated ? "通过" : "未通过")
                        << "：步长=" << replay.step_ms << " ms，相位预算=" << replay.phase_budget_ms
                        << " ms，时长=" << replay.duration_ms << " ms，advance=" << replay.advance_calls
                        << '/' << replay.advance_limit << "，单轴命令上限=" << replay.command_axis_limit_counts
                        << " counts，最大单轴命令=" << replay.max_abs_command_axis_counts << " counts，模拟ACK="
                        << replay.acknowledged_commands << "条、L1=" << replay.acknowledged_l1_counts << " counts；尾部="
                        << checked(replay.tail_checked) << "，相位边界=" << checked(replay.phase_edge_checked)
                        << "，时限=" << checked(replay.deadline_checked) << "，取消=" << checked(replay.cancellation_checked)
                        << "，UNKNOWN=" << checked(replay.unknown_receipt_checked) << "，NOT_SENT=" << checked(replay.not_sent_checked)
                        << "。仅为模拟条件与回执，未验证实测相位、Worker累计/滚动预算或物理效果；候选仍未校准。";
                    result.messages.push_back(summary.str());
                }
                state.report = std::move(result);
                state.status = "后台分析完成；未连接设备。";
            });
            status = "正在后台分析已完成数据；未连接设备。";
        }
        help("后台分析已完成数据，并用生产执行器检查候选的模拟命令、尾部、取消和异常回执。需数据基线匹配；通过仍不代表物理验收。");
        ImGui::EndDisabled();
        if (report) {
            for (const auto& message : report->messages) ImGui::TextWrapped("%s", message.c_str());
            if (report->predictions_available) ImGui::Text("中心误差：训练 %.3f → %.3f；留出预测 %.3f → %.3f；候选实测：未提供",
                report->fit_before.robust_center_error, report->fit_predicted.robust_center_error,
                report->holdout_before.robust_center_error, report->holdout_predicted.robust_center_error);
            ImGui::InputText("独立新结果目录", &result_directory);
            help("默认归入cache/recoil/tuning下的独立目录；目标必须不存在，已有分析和留出记录不会被覆盖。");
            if (ImGui::Button("保存分析和候选")) {
                launch([](Impl& state) {
                    prepare_output_parent(state.result_directory);
                    recoil_tuner::save_result(std::filesystem::u8path(state.result_directory), *state.report, state.status);
                });
            }
            help("保存当前报告、模拟条件和可用候选到独立新目录；不会发布曲线，保存失败时保留当前分析显示。");
            ImGui::BeginDisabled(!report->candidate);
            if (ImGui::Button("将候选载入草稿预览")) {
                draft = base; draft.points.clear(); tuning = {};
                for (const auto& p : report->candidate->points) draft.points.push_back({p.time_ms, p.x_counts, p.y_counts});
                draft.state = RecoilProfileState::SCHEMA_VALID; edited(); status = "优化候选已载入草稿；未发布、未实测。";
            }
            help("将候选载入内存草稿，重新编译预览并清除人工校准确认；需另存版本后才有磁盘曲线。");
            ImGui::EndDisabled();
        }
    }
    static constexpr double kZero = 0, kTwoHundred = 200;
};
RecoilPanel::RecoilPanel() : impl_(std::make_unique<Impl>()) {}
RecoilPanel::~RecoilPanel() = default;
void RecoilPanel::poll() noexcept {
    try { impl_->poll(); }
    catch (...) { impl_->status = "后台结果应用失败；请重新加载已保存资料。"; }
}
bool RecoilPanel::busy() const noexcept { return impl_->job || impl_->pending_action; }
void RecoilPanel::request_cancel() noexcept {
    if(impl_->job_cancellation)impl_->job_cancellation->store(true);
    impl_->workflow_after_calibration.reset(); impl_->workflow_prepare_next.reset();
    if (impl_->pending_action) {
        impl_->pending_action = {};
        impl_->status = "已取消尚未开始的弹道任务。";
        return;
    }
    if (!impl_->job) return;
    impl_->cancel_requested = true;
    impl_->job->cancel();
}
void RecoilPanel::render(const RuntimeSnapshot& snapshot, AppConfig& config, bool can_edit) noexcept {
    try {
        poll();
        if (busy()) { ImGui::TextUnformatted("弹道任务正在后台处理；完成后可继续编辑。"); return; }
        DisabledScope disabled(!can_edit || busy());
        impl_->settings(snapshot, config);
        if (!snapshot.recoil_archive.error.empty()) ImGui::TextWrapped("射击归档异常：%s；详细状态见调试 / 运行诊断。", snapshot.recoil_archive.error.c_str());
        if (!impl_->status.empty()) ImGui::TextWrapped("%s", impl_->status.c_str());
    } catch (...) { impl_->status = "弹道面板操作失败；请检查文件与数据。"; }
}
void RecoilPanel::render_connections(AppConfig& config, bool can_edit) noexcept {
    try {
        DisabledScope disabled(!can_edit);
        impl_->connections(config);
    } catch (...) { impl_->status = "GSI配置面板操作失败。"; }
}
void RecoilPanel::render_diagnostics(const RuntimeSnapshot& snapshot) noexcept {
    try {
        const auto& a = snapshot.recoil_archive;
        if (a.acquisition_run_id.empty()) {
            ImGui::TextUnformatted("本会话暂无射击归档。");
            return;
        }
        ImGui::TextWrapped("射击归档：%s；完整 %llu / 不完整 %llu；%s", a.available ? "可用" : "不可用",
            static_cast<unsigned long long>(a.complete_batches), static_cast<unsigned long long>(a.incomplete_batches), a.directory.c_str());
        if (!a.error.empty()) ImGui::TextWrapped("归档错误：%s", a.error.c_str());
    } catch (...) { impl_->status = "射击归档状态显示失败。"; }
}
void RecoilPanel::render_tools(const RuntimeSnapshot& snapshot, AppConfig& config, bool can_edit,
    OverlayActions& actions, const debug_session::Snapshot* debug_snapshot) noexcept {
    try {
        poll();
        ImGui::TextUnformatted("弹道工具与射击归档");
        if (busy()) {
            if (actions.debug_action == debug_session::Action::CANCEL) request_cancel();
            ImGui::TextUnformatted("正在后台处理文件或分析；已开始的同步操作完成后退出，不会回滚已保存结果。");
            if (ImGui::Button("请求取消弹道任务")) request_cancel();
            help("取消尚未开始的任务；已经开始的同步存储和分析等待完成。不会连接设备。");
            return;
        }
        { DisabledScope disabled(!can_edit || busy());
            const auto prior_sensitivity = config.recoil.sensitivity;
            const auto prior_directory = config.recoil.profile_directory;
            impl_->calibration_settings(config);
            if (prior_sensitivity != config.recoil.sensitivity || prior_directory != config.recoil.profile_directory) {
                actions.debug_plan_edited = true; impl_->workflow_samples.clear();
                impl_->workflow_count_ok = false; impl_->workflow_confirmed = false;
                impl_->workflow_locked_prefix_ms = 0;
                impl_->workflow_after_calibration.reset(); impl_->workflow_prepare_next.reset();
            }
            impl_->workflow(snapshot, config, actions, debug_snapshot, can_edit);
            ImGui::Checkbox("高级：曲线编辑与数据集优化", &impl_->show_editor);
            help("展开曲线草稿、人工校准与离线优化工具；不会自动加载、激活或执行曲线。");
            if (impl_->show_editor) impl_->editor(snapshot, config);
            if (ImGui::TreeNode("高级：射击节奏")) {
                impl_->timing_settings(snapshot, config, can_edit);
                ImGui::TreePop();
            }
        }
        if (!impl_->status.empty()) ImGui::TextWrapped("%s", impl_->status.c_str());
    } catch (...) { impl_->status = "弹道工具操作失败；请检查文件与数据。"; }
}
