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
    auto frame = [&](const char* focus = nullptr) {
        ImGui::NewFrame();
        auto* prior = ImGui::FindWindowByName(title);
        if (focus && prior) ImGui::SetFocusID(prior->GetID(focus), prior);
        ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({1100, 2000});
        ImGui::Begin(title, nullptr, ImGuiWindowFlags_NoSavedSettings);
        OverlayActions actions;
        panel.render(config, &snapshot, actions);
        ImGui::End(); ImGui::Render();
        return actions;
    };
    auto click = [&](const char* label) {
        auto& io = ImGui::GetIO(); io.AddMousePosEvent(-100, -100); frame(label);
        auto* window = ImGui::FindWindowByName(title);
        expect(window && ImGui::GetCurrentContext()->NavId == window->GetID(label), "交互必须定位真实控件");
        const auto rect = ImGui::WindowRectRelToAbs(window, window->NavRectRel[ImGuiNavLayer_Main]);
        io.AddMousePosEvent(rect.GetCenter().x, rect.GetCenter().y); frame();
        io.AddMouseButtonEvent(0, true); frame();
        io.AddMouseButtonEvent(0, false); return frame();
    };
    auto actions = frame();
    expect(actions.debug_action == debug_session::Action::LIST_MOVEMENT_PLANS && actions.movement_draft_changed,
        "首次显示必须异步查询方案库并发布草稿，不输出设备");
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
    actions = frame(); expect(actions.movement_draft_changed && !actions.debug_plan_edited, "载入身法方案只更新草稿，不撤销启用");
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
    click("方案名称"); edit_io.AddInputCharactersUTF8("修复后的方案"); actions = frame();
    expect(actions.movement_draft_changed && !actions.debug_request.plan_text.empty(), "修复参数立即恢复有效草稿");
    actions = click("测试一次"); expect(actions.debug_action == debug_session::Action::RUN_MOVEMENT_TEST,
        "修复草稿无需重新授权即可启动");
    actions = click("关闭身法测试");
    expect(actions.debug_action == debug_session::Action::CANCEL, "空闲且启用时必须可以撤销授权");
    snapshot.busy = true; actions = click("关闭身法测试");
    expect(actions.debug_action == debug_session::Action::CANCEL, "运行中必须可以请求停止");

}
}

int main() {
    ImGui::CreateContext(); auto& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.ConfigInputTrickleEventQueue = false;
    io.DisplaySize = {1200, 2100}; io.DeltaTime = 1.f / 60;
    unsigned char* pixels; int width, height; io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    interactions();
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
