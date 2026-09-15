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

void check_weapon_draft_isolation() {
    DebugPanel panel; AppConfig config; debug_session::Snapshot snapshot;
    snapshot.timing_catalog = weapon::default_timing_catalog(); snapshot.timing_catalog_valid = true;
    int page = 1;
    auto item_id = [](ImGuiWindow* window, const char* label, int item_index) {
        // ImGui::Combo为每个列表项PushID(index)，自定义BeginCombo武器列表没有这一层。
        return item_index < 0 ? window->GetID(label) : ImHashStr(label,0,window->GetID(item_index));
    };
    auto frame = [&](const char* focus = nullptr, bool popup = false, int item_index = -1) {
        ImGui::NewFrame();
        auto* prior = ImGui::FindWindowByName(popup ? "##Combo_00" : "武器草稿交互回归");
        if (focus && prior) ImGui::SetFocusID(item_id(prior,focus,item_index), prior);
        ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize({1000,2000});
        ImGui::Begin("武器草稿交互回归",nullptr,ImGuiWindowFlags_NoSavedSettings);
        OverlayActions actions; panel.render_status(&snapshot,actions);
        if (page == 1) panel.render_fire(config,&snapshot,actions);
        else panel.render_counterpulse(config,&snapshot,actions);
        ImGui::End(); ImGui::Render(); return actions;
    };
    auto click = [&](const char* label, bool popup = false, int item_index = -1) {
        auto& io = ImGui::GetIO(); io.AddMousePosEvent(-100,-100); frame(label,popup,item_index);
        auto* window = ImGui::FindWindowByName(popup ? "##Combo_00" : "武器草稿交互回归");
        expect(window && ImGui::GetCurrentContext()->NavId == item_id(window,label,item_index),"武器回归必须定位真实控件");
        const auto rect = ImGui::WindowRectRelToAbs(window,window->NavRectRel[ImGuiNavLayer_Main]);
        io.AddMousePosEvent(rect.GetCenter().x,rect.GetCenter().y); frame();
        io.AddMouseButtonEvent(0,true); frame(); io.AddMouseButtonEvent(0,false); return frame();
    };
    frame(); frame(); click("带入武器"); frame(); click("p250",true);
    auto action = click("校验计划");
    expect(action.debug_request.shot_hold_ms == 60 && action.debug_request.fire_interval_ms == 350,"选择p250必须带入60/350");
    action = click("带入所选武器参数");
    expect(action.debug_action == debug_session::Action::NONE,"重新带入选定武器不能读取独立设置文件");
    action = click("校验计划");
    expect(action.debug_request.fire_interval_ms == 350,"重新带入必须保持选定p250参数，不能恢复60/600");
    snapshot.plan = {{"baseline","stationary"},{"shot_hold_ms",60},{"fire_interval_ms",600}};
    snapshot.state = debug_session::State::FAILED; snapshot.generation = 2; frame();
    action = click("校验计划");
    expect(action.debug_request.fire_interval_ms == 350,"空或无效文件载入失败增加generation不能用旧60/600覆盖所选武器");
    // 射击完成后切页，随后读取目录或失败增加generation，旧原地计划都不得进入急停草稿。
    snapshot.plan = {{"baseline","stationary"},{"move_ms",1},{"shots",15},{"shot_hold_ms",60},{"fire_interval_ms",600}};
    snapshot.generation = 10; snapshot.state = debug_session::State::COMPLETED; frame();
    page = 0; frame(); ++snapshot.generation; frame();
    action = click("校验计划");
    const auto counter = debug_session::Json::parse(action.debug_request.plan_text);
    expect(counter.value("baseline","") == "counter" && counter.value("move_ms",0) == 500 && counter.value("shots",0) == 20,
        "历史stationary快照不能将counter草稿污染为move1或15次");
    expect(counter.value("overlap_fire_interval",false) && counter.value("counter_hold_ms",0) == 40 &&
        counter.value("counter_delay_ms",-1) == 0 && counter.value("shot_after_release_ms",0) == 18 &&
        counter.value("shot_hold_ms",0) == 5 && counter.value("fire_interval_ms",0) == 300 &&
        counter.value("fire_delay_ms",-1) == 0 && !counter.value("move_during_fire_delay",true),
        "新建GUI急停草稿须为H40动态预算：20次、移动上限500、间隔300");
    click("带入所选武器参数"); action = click("校验计划");
    const auto imported = debug_session::Json::parse(action.debug_request.plan_text);
    expect(imported.value("shot_hold_ms",0) == 60 && imported.value("fire_interval_ms",0) == 350 && imported.value("move_ms",0) == 500,
        "急停页必须复用所选武器两字段且保持独立动作参数");
    page = 1; frame(); action = click("校验计划");
    expect(action.debug_request.fire_interval_ms == 350,"切回射击页必须保留独立武器草稿");
    snapshot.draft_plan = {{"schema_version",2},{"baseline","stationary"},{"move_ms",77},{"shots",4},
        {"shot_hold_ms",9},{"fire_interval_ms",510}};
    snapshot.draft_plan_mode = debug_session::Mode::COUNTERPULSE; snapshot.draft_plan_revision = 1;
    frame(); page = 0; frame(); action = click("校验计划");
    auto explicit_plan = debug_session::Json::parse(action.debug_request.plan_text);
    expect(explicit_plan.value("baseline","") == "stationary" && explicit_plan.value("move_ms",0) == 77,
        "用户明确载入的stationary动作必须保留，不能强制恢复counter");
    click("带入所选武器参数"); action = click("校验计划");
    explicit_plan = debug_session::Json::parse(action.debug_request.plan_text);
    expect(explicit_plan.value("move_ms",0) == 77 && explicit_plan.value("shots",0) == 4 &&
        explicit_plan.value("fire_interval_ms",0) == 350,"共享武器带入只能修改两字段，保留用户明确动作计划");
    snapshot.draft_plan = {{"shot_hold_ms",70},{"fire_interval_ms",410}};
    snapshot.draft_plan_mode = debug_session::Mode::FIRE_TEST; ++snapshot.draft_plan_revision; frame();
    action = click("校验计划");
    expect(debug_session::Json::parse(action.debug_request.plan_text).value("move_ms",0) == 77,
        "异页完成的射击文件载入不能写急停草稿");
    page = 1; frame(); action = click("校验计划");
    expect(action.debug_request.shot_hold_ms == 70 && action.debug_request.fire_interval_ms == 410,
        "明确文件导入必须在射击页使用新两字段");
    snapshot.draft_plan = {{"schema_version",2},{"baseline","counter"},{"move_ms",90},{"shots",6},
        {"fire_delay_ms",200},{"move_during_fire_delay",true},{"shot_hold_ms",9},{"fire_interval_ms",510},
        {"late_tolerance_ms",7},{"counter_hold_ms",40}};
    snapshot.draft_plan_mode = debug_session::Mode::COUNTERPULSE; ++snapshot.draft_plan_revision;
    frame(); page = 0; frame(); click("移动 / ms");
    auto& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl,true); io.AddKeyEvent(ImGuiKey_A,true); frame();
    io.AddKeyEvent(ImGuiKey_A,false); io.AddKeyEvent(ImGuiMod_Ctrl,false); frame();
    io.AddInputCharactersUTF8("110"); frame(); io.AddKeyEvent(ImGuiKey_Enter,true); frame();
    io.AddKeyEvent(ImGuiKey_Enter,false); frame(); action = click("校验计划");
    const auto edited_plan = debug_session::Json::parse(action.debug_request.plan_text);
    expect(edited_plan.value("move_ms",0) == 110 && edited_plan.value("fire_delay_ms",0) == 200 &&
        edited_plan.value("move_during_fire_delay",false) && edited_plan.value("late_tolerance_ms",0) == 7 &&
        edited_plan.value("shots",0) == 6 && !edited_plan.value("overlap_fire_interval",false),
        "显式导入旧计划后编辑移动必须保留200ms等待、并行、容差与其余字段");
    click("按武器间隔动态移动"); action = click("校验计划");
    const auto dynamic = debug_session::Json::parse(action.debug_request.plan_text);
    expect(dynamic.value("overlap_fire_interval",false) && dynamic.value("fire_delay_ms",-1) == 0 &&
        !dynamic.value("move_during_fire_delay",true) && dynamic.value("move_ms",0) == 110,
        "显式切动态模式才清额外等待，并将已有移动时长作为上限");
    click("基准动作"); frame(); click("原地",true,2); action = click("校验计划");
    const auto stationary = debug_session::Json::parse(action.debug_request.plan_text);
    expect(stationary.value("baseline","") == "stationary" && !stationary.value("overlap_fire_interval",true) &&
        stationary.value("fire_delay_ms",0) == 1,"切换原地必须禁用动态移动并保留既有最小等待规则");
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
    check_weapon_draft_isolation();
    ImGui::DestroyContext(); return failures ? 1 : 0;
}
