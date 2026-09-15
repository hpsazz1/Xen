#include "overlay/debug_panel.h"
#include "overlay/overlay.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <cmath>
#include <iostream>
#include <limits>
namespace {
int failures = 0;
void expect(bool result, const char* message) {
    if (!result) { ++failures; std::cerr << message << '\n'; }
}
int draw(DebugPanel& panel, const debug_session::Snapshot* snapshot, int page) {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({1000, 2000});
    ImGui::Begin("调试面板无设备回归", nullptr, ImGuiWindowFlags_NoSavedSettings);
    OverlayActions actions; AppConfig config;
    panel.render_status(snapshot, actions);
    if (page == 0) panel.render_counterpulse(config, snapshot, actions);
    if (page == 1) panel.render_fire(config, snapshot, actions);
    if (page == 2) panel.render_manual(snapshot, actions);
    panel.render_results(snapshot);
    expect(actions.debug_action == debug_session::Action::NONE && !actions.debug_allow_physical_output &&
        actions.debug_confirmation.empty() && !actions.training_start_requested && actions.runtime_intents.empty(),
        "浏览或切页不能继承授权、启动任务或输出设备");
    ImGui::End(); ImGui::Render();
    auto* data = ImGui::GetDrawData(); expect(data && data->Valid, "快照应可绘制");
    if (data) for (int n = 0; n < data->CmdListsCount; ++n)
        for (const auto& v : data->CmdLists[n]->VtxBuffer)
            expect(std::isfinite(v.pos.x) && std::isfinite(v.pos.y), "缺测、零值和非有限数据不能生成非法坐标");
    return data ? data->TotalVtxCount : 0;
}
// 通过真实控件点击检查动作交付；所有动作仅留在本地值对象，不传给Session或设备。
void check_foreground_start() {
    DebugPanel panel;
    debug_session::Snapshot snapshot;
    AppConfig config;
    auto frame = [&](const char* focus = nullptr) {
        ImGui::NewFrame();
        auto* prior = ImGui::FindWindowByName("调试启动交互回归");
        if (focus && prior) ImGui::SetFocusID(prior->GetID(focus), prior);
        ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize({1000,2000});
        ImGui::Begin("调试启动交互回归",nullptr,ImGuiWindowFlags_NoSavedSettings);
        OverlayActions actions;
        panel.render_status(&snapshot,actions);
        panel.render_fire(config,&snapshot,actions);
        ImGui::End(); ImGui::Render();
        return actions;
    };
    auto click = [&](const char* label) {
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(-100,-100); frame(label);
        auto* window = ImGui::FindWindowByName("调试启动交互回归");
        expect(window && ImGui::GetCurrentContext()->NavId == window->GetID(label), "回归必须定位真实控件");
        const auto rect = ImGui::WindowRectRelToAbs(window,window->NavRectRel[ImGuiNavLayer_Main]);
        expect(window->ClipRect.Contains(rect.GetCenter()), "交互控件必须位于窗口可见区域");
        io.AddMousePosEvent(rect.GetCenter().x,rect.GetCenter().y); frame();
        io.AddMouseButtonEvent(0,true); frame();
        io.AddMouseButtonEvent(0,false); return frame();
    };
    frame(); frame();
    auto action = click("显示 HUD");
    expect(action.debug_action == debug_session::Action::SHOW_HUD && !action.debug_plan_edited &&
        !action.debug_allow_physical_output,"空闲HUD开关须交付显示动作，不改草稿或授予物理输出");
    snapshot.hud_requested = true; frame();
    action = click("显示 HUD");
    expect(action.debug_action == debug_session::Action::HIDE_HUD && !action.debug_plan_edited,
        "同一HUD开关应取消显示，不能重复提交任务");
    snapshot.hud_requested = false; frame();
    action = click("准备");
    expect(action.debug_action == debug_session::Action::PREPARE && action.debug_plan_edited, "准备按钮必须发出原生准备动作并使旧重复模板失效");
    snapshot.state = debug_session::State::PREPARED;
    snapshot.physical = true; snapshot.generation = 1; snapshot.prepared_id = "ui-prepared-1";
    snapshot.plan = {{"baseline","stationary"},{"shot_hold_ms",80},{"fire_interval_ms",800}};
    frame();
    action = click("由用户前台启动本次任务");
    expect(action.debug_action == debug_session::Action::NONE, "未勾选不能启动物理任务");
    click("允许本次真实物理输出");
    action = click("由用户前台启动本次任务");
    expect(action.debug_action == debug_session::Action::START && action.debug_prepared_id == "ui-prepared-1" &&
        action.debug_allow_physical_output && !action.debug_plan_edited && action.debug_confirmation == debug_session::physical_confirmation(),
        "用户勾选并点击启动后必须交付匹配身份、物理许可及内部core确认，不要求手输令牌");
    frame();
    action = click("由用户前台启动本次任务");
    expect(action.debug_action == debug_session::Action::NONE, "启动授权不得自动重放第二次");
    click("准备"); ++snapshot.generation; snapshot.prepared_id = "ui-prepared-2"; frame();
    click("允许本次真实物理输出");
    ++snapshot.generation; snapshot.prepared_id = "ui-prepared-external"; frame();
    action = click("由用户前台启动本次任务");
    expect(action.debug_action == debug_session::Action::NONE, "准备身份变化必须撤销旧勾选");
    ImGui::GetIO().AddMousePosEvent(-100,-100); frame();
}

}
int main() {
    ImGui::CreateContext(); auto& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.ConfigInputTrickleEventQueue = false; io.DisplaySize = {1100, 2100}; io.DeltaTime = 1.f/60;
    unsigned char* pixels; int width, height; io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    {
        DebugPanel panel;
        for (int p = 0; p < 3; ++p) draw(panel, nullptr, p);
        debug_session::Snapshot s; s.physical = true; s.state = debug_session::State::PREPARED; s.prepared_id = "不会自动继承的身份";
        auto result = std::make_shared<debug_session::Json>();
        (*result)["shots"] = debug_session::Json::array({
            {{"previous_down_submit_interval_ms", nullptr},{"observed_hold_ms",nullptr}},
            {{"previous_down_submit_interval_ms", 0},{"down_ack_to_up_submit_ms",0},{"observed_hold_ms",0},{"valid_command_hold",true}},
            {{"previous_down_submit_interval_ms", std::numeric_limits<double>::infinity()},{"valid_command_hold",false}},
            {{"previous_down_submit_interval_ms", 700},{"down_ack_to_up_submit_ms",59},{"observed_hold_ms",60},{"valid_command_hold",true}}});
        s.result = result;
        for (int p = 0; p < 3; ++p) draw(panel, &s, p);
        // 原生物理报告外壳也须读取内嵌分析；原地模式不得展示模型判断。
        auto wrapped = std::make_shared<debug_session::Json>();
        (*wrapped)["sampling_analysis"] = *result;
        s.result = wrapped; s.plan = {{"baseline","stationary"}};
        const auto wrapped_vertices = draw(panel, &s, 3);
        auto without_report = s; without_report.result.reset();
        expect(wrapped_vertices > draw(panel, &without_report, 3), "物理报告外壳必须实际绘制内嵌分析，不能静默遗漏时序图");
        for (int p = 0; p < 3; ++p) draw(panel, &s, p);
        s.live = result;
        s.busy = true; s.state = debug_session::State::CLEANUP_UNKNOWN;
        for (int p = 0; p < 3; ++p) draw(panel, &s, p);
    }
    check_foreground_start();
    ImGui::DestroyContext(); return failures ? 1 : 0;
}
