#include "overlay/recoil_target_panel.h"
#include "overlay/overlay.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <cmath>
#include <iostream>

namespace {
int failures = 0;
void expect(bool ok, const char* message) {
    if (!ok) { ++failures; std::cerr << message << '\n'; }
}
struct Harness {
    RecoilTargetPanel panel;
    AppConfig config;
    debug_session::Snapshot snapshot;
    ImVec2 size{900, 1400};
    bool can_edit = true;
    Harness() { config.recoil.profile_directory = "cache/ui-test-only/profiles"; config.recoil.sensitivity = 1.4; }
    ImGuiID id(ImGuiWindow* window, const char* label) {
        return ImHashStr(label, 0, window->GetID("recoil_target_panel"));
    }
    OverlayActions frame(const char* focus = nullptr) {
        ImGui::NewFrame();
        if (const auto window = ImGui::FindWindowByName("固定目标UI合同"))
            if (focus) ImGui::SetFocusID(id(window, focus), window);
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(size);
        ImGui::Begin("固定目标UI合同", nullptr, ImGuiWindowFlags_NoSavedSettings);
        OverlayActions actions;
        panel.render(config, actions, &snapshot, nullptr, can_edit);
        ImGui::End();
        ImGui::Render();
        const auto* data = ImGui::GetDrawData();
        expect(data && data->Valid, "无图形设备也能绘制面板合同");
        if (data) for (int n = 0; n < data->CmdListsCount; ++n)
            for (const auto& vertex : data->CmdLists[n]->VtxBuffer)
                expect(std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y), "最小窗口绘制坐标必须有限");
        expect(!actions.debug_allow_physical_output && actions.debug_confirmation.empty(),
            "固定目标面板不得授予物理输出许可");
        expect(actions.debug_action != debug_session::Action::START, "固定目标面板不得发出START");
        return actions;
    }
    OverlayActions click(const char* label) {
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(-100, -100); frame(label);
        const auto window = ImGui::FindWindowByName("固定目标UI合同");
        expect(window && ImGui::GetCurrentContext()->NavId == id(window, label), "必须定位真实固定目标控件");
        const auto rect = ImGui::WindowRectRelToAbs(window, window->NavRectRel[ImGuiNavLayer_Main]);
        expect(window->ClipRect.Contains(rect.GetCenter()), "测试点击必须在可见窗口内");
        io.AddMousePosEvent(rect.GetCenter().x, rect.GetCenter().y); frame();
        io.AddMouseButtonEvent(0, true); frame();
        io.AddMouseButtonEvent(0, false); return frame();
    }
    void edit_text(const char* label, const char* text) {
        click(label);
        auto& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_A, true); frame();
        io.AddKeyEvent(ImGuiKey_A, false); io.AddKeyEvent(ImGuiMod_Ctrl, false); frame();
        io.AddInputCharactersUTF8(text);
        const auto action = frame();
        expect(action.debug_plan_edited, "输入路径必须使已准备模板失效");
        io.AddKeyEvent(ImGuiKey_Enter, true); frame();
        io.AddKeyEvent(ImGuiKey_Enter, false); frame();
    }
};
}

int main() {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigInputTrickleEventQueue = false;
    io.DisplaySize = {1000, 1500}; io.DeltaTime = 1.0f / 60;
    unsigned char* pixels; int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    {
        Harness h;
        expect(h.frame().debug_action == debug_session::Action::NONE, "首次浏览不得准备或运行");
        h.frame();
        const char* observe = "1. 准备短录像（无输出）";
        auto action = h.click(observe);
        expect(action.debug_action == debug_session::Action::PREPARE && action.debug_plan_edited &&
            action.debug_request.mode == debug_session::Mode::RECOIL_TARGET, "短录像只交付固定目标PREPARE");
        auto options = action.debug_request.recoil_target_options;
        expect(options.value("mode", "") == "observe" && !options.value("fire", true), "默认观察必须无射击");
        expect(options.at("roi") == debug_session::Json::array({0,0,0,0}), "观察阶段允许尚未选择ROI");
        expect(options.value("target_shots", 0) <= 5 && options.value("duration_ms", 0) <= 3000,
            "首阶段弹数和时域有界");
        expect(options.value("profile_path", "").find("ui-test-only") != std::string::npos,
            "父路径来自配置目录，不能硬编码另一工作树");
        expect(!options.value("control_observed_stable_confirmed", true), "人工稳定确认默认未获得");
        h.snapshot.state = debug_session::State::COMPLETED;
        h.snapshot.generation = 1;
        h.snapshot.result = std::make_shared<debug_session::Json>(debug_session::Json{
            {"mode", "recoil_target"}, {"target_mode", "calibrate"}, {"weapon_id", "ak47"},
            {"fire", false}, {"success", true}, {"cleanup_known", true},
            {"calibration_path", "wrong-run-calibration.json"}});
        expect(h.frame().debug_action == debug_session::Action::NONE, "错阶段结果不得自动准备下一轮");
        action = h.click(observe);
        expect(action.debug_request.recoil_target_options.value("calibration_path", "") == "",
            "观察请求不能消费旧标定结果污染草稿");
        h.edit_text("父曲线 JSON", "cache/custom-parent.json");
        ++h.snapshot.generation;
        h.snapshot.result = std::make_shared<debug_session::Json>(debug_session::Json{
            {"mode", "recoil_target"}, {"target_mode", "observe"}, {"weapon_id", "ak47"},
            {"fire", false}, {"success", true}, {"cleanup_known", true},
            {"calibration_path", "unexpected.json"}, {"capture_path", "old-run"}});
        h.frame();
        action = h.click(observe);
        options = action.debug_request.recoil_target_options;
        expect(options.value("profile_path", "") == "cache/custom-parent.json", "父路径编辑准确交付");
        expect(options.value("calibration_path", "") == "" && options.value("control_reference_path", "") == "",
            "编辑后旧结果不得恢复标定或无射击资格");
        expect(h.frame().debug_action == debug_session::Action::NONE, "准备及结果不触发自动续跑");

        h.snapshot.busy = true;
        h.snapshot.state = debug_session::State::RUNNING;
        action = h.click("停止本轮");
        expect(action.debug_action == debug_session::Action::CANCEL, "繁忙时仍可停止本轮");
        h.snapshot.busy = false;
        h.can_edit = false;
        expect(h.frame().debug_action == debug_session::Action::NONE, "只读状态不提交动作");
        h.size = {480, 640};
        for (int i = 0; i < 3; ++i) h.frame();
        expect(h.config.recoil.sensitivity == 1.4 && h.config.recoil.profile_directory == "cache/ui-test-only/profiles",
            "面板保持用户灵敏度与生产活动配置");
    }
    ImGui::DestroyContext();
    return failures ? 1 : 0;
}
