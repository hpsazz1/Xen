#include "overlay/movement_panel.h"
#include "overlay/overlay.h"
#include "movement/timeline.h"
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <cmath>
#include <string>

namespace {
using namespace debug_session;
using movement::timeline::Plan;
using movement::timeline::ActionKind;
using movement::timeline::Key;
using TimelineAction = movement::timeline::Action;

void tip(const char* text) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}
bool button(const char* label, const char* help, bool enabled = true) {
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::Button(label);
    ImGui::EndDisabled(); tip(help); return clicked;
}
bool text_field(const char* label, std::string& value, const char* help) {
    ImGui::SetNextItemWidth(std::max(80.f, ImGui::GetContentRegionAvail().x * .7f));
    const bool changed = ImGui::InputText(label, &value);
    tip(help); return changed;
}
const char* action_label(const TimelineAction& action) {
    if (action.kind == ActionKind::TURN) return action.angle_degrees < 0 ? "鼠标左转" : "鼠标右转";
    switch (action.key) {
    case Key::W: return "W";
    case Key::A: return "A";
    case Key::S: return "S";
    case Key::D: return "D";
    case Key::SPACE: return "空格 / 跳跃";
    case Key::CTRL: return "Left Ctrl / 蹲";
    }
    return "未知";
}
void timeline(const Plan& plan) {
    const int duration = movement::timeline::duration_ms(plan);
    ImGui::Text("时间轴：0～%d ms（每行对应一个动作）", duration);
    if (plan.actions.empty() || duration <= 0) return;
    if (ImGui::BeginChild("movement_timeline", {0, 180}, true)) {
        auto* draw = ImGui::GetWindowDrawList();
        const float label_width = 125;
        const float width = std::max(40.f, ImGui::GetContentRegionAvail().x - label_width);
        for (std::size_t i = 0; i < plan.actions.size(); ++i) {
            const auto& action = plan.actions[i];
            const auto origin = ImGui::GetCursorScreenPos();
            ImGui::Text("%zu. %s", i + 1, action_label(action));
            const float start = std::clamp(static_cast<float>(action.start_ms) / duration, 0.f, 1.f);
            const float end = std::clamp(static_cast<float>(static_cast<double>(action.start_ms) + action.duration_ms) / duration, 0.f, 1.f);
            draw->AddRectFilled({origin.x + label_width, origin.y}, {origin.x + label_width + width, origin.y + 16}, ImGui::GetColorU32(ImGuiCol_FrameBg));
            draw->AddRectFilled({origin.x + label_width + width * start, origin.y},
                {origin.x + label_width + width * std::max(start, end), origin.y + 16},
                ImGui::GetColorU32(action.kind == ActionKind::TURN ? ImGuiCol_PlotLines : ImGuiCol_ButtonHovered));
        }
    }
    ImGui::EndChild();
}
}

struct MovementPanel::Impl {
    Plan plan;
    Request request;
    std::string error, prepared_id;
    bool edited = true, allow = false;
    std::uint64_t seen_draft = 0, prepare_generation = 0, bound_generation = 0;

    Impl() {
        request.mode = Mode::MOVEMENT_TEST;
        request.output_root = "cache/debug";
        plan.actions.push_back({ActionKind::KEY, Key::SPACE, 0, 40, 0});
    }
    void changed(OverlayActions& actions) {
        edited = true; allow = false; prepared_id.clear();
        actions.debug_plan_edited = true;
    }
    void send(Action action, OverlayActions& actions, const Snapshot* snapshot) {
        if (action != Action::CANCEL && action != Action::LOAD_PLAN) {
            if (!movement::timeline::validate_plan(plan, error)) return;
            request.plan_text = movement::timeline::plan_json(plan).dump(2);
        }
        actions.debug_action = action;
        actions.debug_request = request;
        if (action == Action::PREPARE) {
            edited = false; allow = false; prepared_id.clear();
            prepare_generation = snapshot ? snapshot->generation : 0;
            actions.debug_plan_edited = true;
        }
    }
    void sync(const Snapshot* snapshot, OverlayActions& actions) {
        if (!snapshot || snapshot->busy) return;
        if (snapshot->draft_plan_revision != seen_draft && snapshot->draft_plan_mode == Mode::MOVEMENT_TEST) {
            seen_draft = snapshot->draft_plan_revision;
            Plan loaded;
            if (movement::timeline::parse_plan(snapshot->draft_plan, loaded, error)) {
                plan = std::move(loaded); changed(actions);
            }
        }
        if (!edited && snapshot->state == State::PREPARED && snapshot->generation == prepare_generation + 1 &&
            snapshot->plan.is_object() && snapshot->plan.contains("actions") &&
            snapshot->plan == Json::parse(request.plan_text) &&
            snapshot->generation != bound_generation) {
            prepared_id = snapshot->prepared_id; bound_generation = snapshot->generation; allow = false;
        }
    }
    void copy_preset(const movement::Config& config, bool spin, bool right, OverlayActions& actions) {
        Plan copy;
        copy.name = spin ? "旋转跳副本" : "大跳副本";
        copy.notes = "主动发送空格的固定方向时间轴；原辅助页的物理跳跃触发与方向选择保持不变。";
        copy.actions.push_back({ActionKind::KEY, Key::SPACE, 0, 40, 0});
        const int start = config.jump_delay_ms;
        const int duration = spin ? config.spin_duration_ms : config.large_duration_ms;
        const double angle = spin ? config.spin_angle_degrees : config.large_angle_degrees;
        const bool first_right = spin && right;
        copy.actions.push_back({ActionKind::KEY, first_right ? Key::D : Key::A, start, duration, 0});
        copy.actions.push_back({ActionKind::TURN, Key::W, start, duration, first_right ? angle : -angle});
        if (!spin) {
            copy.actions.push_back({ActionKind::KEY, Key::D, start + duration, duration, 0});
            copy.actions.push_back({ActionKind::TURN, Key::W, start + duration, duration, angle});
            if (config.large_ctrl_enabled)
                copy.actions.push_back({ActionKind::KEY, Key::CTRL, config.large_ctrl_delay_ms, config.large_ctrl_hold_ms, 0});
        }
        plan = std::move(copy); error.clear(); changed(actions);
    }
    void editor(OverlayActions& actions) {
        bool change = text_field("方案名称", plan.name, "独立方案文件的名称，不修改原旋转跳或大跳配置。");
        change |= text_field("地图（可选）", plan.map, "仅保存地图标注；本阶段不自动识别地图或角色位置。");
        change |= text_field("点位（可选）", plan.location, "保存点位名称；每次测试前由用户对齐位置和朝向。");
        change |= text_field("起始说明", plan.notes, "记录起始朝向、站位和人工准备步骤；不参与设备执行。");
        if (button("添加动作", "添加独立动作；时间从脚本启动起算，与其他行可以重叠。")) {
            plan.actions.push_back({}); change = true;
        }
        int remove = -1, duplicate = -1;
        if (ImGui::BeginTable("movement_actions", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("动作"); ImGui::TableSetupColumn("开始 / ms");
            ImGui::TableSetupColumn("时长 / ms"); ImGui::TableSetupColumn("角度 / 度");
            ImGui::TableSetupColumn("编辑"); ImGui::TableHeadersRow();
            const char* types[]{"W", "A", "S", "D", "空格 / 跳跃", "Left Ctrl / 蹲", "鼠标左转", "鼠标右转"};
            for (std::size_t i = 0; i < plan.actions.size(); ++i) {
                auto& action = plan.actions[i];
                ImGui::PushID(static_cast<int>(i)); ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                int kind = action.kind == ActionKind::TURN ? (action.angle_degrees < 0 ? 6 : 7) : static_cast<int>(action.key);
                ImGui::SetNextItemWidth(-1);
                if (ImGui::Combo("##action", &kind, types, 8)) {
                    action.kind = kind < 6 ? ActionKind::KEY : ActionKind::TURN;
                    if (kind < 6) action.key = static_cast<Key>(kind);
                    else action.angle_degrees = (kind == 6 ? -1 : 1) * std::max(1.0, std::abs(action.angle_degrees));
                    change = true;
                }
                tip("不同按键和鼠标可并行；同一按键重叠视为持续按住。两段鼠标旋转不能重叠。");
                ImGui::TableSetColumnIndex(1); ImGui::SetNextItemWidth(-1);
                change |= ImGui::InputInt("##start", &action.start_ms, 0, 0);
                tip("相对脚本启动的毫秒数；修改本行不会移动其他动作。");
                ImGui::TableSetColumnIndex(2); ImGui::SetNextItemWidth(-1);
                change |= ImGui::InputInt("##duration", &action.duration_ms, 0, 0);
                tip("按住或旋转的持续毫秒数，必须大于零；中间留空可实现连续点按。");
                ImGui::TableSetColumnIndex(3); ImGui::SetNextItemWidth(-1);
                if (action.kind == ActionKind::TURN) {
                    double angle = std::abs(action.angle_degrees);
                    if (ImGui::InputDouble("##angle", &angle, 0, 0, "%.2f")) {
                        action.angle_degrees = (kind == 6 ? -1 : 1) * std::abs(angle); change = true;
                    }
                    tip("水平旋转目标角度；使用共享灵敏度与 m_yaw 换算，实际角度需游戏内核对。");
                } else ImGui::TextDisabled("—");
                ImGui::TableSetColumnIndex(4);
                if (button("复制", "复制当前动作及其时间，之后可分别修改。")) duplicate = static_cast<int>(i);
                ImGui::SameLine();
                if (button("删除", "只删除当前动作，不移动其他动作。")) remove = static_cast<int>(i);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (duplicate >= 0) { const auto copied = plan.actions[duplicate]; plan.actions.push_back(copied); change = true; }
        if (remove >= 0) { plan.actions.erase(plan.actions.begin() + remove); change = true; }
        change |= ImGui::Checkbox("屏蔽物理 WASD", &plan.mask.enabled);
        tip("只屏蔽选中的物理方向键，脚本方向键仍可执行；结束先释放脚本输入再恢复物理直通，仍按住 W 会继续前进。");
        ImGui::BeginDisabled(!plan.mask.enabled);
        const char* keys[]{"W", "A", "S", "D"};
        for (int i = 0; i < 4; ++i) {
            if (i) ImGui::SameLine();
            bool selected = (plan.mask.wasd & (1 << i)) != 0;
            if (ImGui::Checkbox(keys[i], &selected)) {
                plan.mask.wasd = selected ? plan.mask.wasd | (1 << i) : plan.mask.wasd & ~(1 << i); change = true;
            }
            tip("选择此物理方向键是否在屏蔽区间内交给脚本接管。");
        }
        bool first_jump = plan.mask.start_ms == -1;
        if (ImGui::Checkbox("首次空格开始屏蔽", &first_jump)) { plan.mask.start_ms = first_jump ? -1 : 0; change = true; }
        tip("以脚本首次空格按下为起点，不检测实际离地；无空格动作时不屏蔽，需要屏蔽时请指定起点。");
        if (!first_jump) {
            change |= ImGui::InputInt("屏蔽开始 / ms", &plan.mask.start_ms);
            tip("从脚本启动起算，屏蔽开始时也拦截已经持续按住的物理方向键。");
        }
        change |= ImGui::InputInt("屏蔽时长 / ms", &plan.mask.duration_ms);
        tip("0 表示持续到脚本结束和清理；正数指定屏蔽时长。");
        ImGui::EndDisabled();
        if (change) changed(actions);
        std::string validation;
        if (!movement::timeline::validate_plan(plan, validation)) ImGui::TextWrapped("待修正：%s", validation.c_str());
        else timeline(plan);
    }
};

MovementPanel::MovementPanel() : impl_(std::make_unique<Impl>()) {}
MovementPanel::~MovementPanel() = default;

void MovementPanel::render(const AppConfig& config, const Snapshot* snapshot, OverlayActions& actions) noexcept {
    try {
        auto& ui = *impl_;
        ui.sync(snapshot, actions);
        const bool busy = snapshot && snapshot->busy;
        ImGui::TextWrapped("手动编排并行动作，保存为独立方案；每次执行前请对齐站位与朝向。修改只影响下次准备。");
        if (ImGui::CollapsingHeader("原旋转跳与大跳预设")) {
            const auto& move = config.movement;
            ImGui::TextWrapped("原辅助页继续保留物理跳跃触发、A/D 选侧和固定 A→D 大跳逻辑。下面只复制参数为主动发送空格的自定义脚本。");
            ImGui::Text("旋转跳 %.2f° / %d ms；大跳每侧 %.2f° / %d ms", move.spin_angle_degrees, move.spin_duration_ms, move.large_angle_degrees, move.large_duration_ms);
            if (button("复制左旋转跳", "以当前旋转跳参数生成主动跳跃、固定向左的时间轴。")) ui.copy_preset(move, true, false, actions);
            ImGui::SameLine();
            if (button("复制右旋转跳", "以当前旋转跳参数生成主动跳跃、固定向右的时间轴。")) ui.copy_preset(move, true, true, actions);
            ImGui::SameLine();
            if (button("复制大跳", "复制当前 A→D 时长、转角及可选定时蹲；不修改原预设。")) ui.copy_preset(move, false, false, actions);
        }
        ui.editor(actions);
        if (!ui.error.empty()) ImGui::TextWrapped("方案错误：%s", ui.error.c_str());
        ImGui::Separator();
        if (text_field("方案文件路径", ui.request.load_path, "输入独立 JSON 文件路径，再点击载入；文件读取在后台完成。")) ui.changed(actions);
        if (button("载入方案", "后台载入独立方案文件并替换当前草稿。", !busy && !ui.request.load_path.empty())) ui.send(Action::LOAD_PLAN, actions, snapshot);
        if (text_field("保存及结果根目录", ui.request.output_root, "保存新计划和测试报告的根目录；后台创建独立目录，不覆盖旧方案。")) ui.changed(actions);
        if (button("校验方案", "仅校验时间轴及参数，不输出设备。", !busy)) ui.send(Action::VALIDATE, actions, snapshot);
        ImGui::SameLine();
        if (button("保存新方案", "后台保存当前独立方案，保留旧文件。", !busy)) ui.send(Action::SAVE_PLAN, actions, snapshot);
        ImGui::SameLine();
        if (button("准备测试", "冻结当前方案与共享灵敏度；编辑后重新准备。此按钮不发送输入。", !busy)) ui.send(Action::PREPARE, actions, snapshot);
        ImGui::Text("共享灵敏度 %.3f；m_yaw %.6f", config.movement.sensitivity, config.movement.yaw_degrees_per_count);
        tip("换算参数在辅助 / 身法修改；脚本目标角度不等于游戏已实际完成的转角。");
        const bool ready = snapshot && !busy && snapshot->state == State::PREPARED && !ui.edited &&
            !ui.prepared_id.empty() && ui.prepared_id == snapshot->prepared_id && snapshot->generation == ui.bound_generation;
        ImGui::BeginDisabled(!ready);
        ImGui::Checkbox("允许本次真实物理输出", &ui.allow);
        ImGui::EndDisabled();
        tip("授权当前已准备方案；修改或重新准备后清除。启动需设备与输入状态正常，物理 WASD 可以持续按住。");
        if (button("启动本次测试", "执行一次已准备方案；忙碌不排队。也可使用全局调试测试键，End 急停。", ready && ui.allow)) {
            ui.send(Action::START, actions, snapshot);
            actions.debug_prepared_id = ui.prepared_id;
            actions.debug_allow_physical_output = ui.allow;
            actions.debug_confirmation = physical_confirmation();
            ui.edited = true; ui.allow = false;
        }
        ImGui::SameLine();
        if (button("停止测试", "请求取消并释放脚本输入、恢复物理按键；清理未确认时保留故障。", busy)) ui.send(Action::CANCEL, actions, snapshot);
        if (snapshot && !snapshot->message.empty()) ImGui::TextWrapped("任务反馈：%s", snapshot->message.c_str());
        ImGui::TextWrapped("全局测试键与执行结果沿用调试模块。计时完成只证明命令执行，实际身法效果需本地游戏核对。");
    } catch (...) {
        impl_->changed(actions);
        ImGui::TextUnformatted("身法草稿无法显示，请重新载入有效方案。");
    }
}
