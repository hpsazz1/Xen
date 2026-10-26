#include "overlay/movement_panel.h"
#include "overlay/overlay.h"
#include "movement/timeline.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <cmath>
#include <iostream>

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
    expect(initial.at("voice_command").get<std::string>().empty(), "道具默认不能发布身法语音口令");

    movement::timeline::Plan grenade;
    grenade.name = "Mirage T1 VIP R1"; grenade.map = "de_mirage";
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
            "道具列表须优先显示Mirage五点且不混入身法库");
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
    snapshot.movement_test_mode = Mode::MOVEMENT_TEST; frame();
    actions = click("启用道具测试");
    expect(actions.debug_action == Action::ENABLE_MOVEMENT_TEST && actions.debug_request.mode == Mode::GRENADE_TEST,
        "另一页重新拥有F11后，道具需再次显式启用");
    snapshot.movement_test_mode = Mode::GRENADE_TEST; frame();
    actions = click("关闭道具测试");
    expect(actions.debug_action == Action::CANCEL, "道具测试必须可以撤销许可和清理");
}

}

int main() {
    ImGui::CreateContext(); auto& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.ConfigInputTrickleEventQueue = false;
    io.DisplaySize = {1200, 2100}; io.DeltaTime = 1.f / 60;
    unsigned char* pixels; int width, height; io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    interactions();
    grenade_interactions();
    for (const ImVec2 size : {ImVec2{620, 480}, ImVec2{820, 600}}) {
        io.DisplaySize = size; MovementPanel panel;
        ImGui::NewFrame(); ImGui::SetNextWindowSize(size);
        ImGui::Begin("身法窄窗口回归", nullptr, ImGuiWindowFlags_NoSavedSettings);
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
