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
    frame(); frame();
    auto actions = click("准备测试");
    expect(actions.debug_action == debug_session::Action::PREPARE && actions.debug_plan_edited &&
        actions.debug_request.mode == debug_session::Mode::MOVEMENT_TEST, "准备必须交付身法模式并使旧模板失效");
    const auto original = debug_session::Json::parse(actions.debug_request.plan_text);
    snapshot.state = debug_session::State::PREPARED; snapshot.generation = 1;
    snapshot.prepared_id = "movement-1"; snapshot.plan = original; frame();
    actions = click("启动本次测试");
    expect(actions.debug_action == debug_session::Action::NONE, "未勾选不能启动");
    click("允许本次真实物理输出"); actions = click("启动本次测试");
    expect(actions.debug_action == debug_session::Action::START && actions.debug_allow_physical_output &&
        actions.debug_prepared_id == "movement-1" && actions.debug_confirmation == debug_session::physical_confirmation(),
        "确认并启动必须交付准确身份与物理确认");
    actions = click("启动本次测试");
    expect(actions.debug_action == debug_session::Action::NONE, "启动不能自动重放");
    click("准备测试"); ++snapshot.generation; snapshot.prepared_id = "movement-2"; frame();
    click("允许本次真实物理输出"); actions = click("添加动作");
    expect(actions.debug_plan_edited, "添加动作必须使已准备状态失效");
    actions = click("启动本次测试"); expect(actions.debug_action == debug_session::Action::NONE, "编辑后不能使用旧身份启动");
    actions = click("校验方案");
    expect(debug_session::Json::parse(actions.debug_request.plan_text).at("actions").size() == 2,
        "添加动作必须进入实际交付计划");
    click("准备测试"); ++snapshot.generation; snapshot.prepared_id = "fire-external";
    snapshot.plan = {{"shot_hold_ms", 80}}; frame();
    click("允许本次真实物理输出"); actions = click("启动本次测试");
    expect(actions.debug_action == debug_session::Action::NONE, "异页准备结果不能绑定身法启动");
    snapshot.draft_plan_revision = 1; snapshot.draft_plan_mode = debug_session::Mode::FIRE_TEST;
    snapshot.draft_plan = {{"shot_hold_ms", 70}}; frame();
    actions = click("校验方案");
    expect(debug_session::Json::parse(actions.debug_request.plan_text).at("actions").size() == 2, "异页载入不能改写身法草稿");
    movement::timeline::Plan loaded;
    loaded.name = "明确载入方案";
    loaded.actions.push_back({movement::timeline::ActionKind::KEY, movement::timeline::Key::SPACE, 100, 60, 0});
    snapshot.draft_plan_mode = debug_session::Mode::MOVEMENT_TEST; snapshot.draft_plan_revision = 2;
    snapshot.draft_plan = movement::timeline::plan_json(loaded);
    actions = frame(); expect(actions.debug_plan_edited, "明确身法文件载入必须使准备失效");
    actions = click("校验方案");
    expect(debug_session::Json::parse(actions.debug_request.plan_text) == snapshot.draft_plan, "明确载入必须完整进入草稿");
    snapshot.busy = true; actions = click("停止测试");
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
        expect(actions.debug_action == debug_session::Action::NONE, "浏览页面不能产生任务");
        ImGui::End(); ImGui::Render();
        const auto* data = ImGui::GetDrawData();
        for (int n = 0; n < data->CmdListsCount; ++n)
            for (const auto& vertex : data->CmdLists[n]->VtxBuffer)
                expect(std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y), "时间轴不能生成非有限坐标");
    }
    ImGui::DestroyContext();
    return failures ? 1 : 0;
}
