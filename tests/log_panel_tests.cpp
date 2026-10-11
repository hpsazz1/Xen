#include "overlay/log_panel_internal.h"
#include <imgui_internal.h>
#include <cmath>
#include <iostream>
#include <vector>

namespace {
int failures = 0;
void expect(bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << message << '\n'; }
}
void reading_position(float scale, float width) {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.LogFilename = nullptr;
    io.DisplaySize = {640, 480}; io.DeltaTime = 1.0f / 60.0f;
    ImGui::GetStyle().FontScaleMain = scale;
    unsigned char* pixels = nullptr; int atlas_width = 0, atlas_height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &atlas_width, &atlas_height);
    std::vector<std::string> lines;
    for (int i = 0; i < 64; ++i) lines.push_back("error " + std::to_string(i) + ": retained diagnostic detail");
    float scroll = 0, maximum = 0;
    ImVec2 hover{};
    auto frame = [&](float requested_scroll = -1.0f, bool reopen = false) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({width, 220});
        ImGui::Begin("log-fixture", nullptr, ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
        if (requested_scroll >= 0) ImGui::SetNextWindowScroll({-1, requested_scroll});
        overlay::detail::render_log_entries(lines, {0.5f, 0.5f, 0.5f, 1}, reopen);
        const auto* child = ImGui::GetCurrentWindow()->DC.ChildWindows.back();
        scroll = child->Scroll.y; maximum = child->ScrollMax.y;
        hover = child->InnerRect.GetCenter();
        ImGui::End(); ImGui::Render();
        expect(ImGui::GetDrawData()->CmdListsCount > 0, "日志内容必须生成实际ImGui绘制数据");
    };
    frame(); frame(); frame();
    expect(maximum > 0 && std::abs(scroll - maximum) < 1, "首次日志应显示最新内容");
    io.AddMousePosEvent(hover.x, hover.y); frame();
    io.AddMouseWheelEvent(0, 3); frame(); frame();
    const float reading = scroll;
    expect(reading < maximum - 1, "合成滚轮必须实际移离日志底部");
    lines.erase(lines.begin()); lines.push_back("error 64: newly arrived diagnostic");
    frame(); frame();
    std::cout << "scale=" << scale << " reading_before=" << reading << " reading_after=" << scroll
        << " max=" << maximum << " drift=" << scroll-reading << '\n';
    expect(std::abs(scroll - reading) < 1, "新日志不得打断向上阅读的位置");
    frame(maximum); frame();
    lines.erase(lines.begin()); lines.push_back(std::string(300, 'x')); frame(); frame();
    expect(std::abs(scroll - maximum) < 1, "回到底部后必须继续跟随新增长日志");
    frame(0); frame();
    frame(-1, true); frame();
    expect(std::abs(scroll - maximum) < 1, "重新打开日志仍按既有工作流显示最新内容");
    lines.clear(); frame(); frame();
    expect(scroll == 0 && maximum == 0, "清空日志后滚动范围必须归零");
    ImGui::DestroyContext();
}
}
int main() {
    // 只有内存ImGui上下文和合成事件，不调用平台后端、原生窗口、设备或系统输入。
    reading_position(1.0f, 460); reading_position(1.25f, 320);
    std::cout << "native_windows=0 system_input=0\n";
    return failures ? 1 : 0;
}
