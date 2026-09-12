#include "overlay/recoil_panel.h"
#include "recoil/recoil_store.h"
#include "recoil/recoil_calibration_io.h"
#include "recoil_tuner/recoil_tuner.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <future>
#include <limits>
#include <sstream>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {
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
    std::future<recoil_tuner::Report> job;
    RecoilCalibrationPrepareRequest calibration_request;
    std::string calibration_config = "config.ini";
    std::string calibration_output = new_output_directory("calibration");
    std::string calibration_request_file, calibration_command;
    std::string calibration_prepared_identity;

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
        if (ImGui::Button("带入当前环境声明")) {
            calibration_request.environment = {base.weapon_id, config.recoil.game_build, config.recoil.input_path,
                config.recoil.conditions, config.recoil.sensitivity};
            calibration_request.hold_virtual_key = config.recoil.hold_virtual_key;
        }
        help("把当前已加载曲线的武器及应用中的环境声明复制到准备表单；仍需核对与本次试验一致，不产生校准结果。");
        if (ImGui::TreeNode("环境与一次性预算")) {
            auto& e = calibration_request.environment; auto& l = calibration_request.limits;
            ImGui::TextWrapped("预算必须按本次试验明确填写；命令相位预算仅约束发送时限，不是已验证的物理相位容差。");
            ImGui::InputText("武器标识", &e.weapon_id); help("必须与所选磁盘曲线的武器标识相同；不会根据此字段生成新弹道。");
            ImGui::InputText("游戏版本", &e.game_build); help("填写本次试验实际游戏版本，变化后旧校准声明不能直接复用。");
            ImGui::InputText("输入路径", &e.input_path); help("本次真实设备输入路径标识，需与应用配置和之后的校准记录一致。");
            ImGui::InputText("适用条件", &e.conditions); help("记录本次姿态、开镜等固定条件；GSI武器识别不证明这些条件已满足。");
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
                RecoilCalibrationPrepareRequest loaded_request;
                if (load_recoil_calibration_request(calibration_request_file, loaded_request, status))
                    calibration_request = std::move(loaded_request);
            }
            help("从上面的请求文件回填环境和预算，当前所选曲线与新的输出目录仍需单独核对。");
            ImGui::TreePop();
        }
        ImGui::BeginDisabled(selected_file.empty() || calibration_output.empty());
        if (ImGui::Button("准备独立校准会话")) {
            calibration_command.clear();
            wchar_t executable[32768]{};
            const auto length = GetModuleFileNameW(nullptr, executable, 32768);
            if (length == 0 || length == 32768) status = "无法定位独立校准工具。";
            else {
                auto request = calibration_request;
                request.profile_path = std::filesystem::u8path(config.recoil.profile_directory) / std::filesystem::u8path(selected_file);
                request.config_path = std::filesystem::u8path(calibration_config);
                request.output_directory = std::filesystem::u8path(calibration_output);
                request.executable_path = std::filesystem::path(executable).parent_path() / "xen_recoil_calibration.exe";
                RecoilCalibrationPrepared prepared;
                prepare_output_parent(calibration_output);
                if (prepare_recoil_calibration(request, prepared, status)) {
                    calibration_command = prepared.launch_command;
                    calibration_prepared_identity = selected_file + " / " + prepared.manifest.session_id +
                        " / " + prepared.manifest.profile_file_sha256;
                    status = "校准会话已准备；尚未启动设备，也未声明校准通过。";
                }
            }
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
        preview_valid = compile_recoil_profile(draft, tuning, preview, status);
    }
    void edited() {
        report.reset();
        if (undo.size() >= 20) undo.erase(undo.begin());
        undo.push_back(checkpoint); redo.clear(); checkpoint = {draft, tuning}; calibration_confirmed = false; compile();
    }
    void refresh(const RecoilConfig& config) {
        try { RecoilStore store(std::filesystem::u8path(config.profile_directory)); store.list(files, status); } catch (...) { status = "曲线目录无效。"; }
    }
    void load_file(const RecoilConfig& config, const std::string& file) {
        try { RecoilStore store(std::filesystem::u8path(config.profile_directory)); RecoilProfile p;
        if (store.load(file, p, status)) { selected_file = file; set_draft(p); status = "已加载独立草稿；活动曲线未改变。"; } } catch (...) { status = "曲线文件或目录无效。"; }
    }
    void settings(const RuntimeSnapshot& snapshot, AppConfig& config) {
        auto& c = config.recoil;
        if (form("recoil_settings")) {
            row("启用压枪", "只在真实射击事实、源端焦点、GSI武器与匹配校准配置有效时补偿；启用本身不移动。");
            ImGui::Checkbox("##recoil_enabled", &c.enabled);
            row("Aim混合模式", "关闭时选择独立压枪阶段能力；混合需外部运动账本和对应物理验收，不改变Aim参数。");
            ImGui::Checkbox("##recoil_mixed", &c.mixed_aim);
            row("曲线目录", "新配置默认使用cache/recoil/profiles；旧配置中的自定义目录保留。只加载活动索引或固定版本覆盖，新文件不会自动激活。");
            ImGui::InputText("##recoil_directory", &c.profile_directory);
            row("游戏版本", "必须与所选曲线的校准条件一致；改变后旧曲线不可伪装适配。"); ImGui::InputText("##recoil_build", &c.game_build);
            row("输入路径", "校准时使用的设备输入路径，不是压枪增益。"); ImGui::InputText("##recoil_path", &c.input_path);
            row("游戏灵敏度", "与实际游戏设置一致；不能把灵敏度当某一把武器的力度滑块。"); ImGui::InputDouble("##recoil_sensitivity", &c.sensitivity, 0, 0, "%.4f");
            row("适用条件", "填写校准时的姿态与开镜条件标识；GSI识别武器不证明这些条件。"); ImGui::InputText("##recoil_conditions", &c.conditions);
            row("混合图像有效期 / ms", "包含映射不确定度；过期图像不能支撑Aim混合补偿。"); ImGui::InputInt("##recoil_age", &c.max_observation_age_ms);
            row("固定版本覆盖", "使用下方已校准文件，保持活动索引不变。保存配置后持续有效，不会在会话结束时自动关闭；需手动关闭后恢复活动版本匹配。"); ImGui::Checkbox("##recoil_trial", &c.use_trial);
            row("覆盖文件", "曲线目录内的已保存文件名，禁止路径逃逸；仍须匹配武器与校准条件，不能执行未校准候选。"); ImGui::InputText("##recoil_trial_file", &c.trial_file);
            ImGui::EndTable();
        }
        ImGui::TextWrapped("武器：%s（%s）", snapshot.weapon_snapshot.canonical_id.empty() ? "未知" : snapshot.weapon_snapshot.canonical_id.c_str(),
            weapon::status_name(snapshot.weapon_snapshot.status));
        if (!snapshot.recoil_profile_status.empty()) ImGui::TextWrapped("曲线匹配：%s", snapshot.recoil_profile_status.c_str());
        if (snapshot.recoil_telemetry_available) {
            ImGui::TextWrapped("压枪状态：%s；已确认 X %.2f / Y %.2f counts", reason_text(snapshot.recoil.reason),
                snapshot.recoil.confirmed_x, snapshot.recoil.confirmed_y);
        } else ImGui::TextUnformatted("本会话暂无压枪执行记录。");
        if (ImGui::CollapsingHeader("GSI自动武器识别配置")) {
            auto& g = config.gsi;
            if (form("gsi_settings")) {
                row("启用GSI", "武器上下文来源，不是逐发、前台或停稳证据。"); ImGui::Checkbox("##gsi_enabled", &g.enabled);
                row("本地绑定地址", "单机建议127.0.0.1；双机需明确可达地址及允许来源，不默认监听公网。"); ImGui::InputText("##gsi_bind", &g.bind_address);
                row("接收端口", "游戏GSI配置需指向此HTTP接收端口。"); int port = g.port;
                if (ImGui::InputInt("##gsi_port", &port)) g.port = static_cast<std::uint16_t>(std::clamp(port, 1, 65535));
                row("本玩家标识", "仅接受对应玩家；观战或身份不匹配不沿用旧武器。"); ImGui::InputText("##gsi_player", &g.expected_player_id);
                row("允许源IPv4", "仅允许指定源主机请求；认证值仍必须由环境提供。"); ImGui::InputText("##gsi_peer", &g.allowed_peer_ipv4);
                row("上下文有效期 / ms", "同一源时间的重复包不续命；不是逐发时间精度。"); ImGui::InputInt("##gsi_ttl", &g.ttl_ms);
                row("请求超时 / ms", "限制HTTP接收时间，不在设备实时线程解析请求。"); ImGui::InputInt("##gsi_timeout", &g.request_timeout_ms);
                ImGui::EndTable();
            }
            ImGui::TextWrapped("认证由环境变量 XEN_GSI_TOKEN 提供；此处不输入或显示认证值。");
        }
    }

    void editor(const RuntimeSnapshot& snapshot, AppConfig& config) {
        std::unique_ptr<RecoilStore> owned_store;
        try { owned_store = std::make_unique<RecoilStore>(std::filesystem::u8path(config.recoil.profile_directory)); }
        catch (...) { status = "曲线目录无效，请修正后重试。"; return; }
        auto& store = *owned_store;
        if (ImGui::Button("刷新曲线目录")) refresh(config.recoil);
        help("重新列出所配置目录中的已保存曲线；不会挑选最新文件或自动切换活动版本。");
        ImGui::SameLine();
        if (ImGui::Button("加载当前武器活动版本")) {
            auto lookup = config.recoil; lookup.use_trial = false;
            auto p = store.resolve(lookup, snapshot.weapon_snapshot.canonical_id, status);
            if (p) { selected_file.clear(); set_draft(*p); }
        }
        help("按当前GSI武器和配置条件读取活动版本作为草稿；不会修改正在执行的版本或固定版本覆盖设置。");
        if (ImGui::BeginCombo("已保存版本", selected_file.empty() ? "选择独立版本" : selected_file.c_str())) {
            for (const auto& file : files) if (ImGui::Selectable(file.file.c_str(), selected_file == file.file)) load_file(config.recoil, file.file);
            ImGui::EndCombo();
        }
        help("选择要编辑的磁盘版本；选择行为只加载草稿，不会发布或激活。");
        ImGui::InputText("文件名", &manual_file); help("填写曲线目录内的文件名；目录外路径和不合规曲线会被拒绝。"); ImGui::SameLine();
        if (ImGui::Button("加载文件")) load_file(config.recoil, manual_file);
        help("从曲线目录读取上面的文件到编辑草稿，保留活动索引。");
        if (ImGui::Button("加载覆盖文件")) load_file(config.recoil, config.recoil.trial_file);
        help("读取配置中固定覆盖的文件到草稿；不会开启覆盖或改变其保存状态。");
        if (!loaded) { ImGui::TextWrapped("先加载已有曲线，再编辑草稿；不会自动创建虚构弹道。"); return; }
        ImGui::TextWrapped("编辑对象固定：%s / %s / 基线 %llu；GSI换枪不会切走当前草稿。", base.weapon_id.c_str(), base.id.c_str(),
            static_cast<unsigned long long>(base.revision));
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
            if (store.save_new(candidate, selected_file, status)) { refresh(config.recoil); status = "候选已另存；未校准、未激活。"; }
        }
        help("将当前通过编译的草稿另存为未校准候选；此操作不授予物理输出资格，也不更新活动索引。");
        ImGui::EndDisabled();
        prepare_panel(config);
        if (ImGui::TreeNode("人工校准证据与版本发布")) {
            ImGui::TextWrapped("只填写已完成实机的真实证据；输入字段不会产生实测，修改草稿后需重新确认。");
            ImGui::InputText("校准游戏版本", &calibration.game_build); help("填写真实校准Run使用的游戏版本，须与之后应用配置一致。");
            ImGui::InputText("校准输入路径", &calibration.input_path); help("填写真实校准Run的设备输入路径，不能把不同设备的校准直接替用。");
            ImGui::InputText("校准适用条件", &calibration.conditions); help("填写已人工核对的姿态、开镜等条件；只保存声明，不自动测量。");
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
                std::error_code ec;
                if (!std::filesystem::exists(std::filesystem::u8path(calibration.evidence), ec) || ec) status = "校准证据路径不存在，不能保存已校准声明。";
                else {
                    auto candidate = preview; candidate.revision = save_revision; candidate.state = RecoilProfileState::CALIBRATED;
                    candidate.calibration = calibration; candidate.phase_tolerance_ms = phase_ms; candidate.recovery_ms = recovery_ms;
                    if (store.save_new(candidate, selected_file, status, true)) { refresh(config.recoil); status = "已保存人工声明的校准版本；仍未自动激活。"; }
                }
            }
            help("另存包含人工校准声明的新版本；须有有效草稿、人工确认、新版本号和存在的证据路径，保存后仍不自动激活。");
            ImGui::EndDisabled();
            ImGui::TextWrapped("发布对象：%s", selected_file.empty() ? "尚未选择已保存文件" : selected_file.c_str());
            ImGui::BeginDisabled(selected_file.empty());
            if (ImGui::Button("发布已选保存版本")) {
                RecoilProfile saved;
                if (store.load(selected_file, saved, status) && store.set_active(saved.weapon_id, selected_file, status)) status = "已更新活动索引；停止状态发布，下次新会话生效。";
            }
            help("发布上面显示的磁盘文件，更新该武器的活动索引；不会发布未保存草稿，下一会话使用新选择。");
            ImGui::SameLine(); if (ImGui::Button("设为固定版本覆盖")) { config.recoil.use_trial = true; config.recoil.trial_file = selected_file; status = "已设置固定版本覆盖；活动索引不变。保存配置后持续有效，需手动关闭，不会随会话结束失效。"; }
            help("将已选磁盘文件设为配置中的固定覆盖；生产仍要求已校准并匹配条件。保存后持续有效，需手动关闭。");
            ImGui::EndDisabled();
            if (ImGui::Button("回退该武器活动版本")) if (store.rollback(base.weapon_id, status)) status = "已回退活动索引；下一会话生效。";
            help("回退当前编辑武器的活动索引到之前的已保存版本，下一会话生效；不会删除候选或自动关闭固定版本覆盖。");
            ImGui::TreePop();
        }
        tuner();
    }
    void tuner() {
        if (!ImGui::CollapsingHeader("独立弹道自动优化器")) return;
        ImGui::TextWrapped("读取已完成独立压枪数据，批次间生成候选。GSI不提供逐发残差；缺响应H或独立留出将明确拒绝。");
        ImGui::InputText("Trial数据集JSON", &dataset_path);
        help("选择已完成、已核对基线与响应H的独立试验数据集；不接受把GSI包当作逐发测量。");
        if (ImGui::Button("导入数据集")) { dataset_loaded = recoil_tuner::load_dataset(std::filesystem::u8path(dataset_path), dataset, status); report.reset(); }
        help("读取并验证数据集，清除旧分析显示；不会开始采集、训练或设备操作。");
        ImGui::InputScalar("优化代际", ImGuiDataType_U64, &request.generation);
        help("本次优化的正整数代际，用于数据与留出使用记录；重复消费同一留出会被拒绝。");
        ImGui::InputText("候选版本标识", &request.candidate_revision);
        help("为此次候选提供独立标识；保存为生产曲线时仍需另存大于基线的数值版本，不自动覆盖活动版本。");
        ImGui::InputDouble("每轴修改预算 / counts", &request.max_axis_correction_counts);
        help("限制本次优化单轴曲线修正量，单位设备counts；不是鼠标发送预算或物理校准结论。");
        ImGui::InputDouble("总修改预算 / counts", &request.max_total_correction_counts);
        help("限制本次优化的总修正量；提高预算不会补齐缺失的响应H、留出或真实试验。");
        ImGui::BeginDisabled(!dataset_loaded || !preview_valid || job.valid());
        if (ImGui::Button("分析并生成独立候选")) {
            bool same = dataset.base_profile_revision == std::to_string(base.revision) && dataset.base_curve.size() == base.points.size();
            if (same) for (std::size_t i = 0; i < base.points.size(); ++i) same &= dataset.base_curve[i].time_ms == base.points[i].time_ms &&
                dataset.base_curve[i].x_counts == base.points[i].x_counts && dataset.base_curve[i].y_counts == base.points[i].y_counts;
            if (!same) status = "数据集基线与加载的执行版本不一致，拒绝用草稿回填旧Run。";
            else {
                const auto dataset_copy = dataset; const auto request_copy = request; const auto base_copy = base;
                job = std::async(std::launch::async, [dataset_copy, request_copy, base_copy] {
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
                    return result;
                });
                status = "正在后台分析已完成数据；未连接设备。";
            }
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
                prepare_output_parent(result_directory);
                recoil_tuner::save_result(std::filesystem::u8path(result_directory), *report, status);
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
void RecoilPanel::render(const RuntimeSnapshot& snapshot, AppConfig& config, bool can_edit) noexcept {
    try {
        if (impl_->job.valid() && impl_->job.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) impl_->report = impl_->job.get();
        ImGui::Separator(); ImGui::TextUnformatted("压枪与弹道优化");
        ImGui::TextWrapped("运行中不能编辑或发布；GSI自动匹配活动曲线，下面的草稿不会热改执行版本。");
        if (!snapshot.recoil_archive.acquisition_run_id.empty()) {
            const auto& a = snapshot.recoil_archive;
            ImGui::TextWrapped("射击归档：%s；完整 %llu / 不完整 %llu；%s", a.available ? "可用" : "不可用",
                static_cast<unsigned long long>(a.complete_batches), static_cast<unsigned long long>(a.incomplete_batches), a.directory.c_str());
            if (!a.error.empty()) ImGui::TextWrapped("归档错误：%s", a.error.c_str());
        }
        { DisabledScope disabled(!can_edit || impl_->job.valid());
        impl_->settings(snapshot, config);
        ImGui::Checkbox("打开弹道编辑与优化", &impl_->show_editor);
        help("展开曲线草稿、人工校准与离线优化工具；不会自动加载、激活或执行曲线。运行或后台分析期间禁用编辑。");
        if (impl_->show_editor) impl_->editor(snapshot, config);
        }
        if (!impl_->status.empty()) ImGui::TextWrapped("%s", impl_->status.c_str());
    } catch (...) { impl_->status = "弹道面板操作失败；活动版本保持原有存储结果，请检查文件与数据。"; }
}
