#include "overlay/debug_panel.h"
#include "overlay/overlay.h"
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace {
using namespace debug_session;
void tip(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", text);
}
bool button(const char* label, const char* help, bool enabled = true) {
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::Button(label);
    ImGui::EndDisabled(); tip(help); return clicked;
}
bool input(const char* label, std::string& value, const char* help) {
    const bool changed = ImGui::InputText(label, &value); tip(help); return changed;
}
bool integer(const char* label, int& value, int low, int high, const char* help) {
    const bool changed = ImGui::InputInt(label, &value);
    if (changed) value = std::clamp(value, low, high);
    tip(help); return changed;
}
const char* state_label(State state) {
    switch (state) {
        case State::IDLE: return "空闲";
        case State::WORKING: return "后台处理中";
        case State::PREPARED: return "已准备，等待前台启动";
        case State::RUNNING: return "运行中";
        case State::STOPPING: return "正在停止与清理";
        case State::COMPLETED: return "完成";
        case State::CANCELED: return "已取消";
        case State::FAILED: return "失败";
        case State::CLEANUP_UNKNOWN: return "设备清理未知";
    }
    return "未知";
}
void json_value(const Json& value, int depth = 0) {
    if (value.is_object() && depth < 3) {
        std::size_t count = 0;
        for (auto it = value.begin(); it != value.end() && count < 32; ++it, ++count) {
            ImGui::PushID(static_cast<int>(count));
            if (it->is_object()) {
                if (ImGui::TreeNode(it.key().c_str())) { json_value(*it, depth + 1); ImGui::TreePop(); }
            } else {
                ImGui::TextUnformatted(it.key().c_str()); ImGui::SameLine(); json_value(*it, depth + 1);
            }
            ImGui::PopID();
        }
        if (value.size() > 32) ImGui::TextDisabled("更多字段见完整报告。");
    } else if (value.is_array()) {
        ImGui::Text("数组：%zu项，完整内容见报告。", value.size());
    } else if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        ImGui::TextWrapped("%s", text.substr(0, 512).c_str());
    } else if (value.is_object()) {
        ImGui::TextDisabled("嵌套参数见完整报告。");
    } else {
        ImGui::TextUnformatted(value.dump().c_str());
    }
}
// 显示有界样本，缺测不插值；ACK到提交与ACK到ACK使用独立字段和坐标标签。
void timing_plot(const Json& values, const char* field, const char* label) {
    const std::size_t count = std::min<std::size_t>(values.size(), 256);
    std::array<float, 256> samples{};
    std::array<bool, 256> valid{};
    float maximum = 1;
    for (std::size_t i = 0; i < count; ++i) {
        if (!values[i].is_object()) continue;
        const auto x = values[i].find(field);
        if (x != values[i].end() && x->is_number()) {
            const float n = x->get<float>();
            if (std::isfinite(n) && n >= 0) { samples[i] = n; valid[i] = true; }
        }
        maximum = std::max(maximum, samples[i]);
    }
    ImGui::Text("%s（0～%.3f ms）", label, maximum);
    const auto origin = ImGui::GetCursorScreenPos();
    const float width = std::max(80.f, ImGui::GetContentRegionAvail().x);
    ImGui::Dummy({width, 100});
    auto* draw = ImGui::GetWindowDrawList();
    const auto color = ImGui::GetColorU32(ImGuiCol_PlotLines);
    draw->AddRect(origin, {origin.x + width, origin.y + 100}, ImGui::GetColorU32(ImGuiCol_Border));
    ImVec2 previous{};
    for (std::size_t i = 0; i < count; ++i) {
        const ImVec2 point{origin.x + 4 + static_cast<float>(i) * (width - 8) / static_cast<float>(std::max<std::size_t>(1, count - 1)),
            origin.y + 96 - 90 * (samples[i] / maximum)};
        if (valid[i]) {
            draw->AddCircleFilled(point, 2, color);
            if (i && valid[i - 1]) draw->AddLine(previous, point, color, 1.5f);
        }
        previous = point;
    }
}
void json_block(const char* title, const Json& value) {
    if (ImGui::TreeNode(title)) { json_value(value); ImGui::TreePop(); }
}
}

struct DebugPanel::Impl {
    Request request;
    bool allow = false, edited = true;
    bool draft_change_pending = false;
    std::string load_path, weapon_id;
    std::uint64_t seen_generation = 0;
    std::uint64_t requested_prepare_generation = 0, bound_prepare_generation = 0;
    Json seen_plan;
    Mode prepared_mode = Mode::COUNTERPULSE;
    std::string prepared_id;
    int shots = 8, move = 120, counter = 40, delay = 0, release = 18, hold = 5, interval = 280, direction = 0;
    bool move_parallel = true;
    int baseline = 0;

    void changed(bool invalidate_repeat = true) {
        edited = true; allow = false;
        if (invalidate_repeat) draft_change_pending = true;
    }
    struct PublishDraftChange {
        Impl& owner; OverlayActions& actions;
        ~PublishDraftChange() {
            actions.debug_plan_edited |= owner.draft_change_pending;
            owner.draft_change_pending = false;
        }
    };
    void send(Action action, OverlayActions& actions) {
        actions.debug_action = action; actions.debug_request = request;
        if (action == Action::PREPARE) {
            draft_change_pending = true;
            edited = false; prepared_mode = request.mode; prepared_id.clear();
            requested_prepare_generation = seen_generation;
            allow = false;
        }
    }
    void sync(const Snapshot* s) {
        if (!s || s->busy) return;
        if (s->state == State::PREPARED && !edited && prepared_mode == request.mode &&
            s->generation > requested_prepare_generation) {
            if (prepared_id != s->prepared_id || bound_prepare_generation != s->generation) {
                // 准备身份只绑定新的后台结果；旧快照及新任务不能继承前次勾选。
                allow = false;
                prepared_id = s->prepared_id;
                bound_prepare_generation = s->generation;
            }
        }
        if (s->generation == seen_generation && s->plan == seen_plan) return;
        seen_generation = s->generation; seen_plan = s->plan;
        if (s->plan.is_object()) {
            const auto& p = s->plan;
            if (request.mode == Mode::FIRE_TEST) {
                request.shot_hold_ms = p.value("shot_hold_ms", request.shot_hold_ms);
                request.fire_interval_ms = p.value("fire_interval_ms", request.fire_interval_ms);
            } else if (request.mode == Mode::COUNTERPULSE || request.mode == Mode::DERIVE_PLAN) {
                request.plan_text = p.dump(2);
                shots = p.value("shots", shots); move = p.value("move_ms", move);
                counter = p.value("counter_hold_ms", counter); delay = p.value("counter_delay_ms", delay);
                release = p.value("shot_after_release_ms", release); hold = p.value("shot_hold_ms", hold);
                interval = p.value("fire_interval_ms", interval); direction = p.value("direction", 2) == 8 ? 1 : 0;
                const auto b = p.value("baseline", std::string("counter"));
                baseline = b == "no_counter" ? 1 : b == "stationary" ? 2 : 0;
                move_parallel = p.value("move_during_fire_delay", move_parallel);
            }
        }
        if (s->sampling.is_object()) request.sampling_text = s->sampling.dump(2);
    }
    void mode(Mode value) {
        if (request.mode != value) {
            const bool derived = request.mode == Mode::DERIVE_PLAN && value == Mode::COUNTERPULSE;
            request.mode = value;
            if (!derived) request.plan_text.clear();
            if (value == Mode::FIRE_TEST) request.show_hud = false;
            changed();
        }
    }
    void plan_from_fields() {
        request.plan_text = Json{{"schema_version",2},{"baseline",baseline == 0 ? "counter" : baseline == 1 ? "no_counter" : "stationary"},
            {"capture_enabled",false},{"shots",shots},{"move_ms",move},{"counter_hold_ms",counter},
            {"counter_delay_ms",delay},{"shot_after_release_ms",release},{"shot_hold_ms",hold},
            {"fire_interval_ms",interval},{"fire_delay_ms",baseline == 2 ? 1 : 0},
            {"direction",direction ? 8 : 2},{"move_during_fire_delay",move_parallel},{"late_tolerance_ms",5}}.dump(2);
    }
    void controls(const Snapshot* s, OverlayActions& actions) {
        const bool idle = !s || !s->busy;
        if (input("结果根目录", request.output_root, "每组使用独立目录，保留原始Run；准备和报告由后台执行。")) changed();
        if (request.mode != Mode::FIRE_TEST && ImGui::Checkbox("常驻 HUD", &request.show_hud)) changed(false);
        tip("显示原生非激活HUD；模型结论不代表真实停稳。隐藏继续任务，关闭HUD请求停止。");
        if (button("校验计划", "仅校验实验参数，不连接或输出设备。", idle)) send(Action::VALIDATE, actions);
        ImGui::SameLine();
        if (button("保存新计划", "在结果目录中保存新计划，不覆盖生产配置。", idle)) send(Action::SAVE_PLAN, actions);
        ImGui::SameLine();
        if (button("准备", "后台冻结本次参数；编辑后必须重新准备。此按钮不会输出鼠标。", idle)) send(Action::PREPARE, actions);
        ImGui::Separator();
        const bool ready = s && s->state == State::PREPARED && !edited &&
            !prepared_id.empty() && prepared_id == s->prepared_id && prepared_mode == request.mode;
        const bool physical = request.mode == Mode::COUNTERPULSE || request.mode == Mode::FIRE_TEST;
        if (physical) ImGui::TextWrapped("启动前须停止 Runtime 和输入记录，并确认已有 KMBOX 连接的实际命令超时不超过100 ms、连接超时不超过2000 ms。参数需在设置中保存；已有连接不会被草稿重配，重新连接后再准备。设备清理未知时禁止启动。");
        if (physical) {
        ImGui::BeginDisabled(!ready);
        ImGui::Checkbox("允许本次真实物理输出", &allow);
        tip("勾选后由用户点击下方启动按钮；只授权本次已准备任务。修改草稿、重新准备或身份变化均撤销勾选。");
        ImGui::EndDisabled();
        }
        if (button("由用户前台启动本次任务", "仅启动冻结身份匹配的本次任务。启动前后台再次核对唯一设备职责与清理状态。",
                   ready && (!physical || allow))) {
            actions.debug_action = Action::START; actions.debug_request = request;
            actions.debug_prepared_id = prepared_id; actions.debug_allow_physical_output = allow;
            // GUI以显式勾选和前台点击授权；CLI与原生执行核仍保留既有双参数校验。
            actions.debug_confirmation = physical && allow ? physical_confirmation() : "";
            changed(false);
        }
        if (s && !s->message.empty()) ImGui::TextWrapped("任务反馈：%s", s->message.c_str());
        if (edited && s && s->state == State::PREPARED) ImGui::TextDisabled("草稿已变更，请重新准备。");
    }
};
DebugPanel::DebugPanel() : impl_(std::make_unique<Impl>()) {}
DebugPanel::~DebugPanel() = default;
void DebugPanel::render_status(const Snapshot* s, OverlayActions& actions) noexcept {
    Impl::PublishDraftChange publish{*impl_,actions};
    try {
        impl_->sync(s);
        ImGui::Text("当前任务：%s", s ? state_label(s->state) : "空闲");
        if (s && !s->message.empty()) ImGui::TextWrapped("%s", s->message.c_str());
        if (s) ImGui::TextDisabled(s->repeat_ready ? "快捷键模板已准备；独立开关启用后每按一次执行一组。" : "快捷键模板未就绪；先检查参数并重新准备。");
        if (button("停止当前调试任务", "直接请求取消；停止中仍等待设备清理，取消不等于释放确认。", s && s->busy))
            impl_->send(Action::CANCEL, actions);
        ImGui::SameLine();
        if (button(s && s->hud_visible ? "隐藏 HUD" : "显示 HUD", "只改变HUD可见性；隐藏不会停止任务。", s && (s->busy || s->result)))
            impl_->send(s->hud_visible ? Action::HIDE_HUD : Action::SHOW_HUD, actions);
        ImGui::Separator();
    } catch (...) { ImGui::TextUnformatted("调试快照无法显示。"); }
}
void DebugPanel::render_counterpulse(const AppConfig& config, const Snapshot* s, OverlayActions& actions) noexcept {
    Impl::PublishDraftChange publish{*impl_,actions};
    try {
        auto& d = *impl_; d.mode(Mode::COUNTERPULSE);
        ImGui::TextWrapped("实验草稿独立于生产急停。动作顺序：移动 → 释放 → 反向 → 释放后等待 → 按住左键；由既有严格计划校验约束。");
        ImGui::BeginDisabled(s && s->busy);
        bool changed = false;
        changed |= ImGui::Combo("基准动作", &d.baseline, "反向制动\0无反向对照\0原地\0"); tip("选择实验动作；不改变生产H40策略，也不将模型基准称作实测。");
        changed |= ImGui::Combo("移动方向", &d.direction, "D → A\0A → D\0"); tip("正向移动与反向制动方向，仅支持正式计划允许的左右方向。");
        changed |= integer("每组次数", d.shots, 1, 30, "1至30次；总时长和组合约束仍由原生计划严格校验。");
        changed |= integer("移动 / ms", d.move, 1, 500, "正向移动阶段时长，1至500ms。");
        changed |= integer("反向前等待 / ms", d.delay, 0, 200, "从正向UP协议ACK起算；仅反向模式允许非零。");
        changed |= integer("反向保持 / ms", d.counter, 1, 200, "从反向DOWN协议ACK起算，1至200ms。");
        changed |= integer("释放后等待 / ms", d.release, 0, 20, "反向UP协议ACK之后等待；这是模型计划参数，不是停稳观测。");
        changed |= integer("左键按住 / ms", d.hold, 1, 2000, "从DOWN协议ACK至UP提交的计划时长。");
        changed |= integer("DOWN提交最小间隔 / ms", d.interval, 1, 5000, "下一次DOWN的提交下限；移动阶段可能使实际间隔更长。");
        if (button("从当前急停参数带入草稿", "复制当前配置的保持及释放等待；不回写生产配置，不修改其他动作。")) {
            d.counter = config.auto_stop.counter_hold_ms; d.release = config.auto_stop.shot_after_release_ms; changed = true;
        }
        if (changed || d.request.plan_text.empty()) { d.plan_from_fields(); d.changed(); }
        if (ImGui::InputTextMultiline("完整动作计划 JSONC", &d.request.plan_text, { -1, 190 })) d.changed();
        tip("直接编辑正式计划格式，未知字段或越界会拒绝；草稿变更使旧准备失效。");
        input("载入文件", d.load_path, "只在点击载入时由后台读取计划或采样设置，不逐帧访问磁盘。");
        if (button("载入计划", "读取既有JSON/JSONC计划；保留原文件。")) { d.request.load_path = d.load_path; d.changed(); d.send(Action::LOAD_PLAN, actions); }
        ImGui::SameLine();
        if (button("载入模型采样设置", "载入默认基准与模型参数；不宣称做过默认参数重评。")) { d.request.load_path = d.load_path; d.changed(); d.send(Action::LOAD_SAMPLING, actions); }
        if (ImGui::InputTextMultiline("模型采样设置 JSONC", &d.request.sampling_text, {-1, 130})) d.changed();
        tip("空值沿用原生默认设置。模型参数与真实观察保持独立。");
        d.controls(s, actions); ImGui::EndDisabled();
        if (s) { json_block("本次冻结实际计划", s->plan); json_block("默认基准与实际模型参数", s->sampling); }
        render_results(s);
    } catch (...) { ImGui::TextUnformatted("急停草稿显示失败，请检查计划字段类型。"); }
}
void DebugPanel::render_fire(const AppConfig& config, const Snapshot* s, OverlayActions& actions) noexcept {
    Impl::PublishDraftChange publish{*impl_,actions};
    try {
        auto& d = *impl_; d.mode(Mode::FIRE_TEST);
        ImGui::TextWrapped("独立原地测试，每组固定15次按住；不包含反向急停或模型停稳结论。实际子弹数未知。");
        ImGui::BeginDisabled(s && s->busy);
        if (integer("点射按住 / ms", d.request.shot_hold_ms, 1, 2000, "测试范围1至2000ms；从DOWN协议ACK起算至UP提交。")) d.changed();
        if (integer("射击提交间隔 / ms", d.request.fire_interval_ms, 1, 5000, "测试范围1至5000ms且大于按住；生产表另按较窄范围校验。")) d.changed();
        if (button("读取共享武器资料", "后台读取辅助页管理的唯一共享目录，不修改文件或生产选择。")) {
            d.request.load_path = config.weapon_timing_file; d.send(Action::LOAD_WEAPON_TIMING, actions);
        }
        if (s && s->timing_catalog_valid) {
            if (ImGui::BeginCombo("带入武器", d.weapon_id.empty() ? "选择已启用武器" : d.weapon_id.c_str())) {
                for (const auto& p : s->timing_catalog.profiles) {
                    ImGui::BeginDisabled(!p.enabled);
                    if (ImGui::Selectable(p.canonical_id.data(), d.weapon_id == p.canonical_id)) {
                        d.weapon_id = p.canonical_id; d.request.shot_hold_ms = p.shot_hold_ms;
                        d.request.fire_interval_ms = p.fire_interval_ms; d.changed();
                    }
                    ImGui::EndDisabled(); tip("显式复制共享表值到独立草稿；R8留档禁用，不能激活。");
                }
                ImGui::EndCombo();
            }
            tip("共享表在辅助页管理；此处选择不会热改生产武器。");
        }
        input("两字段设置文件", d.load_path, "读取已有shot_hold_ms/fire_interval_ms文件；工程默认80/800不会覆盖用户文件。");
        if (button("载入射击设置", "严格两字段原生转换；每组仍固定15次。")) { d.request.load_path = d.load_path; d.changed(); d.send(Action::LOAD_FIRE_SETTINGS, actions); }
        d.controls(s, actions); ImGui::EndDisabled();
        if (s) json_block("本次实际射击计划", s->plan);
        render_results(s);
    } catch (...) { ImGui::TextUnformatted("射击草稿显示失败。"); }
}
void DebugPanel::render_manual(const Snapshot* s, OverlayActions& actions) noexcept {
    Impl::PublishDraftChange publish{*impl_,actions};
    try {
        auto& d = *impl_;
        ImGui::SeparatorText("原生人工模型录制与离线工作流");
        ImGui::TextWrapped("上方输入记录保留原始接收域评分；此处录制与模型评价独立标注，均不代表实测停稳。");
        ImGui::BeginDisabled(s && s->busy);
        if (input("原始记录 / 报告路径", d.request.input_path, "离线分析不连接设备；重评使用新目录，保留原始证据。")) d.changed();
        if (input("派生结果根目录", d.request.output_root, "每次分析或派生创建新产物，不覆盖原始Run。")) d.changed();
        if (integer("模型录制上限 / ms", d.request.recording_duration_ms, 1000, 120000, "有界人工录制；用户真实键鼠为数据，不发送自动输出。")) d.changed();
        if (button("准备人工模型录制", "复用唯一设备输入订阅；准备后通过明确前台操作启动，不自动打开第二设备。")) {
            d.mode(Mode::MANUAL_RECORDING); d.send(Action::PREPARE, actions);
        }
        if (d.request.mode == Mode::MANUAL_RECORDING) d.controls(s, actions);
        ImGui::SeparatorText("离线重评参数");
        if (ImGui::Checkbox("使用自定义模型参数重评", &d.request.override_sampling)) d.changed();
        tip("默认关闭，沿用原Run模型参数。开启后仅影响新重评，不覆盖原始记录或生产配置。");
        if (d.request.override_sampling) {
            if (ImGui::InputTextMultiline("自定义重评参数 JSONC", &d.request.sampling_text, {-1,130})) d.changed();
            tip("使用当前模型参数草稿重新评价；请核对来源和修改项。原Run参数和结果保留在原目录。");
        } else ImGui::TextDisabled("本次离线重评沿用原Run的模型参数。");
        if (button("离线重评人工记录", "仅分析原始人工记录；不连接设备，不进行动作复测。")) { d.mode(Mode::EVALUATE_MANUAL); d.send(Action::REEVALUATE, actions); }
        ImGui::SameLine();
        if (button("离线重评命令记录", "按已有命令ACK事实重算模型；与物理复测分开。")) { d.mode(Mode::EVALUATE_COMMANDS); d.send(Action::REEVALUATE, actions); }
        integer("候选片段序号", d.request.candidate_index, 0, 10000, "选择离线报告中的候选片段；后台检查存在性，序号从0开始。");
        if (button("派生可编辑动作计划", "从候选片段生成新草稿；切换急停测试后编辑、校验和重新准备，绝不自动执行。")) { d.mode(Mode::DERIVE_PLAN); d.send(Action::DERIVE_PLAN, actions); }
        ImGui::SameLine();
        if (button("派生默认基准", "生成独立默认基准分析产物；保留来源身份，不把阈线对照称为已重跑模型。")) { d.mode(Mode::DERIVE_DEFAULTS); d.send(Action::DERIVE_DEFAULTS, actions); }
        ImGui::EndDisabled(); render_results(s);
    } catch (...) { ImGui::TextUnformatted("人工工作流显示失败。"); }
}
void DebugPanel::render_results(const Snapshot* s) noexcept {
    try {
        if (!s) return;
        if (!s->report_directory.empty()) ImGui::TextWrapped("报告目录：%s", s->report_directory.c_str());
        const Json* result = s->result.get();
        if (s->live && (s->busy || !result)) result = s->live.get();
        if (!result || !result->is_object()) return;
        const auto nested = result->find("sampling_analysis");
        if (nested != result->end() && nested->is_object()) result = &*nested;
        const bool show_model = !s->plan.is_object() || s->plan.value("baseline",std::string{}) != "stationary";
        ImGui::TextWrapped(show_model ? "图例：时序绿=有效、灰=未知；模型绿=阈内、橙=阈外、灰=不可评估。单位ms；零值显示0，缺测显示未知。模型阈内不代表真实停稳。" : "时序绿=有效、灰=未知。单位ms；零值显示0，缺测显示未知。原地射击不生成停稳判断，实际子弹数未知。");
        const auto it = result->find("shots");
        if (it != result->end() && it->is_array()) {
            const auto& values = *it;
            const std::size_t count = std::min<std::size_t>(values.size(), 256);
            ImGui::Text("显示样本 %zu / %zu（最多256）", count, values.size());
            timing_plot(values, "previous_down_submit_interval_ms", "相邻DOWN提交间隔");
            timing_plot(values, "down_ack_to_up_submit_ms", "DOWN协议ACK至UP提交（按住时长）");
            ImGui::TextWrapped("按住时长截至UP提交；下表另列UP协议ACK，包含释放命令的确认等待，二者不混用。");
            if (ImGui::BeginTable("debug_timing", show_model ? 6 : 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("序号"); ImGui::TableSetupColumn("DOWN提交间隔 / ms");
                ImGui::TableSetupColumn("DOWN ACK至UP提交 / ms"); ImGui::TableSetupColumn("DOWN ACK至UP ACK / ms"); ImGui::TableSetupColumn("时序有效性"); if (show_model) ImGui::TableSetupColumn("模型分类 / 分色柱"); ImGui::TableHeadersRow();
                for (std::size_t i = 0; i < count; ++i) {
                    if (!values[i].is_object()) continue;
                    ImGui::PushID(static_cast<int>(i)); ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("%zu", i + 1);
                    for (const auto* field : {"previous_down_submit_interval_ms", "down_ack_to_up_submit_ms", "observed_hold_ms"}) {
                        ImGui::TableNextColumn(); const auto v = values[i].find(field);
                        if (v != values[i].end() && v->is_number() && std::isfinite(v->get<double>())) ImGui::Text("%.3f", v->get<double>());
                        else ImGui::TextUnformatted("未知");
                    }
                    ImGui::TableNextColumn(); const bool valid = values[i].value("valid_command_hold", false);
                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, valid ? ImVec4(.2f,.65f,.4f,1) : ImVec4(.5f,.5f,.5f,1));
                    ImGui::ProgressBar(valid ? .95f : 1.f, {-1, 15}, valid ? "有效时序" : "未知/无效");
                    ImGui::PopStyleColor();
                    if (show_model) {
                    ImGui::TableNextColumn();
                    const auto model = values[i].find("down_model");
                    bool model_known = false, within = false;
                    if (model != values[i].end() && model->is_object()) {
                        const auto threshold = model->find("within_model_threshold");
                        model_known = model->value("valid",false) && threshold != model->end() && threshold->is_boolean();
                        if (model_known) within = threshold->get<bool>();
                    }
                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, !model_known ? ImVec4(.5f,.5f,.5f,1) :
                        within ? ImVec4(.2f,.65f,.4f,1) : ImVec4(.85f,.5f,.15f,1));
                    ImGui::ProgressBar(1.f, {-1,15}, !model_known ? "不可评估" : within ? "模型阈内" : "模型阈外");
                    ImGui::PopStyleColor();
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }
        json_block(show_model ? "模型假设、默认基准与评价明细" : "射击时序明细", *result);
        ImGui::TextDisabled("人工观察：以用户实际记录为准；本页不自动生成视觉验收结论。");
    } catch (...) { ImGui::TextUnformatted("报告字段不可显示，原始报告仍保留。"); }
}
