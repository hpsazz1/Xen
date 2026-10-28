#include "overlay/movement_panel.h"
#include "overlay/overlay.h"
#include "movement/timeline.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>

namespace {
int failures = 0;
void expect(bool pass, const char* message) {
    if (!pass) { ++failures; std::cerr << message << '\n'; }
}
void interactions() {
    MovementPanel panel;
    AppConfig config;
    debug_session::Snapshot snapshot;
    const char* title = "身法编排交互回归";
    int focus_row = -1;
    auto control_id = [&](ImGuiWindow* window, const char* label) {
        return focus_row < 0 ? window->GetID(label) : ImHashStr(label, 0,
            ImHashData(&focus_row, sizeof(focus_row), window->GetID("movement_actions")));
    };
    auto control_window = [&](ImGuiWindow* outer) {
        if (focus_row < 0) return outer;
        const auto* table = ImGui::TableFindByID(outer->GetID("movement_actions"));
        return table ? table->InnerWindow : outer;
    };
    auto frame = [&](const char* focus = nullptr) {
        ImGui::NewFrame();
        auto* prior = ImGui::FindWindowByName(title);
        if (focus && prior) ImGui::SetFocusID(control_id(prior, focus), control_window(prior));
        ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({1100, 2000});
        ImGui::Begin(title, nullptr, ImGuiWindowFlags_NoSavedSettings);
        OverlayActions actions;
        panel.render(config, &snapshot, actions);
        ImGui::End(); ImGui::Render();
        return actions;
    };
    auto click = [&](const char* label, int row = -1) {
        focus_row = row;
        auto& io = ImGui::GetIO(); io.AddMousePosEvent(-100, -100); frame(label);
        auto* window = ImGui::FindWindowByName(title);
        expect(window && ImGui::GetCurrentContext()->NavId == control_id(window, label), "交互必须定位真实控件");
        auto* owner = control_window(window);
        const auto rect = ImGui::WindowRectRelToAbs(owner, owner->NavRectRel[ImGuiNavLayer_Main]);
        io.AddMousePosEvent(rect.GetCenter().x, rect.GetCenter().y); frame();
        io.AddMouseButtonEvent(0, true); frame();
        io.AddMouseButtonEvent(0, false); return frame();
    };
    auto actions = frame();
    expect(actions.debug_action == debug_session::Action::LIST_MOVEMENT_PLANS && !actions.movement_draft_changed,
        "首次显示只查询方案库，自动初始化不得伪装手动编辑");
    actions = frame();
    expect(actions.debug_action == debug_session::Action::NONE, "普通帧不重复扫描方案库");
    actions = click("启用身法测试");
    expect(actions.debug_action == debug_session::Action::ENABLE_MOVEMENT_TEST && !actions.debug_plan_edited &&
        actions.debug_request.mode == debug_session::Mode::MOVEMENT_TEST, "启用必须交付最新身法草稿且不使调试模板失效");
    const auto original = debug_session::Json::parse(actions.debug_request.plan_text);
    snapshot.movement_test_enabled = true; frame();
    actions = click("测试一次");
    expect(actions.debug_action == debug_session::Action::RUN_MOVEMENT_TEST &&
        debug_session::Json::parse(actions.debug_request.plan_text) == original, "启用后无需准备或勾选即可测试最新草稿");
    actions = click("添加动作");
    expect(actions.movement_draft_changed && !actions.debug_plan_edited &&
        debug_session::Json::parse(actions.debug_request.plan_text).at("actions").size() == 2,
        "编辑必须发布最新草稿且保持测试启用");
    actions = click("测试一次");
    expect(actions.debug_action == debug_session::Action::RUN_MOVEMENT_TEST &&
        debug_session::Json::parse(actions.debug_request.plan_text).at("actions").size() == 2,
        "改参后可直接再次测试且必须携带修改");
    snapshot.busy = true;
    actions = click("添加动作"); expect(actions.movement_draft_changed, "执行中允许修改下一次草稿");
    actions = click("测试一次"); expect(actions.debug_action == debug_session::Action::NONE, "忙碌时不能排队重放");
    snapshot.busy = false;
    snapshot.draft_plan_revision = 1; snapshot.draft_plan_mode = debug_session::Mode::FIRE_TEST;
    snapshot.draft_plan = {{"shot_hold_ms", 70}}; frame();
    actions = click("校验方案");
    expect(debug_session::Json::parse(actions.debug_request.plan_text).at("actions").size() == 3, "异页载入不能改写身法草稿");
    movement::timeline::Plan loaded;
    loaded.name = "明确载入方案";
    loaded.actions.push_back({movement::timeline::ActionKind::KEY, movement::timeline::Key::SPACE, 100, 60, 0});
    snapshot.draft_plan_mode = debug_session::Mode::MOVEMENT_TEST; snapshot.draft_plan_revision = 2;
    snapshot.draft_plan = movement::timeline::plan_json(loaded);
    snapshot.movement_saved_plans = {{"cache/movement/plans/one.json", loaded.name}};
    snapshot.movement_catalog_revision = 1;
    snapshot.movement_selected_path = snapshot.movement_saved_plans.front().path;
    actions = frame(); expect(!actions.movement_draft_changed && !actions.debug_plan_edited,
        "载入同步不得伪装手动编辑或撤销语音来源");
    snapshot.movement_voice_selected = true;
    ++snapshot.draft_plan_revision; ++snapshot.movement_catalog_revision;
    actions = frame(); expect(!actions.movement_draft_changed, "语音草稿及目录自动同步不得手动接管");
    actions = click("测试一次");
    expect(actions.debug_action == debug_session::Action::RUN_MOVEMENT_TEST &&
        debug_session::Json::parse(actions.debug_request.plan_text) == snapshot.draft_plan, "载入后可直接测试完整新草稿");
    actions = click("保存方案");
    expect(actions.debug_action == debug_session::Action::SAVE_PLAN && !actions.debug_request.movement_save_as &&
        actions.debug_request.load_path == snapshot.movement_selected_path, "保存直接使用库中选定路径，不需手填");
    actions = click("另存新方案");
    expect(actions.debug_action == debug_session::Action::SAVE_PLAN && actions.debug_request.movement_save_as, "另存显式创建新方案");
    actions = click("刷新方案"); expect(actions.debug_action == debug_session::Action::LIST_MOVEMENT_PLANS, "刷新显式查询后台目录");
    snapshot.movement_saved_plans.insert(snapshot.movement_saved_plans.begin(),
        {"cache/movement/plans/unavailable.json", "待载入方案"});
    ++snapshot.movement_catalog_revision; frame();
    click("已保存方案");
    frame(); // 弹出窗口在下一帧完成自动尺寸与位置计算，再定位首项。
    auto* popup = ImGui::FindWindowByName("##Combo_00");
    expect(popup && popup->Active, "已保存方案必须打开真实下拉列表");
    if (popup && popup->Active) {
        // 点击弹出窗口首行，验证真实选择会交付后台载入。
        const ImVec2 pos{popup->Pos.x + ImGui::GetStyle().WindowPadding.x + 30,
            popup->Pos.y + ImGui::GetStyle().WindowPadding.y + ImGui::GetTextLineHeight() * .5f};
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(pos.x, pos.y); frame();
        io.AddMouseButtonEvent(0, true); frame();
        io.AddMouseButtonEvent(0, false); actions = frame();
        expect(actions.debug_action == debug_session::Action::LOAD_PLAN &&
            actions.debug_request.load_path == snapshot.movement_saved_plans.front().path, "下拉选择必须立即载入选定文件");
    }
    actions = click("保存方案");
    expect(actions.debug_action == debug_session::Action::SAVE_PLAN &&
        actions.debug_request.load_path == snapshot.movement_selected_path,
        "载入尚未成功时保存仍指向原方案，不能用旧草稿覆盖新目标");
    click("方案名称");
    auto& edit_io = ImGui::GetIO();
    edit_io.AddKeyEvent(ImGuiMod_Ctrl, true); edit_io.AddKeyEvent(ImGuiKey_A, true); frame();
    edit_io.AddKeyEvent(ImGuiKey_A, false); edit_io.AddKeyEvent(ImGuiMod_Ctrl, false);
    edit_io.AddKeyEvent(ImGuiKey_Backspace, true); actions = frame();
    expect(actions.movement_draft_changed && actions.debug_request.plan_text.empty() && !actions.debug_plan_edited,
        "无效草稿仍须发布空文本，防止热键继续执行旧有效草稿");
    edit_io.AddKeyEvent(ImGuiKey_Backspace, false); frame();
    actions = click("测试一次"); expect(actions.debug_action == debug_session::Action::NONE, "无效草稿不能启动");
    actions = click("开始录制");
    expect(actions.debug_action == debug_session::Action::START_MOVEMENT_RECORDING &&
        actions.debug_request.movement_record_motion_mode == 1,
        "无效草稿仍可开始录制，且交付鼠标解释选项");
    snapshot.movement_recording = true; snapshot.busy = true; frame();
    actions = click("添加动作"); expect(!actions.movement_draft_changed, "录制期间禁用草稿编辑");
    actions = click("保存方案"); expect(actions.debug_action == debug_session::Action::NONE, "录制期间不能保存旧草稿");
    actions = click("停止录制并生成方案");
    expect(actions.debug_action == debug_session::Action::STOP_MOVEMENT_RECORDING,
        "录制期间即便忙碌和草稿无效也必须可以停止");
    snapshot.movement_recording = false; snapshot.busy = false; frame();
    click("方案名称"); edit_io.AddInputCharactersUTF8("修复后的方案"); actions = frame();
    expect(actions.movement_draft_changed && !actions.debug_request.plan_text.empty(), "修复参数立即恢复有效草稿");
    actions = click("测试一次"); expect(actions.debug_action == debug_session::Action::RUN_MOVEMENT_TEST,
        "修复草稿无需重新授权即可启动");
    actions = click("关闭身法测试");
    expect(actions.debug_action == debug_session::Action::CANCEL, "空闲且启用时必须可以撤销授权");
    snapshot.busy = true; actions = click("关闭身法测试");
    expect(actions.debug_action == debug_session::Action::CANCEL, "运行中必须可以请求停止");

    snapshot.busy = false;
    loaded.mask.enabled = false;
    loaded.actions.clear();
    movement::timeline::Action physical;
    physical.key = movement::timeline::Key::W; physical.physical = true;
    loaded.actions.push_back(physical);
    auto mask = physical; mask.kind = movement::timeline::ActionKind::MASK; mask.physical = false;
    loaded.actions.push_back(mask);
    auto move = mask; move.kind = movement::timeline::ActionKind::MOVE; move.dx = 12; move.dy = -3;
    loaded.actions.push_back(move);
    auto wheel = mask; wheel.kind = movement::timeline::ActionKind::WHEEL; wheel.duration_ms = 1; wheel.wheel = -1;
    loaded.actions.push_back(wheel);
    auto left = mask; left.kind = movement::timeline::ActionKind::BUTTON;
    left.button = movement::timeline::Button::LEFT; loaded.actions.push_back(left);
    auto right = left; right.button = movement::timeline::Button::RIGHT; loaded.actions.push_back(right);
    loaded.notes = "测试源：setpos -520.25 378 160;setang 1 -89 0；后续中文说明；quit";
    snapshot.draft_plan = movement::timeline::plan_json(loaded); ++snapshot.draft_plan_revision; frame();
    actions = click("校验方案");
    expect(actions.debug_action == debug_session::Action::VALIDATE &&
        debug_session::Json::parse(actions.debug_request.plan_text) == snapshot.draft_plan,
        "屏蔽、位移、瞬时滚轮、左右按钮和物理来源必须可完整载入编辑器");
    std::string copied;
    auto& clipboard_io = ImGui::GetIO();
    const auto old_set_clipboard = clipboard_io.SetClipboardTextFn;
    void* old_clipboard_data = clipboard_io.ClipboardUserData;
    clipboard_io.ClipboardUserData = &copied;
    clipboard_io.SetClipboardTextFn = [](void* data, const char* value) { *static_cast<std::string*>(data) = value; };
    actions = click("复制定位指令");
    expect(copied == "setpos -520.25 378 160;setang 1 -89 0" &&
        !actions.movement_draft_changed && actions.debug_action == debug_session::Action::NONE,
        "复制只包含定位瞄角数字，不混入备注或后续命令、不启动输出或改变草稿");
    clipboard_io.SetClipboardTextFn = old_set_clipboard;
    clipboard_io.ClipboardUserData = old_clipboard_data;
    actions = click("物理轨道", 0);
    movement::timeline::Plan edited; std::string error;
    expect(actions.movement_draft_changed && movement::timeline::parse_plan(
        debug_session::Json::parse(actions.debug_request.plan_text), edited, error) && !edited.actions[0].physical,
        "物理来源控件必须可修改并发布草稿");
    click("X##dx", 2);
    edit_io.AddKeyEvent(ImGuiMod_Ctrl, true); edit_io.AddKeyEvent(ImGuiKey_A, true); frame();
    edit_io.AddKeyEvent(ImGuiKey_A, false); edit_io.AddKeyEvent(ImGuiMod_Ctrl, false);
    edit_io.AddInputCharactersUTF8("37"); frame();
    edit_io.AddKeyEvent(ImGuiKey_Enter, true); actions = frame();
    expect(actions.movement_draft_changed && movement::timeline::parse_plan(
        debug_session::Json::parse(actions.debug_request.plan_text), edited, error) && edited.actions[2].dx == 37,
        "录制的鼠标位移必须可以直接修改计数");
    edit_io.AddKeyEvent(ImGuiKey_Enter, false); frame();
    click("水平角度##move_angle", 2);
    edit_io.AddKeyEvent(ImGuiMod_Ctrl, true); edit_io.AddKeyEvent(ImGuiKey_A, true); frame();
    edit_io.AddKeyEvent(ImGuiKey_A, false); edit_io.AddKeyEvent(ImGuiMod_Ctrl, false);
    edit_io.AddInputCharactersUTF8("-12.5"); frame();
    edit_io.AddKeyEvent(ImGuiKey_Enter, true); actions = frame();
    expect(actions.movement_draft_changed && movement::timeline::parse_plan(
        debug_session::Json::parse(actions.debug_request.plan_text), edited, error) &&
        edited.actions[2].dx == static_cast<int>(std::round(-12.5 /
            (config.movement.sensitivity * config.movement.yaw_degrees_per_count))) && edited.actions[2].dy == -3,
        "录制位移修改水平角度应只换算X，保留Y计数");
    edit_io.AddKeyEvent(ImGuiKey_Enter, false); frame();

}
void grenade_interactions() {
    using namespace debug_session;
    MovementPanel panel(Mode::GRENADE_TEST);
    AppConfig config;
    Snapshot snapshot;
    const char* title = "道具测试交互回归";
    auto frame = [&](const char* focus = nullptr) {
        ImGui::NewFrame();
        if (auto* prior = ImGui::FindWindowByName(title); focus && prior)
            ImGui::SetFocusID(prior->GetID(focus), prior);
        ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({1100, 2000});
        ImGui::Begin(title, nullptr, ImGuiWindowFlags_NoSavedSettings);
        OverlayActions actions; panel.render(config, &snapshot, actions);
        ImGui::End(); ImGui::Render(); return actions;
    };
    auto click = [&](const char* label) {
        auto& io = ImGui::GetIO(); io.AddMousePosEvent(-100, -100); frame(label);
        auto* window = ImGui::FindWindowByName(title);
        expect(window && ImGui::GetCurrentContext()->NavId == window->GetID(label), "道具交互须定位真实控件");
        const auto rect = ImGui::WindowRectRelToAbs(window, window->NavRectRel[ImGuiNavLayer_Main]);
        io.AddMousePosEvent(rect.GetCenter().x, rect.GetCenter().y); frame();
        io.AddMouseButtonEvent(0, true); frame();
        io.AddMouseButtonEvent(0, false); return frame();
    };
    auto actions = frame();
    expect(actions.debug_action == Action::LIST_MOVEMENT_PLANS && actions.debug_request.mode == Mode::GRENADE_TEST &&
        actions.debug_request.movement_library_root == "cache/grenade/plans" && !actions.movement_draft_changed,
        "首次道具浏览只查询独立道具库，不启用或发布草稿");
    snapshot.movement_test_enabled = true; snapshot.movement_test_mode = Mode::MOVEMENT_TEST;
    actions = frame();
    expect(actions.debug_action == Action::NONE && !actions.movement_draft_changed,
        "身法拥有F11时浏览道具页不得自动接管");
    actions = click("启用道具测试");
    expect(actions.debug_action == Action::ENABLE_MOVEMENT_TEST && actions.debug_request.mode == Mode::GRENADE_TEST,
        "道具页必须显式启用自己的F11域");
    const auto initial = Json::parse(actions.debug_request.plan_text);
    expect(initial.at("voice_command").get<std::string>().empty(), "新道具默认不臆造语音口令");

    movement::timeline::Plan grenade;
    grenade.name = "Mirage T1 VIP R1"; grenade.map = "de_mirage";
    grenade.location = "Window"; grenade.voice_command = "保留旧字段";
    grenade.notes = "setpos 1296 -352 -167.968750;setang -41.580170 -173.471710 0";
    movement::timeline::Action button;
    button.kind = movement::timeline::ActionKind::BUTTON; button.duration_ms = 1000;
    grenade.actions.push_back(button);
    snapshot.grenade_saved_plans = {{"cache/grenade/plans/other.json", "烟雾 Anubis", "de_anubis", ""},
        {"cache/grenade/plans/mirage-t1.json", grenade.name, grenade.map, ""}};
    snapshot.grenade_selected_path = snapshot.grenade_saved_plans.back().path;
    snapshot.grenade_catalog_revision = 1;
    snapshot.movement_saved_plans = {{"cache/movement/plans/ancient.json", "身法原方案", "de_ancient", ""}};
    snapshot.movement_selected_path = snapshot.movement_saved_plans.front().path;
    snapshot.movement_catalog_revision = 1;
    snapshot.draft_plan_mode = Mode::GRENADE_TEST; snapshot.draft_plan_revision = 1;
    snapshot.draft_plan = movement::timeline::plan_json(grenade);
    snapshot.grenade_loaded_draft = snapshot.draft_plan; snapshot.grenade_loaded_revision = 1;
    snapshot.movement_test_mode = Mode::GRENADE_TEST;
    actions = frame();
    expect(!actions.movement_draft_changed, "道具载入同步不能伪装手工编辑");
    actions = click("投掷一次");
    expect(actions.debug_action == Action::RUN_MOVEMENT_TEST && actions.debug_request.mode == Mode::GRENADE_TEST &&
        Json::parse(actions.debug_request.plan_text) == snapshot.draft_plan,
        "投掷使用当前道具完整草稿和道具域");
    actions = click("保存方案");
    expect(actions.debug_action == Action::SAVE_PLAN && actions.debug_request.load_path == snapshot.grenade_selected_path &&
        actions.debug_request.movement_library_root == "cache/grenade/plans",
        "道具保存必须使用独立库和选中项");
    auto movement_plan = grenade; movement_plan.name = "来自另一页的身法";
    snapshot.draft_plan = movement::timeline::plan_json(movement_plan);
    snapshot.draft_plan_mode = Mode::MOVEMENT_TEST; ++snapshot.draft_plan_revision;
    ++snapshot.movement_catalog_revision; frame();
    actions = click("校验方案");
    expect(Json::parse(actions.debug_request.plan_text).at("name") == grenade.name &&
        actions.debug_request.load_path == snapshot.grenade_selected_path,
        "身法草稿或目录变化不得覆盖道具草稿和保存目标");
    click("道具方案"); frame();
    auto* popup = ImGui::FindWindowByName("##Combo_00");
    expect(popup && popup->Active, "道具方案列表应打开真实下拉");
    if (popup && popup->Active) {
        const ImVec2 pos{popup->Pos.x + ImGui::GetStyle().WindowPadding.x + 30,
            popup->Pos.y + ImGui::GetStyle().WindowPadding.y + ImGui::GetTextLineHeight() * .5f};
        auto& io = ImGui::GetIO(); io.AddMousePosEvent(pos.x, pos.y); frame();
        io.AddMouseButtonEvent(0, true); frame(); io.AddMouseButtonEvent(0, false); actions = frame();
        expect(actions.debug_action == Action::LOAD_PLAN && actions.debug_request.load_path == snapshot.grenade_selected_path,
            "道具列表须按地图编号规则排列且不混入身法库");
    }
    // 道具载入已完成但页面尚未渲染，随后身法载入覆盖全局草稿；返回仍须匹配道具保存目标。
    auto pending_grenade = grenade; pending_grenade.name = "Mirage T2 VIP R1";
    snapshot.grenade_loaded_draft = movement::timeline::plan_json(pending_grenade);
    ++snapshot.grenade_loaded_revision;
    snapshot.grenade_selected_path = "cache/grenade/plans/mirage-t2.json";
    ++snapshot.grenade_catalog_revision;
    snapshot.draft_plan = movement::timeline::plan_json(movement_plan);
    snapshot.draft_plan_mode = Mode::MOVEMENT_TEST; ++snapshot.draft_plan_revision;
    snapshot.movement_loaded_draft = snapshot.draft_plan; snapshot.movement_loaded_revision = 1;
    frame();
    actions = click("保存方案");
    expect(Json::parse(actions.debug_request.plan_text).at("name") == pending_grenade.name &&
        actions.debug_request.load_path == snapshot.grenade_selected_path,
        "异步跨页载入不能挤掉未显示的道具草稿并用旧内容覆盖新文件");
    actions = click("保存方案");
    expect(actions.debug_action == Action::SAVE_PLAN && actions.debug_request.mode == Mode::GRENADE_TEST &&
        actions.debug_request.load_path == snapshot.grenade_selected_path &&
        Json::parse(actions.debug_request.plan_text).at("actions") == movement::timeline::plan_json(pending_grenade).at("actions"),
        "移除语音控件后，道具保存仍使用独立库且保持原动作时序");
    const auto identity_before_preset = Json::parse(actions.debug_request.plan_text);
    const char* preset_names[]{"左键投掷","左键跳投","W 跳投","蹲跳投","蹲 W 跳投","左右双键 W 跳投"};
    const int action_counts[]{1,2,3,3,4,4};
    for (int index = 0; index < 6; ++index) {
        click("投掷动作"); frame();
        auto* preset_popup = ImGui::FindWindowByName("##Combo_00");
        expect(preset_popup && preset_popup->Active, "投掷预设必须打开真实下拉列表");
        if (!preset_popup || !preset_popup->Active) continue;
        const ImVec2 position{preset_popup->Pos.x + ImGui::GetStyle().WindowPadding.x + 30,
            preset_popup->Pos.y + ImGui::GetStyle().WindowPadding.y +
                ImGui::GetTextLineHeight() * .5f + index * ImGui::GetTextLineHeightWithSpacing()};
        auto& io = ImGui::GetIO(); io.AddMousePosEvent(position.x,position.y); frame();
        io.AddMouseButtonEvent(0,true); frame(); io.AddMouseButtonEvent(0,false); actions = frame();
        expect(actions.movement_draft_changed && actions.debug_action == Action::NONE &&
            actions.debug_request.mode == Mode::GRENADE_TEST && actions.debug_request.load_path == snapshot.grenade_selected_path,
            "选择投掷预设只更新当前草稿，不保存、不执行、不换保存目标");
        if (actions.debug_request.plan_text.empty()) continue;
        const auto value = Json::parse(actions.debug_request.plan_text);
        for (const auto* field : {"name","map","location","notes","voice_command"})
            expect(value.at(field) == identity_before_preset.at(field), "套用动作不得覆盖点位身份、坐标或兼容字段");
        movement::timeline::Plan applied; std::string error;
        expect(movement::timeline::parse_plan(value,applied,error) &&
            applied.actions.size() == static_cast<std::size_t>(action_counts[index]), preset_names[index]);
        expect(applied.mask.enabled && applied.mask.wasd == 15 && applied.mask.start_ms == 0 && applied.mask.duration_ms == 0,
            "预设复用原配方完整的方向键屏蔽参数");
        if (applied.actions.size() != static_cast<std::size_t>(action_counts[index])) continue;
        const auto& first = applied.actions.front();
        expect(first.kind == movement::timeline::ActionKind::BUTTON && first.start_ms == 0 && first.duration_ms == 1000,
            "既有预设拉栓时长和起点保持");
        if (index == 5) {
            expect(first.button == movement::timeline::Button::RIGHT &&
                applied.actions[1].button == movement::timeline::Button::LEFT && applied.actions[1].duration_ms == 1001,
                "双键预设保留先松右键、后松左键的原1ms差异");
        } else expect(first.button == movement::timeline::Button::LEFT, "普通投掷预设使用左键");
        for (const auto& item : applied.actions) if (item.kind == movement::timeline::ActionKind::KEY) {
            expect(!item.physical, "预设按键保持既有软件轨道");
            if (item.key == movement::timeline::Key::SPACE)
                expect(item.start_ms == 1000 && item.duration_ms == 40, "跳投保留原空格时序");
            else if (item.key == movement::timeline::Key::W)
                expect(item.start_ms == 1000 && item.duration_ms == 100, "W跳投保留原方向键时序");
            else if (item.key == movement::timeline::Key::CTRL)
                expect(item.start_ms == 0 && item.duration_ms == (index == 4 ? 1100 : 1040), "蹲姿保持至原动作结束");
            else expect(false, "预设不得增加未声明按键");
        }
    }
    snapshot.movement_test_mode = Mode::MOVEMENT_TEST; frame();
    actions = click("启用道具测试");
    expect(actions.debug_action == Action::ENABLE_MOVEMENT_TEST && actions.debug_request.mode == Mode::GRENADE_TEST,
        "另一页重新拥有F11后，道具需再次显式启用");
    snapshot.movement_test_mode = Mode::GRENADE_TEST; frame();
    actions = click("关闭道具测试");
    expect(actions.debug_action == Action::CANCEL, "道具测试必须可以撤销许可和清理");
}

void grenade_editor_cleanup_interactions() {
    using namespace debug_session;
    MovementPanel panel(Mode::GRENADE_TEST);
    AppConfig config;
    Snapshot snapshot;
    const char* title = "道具编辑精简交互回归";
    auto frame = [&](const char* focus = nullptr, std::string* rendered = nullptr) {
        ImGui::NewFrame();
        if (auto* prior = ImGui::FindWindowByName(title); focus && prior)
            ImGui::SetFocusID(prior->GetID(focus), prior);
        ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({1100, 2000});
        ImGui::Begin(title, nullptr, ImGuiWindowFlags_NoSavedSettings);
        if (rendered) ImGui::LogToBuffer();
        OverlayActions actions; panel.render(config, &snapshot, actions);
        if (rendered) { *rendered = ImGui::GetCurrentContext()->LogBuffer.c_str(); ImGui::LogFinish(); }
        ImGui::End(); ImGui::Render(); return actions;
    };
    auto click = [&](const char* label) {
        auto& io = ImGui::GetIO(); io.AddMousePosEvent(-100, -100); frame(label);
        auto* window = ImGui::FindWindowByName(title);
        expect(window && ImGui::GetCurrentContext()->NavId == window->GetID(label), "精简道具页须定位真实控件");
        const auto rect = ImGui::WindowRectRelToAbs(window, window->NavRectRel[ImGuiNavLayer_Main]);
        io.AddMousePosEvent(rect.GetCenter().x, rect.GetCenter().y); frame();
        io.AddMouseButtonEvent(0, true); frame(); io.AddMouseButtonEvent(0, false); return frame();
    };
    auto edit = [&](const char* label, const char* value) {
        click(label); auto& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_A, true); frame();
        io.AddKeyEvent(ImGuiKey_A, false); io.AddKeyEvent(ImGuiMod_Ctrl, false);
        io.AddInputCharactersUTF8(value); return frame();
    };
    auto popup_click = [&](int row) {
        frame(); auto* popup = ImGui::FindWindowByName("##Combo_00");
        expect(popup && popup->Active, "排序交互须打开真实下拉");
        if (!popup || !popup->Active) return OverlayActions{};
        const ImVec2 pos{popup->Pos.x + ImGui::GetStyle().WindowPadding.x + 30,
            popup->Pos.y + ImGui::GetStyle().WindowPadding.y + ImGui::GetTextLineHeight() * .5f +
                row * ImGui::GetTextLineHeightWithSpacing()};
        auto& io = ImGui::GetIO(); io.AddMousePosEvent(pos.x, pos.y); frame();
        io.AddMouseButtonEvent(0, true); frame(); io.AddMouseButtonEvent(0, false); return frame();
    };

    movement::timeline::Plan plan;
    plan.name = "T1 VIP烟"; plan.map = "de_mirage"; plan.location = "旧站位信息必须保留";
    plan.notes = "setpos 1296 -352 -167.968750;setang -41.580170 -173.471710 0；原有附注；quit";
    movement::timeline::Action throw_action;
    throw_action.kind = movement::timeline::ActionKind::BUTTON; throw_action.duration_ms = 1000;
    plan.actions.push_back(throw_action);
    snapshot.grenade_loaded_draft = movement::timeline::plan_json(plan); snapshot.grenade_loaded_revision = 1;
    snapshot.grenade_selected_path = "cache/grenade/plans/edit-only.json"; snapshot.grenade_catalog_revision = 1;
    frame();
    std::string rendered;
    frame(nullptr, &rendered);
    const auto occurrences = [&](const std::string& needle) {
        std::size_t count = 0, at = 0;
        while ((at = rendered.find(needle, at)) != std::string::npos) { ++count; at += needle.size(); }
        return count;
    };
    expect(occurrences("道具名称") == 1 && occurrences("定位与瞄角指令") == 1 &&
        occurrences("点位（可选）") == 0 && occurrences("起始说明") == 0 && occurrences("方案名称") == 0,
        "道具页只渲染一个名称和一个定位指令框，不重复显示旧点位或起始说明");
    auto actions = click("保存方案");
    auto original_saved = Json::parse(actions.debug_request.plan_text);
    expect(!original_saved.contains("team") || original_saved["team"] == "T", "旧编号方案保存只能补入原T阵营");
    original_saved.erase("team");
    expect(original_saved == snapshot.grenade_loaded_draft,
        "仅打开和保存精简界面除独立阵营外不得迁移或改写旧location、notes及动作");
    actions = edit("道具名称", "T1 VIP烟 自定义");
    expect(actions.movement_draft_changed && actions.debug_action == Action::NONE,
        "单一名称输入框可以直接编辑且不自动保存");
    const char* changed_notes = "setpos 1300 -351 -167.968750;setang -42.5 -174.25 0；原有附注；quit";
    actions = edit("定位与瞄角指令", changed_notes);
    expect(actions.movement_draft_changed && actions.debug_action == Action::NONE &&
        Json::parse(actions.debug_request.plan_text).at("notes") == changed_notes,
        "定位指令框必须直接编辑notes，附注完整保留，不是只读提取框");
    std::string copied; auto& io = ImGui::GetIO();
    const auto old_set_clipboard = io.SetClipboardTextFn; void* old_clipboard_data = io.ClipboardUserData;
    io.ClipboardUserData = &copied;
    io.SetClipboardTextFn = [](void* data, const char* value) { *static_cast<std::string*>(data) = value; };
    actions = click("复制定位指令");
    expect(copied == "setpos 1300 -351 -167.968750;setang -42.5 -174.25 0" &&
        !actions.movement_draft_changed && actions.debug_action == Action::NONE,
        "编辑后的复制只取setpos/setang，不含附注或后续命令且不执行动作");
    io.SetClipboardTextFn = old_set_clipboard; io.ClipboardUserData = old_clipboard_data;
    actions = click("保存方案");
    auto expected = snapshot.grenade_loaded_draft; expected["name"] = "T1 VIP烟 自定义"; expected["notes"] = changed_notes;
    const auto saved_team = Json::parse(actions.debug_request.plan_text).value("team",std::string{});
    if (!saved_team.empty()) expected["team"] = saved_team;
    expect(saved_team.empty() || saved_team == "T", "编辑名称和指令不得改变已知阵营");
    expect(actions.debug_action == Action::SAVE_PLAN && actions.debug_request.load_path == snapshot.grenade_selected_path &&
        Json::parse(actions.debug_request.plan_text) == expected,
        "保存仅更新编辑过的名称与指令，隐藏的旧location、动作和保存目标不变");

    const auto saved = [](const char* id, const char* name, const char* map) {
        return MovementSavedPlan{"cache/grenade/plans/" + std::string(id) + ".json",
            "文件附名 " + std::string(id), map, "", name};
    };
    // 故意打乱快照；短名相同的两项由路径确定顺序，选择时不能串到另一项。
    snapshot.grenade_saved_plans = {
        saved("ct1", "CT1 VIP烟", "de_mirage"), saved("t10", "T10 B小烟", "de_mirage"),
        saved("anubis", "T1 B烟", "de_anubis"), saved("vip-b", "T1 VIP烟", "de_mirage"),
        saved("vip-backup", "T1 VIP烟 备用瞄角", "de_mirage"), saved("t2", "T2 B小烟", "de_mirage"),
        saved("cache", "T1 B烟", "de_cache"), saved("dust", "T1 B烟", "de_dust2"),
        saved("other", "A点烟", "de_mirage"), saved("t1-b", "T1 B小烟", "de_mirage"),
        saved("vip-a", "T1 VIP烟", "de_mirage"), saved("ancient", "T1 B烟", "de_ancient"),
        saved("train", "T1 B烟", "de_train"), saved("inferno", "T1 B烟", "de_inferno"),
        saved("nuke", "T1 B烟", "de_nuke"), saved("overpass", "T1 B烟", "de_overpass"),
        saved("vertigo", "T1 B烟", "de_vertigo"), saved("custom", "T1 B烟", "de_custom")};
    ++snapshot.grenade_catalog_revision; frame();
    click("道具方案"); frame(nullptr, &rendered);
    expect(rendered.find("文件附名") == std::string::npos, "下拉使用raw_name短名，不重复显示文件附名");
    std::size_t previous = 0;
    const char* ordered_labels[]{"T1 B小烟 / de_mirage", "T1 VIP烟 / de_mirage", "T1 VIP烟 / de_mirage",
        "T1 VIP烟 备用瞄角 / de_mirage", "T2 B小烟 / de_mirage", "T10 B小烟 / de_mirage",
        "CT1 VIP烟 / de_mirage", "A点烟 / de_mirage", "T1 B烟 / de_dust2", "T1 B烟 / de_inferno",
        "T1 B烟 / de_ancient", "T1 B烟 / de_anubis", "T1 B烟 / de_nuke", "T1 B烟 / de_overpass",
        "T1 B烟 / de_train", "T1 B烟 / de_vertigo", "T1 B烟 / de_cache", "T1 B烟 / de_custom"};
    for (const auto* label : ordered_labels) {
        const auto at = rendered.find(label, previous);
        expect(at != std::string::npos, "全部地图列表须按固定地图顺序、T数字、CT和其他站位排列，备用瞄角相邻");
        if (at != std::string::npos) previous = at + std::string(label).size();
    }
    actions = popup_click(1);
    expect(actions.debug_action == Action::LOAD_PLAN && actions.debug_request.load_path == "cache/grenade/plans/vip-a.json",
        "同名道具按路径稳定排序且第一项载入正确文件");
    click("道具方案"); actions = popup_click(2);
    expect(actions.debug_action == Action::LOAD_PLAN && actions.debug_request.load_path == "cache/grenade/plans/vip-b.json",
        "同名道具第二项使用独立控件身份，不串到第一个路径");
    click("筛选地图"); actions = popup_click(1);
    expect(actions.debug_action == Action::NONE && !actions.movement_draft_changed,
        "筛选地图只修改显示范围，不载入或修改方案");
    click("道具方案"); frame(nullptr, &rendered);
    expect(rendered.find("T1 B小烟") != std::string::npos && rendered.find("T10 B小烟") != std::string::npos &&
        rendered.find(" / de_") == std::string::npos && rendered.find("文件附名") == std::string::npos &&
        rendered.find("de_dust2") == std::string::npos,
        "单地图下拉仅显示该地图短名，省略地图和文件附名");
    actions = popup_click(3);
    expect(actions.debug_action == Action::LOAD_PLAN && actions.debug_request.load_path == "cache/grenade/plans/vip-backup.json",
        "筛选后备用瞄角仍保持相邻顺序并载入对应文件");
    actions = click("保存方案");
    expect(actions.debug_request.load_path == snapshot.grenade_selected_path && Json::parse(actions.debug_request.plan_text) == expected,
        "列表点击未获载入回执前，不改变草稿或原保存目标");
}

void grenade_map_team_interactions() {
    using namespace debug_session;
    auto panel = std::make_unique<MovementPanel>(Mode::GRENADE_TEST);
    AppConfig config;
    Snapshot snapshot;
    const char* title = "道具地图阵营交互回归";
    const auto gsi = [&](const char* map, weapon::Team team) {
        snapshot.movement_map.valid = true;
        snapshot.movement_map.map_name = map;
        snapshot.movement_map.local_team = team;
        snapshot.movement_map.status = weapon::Status::READY;
        snapshot.movement_map.valid_until = weapon::Clock::now() + std::chrono::seconds(30);
        ++snapshot.movement_map.context_epoch;
    };
    auto frame = [&](const char* focus = nullptr, bool popup_focus = false) {
        ImGui::NewFrame();
        if (auto* prior = ImGui::FindWindowByName(popup_focus ? "##Combo_00" : title); focus && prior)
            ImGui::SetFocusID(prior->GetID(focus),prior);
        ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize({1100,2000});
        ImGui::Begin(title,nullptr,ImGuiWindowFlags_NoSavedSettings);
        OverlayActions actions; panel->render(config,&snapshot,actions);
        ImGui::End(); ImGui::Render(); return actions;
    };
    auto click = [&](const char* label) {
        auto& io = ImGui::GetIO(); io.AddMousePosEvent(-100,-100); frame(label);
        auto* window = ImGui::FindWindowByName(title);
        expect(window && ImGui::GetCurrentContext()->NavId == window->GetID(label),"地图阵营交互须定位真实控件");
        if (!window) return OverlayActions{};
        const auto rect = ImGui::WindowRectRelToAbs(window,window->NavRectRel[ImGuiNavLayer_Main]);
        io.AddMousePosEvent(rect.GetCenter().x,rect.GetCenter().y); frame();
        io.AddMouseButtonEvent(0,true); frame(); io.AddMouseButtonEvent(0,false); return frame();
    };
    auto select = [&](const char* label, const char* option) {
        click(label); frame();
        auto* popup = ImGui::FindWindowByName("##Combo_00");
        expect(popup && popup->Active,"地图和阵营必须使用真实下拉列表");
        if (!popup || !popup->Active) return OverlayActions{};
        frame(option,true);
        auto rect = ImGui::WindowRectRelToAbs(popup,popup->NavRectRel[ImGuiNavLayer_Main]);
        ImGui::ScrollToRect(popup,rect,ImGuiScrollFlags_KeepVisibleEdgeY); frame(option,true);
        rect = ImGui::WindowRectRelToAbs(popup,popup->NavRectRel[ImGuiNavLayer_Main]);
        expect(popup->InnerClipRect.Contains(rect.GetCenter()),"下拉目标须先滚动到可见区域再点击");
        auto& io = ImGui::GetIO(); io.AddMousePosEvent(rect.GetCenter().x,rect.GetCenter().y); frame();
        io.AddMouseButtonEvent(0,true); frame(); io.AddMouseButtonEvent(0,false); return frame();
    };
    auto edit = [&](const char* label, const char* value) {
        click(label); auto& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiMod_Ctrl,true); io.AddKeyEvent(ImGuiKey_A,true); frame();
        io.AddKeyEvent(ImGuiKey_A,false); io.AddKeyEvent(ImGuiMod_Ctrl,false);
        io.AddInputCharactersUTF8(value); return frame();
    };
    const auto value = [](const OverlayActions& actions) {
        return actions.debug_request.plan_text.empty() ? Json::object() : Json::parse(actions.debug_request.plan_text);
    };
    gsi("de_mirage",weapon::Team::T);
    auto actions = frame();
    expect(!actions.movement_draft_changed,"GSI默认填充不伪装手动编辑或接管另一测试域");
    actions = click("启用道具测试");
    expect(value(actions).value("map",std::string{}) == "de_mirage" && value(actions).value("team",std::string{}) == "T",
        "新道具地图与阵营默认采用有效GSI");
    actions = edit("道具名称","T垃圾桶左 拱门烟");
    expect(value(actions).value("name",std::string{}) == "T垃圾桶左 拱门烟" && value(actions).value("team",std::string{}) == "T",
        "用户实际自定义名称保存独立阵营，不要求T后接数字");
    actions = select("地图","de_cache");
    expect(actions.movement_draft_changed && actions.debug_action == Action::NONE &&
        value(actions).value("map",std::string{}) == "de_cache","没有已保存方案的地图也能从完整下拉选取");
    actions = select("阵营","CT / 警");
    expect(actions.movement_draft_changed && value(actions).value("team",std::string{}) == "CT",
        "手动阵营下拉将CT写入当前草稿");
    gsi("de_inferno",weapon::Team::T); frame();
    actions = click("保存方案");
    expect(actions.debug_action == Action::SAVE_PLAN && value(actions).value("map",std::string{}) == "de_cache" &&
        value(actions).value("team",std::string{}) == "CT","GSI后续变更不能覆盖已经选择的地图与阵营");
    actions = click("使用当前地图与阵营");
    expect(actions.movement_draft_changed && value(actions).value("map",std::string{}) == "de_inferno" &&
        value(actions).value("team",std::string{}) == "T","明确使用当前GSI时同时更新地图与阵营");
    actions = select("地图","cs_office");
    expect(value(actions).value("map",std::string{}) == "cs_office","完整地图下拉包含非de前缀地图");
    select("地图","自定义地图");
    edit("自定义地图标识","workshop_smoke_training"); actions = click("保存方案");
    expect(actions.debug_action == Action::SAVE_PLAN && value(actions).value("map",std::string{}) == "workshop_smoke_training",
        "地图下拉保留自定义标识输入，未知地图不被GSI自动覆盖");

    panel = std::make_unique<MovementPanel>(Mode::GRENADE_TEST); snapshot = Snapshot{}; frame();
    select("地图","de_mirage"); select("阵营","T / 匪");
    edit("道具名称","T垃圾桶左 拱门烟"); actions = click("保存方案");
    expect(actions.debug_action == Action::SAVE_PLAN && value(actions).value("map",std::string{}) == "de_mirage" &&
        value(actions).value("team",std::string{}) == "T","没有GSI时仍可手动选择全部地图和阵营并保存");

    movement::timeline::Plan custom;
    custom.name = "T垃圾桶左 拱门烟"; custom.map = "de_mirage";
    movement::timeline::Action throw_action; throw_action.kind = movement::timeline::ActionKind::BUTTON;
    throw_action.duration_ms = 1000; custom.actions.push_back(throw_action);
    const auto load = [&](Json saved) {
        snapshot.grenade_loaded_draft = std::move(saved); ++snapshot.grenade_loaded_revision;
        snapshot.grenade_selected_path = "cache/grenade/plans/custom.json"; ++snapshot.grenade_catalog_revision;
        return frame();
    };
    gsi("de_mirage",weapon::Team::T);
    const auto old_custom = movement::timeline::plan_json(custom);
    actions = load(old_custom);
    expect(!actions.movement_draft_changed,"载入缺阵营的旧自定义方案不自动发布改动");
    actions = click("启用道具测试");
    expect(value(actions) == old_custom,"展示GSI默认阵营不能在保存前改写载入的旧JSON");
    actions = click("保存方案");
    auto repaired = old_custom; repaired["team"] = "T";
    expect(actions.debug_action == Action::SAVE_PLAN && value(actions) == repaired,
        "旧自定义方案同图时保存GSI默认阵营，保留名称及原动作");
    auto explicit_ct = old_custom; explicit_ct["team"] = "CT";
    actions = load(explicit_ct);
    expect(!actions.movement_draft_changed,"已保存显式阵营的载入不产生编辑事件");
    actions = click("保存方案");
    expect(value(actions) == explicit_ct,"显式CT阵营优先于名称与当前GSI的T默认");
    custom.name = "T1 兼容旧方案";
    const auto legacy_t = movement::timeline::plan_json(custom);
    gsi("de_mirage",weapon::Team::CT); load(legacy_t); actions = click("保存方案");
    auto expected_t = legacy_t; expected_t["team"] = "T";
    expect(value(actions) == expected_t,"旧T1名称推断优先于当前GSI的CT默认");

    panel = std::make_unique<MovementPanel>(Mode::GRENADE_TEST); snapshot = Snapshot{};
    gsi("de_user_gsi",weapon::Team::T);
    snapshot.grenade_saved_plans = {{"cache/grenade/plans/library-custom.json","T1 库内地图","de_user_library",""}};
    frame(); actions = click("保存方案");
    expect(value(actions).value("map",std::string{}) == "de_user_gsi","当前GSI的未知地图可作为新方案默认");
    actions = select("地图","de_user_library");
    expect(value(actions).value("map",std::string{}) == "de_user_library","方案库中未知地图也能直接下拉选择");
    actions = select("地图","de_user_gsi");
    expect(value(actions).value("map",std::string{}) == "de_user_gsi","当前GSI未知地图同时出现在地图下拉中");
}

void grenade_crouch_delete_interactions() {
    using namespace debug_session;
    auto panel = std::make_unique<MovementPanel>(Mode::GRENADE_TEST);
    AppConfig config; Snapshot snapshot;
    snapshot.movement_map.valid = true; snapshot.movement_map.map_name = "de_mirage";
    snapshot.movement_map.local_team = weapon::Team::T;
    const char* title = "道具蹲姿和删除交互回归";
    auto frame = [&](const char* focus = nullptr, bool popup_focus = false, std::string* rendered = nullptr) {
        ImGui::NewFrame();
        if (auto* prior = ImGui::FindWindowByName(popup_focus ? "##Combo_00" : title); focus && prior)
            ImGui::SetFocusID(prior->GetID(focus),prior);
        ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize({1100,2000});
        ImGui::Begin(title,nullptr,ImGuiWindowFlags_NoSavedSettings);
        if (rendered) ImGui::LogToBuffer();
        OverlayActions actions; panel->render(config,&snapshot,actions);
        if (rendered) { *rendered = ImGui::GetCurrentContext()->LogBuffer.c_str(); ImGui::LogFinish(); }
        ImGui::End(); ImGui::Render(); return actions;
    };
    auto click = [&](const char* label) {
        auto& io = ImGui::GetIO(); io.AddMousePosEvent(-100,-100); frame(label);
        auto* window = ImGui::FindWindowByName(title);
        expect(window && ImGui::GetCurrentContext()->NavId == window->GetID(label),"蹲姿删除交互须定位真实控件");
        if (!window) return OverlayActions{};
        const auto rect = ImGui::WindowRectRelToAbs(window,window->NavRectRel[ImGuiNavLayer_Main]);
        io.AddMousePosEvent(rect.GetCenter().x,rect.GetCenter().y); frame();
        io.AddMouseButtonEvent(0,true); frame(); io.AddMouseButtonEvent(0,false); return frame();
    };
    auto select = [&](const char* label, const char* option) {
        click(label); frame(); auto* popup = ImGui::FindWindowByName("##Combo_00");
        expect(popup && popup->Active,"瞄点姿态必须提供真实下拉列表");
        if (!popup || !popup->Active) return OverlayActions{};
        frame(option,true);
        const auto rect = ImGui::WindowRectRelToAbs(popup,popup->NavRectRel[ImGuiNavLayer_Main]);
        auto& io = ImGui::GetIO(); io.AddMousePosEvent(rect.GetCenter().x,rect.GetCenter().y); frame();
        io.AddMouseButtonEvent(0,true); frame(); io.AddMouseButtonEvent(0,false); return frame();
    };
    const auto value = [](const OverlayActions& actions) {
        return actions.debug_request.plan_text.empty() ? Json::object() : Json::parse(actions.debug_request.plan_text);
    };
    frame();
    auto actions = click("删除方案");
    expect(actions.debug_action == Action::NONE,"未保存草稿没有删除目标，删除按钮须禁用");
    actions = select("瞄点姿态","蹲姿");
    expect(actions.movement_draft_changed && actions.debug_action == Action::NONE && value(actions).value("crouch",false),
        "蹲姿下拉只编辑当前草稿，不提前蹲下、保存或投掷");
    const auto before_preset = value(actions);
    actions = select("投掷动作","左键跳投");
    expect(actions.movement_draft_changed && value(actions).value("crouch",false),"套用动作预设不得覆盖瞄点姿态");
    actions = click("保存方案");
    expect(actions.debug_action == Action::SAVE_PLAN && value(actions).value("crouch",false) &&
        value(actions).value("name",std::string{}) == before_preset.value("name",std::string{}),
        "保存蹲姿独立字段且保持当前名称");
    actions = select("瞄点姿态","站姿");
    expect(actions.movement_draft_changed && !value(actions).value("crouch",false),"站姿下拉可恢复不保持蹲键");

    auto saved = value(click("保存方案")); saved["crouch"] = true;
    snapshot.grenade_loaded_draft = saved; snapshot.grenade_loaded_revision = 1;
    snapshot.grenade_selected_path = "cache/grenade/plans/loaded.json";
    snapshot.grenade_catalog_revision = 1;
    snapshot.grenade_saved_plans = {{snapshot.grenade_selected_path,"已载入方案","de_mirage",""},
        {"cache/grenade/plans/pending.json","等待载入方案","de_mirage",""}};
    actions = frame();
    expect(!actions.movement_draft_changed,"载入蹲姿方案不伪装手工编辑");
    actions = click("保存方案");
    expect(value(actions).value("crouch",false),"载入已存蹲姿须保留姿态字段");
    snapshot.busy = true; frame(); actions = click("删除方案");
    expect(actions.debug_action == Action::NONE,"运行期间禁止归档当前道具");
    snapshot.busy = false; snapshot.movement_recording = true; frame(); actions = click("删除方案");
    expect(actions.debug_action == Action::NONE,"录制期间禁止归档当前道具");
    snapshot.movement_recording = false; frame();

    // 删除只需已载入的保存目标，不能隐式保存或被下一次草稿校验拦住。
    click("道具名称"); auto& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl,true); io.AddKeyEvent(ImGuiKey_A,true); frame();
    io.AddKeyEvent(ImGuiKey_A,false); io.AddKeyEvent(ImGuiMod_Ctrl,false);
    io.AddKeyEvent(ImGuiKey_Backspace,true); actions = frame();
    expect(actions.movement_draft_changed && actions.debug_request.plan_text.empty(),"删除夹具须先产生真实无效草稿");
    io.AddKeyEvent(ImGuiKey_Backspace,false); frame();
    actions = click("删除方案");
    expect(actions.debug_action == Action::DELETE_PLAN && actions.debug_request.load_path == snapshot.grenade_selected_path &&
        actions.debug_request.mode == Mode::GRENADE_TEST && !actions.movement_draft_changed &&
        actions.debug_request.plan_text.empty() && !actions.debug_request.movement_save_as,
        "删除必须交付当前载入路径，忽略无效未保存草稿且不执行保存");
    snapshot.grenade_selected_path.clear(); ++snapshot.grenade_catalog_revision; frame();
    actions = click("删除方案");
    expect(actions.debug_action == Action::NONE,"归档完成清空保存目标后不得重复删除旧路径");

    panel = std::make_unique<MovementPanel>(Mode::MOVEMENT_TEST); snapshot = Snapshot{};
    std::string rendered; frame(nullptr,false,&rendered);
    expect(rendered.find("删除方案") == std::string::npos && rendered.find("瞄点姿态") == std::string::npos,
        "道具删除与瞄点姿态不进入身法编辑器");
}

}

int main() {
    ImGui::CreateContext(); auto& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.ConfigInputTrickleEventQueue = false;
    io.DisplaySize = {1200, 2100}; io.DeltaTime = 1.f / 60;
    unsigned char* pixels; int width, height; io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    interactions();
    grenade_interactions();
    grenade_editor_cleanup_interactions();
    grenade_map_team_interactions();
    grenade_crouch_delete_interactions();
    for (const auto mode : {debug_session::Mode::MOVEMENT_TEST, debug_session::Mode::GRENADE_TEST})
    for (const ImVec2 size : {ImVec2{620, 480}, ImVec2{820, 600}}) {
        io.DisplaySize = size; MovementPanel panel(mode);
        ImGui::NewFrame(); ImGui::SetNextWindowSize(size);
        ImGui::Begin(mode == debug_session::Mode::GRENADE_TEST ? "道具窄窗口回归" : "身法窄窗口回归", nullptr, ImGuiWindowFlags_NoSavedSettings);
        OverlayActions actions; AppConfig config; panel.render(config, nullptr, actions);
        expect(actions.debug_action == debug_session::Action::LIST_MOVEMENT_PLANS, "首次浏览仅查询方案库");
        ImGui::End(); ImGui::Render();
        const auto* data = ImGui::GetDrawData();
        for (int n = 0; n < data->CmdListsCount; ++n)
            for (const auto& vertex : data->CmdLists[n]->VtxBuffer)
                expect(std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y), "时间轴不能生成非有限坐标");
    }
    ImGui::DestroyContext();
    return failures ? 1 : 0;
}
