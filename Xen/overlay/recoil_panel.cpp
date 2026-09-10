#include "overlay/recoil_panel.h"
#include "recoil/recoil_store.h"
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
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) { ImGui::BeginTooltip(); ImGui::TextWrapped("%s", text); ImGui::EndTooltip(); }
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
    std::string selected_file, manual_file, status, dataset_path, result_directory = "recoil-tuning-result";
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
        try { RecoilStore store(config.profile_directory); store.list(files, status); } catch (...) { status = "曲线目录无效。"; }
    }
    void load_file(const RecoilConfig& config, const std::string& file) {
        try { RecoilStore store(config.profile_directory); RecoilProfile p;
        if (store.load(file, p, status)) { selected_file = file; set_draft(p); status = "已加载独立草稿；活动曲线未改变。"; } } catch (...) { status = "曲线文件或目录无效。"; }
    }
    void settings(const RuntimeSnapshot& snapshot, AppConfig& config) {
        auto& c = config.recoil;
        if (form("recoil_settings")) {
            row("启用压枪", "只在真实射击事实、源端焦点、GSI武器与匹配校准配置有效时补偿；启用本身不移动。");
            ImGui::Checkbox("##recoil_enabled", &c.enabled);
            row("Aim混合模式", "关闭时选择独立压枪阶段能力；混合需外部运动账本和对应物理验收，不改变Aim参数。");
            ImGui::Checkbox("##recoil_mixed", &c.mixed_aim);
            row("曲线目录", "只加载明确活动索引或显式试验引用，较新的文件不会自动成为活动曲线。");
            ImGui::InputText("##recoil_directory", &c.profile_directory);
            row("游戏版本", "必须与所选曲线的校准条件一致；改变后旧曲线不可伪装适配。"); ImGui::InputText("##recoil_build", &c.game_build);
            row("输入路径", "校准时使用的设备输入路径，不是压枪增益。"); ImGui::InputText("##recoil_path", &c.input_path);
            row("游戏灵敏度", "与实际游戏设置一致；不能把灵敏度当某一把武器的力度滑块。"); ImGui::InputDouble("##recoil_sensitivity", &c.sensitivity, 0, 0, "%.4f");
            row("适用条件", "填写校准时的姿态与开镜条件标识；GSI识别武器不证明这些条件。"); ImGui::InputText("##recoil_conditions", &c.conditions);
            row("混合图像有效期 / ms", "包含映射不确定度；过期图像不能支撑Aim混合补偿。"); ImGui::InputInt("##recoil_age", &c.max_observation_age_ms);
            row("显式试验引用", "仅使用下方选定文件，保持活动索引不变；结束试验后恢复正常自动匹配。"); ImGui::Checkbox("##recoil_trial", &c.use_trial);
            row("试验文件", "曲线目录内的文件名，禁止路径逃逸；试验仍需匹配校准条件和用户前台触发。"); ImGui::InputText("##recoil_trial_file", &c.trial_file);
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
        try { owned_store = std::make_unique<RecoilStore>(config.recoil.profile_directory); }
        catch (...) { status = "曲线目录无效，请修正后重试。"; return; }
        auto& store = *owned_store;
        if (ImGui::Button("刷新曲线目录")) refresh(config.recoil);
        ImGui::SameLine();
        if (ImGui::Button("加载当前武器活动版本")) {
            auto lookup = config.recoil; lookup.use_trial = false;
            auto p = store.resolve(lookup, snapshot.weapon_snapshot.canonical_id, status);
            if (p) { selected_file.clear(); set_draft(*p); }
        }
        if (ImGui::BeginCombo("已保存版本", selected_file.empty() ? "选择独立版本" : selected_file.c_str())) {
            for (const auto& file : files) if (ImGui::Selectable(file.file.c_str(), selected_file == file.file)) load_file(config.recoil, file.file);
            ImGui::EndCombo();
        }
        ImGui::InputText("文件名", &manual_file); ImGui::SameLine();
        if (ImGui::Button("加载文件")) load_file(config.recoil, manual_file);
        if (ImGui::Button("加载试验引用")) load_file(config.recoil, config.recoil.trial_file);
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
        ImGui::SameLine();
        ImGui::BeginDisabled(undo.empty());
        if (ImGui::Button("撤销")) { redo.push_back({draft, tuning}); const auto previous = undo.back(); undo.pop_back(); draft = previous.draft; tuning = previous.tuning; checkpoint = {draft, tuning}; calibration_confirmed = false; report.reset(); compile(); }
        ImGui::EndDisabled(); ImGui::SameLine(); ImGui::BeginDisabled(redo.empty());
        if (ImGui::Button("重做")) { undo.push_back({draft, tuning}); const auto next = redo.back(); redo.pop_back(); draft = next.draft; tuning = next.tuning; checkpoint = {draft, tuning}; calibration_confirmed = false; report.reset(); compile(); }
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
                    ImGui::TableSetColumnIndex(2); ImGui::SetNextItemWidth(-1); changed |= ImGui::InputDouble("##node_x", &p.x_counts, 0, 0, "%.3f");
                    ImGui::TableSetColumnIndex(3); ImGui::SetNextItemWidth(-1); changed |= ImGui::InputDouble("##node_y", &p.y_counts, 0, 0, "%.3f");
                    ImGui::EndDisabled(); ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (ImGui::Button("追加末端节点") && draft.points.size() < 100000) { const auto tail = draft.points.back(); draft.points.push_back({tail.time_ms + 10, tail.x_counts, tail.y_counts}); changed = true; }
            ImGui::SameLine(); ImGui::BeginDisabled(draft.points.size() <= 2);
            if (ImGui::Button("删除末端节点")) { draft.points.pop_back(); changed = true; }
            ImGui::EndDisabled(); ImGui::TreePop();
        }
        if (changed) edited();
        if (preview_valid) { chart("recoil_x_chart", base, preview, true); chart("recoil_y_chart", base, preview, false); }
        else ImGui::TextWrapped("草稿未通过生产编译：%s", status.c_str());
        ImGui::InputScalar("另存版本号", ImGuiDataType_U64, &save_revision);
        ImGui::BeginDisabled(!preview_valid || save_revision <= base.revision);
        if (ImGui::Button("另存候选版本")) {
            auto candidate = preview; candidate.revision = save_revision;
            if (store.save_new(candidate, selected_file, status)) { refresh(config.recoil); status = "候选已另存；未校准、未激活。"; }
        }
        ImGui::EndDisabled();
        if (ImGui::TreeNode("人工校准证据与版本发布")) {
            ImGui::TextWrapped("只填写已完成实机的真实证据；输入字段不会产生实测，修改草稿后需重新确认。");
            ImGui::InputText("校准游戏版本", &calibration.game_build); ImGui::InputText("校准输入路径", &calibration.input_path);
            ImGui::InputText("校准适用条件", &calibration.conditions); ImGui::InputText("真实Run证据路径", &calibration.evidence);
            double sensitivity = calibration.sensitivity.value_or(0);
            if (ImGui::InputDouble("校准灵敏度", &sensitivity, 0, 0, "%.4f")) calibration.sensitivity = sensitivity;
            ImGui::InputDouble("已验证相位容差 / ms", &phase_ms); ImGui::InputDouble("已验证恢复时间 / ms", &recovery_ms);
            ImGui::Checkbox("我已人工完成当前曲线的校准与证据核对", &calibration_confirmed);
            ImGui::BeginDisabled(!preview_valid || !calibration_confirmed || save_revision <= base.revision);
            if (ImGui::Button("另存人工校准版本")) {
                std::error_code ec;
                if (!std::filesystem::exists(calibration.evidence, ec) || ec) status = "校准证据路径不存在，不能保存已校准声明。";
                else {
                    auto candidate = preview; candidate.revision = save_revision; candidate.state = RecoilProfileState::CALIBRATED;
                    candidate.calibration = calibration; candidate.phase_tolerance_ms = phase_ms; candidate.recovery_ms = recovery_ms;
                    if (store.save_new(candidate, selected_file, status, true)) { refresh(config.recoil); status = "已保存人工声明的校准版本；仍未自动激活。"; }
                }
            }
            ImGui::EndDisabled();
            ImGui::TextWrapped("发布对象：%s", selected_file.empty() ? "尚未选择已保存文件" : selected_file.c_str());
            ImGui::BeginDisabled(selected_file.empty());
            if (ImGui::Button("发布已选保存版本")) {
                RecoilProfile saved;
                if (store.load(selected_file, saved, status) && store.set_active(saved.weapon_id, selected_file, status)) status = "已更新活动索引；停止状态发布，下次新会话生效。";
            }
            ImGui::SameLine(); if (ImGui::Button("设为显式试验引用")) { config.recoil.use_trial = true; config.recoil.trial_file = selected_file; status = "仅设置试验引用，活动索引保持不变；保存应用配置后下次启动生效。"; }
            ImGui::EndDisabled();
            if (ImGui::Button("回退该武器活动版本")) if (store.rollback(base.weapon_id, status)) status = "已回退活动索引；下一会话生效。";
            ImGui::TreePop();
        }
        tuner();
    }
    void tuner() {
        if (!ImGui::CollapsingHeader("独立弹道自动优化器")) return;
        ImGui::TextWrapped("读取已完成独立压枪数据，批次间生成候选。GSI不提供逐发残差；缺响应H或独立留出将明确拒绝。");
        ImGui::InputText("Trial数据集JSON", &dataset_path);
        if (ImGui::Button("导入数据集")) { dataset_loaded = recoil_tuner::load_dataset(dataset_path, dataset, status); report.reset(); }
        ImGui::InputScalar("优化代际", ImGuiDataType_U64, &request.generation);
        ImGui::InputText("候选版本标识", &request.candidate_revision);
        ImGui::InputDouble("每轴修改预算 / counts", &request.max_axis_correction_counts);
        ImGui::InputDouble("总修改预算 / counts", &request.max_total_correction_counts);
        ImGui::BeginDisabled(!dataset_loaded || !preview_valid || job.valid());
        if (ImGui::Button("分析并生成独立候选")) {
            bool same = dataset.base_profile_revision == std::to_string(base.revision) && dataset.base_curve.size() == base.points.size();
            if (same) for (std::size_t i = 0; i < base.points.size(); ++i) same &= dataset.base_curve[i].time_ms == base.points[i].time_ms &&
                dataset.base_curve[i].x_counts == base.points[i].x_counts && dataset.base_curve[i].y_counts == base.points[i].y_counts;
            if (!same) status = "数据集基线与加载的执行版本不一致，拒绝用草稿回填旧Run。";
            else {
                const auto dataset_copy = dataset; const auto request_copy = request; const auto base_copy = base;
                job = std::async(std::launch::async, [dataset_copy, request_copy, base_copy] {
                    return recoil_tuner::optimize_profile_recorded(dataset_copy, request_copy, base_copy, [base_copy](const auto& points, std::string& error) {
                        auto candidate = base_copy; candidate.state = RecoilProfileState::SCHEMA_VALID; candidate.points.clear();
                        for (const auto& p : points) candidate.points.push_back({p.time_ms, p.x_counts, p.y_counts});
                        RecoilProfile compiled; return compile_recoil_profile(candidate, {}, compiled, error);
                    });
                });
                status = "正在后台分析已完成数据；未连接设备。";
            }
        }
        ImGui::EndDisabled();
        if (report) {
            for (const auto& message : report->messages) ImGui::TextWrapped("%s", message.c_str());
            if (report->predictions_available) ImGui::Text("中心误差：训练 %.3f → %.3f；留出预测 %.3f → %.3f；候选实测：未提供",
                report->fit_before.robust_center_error, report->fit_predicted.robust_center_error,
                report->holdout_before.robust_center_error, report->holdout_predicted.robust_center_error);
            ImGui::InputText("独立新结果目录", &result_directory);
            if (ImGui::Button("保存分析和候选")) recoil_tuner::save_result(result_directory, *report, status);
            ImGui::BeginDisabled(!report->candidate);
            if (ImGui::Button("将候选载入草稿预览")) {
                draft = base; draft.points.clear(); tuning = {};
                for (const auto& p : report->candidate->points) draft.points.push_back({p.time_ms, p.x_counts, p.y_counts});
                draft.state = RecoilProfileState::SCHEMA_VALID; edited(); status = "优化候选已载入草稿；未发布、未实测。";
            }
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
        { DisabledScope disabled(!can_edit || impl_->job.valid());
        impl_->settings(snapshot, config);
        ImGui::Checkbox("打开弹道编辑与优化", &impl_->show_editor);
        if (impl_->show_editor) impl_->editor(snapshot, config);
        }
        if (!impl_->status.empty()) ImGui::TextWrapped("%s", impl_->status.c_str());
    } catch (...) { impl_->status = "弹道面板操作失败；活动版本保持原有存储结果，请检查文件与数据。"; }
}
