#include <imgui.h>
#include <imgui_internal.h>
#include <iostream>

// 只有内存上下文与合成ImGui事件；不初始化平台/渲染后端或系统输入。
int main() {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = {600, 400};
    io.DeltaTime = 0.05f;
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    int value = 20;
    ImVec2 center{};
    bool drawing_seen = false, draw_count_valid = true;
    int failures = 0;
    auto expect = [&](bool condition, const char* message) {
        std::cout << message << '=' << (condition ? "PASS" : "FAIL") << '\n';
        if (!condition) ++failures;
    };
    auto frame = [&] {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({20, 20});
        ImGui::SetNextWindowSize({400, 180});
        ImGui::Begin("memory-only", nullptr, ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
        ImGui::SetNextItemWidth(250);
        ImGui::SliderInt("timeline", &value, 0, 100);
        const auto id = ImGui::GetItemID();
        const auto minimum = ImGui::GetItemRectMin(), maximum = ImGui::GetItemRectMax();
        center = {minimum.x + 120, (minimum.y + maximum.y) * 0.5f};
        const bool temporary_input = ImGui::TempInputIsActive(id);
        ImGui::End();
        ImGui::Render();
        const auto* draw = ImGui::GetDrawData();
        if (draw && draw->CmdLists.Size > 0) {
            drawing_seen = true;
            draw_count_valid &= draw->CmdListsCount == draw->CmdLists.Size;
        }
        return temporary_input;
    };
    frame(); frame();
    io.AddMousePosEvent(center.x, center.y);
    frame();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddMouseButtonEvent(0, true);
    expect(frame(), "Ctrl点击进入文字输入");
    io.AddMouseButtonEvent(0, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    frame();
    io.AddInputCharactersUTF8("77");
    frame();
    io.AddKeyEvent(ImGuiKey_Enter, true);
    frame();
    io.AddKeyEvent(ImGuiKey_Enter, false);
    frame();
    expect(value == 77, "Enter提交输入值");
    // 间隔超过双击时间，随后明确只做普通点击。
    for (int i = 0; i < 10; ++i) frame();
    io.AddMouseButtonEvent(0, true);
    bool no_text_flicker = !frame();
    io.AddMouseButtonEvent(0, false);
    no_text_flicker &= !frame();
    no_text_flicker &= !frame();
    expect(no_text_flicker, "提交后普通点击不闪回文字输入");
    expect(value >= 0 && value <= 100, "普通点击保留范围合同");
    const int before_cancel = value;
    for (int i = 0; i < 10; ++i) frame();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddMouseButtonEvent(0, true);
    expect(frame(), "再次Ctrl点击可编辑");
    io.AddMouseButtonEvent(0, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    frame();
    io.AddInputCharactersUTF8("42"); frame();
    io.AddKeyEvent(ImGuiKey_Escape, true); frame();
    io.AddKeyEvent(ImGuiKey_Escape, false); frame();
    expect(value == before_cancel, "Escape取消保留原值");
    expect(drawing_seen && draw_count_valid, "绘制列表计数一致");
    std::cout << "imgui=" << ImGui::GetVersion() << " native_windows=0 system_input=0\n";
    ImGui::DestroyContext();
    return failures ? 1 : 0;
}
