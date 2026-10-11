#include "overlay/log_panel_internal.h"
#include <imgui_internal.h>
#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

namespace {
int failures = 0;
void expect(bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << message << '\n'; }
}
struct FakeClipboard {
    std::vector<std::string> writes;
    void install() {
        auto& platform = ImGui::GetPlatformIO();
        platform.Platform_ClipboardUserData = this;
        platform.Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text) {
            auto* self = static_cast<FakeClipboard*>(ImGui::GetPlatformIO().Platform_ClipboardUserData);
            self->writes.emplace_back(text);
        };
        platform.Platform_GetClipboardTextFn = [](ImGuiContext*) { return ""; };
        platform.Platform_SetImeDataFn = nullptr;
        platform.Platform_OpenInShellFn = nullptr;
    }
};
void reading_position(float scale, float width) {
    ImGui::CreateContext();
    FakeClipboard clipboard; clipboard.install();
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
    expect(clipboard.writes.empty(), "阅读日志不应自动复制");
    ImGui::DestroyContext();
}

void copy_actions(float scale, float width) {
    ImGui::CreateContext();
    FakeClipboard clipboard; clipboard.install();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.LogFilename = nullptr;
    io.DisplaySize = {640, 480}; io.DeltaTime = 1.0f / 60.0f;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::GetStyle().FontScaleMain = scale;
    unsigned char* pixels = nullptr; int atlas_width = 0, atlas_height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &atlas_width, &atlas_height);
    const std::string diagnostic = "[2026-10-11 12:34:56.789] [error] [fixture] 错误🙂 100% ### detail\n第二行：" + std::string(400, 'x') + "\r\n";
    std::vector<std::string> lines{"[info] unrelated", diagnostic, "[warn] other"};
    overlay::detail::LogFilter filter{};
    ImVec2 input{}, copy{};
    auto frame = [&] {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({width, 260});
        ImGui::Begin("copy-fixture", nullptr, ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
        overlay::detail::render_log_filter(filter);
        const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        input = {(min.x + max.x) / 2, (min.y + max.y) / 2};
        expect(max.x <= ImGui::GetWindowPos().x + ImGui::GetWindowSize().x, "窄布局筛选框不得超出窗口");
        ImGui::SetNextWindowScroll({-1, 0});
        overlay::detail::render_log_entries(lines, {0.5f, 0.5f, 0.5f, 1}, false, filter.data());
        const auto* child = ImGui::GetCurrentWindow()->DC.ChildWindows.back();
        copy = {child->DC.CursorStartPos.x + 8, child->DC.CursorStartPos.y + 5};
        ImGui::End(); ImGui::Render();
        expect(ImGui::GetDrawData()->CmdListsCount > 0, "复制场景必须生成实际绘制数据");
    };
    auto click = [&](ImVec2 at) {
        io.AddMousePosEvent(at.x, at.y); frame();
        io.AddMouseButtonEvent(0, true); frame();
        io.AddMouseButtonEvent(0, false); frame();
    };
    auto set_filter = [&](const char* text) {
        click(input);
        io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_A, true); frame();
        io.AddKeyEvent(ImGuiKey_A, false); io.AddKeyEvent(ImGuiMod_Ctrl, false); frame();
        io.AddKeyEvent(ImGuiKey_Backspace, true); frame();
        io.AddKeyEvent(ImGuiKey_Backspace, false); frame();
        io.AddInputCharactersUTF8(text); frame(); frame();
        expect(std::strcmp(filter.data(), text) == 0, "筛选必须由真实ImGui输入控件接收Unicode文本");
    };
    frame(); frame();
    expect(clipboard.writes.empty(), "初次显示不得修改剪贴板");
    set_filter("错误");
    click(copy);
    expect(clipboard.writes.size() == 1 && clipboard.writes.back() == diagnostic,
        "筛选后只复制所点击完整条目，Unicode/格式字段/百分号/###/多行与换行均须原样保留");
    auto count = clipboard.writes.size();
    for (int i = 0; i < 3; ++i) click(copy);
    expect(clipboard.writes.size() == count + 3, "重复点击每次恰好复制一次");
    count = clipboard.writes.size();
    io.AddMouseButtonEvent(0, true); frame();
    for (int i = 0; i < 40; ++i) frame();
    expect(clipboard.writes.size() == count, "长按不得反复复制");
    io.AddMouseButtonEvent(0, false); frame();
    expect(clipboard.writes.size() == count + 1, "长按松开只复制一次");
    count = clipboard.writes.size();
    set_filter("错误");
    io.AddMousePosEvent(-100, -100); frame();
    io.AddKeyEvent(ImGuiKey_Tab, true); frame();
    io.AddKeyEvent(ImGuiKey_Tab, false); frame();
    std::cout << "keyboard nav_id=" << GImGui->NavId << " nav_window="
        << (GImGui->NavWindow ? GImGui->NavWindow->Name : "none") << '\n';
    io.AddKeyEvent(ImGuiKey_Space, true); frame();
    io.AddKeyEvent(ImGuiKey_Space, false); frame();
    expect(clipboard.writes.size() == count + 1 && clipboard.writes.back() == diagnostic,
        "复制按钮键盘Space激活必须保持精确范围");
    count = clipboard.writes.size();
    set_filter("不存在"); click(copy);
    expect(clipboard.writes.size() == count, "无匹配时不得复制隐藏条目或空字符串");
    set_filter("ERROR"); click(copy);
    expect(clipboard.writes.size() == count, "筛选须按界面说明区分大小写");
    set_filter(""); click(copy);
    expect(clipboard.writes.size() == count + 1 && clipboard.writes.back() == lines.front(),
        "清空筛选恢复原顺序且不混入其他行");
    count = clipboard.writes.size();
    lines = {""}; frame(); frame(); click(copy);
    lines.clear(); frame(); frame(); click(copy);
    expect(clipboard.writes.size() == count, "空条目和空列表均不得擦除剪贴板");
    lines = {"old ### same"}; frame(); frame();
    io.AddMousePosEvent(copy.x, copy.y); frame();
    io.AddMouseButtonEvent(0, true); frame();
    lines[0] = "new ### same"; frame();
    io.AddMouseButtonEvent(0, false); frame();
    expect(clipboard.writes.size() == count, "按下后同位置更新不得复制另一条，正文###也不能混淆ID");
    click(copy);
    expect(clipboard.writes.size() == count + 1 && clipboard.writes.back() == lines.front(),
        "更新取消后重新点击应恢复复制");
    std::cout << "copy scale=" << scale << " width=" << width << " exact_utf8_bytes=" << diagnostic.size()
        << " fake_clipboard_writes=" << clipboard.writes.size() << '\n';
    ImGui::DestroyContext();
}
}
int main() {
    // 只有内存ImGui上下文和合成事件，不调用平台后端、原生窗口、设备或系统输入。
    reading_position(1.0f, 460); reading_position(1.25f, 320);
    copy_actions(1.0f, 460); copy_actions(1.25f, 320);
    std::cout << "native_windows=0 system_input=0 system_clipboard=0\n";
    return failures ? 1 : 0;
}
