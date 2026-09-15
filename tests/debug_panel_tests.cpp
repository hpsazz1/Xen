#include "overlay/debug_panel.h"
#include "overlay/overlay.h"
#include <imgui.h>
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
}
int main() {
    ImGui::CreateContext(); auto& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.DisplaySize = {1100, 2100}; io.DeltaTime = 1.f/60;
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
    ImGui::DestroyContext(); return failures ? 1 : 0;
}
