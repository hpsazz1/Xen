#include "overlay/movement_panel.h"
#include "overlay/overlay.h"
#include "movement/timeline.h"
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <cmath>
#include <limits>
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
std::string action_label(const TimelineAction& action) {
    if (action.kind == ActionKind::TURN) return action.angle_degrees < 0 ? "鼠标左转" : "鼠标右转";
    if (action.kind == ActionKind::MOVE) return "鼠标位移";
    if (action.kind == ActionKind::WHEEL) return "滚轮";
    const std::string prefix = action.kind == ActionKind::MASK ? "屏蔽 " : action.physical ? "物理 " : "软件 ";
    switch (action.key) {
    case Key::W: return prefix + "W";
    case Key::A: return prefix + "A";
    case Key::S: return prefix + "S";
    case Key::D: return prefix + "D";
    case Key::SPACE: return prefix + "空格";
    case Key::CTRL: return prefix + "Ctrl";
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
            ImGui::Text("%zu. %s", i + 1, action_label(action).c_str());
            const float start = std::clamp(static_cast<float>(action.start_ms) / duration, 0.f, 1.f);
            const float end = std::clamp(static_cast<float>(static_cast<double>(action.start_ms) + action.duration_ms) / duration, 0.f, 1.f);
            draw->AddRectFilled({origin.x + label_width, origin.y}, {origin.x + label_width + width, origin.y + 16}, ImGui::GetColorU32(ImGuiCol_FrameBg));
            draw->AddRectFilled({origin.x + label_width + width * start, origin.y},
                {origin.x + label_width + std::max(width * start + 2.f, width * end), origin.y + 16},
                ImGui::GetColorU32(action.kind == ActionKind::TURN ? ImGuiCol_PlotLines : ImGuiCol_ButtonHovered));
        }
    }
    ImGui::EndChild();
}
}

struct MovementPanel::Impl {
    Plan plan;
    Request request;
    std::string error;
    bool initialized = false, catalog_requested = false;
    std::uint64_t seen_draft = 0, seen_catalog = 0;

    Impl() {
        request.mode = Mode::MOVEMENT_TEST;
        request.output_root = "cache/debug";
        plan.actions.push_back({ActionKind::KEY, Key::SPACE, 0, 40, 0});
    }
    bool update_request() {
        request.plan_text.clear();
        if (!movement::timeline::validate_plan(plan, error)) return false;
        request.plan_text = movement::timeline::plan_json(plan).dump(2);
        error.clear();
        return true;
    }
    void changed(OverlayActions& actions) {
        update_request();
        actions.movement_draft_changed = true;
        actions.debug_request = request;
    }
    void send(Action action, OverlayActions& actions, bool save_as = false) {
        if (action != Action::CANCEL && action != Action::LOAD_PLAN && action != Action::LIST_MOVEMENT_PLANS &&
            action != Action::START_MOVEMENT_RECORDING && action != Action::STOP_MOVEMENT_RECORDING &&
            !update_request()) return;
        request.movement_save_as = save_as;
        actions.debug_action = action;
        actions.debug_request = request;
    }
    void sync(const Snapshot* snapshot, OverlayActions& actions) {
        if (!initialized) { initialized = true; changed(actions); }
        if (!snapshot) return;
        if (snapshot->movement_catalog_revision != seen_catalog) {
            seen_catalog = snapshot->movement_catalog_revision;
            request.load_path = snapshot->movement_selected_path;
        }
        if (snapshot->draft_plan_revision != seen_draft && snapshot->draft_plan_mode == Mode::MOVEMENT_TEST) {
            seen_draft = snapshot->draft_plan_revision;
            Plan loaded;
            if (movement::timeline::parse_plan(snapshot->draft_plan, loaded, error)) {
                plan = std::move(loaded); changed(actions);
            }
        }
    }
    void library(const Snapshot* snapshot, OverlayActions& actions, bool busy) {
        const char* selected = "未保存的新方案";
        if (snapshot) {
            for (const auto& item : snapshot->movement_saved_plans)
                if (item.path == request.load_path) { selected = item.name.c_str(); break; }
        }
        ImGui::BeginDisabled(busy);
        ImGui::SetNextItemWidth(std::max(100.f, ImGui::GetContentRegionAvail().x * .7f));
        if (ImGui::BeginCombo("已保存方案", selected)) {
            if (snapshot) {
                for (const auto& item : snapshot->movement_saved_plans) {
                    ImGui::PushID(item.path.c_str());
                    if (ImGui::Selectable(item.name.c_str(), item.path == request.load_path)) {
                        send(Action::LOAD_PLAN, actions);
                        // 只提交目标，成功载入后再由目录快照更新当前保存路径。
                        actions.debug_request.load_path = item.path;
                    }
                    tip(item.path.c_str());
                    ImGui::PopID();
                }
                if (snapshot->movement_saved_plans.empty()) ImGui::TextDisabled("暂无方案，编辑后点击保存方案。");
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        tip("选择后自动载入，可继续修改并保存；同名方案可通过悬浮路径区分。运行期间不切换方案。");
        ImGui::SameLine();
        if (button("刷新方案", "只列出主动保存的方案；测试记录和录制草稿不会加入列表。", !busy))
            send(Action::LIST_MOVEMENT_PLANS, actions);
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
    void editor(OverlayActions& actions, const movement::Config& config) {
        bool change = text_field("方案名称", plan.name, "独立方案文件的名称，不修改原旋转跳或大跳配置。");
        change |= text_field("地图（可选）", plan.map, "仅保存地图标注；本阶段不自动识别地图或角色位置。");
        change |= text_field("点位（可选）", plan.location, "保存点位名称；每次测试前由用户对齐位置和朝向。");
        change |= text_field("起始说明", plan.notes, "记录起始朝向、站位和人工准备步骤；不参与设备执行。");
        if (button("添加动作", "添加独立动作；时间从脚本启动起算，与其他行可以重叠。")) {
            plan.actions.push_back({}); change = true;
        }
        int remove = -1, duplicate = -1;
        if (ImGui::BeginTable("movement_actions", 5,
                ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, {0, 360})) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("动作"); ImGui::TableSetupColumn("开始 / ms");
            ImGui::TableSetupColumn("时长 / ms"); ImGui::TableSetupColumn("动作参数");
            ImGui::TableSetupColumn("编辑"); ImGui::TableHeadersRow();
            const char* types[]{"W", "A", "S", "D", "空格 / 跳跃", "Left Ctrl / 蹲", "鼠标左转", "鼠标右转", "屏蔽 W", "屏蔽 A", "屏蔽 S", "屏蔽 D", "鼠标位移", "滚轮"};
            for (std::size_t i = 0; i < plan.actions.size(); ++i) {
                auto& action = plan.actions[i];
                ImGui::PushID(static_cast<int>(i)); ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                int kind = action.kind == ActionKind::TURN ? (action.angle_degrees < 0 ? 6 : 7) :
                    action.kind == ActionKind::MASK ? 8 + static_cast<int>(action.key) :
                    action.kind == ActionKind::MOVE ? 12 : action.kind == ActionKind::WHEEL ? 13 : static_cast<int>(action.key);
                ImGui::SetNextItemWidth(-1);
                if (ImGui::Combo("##action", &kind, types, 14)) {
                    action.kind = kind < 6 ? ActionKind::KEY : kind < 8 ? ActionKind::TURN : kind < 12 ? ActionKind::MASK : kind == 12 ? ActionKind::MOVE : ActionKind::WHEEL;
                    if (kind < 6) action.key = static_cast<Key>(kind);
                    else if (kind < 8) action.angle_degrees = (kind == 6 ? -1 : 1) * std::max(1.0, std::abs(action.angle_degrees));
                    else if (kind < 12) action.key = static_cast<Key>(kind - 8);
                    if (action.kind != ActionKind::KEY) action.physical = false;
                    if (action.kind == ActionKind::WHEEL) { action.duration_ms = 1; if (!action.wheel) action.wheel = 1; }
                    else if (action.duration_ms <= 0) action.duration_ms = 100;
                    if (action.kind == ActionKind::MOVE && !action.dx && !action.dy) action.dx = 1;
                    change = true;
                }
                tip("按键、屏蔽和鼠标可并行。物理按键轨道在同键屏蔽区间内不输出，软件按键照常执行。");
                ImGui::TableSetColumnIndex(1); ImGui::SetNextItemWidth(-1);
                change |= ImGui::InputInt("##start", &action.start_ms, 0, 0);
                tip("相对脚本启动的毫秒数；修改本行不会移动其他动作。");
                ImGui::TableSetColumnIndex(2); ImGui::SetNextItemWidth(-1);
                if (action.kind == ActionKind::WHEEL) ImGui::TextDisabled("瞬时事件");
                else change |= ImGui::InputInt("##duration", &action.duration_ms, 0, 0);
                tip("按住或旋转的持续毫秒数，必须大于零；中间留空可实现连续点按。");
                ImGui::TableSetColumnIndex(3); ImGui::SetNextItemWidth(-1);
                if (action.kind == ActionKind::TURN) {
                    double angle = std::abs(action.angle_degrees);
                    if (ImGui::InputDouble("##angle", &angle, 0, 0, "%.2f")) {
                        action.angle_degrees = (kind == 6 ? -1 : 1) * std::abs(angle); change = true;
                    }
                    tip("水平旋转目标角度；使用共享灵敏度与 m_yaw 换算，实际角度需游戏内核对。");
                } else if (action.kind == ActionKind::MOVE) {
                    change |= ImGui::InputInt("X##dx", &action.dx, 0, 0);
                    ImGui::SetNextItemWidth(-1);
                    change |= ImGui::InputInt("Y##dy", &action.dy, 0, 0);
                    tip("此区间的鼠标计数总量；按时长分配，不是屏幕像素或游戏角度。");
                    const double degrees_per_count = config.sensitivity * config.yaw_degrees_per_count;
                    const bool angle_valid = std::isfinite(degrees_per_count) && degrees_per_count > 0;
                    double angle = angle_valid ? action.dx * degrees_per_count : 0;
                    ImGui::BeginDisabled(!angle_valid);
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::InputDouble("水平角度##move_angle", &angle, 0, 0, "%.3f") && std::isfinite(angle)) {
                        action.dx = static_cast<int>(std::clamp(std::round(angle / degrees_per_count),
                            static_cast<double>(std::numeric_limits<int>::min()),
                            static_cast<double>(std::numeric_limits<int>::max())));
                        change = true;
                    }
                    ImGui::EndDisabled();
                    tip("按共享灵敏度和 m_yaw 换算为整数 X 计数；负数左转、正数右转。实际转角需游戏内核对，Y 保留原始计数。");
                } else if (action.kind == ActionKind::WHEEL) {
                    change |= ImGui::InputInt("档数##wheel", &action.wheel, 0, 0);
                    tip("有符号滚轮档数，在开始时刻发送一次，不模拟按住。");
                } else if (action.kind == ActionKind::KEY) {
                    change |= ImGui::Checkbox("物理轨道", &action.physical);
                    tip("来自录制的物理按键。回放时扣除同键屏蔽区间；取消勾选后作为软件按键执行。");
                } else ImGui::TextDisabled("物理屏蔽");
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
        const bool recording = snapshot && snapshot->movement_recording;
        ImGui::TextWrapped("录制可同时保留人工操作、原身法软件动作和屏蔽区间。录原快捷身法时，先启动 Runtime，再开始录制并执行身法。");
        ImGui::BeginDisabled(recording || busy);
        const char* motion_modes[]{"只存原始记录", "按相对位移解释", "按累计坐标解释"};
        ImGui::SetNextItemWidth(230);
        ImGui::Combo("录制鼠标解释", &ui.request.movement_record_motion_mode, motion_modes, 3);
        ImGui::EndDisabled();
        tip("原始 XY 语义尚未实机标定。选择相对或累计时按该解释生成可修改的方案；原始数据始终保留。只存原始记录不会生成回放方案。");
        ImGui::TextWrapped("鼠标原始值尚未实机标定：按所选解释生成编辑稿，原始数据始终保留；只存原始记录不生成回放方案。");
        if (!recording) {
            if (button("开始录制", "被动录制，不启动 Runtime，不要求当前草稿有效。录制结束后可继续编辑生成的方案。", !busy))
                ui.send(Action::START_MOVEMENT_RECORDING, actions);
        } else {
            if (button("停止录制并生成方案", "停止采集并保留原始记录；生成当前草稿，点击保存才写入方案文件。不会停止原 Runtime。"))
                ui.send(Action::STOP_MOVEMENT_RECORDING, actions);
            ImGui::SameLine(); ImGui::Text("已录制 %llu 个事件", static_cast<unsigned long long>(snapshot->movement_record_events));
        }
        if (snapshot && !snapshot->movement_recording_path.empty())
            ImGui::TextWrapped("录制文件：%s", snapshot->movement_recording_path.c_str());
        ImGui::Separator();
        ImGui::TextWrapped("手动编排并行动作，保存为独立方案；每次执行前请对齐站位与朝向。修改只影响下次测试，当前执行使用启动时的参数。");
        ImGui::BeginDisabled(recording || (busy && snapshot->state == debug_session::State::WORKING));
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
        ui.editor(actions, config.movement);
        ImGui::EndDisabled();
        if (!ui.error.empty()) ImGui::TextWrapped("方案错误：%s", ui.error.c_str());
        ImGui::Separator();
        ui.library(snapshot, actions, busy || recording);
        if (button("校验方案", "仅校验时间轴及参数，不输出设备。", !busy && !recording)) ui.send(Action::VALIDATE, actions);
        ImGui::SameLine();
        if (button("保存方案", "保存当前修改；已选方案直接更新，新方案自动保存到方案库，无需填写路径。", !busy && !recording)) ui.send(Action::SAVE_PLAN, actions);
        ImGui::SameLine();
        if (button("另存新方案", "将当前草稿保存为新文件，保留原方案。", !busy && !recording)) ui.send(Action::SAVE_PLAN, actions, true);
        ImGui::Text("共享灵敏度 %.3f；m_yaw %.6f", config.movement.sensitivity, config.movement.yaw_degrees_per_count);
        tip("换算参数在辅助 / 身法修改；脚本目标角度不等于游戏已实际完成的转角。");
        const bool enabled = snapshot && snapshot->movement_test_enabled;
        if (!enabled) {
            if (button("启用身法测试", "允许本页后续测试输出；启用本身不执行动作。改参、保存和载入后无需重新启用，End 或关闭测试会撤销。", !busy && !recording))
                ui.send(Action::ENABLE_MOVEMENT_TEST, actions);
        } else {
            ImGui::TextUnformatted("身法测试已启用：修改后可直接测试或使用全局测试键。");
            if (button("测试一次", "按最新有效草稿执行一次；运行期间改参用于下一次，忙碌不排队，End 急停。", !busy && !recording))
                ui.send(Action::RUN_MOVEMENT_TEST, actions);
        }
        ImGui::SameLine();
        if (button("关闭身法测试", "取消当前测试并撤销后续输出授权，释放脚本输入、恢复物理按键；清理未确认时保留故障。", busy || enabled))
            ui.send(Action::CANCEL, actions);
        if (!ui.catalog_requested && !busy && !recording && actions.debug_action == Action::NONE) {
            ui.catalog_requested = true;
            ui.send(Action::LIST_MOVEMENT_PLANS, actions);
        }
        if (snapshot && !snapshot->message.empty()) ImGui::TextWrapped("任务反馈：%s", snapshot->message.c_str());
        ImGui::TextWrapped("全局测试键与执行结果沿用调试模块。计时完成只证明命令执行，实际身法效果需本地游戏核对。");
    } catch (...) {
        impl_->changed(actions);
        ImGui::TextUnformatted("身法草稿无法显示，请重新载入有效方案。");
    }
}
