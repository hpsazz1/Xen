#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "overlay/overlay.h"

#include "log/log.h"

#include <Windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <wrl/client.h>

#ifdef ERROR
#undef ERROR
#endif

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <string>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <misc/cpp/imgui_stdlib.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

namespace {

using Microsoft::WRL::ComPtr;

// codex-theme-v1 的颜色被收敛为语义令牌，页面代码不直接散落颜色值。
constexpr unsigned int kInk = 0x1a1c1f;
constexpr unsigned int kMutedInk = 0x626a73;
constexpr unsigned int kFaintInk = 0x66707c;
constexpr unsigned int kAccent = 0x339cff;
constexpr unsigned int kAccentStrong = 0x0d6fc2;
constexpr unsigned int kAccentSoft = 0xeaf4ff;
constexpr unsigned int kSurface = 0xffffff;
constexpr unsigned int kCanvas = 0xf4f4f5;
constexpr unsigned int kSidebar = 0xf3f3f4;
constexpr unsigned int kGroupSurface = 0xfcfcfc;
constexpr unsigned int kFieldSurface = 0xf7f7f8;
constexpr unsigned int kBorder = 0xe6e6e7;
constexpr unsigned int kBorderStrong = 0xd8d8da;
constexpr unsigned int kNavSelected = 0xe5e5e7;
constexpr unsigned int kNavHovered = 0xebebed;
constexpr unsigned int kSuccess = 0x00a240;
constexpr unsigned int kSuccessSoft = 0xe7f6ed;
constexpr unsigned int kWarning = 0x9a5a00;
constexpr unsigned int kWarningSoft = 0xfff3df;
constexpr unsigned int kDanger = 0xba2623;
constexpr unsigned int kDangerSoft = 0xffeceb;
constexpr unsigned int kSkill = 0x924ff7;
constexpr unsigned int kOnAccent = 0xfefefe;

constexpr float kTopBarHeight = 56.0f;
constexpr float kSidebarWidth = 152.0f;
constexpr float kPanelRounding = 8.0f;
constexpr float kWorkspaceInset = 12.0f;
constexpr float kWindowTitleBarHeight = 36.0f;
constexpr float kTitleButtonWidth = 44.0f;
constexpr int kResizeBorder = 6;
// 与 Xen/app/xen.rc 保持一致，用于标题栏和任务栏图标。
constexpr int kAppIconResourceId = 101;

enum class WorkspacePage {
    OVERVIEW,
    DETECTION,
    AIM,
    INPUT,
    SETTINGS,
};

struct StageTiming {
    const char* label = "";
    double milliseconds = 0.0;
};

UiTheme g_active_theme = UiTheme::LIGHT;

unsigned int themed_rgb(unsigned int rgb) noexcept {
    if (g_active_theme == UiTheme::LIGHT) return rgb;
    switch (rgb) {
        case kInk: return 0xffffff;
        case kMutedInk: return 0xa7abb1;
        case kFaintInk: return 0x858b93;
        case kAccentStrong: return 0x5aadff;
        case kAccentSoft: return 0x203448;
        case kSurface: return 0x181818;
        case kCanvas: return 0x101010;
        case kSidebar: return 0x141414;
        case kGroupSurface: return 0x1f1f1f;
        case kFieldSurface: return 0x292929;
        case kBorder: return 0x2d2d2d;
        case kBorderStrong: return 0x3a3a3a;
        case kNavSelected: return 0x2b2b2b;
        case kNavHovered: return 0x242424;
        case kSuccess: return 0x40c977;
        case kSuccessSoft: return 0x183626;
        case kWarning: return 0xf0ad4e;
        case kWarningSoft: return 0x3a2c18;
        case kDanger: return 0xfa423e;
        case kDangerSoft: return 0x3d1f1f;
        case kSkill: return 0xad7bf9;
        case 0xd9eaff: return 0x263b4e;
        case 0xd8ebff: return 0x21374b;
        case 0xaeb7c3: return 0x666b72;
        case 0xe3e6e9: return 0x303030;
        case 0xd9dde1: return 0x303030;
        case 0xdde9f6: return 0x283c50;
        case 0xcfe2f5: return 0x2c465e;
        case 0xf4f6f8: return 0x242424;
        case 0xf5f6f8: return 0x242424;
        case 0xf8f9fa: return 0x202020;
        case 0xe7eaee: return 0x383838;
        case 0xeff1f3: return 0x2a2a2a;
        case 0xc2c7cc: return 0x60646a;
        case 0xcbd0d5: return 0x55595f;
        default: return rgb;
    }
}

ImVec4 rgba(unsigned int rgb, float alpha = 1.0f) noexcept {
    rgb = themed_rgb(rgb);
    return ImVec4(
        static_cast<float>((rgb >> 16) & 0xff) / 255.0f,
        static_cast<float>((rgb >> 8) & 0xff) / 255.0f,
        static_cast<float>(rgb & 0xff) / 255.0f,
        alpha);
}

ImVec4 raw_rgba(unsigned int rgb, float alpha = 1.0f) noexcept {
    return ImVec4(
        static_cast<float>((rgb >> 16) & 0xff) / 255.0f,
        static_cast<float>((rgb >> 8) & 0xff) / 255.0f,
        static_cast<float>(rgb & 0xff) / 255.0f,
        alpha);
}

void apply_codex_theme(UiTheme theme) {
    g_active_theme = theme;
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.ChildRounding = kPanelRounding;
    style.FrameRounding = 6.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 5.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 5.0f;
    style.WindowPadding = ImVec2(0.0f, 0.0f);
    style.FramePadding = ImVec2(10.0f, 6.0f);
    style.CellPadding = ImVec2(4.0f, 3.0f);
    style.ItemSpacing = ImVec2(8.0f, 7.0f);
    style.ItemInnerSpacing = ImVec2(7.0f, 5.0f);
    style.IndentSpacing = 18.0f;
    style.ScrollbarSize = 11.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 0.0f;
    style.FrameBorderSize = 1.0f;
    style.TabBorderSize = 0.0f;

    auto& colors = style.Colors;
    colors[ImGuiCol_Text] = rgba(kInk);
    colors[ImGuiCol_TextDisabled] = rgba(kMutedInk);
    colors[ImGuiCol_WindowBg] = rgba(kCanvas);
    colors[ImGuiCol_ChildBg] = rgba(kSurface);
    colors[ImGuiCol_PopupBg] = rgba(kSurface);
    colors[ImGuiCol_Border] = rgba(kBorder);
    colors[ImGuiCol_BorderShadow] = rgba(0x000000, 0.0f);
    colors[ImGuiCol_FrameBg] = rgba(kFieldSurface);
    colors[ImGuiCol_FrameBgHovered] = rgba(0xd9eaff);
    colors[ImGuiCol_FrameBgActive] = rgba(0xd8ebff);
    colors[ImGuiCol_TitleBg] = rgba(kSurface);
    colors[ImGuiCol_TitleBgActive] = rgba(kSurface);
    colors[ImGuiCol_MenuBarBg] = rgba(kSurface);
    colors[ImGuiCol_ScrollbarBg] = rgba(kCanvas);
    colors[ImGuiCol_ScrollbarGrab] = rgba(0xcbd1d8);
    colors[ImGuiCol_ScrollbarGrabHovered] = rgba(0xaeb7c3);
    colors[ImGuiCol_CheckMark] = rgba(kAccentStrong);
    colors[ImGuiCol_SliderGrab] = rgba(kAccent);
    colors[ImGuiCol_SliderGrabActive] = rgba(kAccentStrong);
    colors[ImGuiCol_Button] = rgba(0xe3e6e9);
    colors[ImGuiCol_ButtonHovered] = rgba(kAccentSoft);
    colors[ImGuiCol_ButtonActive] = rgba(0xd8ebff);
    colors[ImGuiCol_Header] = rgba(0xd9dde1);
    colors[ImGuiCol_HeaderHovered] = rgba(0xdde9f6);
    colors[ImGuiCol_HeaderActive] = rgba(0xcfe2f5);
    colors[ImGuiCol_Separator] = rgba(kBorder);
    colors[ImGuiCol_SeparatorHovered] = rgba(kAccent);
    colors[ImGuiCol_SeparatorActive] = rgba(kAccentStrong);
    colors[ImGuiCol_ResizeGrip] = rgba(kAccent, 0.20f);
    colors[ImGuiCol_ResizeGripHovered] = rgba(kAccent, 0.50f);
    colors[ImGuiCol_ResizeGripActive] = rgba(kAccent, 0.80f);
    colors[ImGuiCol_Tab] = rgba(0xf4f6f8);
    colors[ImGuiCol_TabHovered] = rgba(kAccentSoft);
    colors[ImGuiCol_TabSelected] = rgba(0xd8ebff);
    colors[ImGuiCol_PlotLines] = rgba(kAccent);
    colors[ImGuiCol_PlotHistogram] = rgba(kSkill);
    colors[ImGuiCol_TableHeaderBg] = rgba(0xf5f6f8);
    colors[ImGuiCol_TableBorderStrong] = rgba(kBorderStrong);
    colors[ImGuiCol_TableBorderLight] = rgba(kBorder);
    colors[ImGuiCol_TableRowBg] = rgba(kSurface);
    colors[ImGuiCol_TableRowBgAlt] = rgba(0xf8f9fa);
    colors[ImGuiCol_NavHighlight] = rgba(kAccent);
}

void apply_window_theme(HWND window, UiTheme theme) noexcept {
    if (!window) return;
    // 自绘标题栏负责双色分区；DWM 仅同步系统阴影和窗口边框语义。
    constexpr DWORD kUseImmersiveDarkMode = 20;
    constexpr DWORD kBorderColor = 34;
    const BOOL enabled = theme == UiTheme::DARK ? TRUE : FALSE;
    DwmSetWindowAttribute(
        window, kUseImmersiveDarkMode, &enabled, sizeof(enabled));

    const auto color_ref = [](unsigned int rgb) noexcept {
        return RGB(
            static_cast<BYTE>((rgb >> 16) & 0xff),
            static_cast<BYTE>((rgb >> 8) & 0xff),
            static_cast<BYTE>(rgb & 0xff));
    };
    const COLORREF border = color_ref(
        theme == UiTheme::DARK ? 0x2d2d2d : kBorder);
    DwmSetWindowAttribute(window, kBorderColor, &border, sizeof(border));
}

const char* page_title(WorkspacePage page) noexcept {
    switch (page) {
        case WorkspacePage::OVERVIEW: return "概览";
        case WorkspacePage::DETECTION: return "检测与采集";
        case WorkspacePage::AIM: return "瞄准控制";
        case WorkspacePage::INPUT: return "输入安全";
        case WorkspacePage::SETTINGS: return "偏好设置";
    }
    return "概览";
}

const char* page_context(WorkspacePage page) noexcept {
    switch (page) {
        case WorkspacePage::OVERVIEW: return "P0 / 本地闭环";
        case WorkspacePage::DETECTION: return "模型与画面";
        case WorkspacePage::AIM: return "追踪与控制";
        case WorkspacePage::INPUT: return "输出与急停";
        case WorkspacePage::SETTINGS: return "运行与窗口";
    }
    return "P0 / 本地闭环";
}

const char* runtime_label(RuntimeState state) noexcept {
    switch (state) {
        case RuntimeState::STOPPED: return "已停止";
        case RuntimeState::STARTING: return "启动中";
        case RuntimeState::RUNNING: return "运行中";
        case RuntimeState::STOPPING: return "停止中";
        case RuntimeState::FAILED: return "运行故障";
    }
    return "未知";
}

ImVec4 runtime_color(RuntimeState state) noexcept {
    switch (state) {
        case RuntimeState::RUNNING: return rgba(kSuccess);
        case RuntimeState::STARTING:
        case RuntimeState::STOPPING: return rgba(kWarning);
        case RuntimeState::FAILED: return rgba(kDanger);
        case RuntimeState::STOPPED: return rgba(kMutedInk);
    }
    return rgba(kMutedInk);
}

ImVec4 capture_color(CaptureStatus status) noexcept {
    switch (status) {
        case CaptureStatus::READY:
        case CaptureStatus::FRAME:
        case CaptureStatus::NO_FRAME: return rgba(kSuccess);
        case CaptureStatus::CLOSED: return rgba(kMutedInk);
        default: return rgba(kDanger);
    }
}

ImVec4 detector_color(DetectionStatus status) noexcept {
    if (status == DetectionStatus::SUCCESS) return rgba(kSuccess);
    if (status == DetectionStatus::NOT_RUN) return rgba(kMutedInk);
    return rgba(kDanger);
}

ImVec4 aim_color(AimStatus status) noexcept {
    if (status == AimStatus::SUCCESS) return rgba(kSuccess);
    if (status == AimStatus::NOT_RUN) return rgba(kMutedInk);
    return rgba(kDanger);
}

ImVec4 mouse_color(MouseStatus status) noexcept {
    switch (status) {
        case MouseStatus::READY: return rgba(kSuccess);
        case MouseStatus::DISABLED: return rgba(kWarning);
        case MouseStatus::CLOSED: return rgba(kMutedInk);
        default: return rgba(kDanger);
    }
}

void status_dot_label(const char* text, const ImVec4& color) {
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float line_height = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(cursor.x + 4.0f, cursor.y + line_height * 0.5f),
        3.5f, ImGui::GetColorU32(color));
    ImGui::Dummy(ImVec2(11.0f, line_height));
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::TextColored(color, "%s", text);
}

void draw_badge(const char* text,
                unsigned int foreground,
                unsigned int background) {
    const ImVec2 text_size = ImGui::CalcTextSize(text);
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const ImVec2 size(text_size.x + 18.0f, 24.0f);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(
        cursor, ImVec2(cursor.x + size.x, cursor.y + size.y),
        ImGui::GetColorU32(rgba(background)), 12.0f);
    draw_list->AddText(
        ImVec2(cursor.x + 9.0f,
               cursor.y + (size.y - text_size.y) * 0.5f),
        ImGui::GetColorU32(rgba(foreground)), text);
    ImGui::Dummy(size);
}

void push_primary_button() {
    ImGui::PushStyleColor(ImGuiCol_Button, rgba(kAccentStrong));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rgba(0x0b64af));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, rgba(0x095897));
    ImGui::PushStyleColor(ImGuiCol_Text, raw_rgba(kOnAccent));
}

void push_danger_button() {
    ImGui::PushStyleColor(ImGuiCol_Button, rgba(kDanger));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rgba(0xa6201e));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, rgba(0x8f1b19));
    ImGui::PushStyleColor(ImGuiCol_Text, raw_rgba(kOnAccent));
}

void pop_colored_button() {
    ImGui::PopStyleColor(4);
}

void begin_surface(const char* id, const ImVec2& size) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, rgba(kGroupSurface));
    ImGui::PushStyleColor(ImGuiCol_Border, rgba(kBorder));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, kPanelRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 8.0f));
    ImGui::BeginChild(id, size, ImGuiChildFlags_Borders);
}

void end_surface() {
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

bool begin_form(const char* id, float label_width = 148.0f) {
    if (!ImGui::BeginTable(
            id, 2,
            ImGuiTableFlags_SizingStretchProp |
            ImGuiTableFlags_BordersInnerH)) {
        return false;
    }
    ImGui::TableSetupColumn(
        "配置项", ImGuiTableColumnFlags_WidthFixed, label_width);
    ImGui::TableSetupColumn(
        "值", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

void form_row(const char* label) {
    ImGui::TableNextRow(ImGuiTableRowFlags_None, 36.0f);
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(rgba(kMutedInk), "%s", label);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-1.0f);
}

bool toggle_switch(const char* id, bool* value) {
    constexpr ImVec2 kSize(38.0f, 22.0f);
    const ImVec2 position = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, kSize);
    if (pressed) *value = !*value;

    const bool hovered = ImGui::IsItemHovered();
    const unsigned int track = *value
        ? (hovered ? 0x258ce8 : kAccent)
        : (hovered ? 0xc2c7cc : 0xcbd0d5);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(
        position, ImVec2(position.x + kSize.x, position.y + kSize.y),
        ImGui::GetColorU32(rgba(track)), kSize.y * 0.5f);
    const float knob_x = *value
        ? position.x + kSize.x - 11.0f
        : position.x + 11.0f;
    draw_list->AddCircleFilled(
        ImVec2(knob_x, position.y + 11.0f), 8.0f,
        ImGui::GetColorU32(raw_rgba(kOnAccent)));
    return pressed;
}

bool theme_selector(const char* id, UiTheme* value) {
    const float gap = 4.0f;
    const float width = std::max(
        72.0f, (ImGui::GetContentRegionAvail().x - gap) * 0.5f);
    bool changed = false;
    ImGui::PushID(id);
    const auto option = [&](const char* label, UiTheme option_value) {
        const bool selected = *value == option_value;
        ImGui::PushStyleColor(
            ImGuiCol_Button,
            selected ? rgba(kAccentStrong) : rgba(kFieldSurface));
        ImGui::PushStyleColor(
            ImGuiCol_ButtonHovered,
            selected ? rgba(kAccent) : rgba(kAccentSoft));
        ImGui::PushStyleColor(
            ImGuiCol_ButtonActive, rgba(kAccentStrong));
        ImGui::PushStyleColor(
            ImGuiCol_Text,
            selected ? raw_rgba(kOnAccent) : rgba(kInk));
        if (ImGui::Button(label, ImVec2(width, 30.0f)) && !selected) {
            *value = option_value;
            changed = true;
        }
        ImGui::PopStyleColor(4);
    };
    option("浅色", UiTheme::LIGHT);
    ImGui::SameLine(0.0f, gap);
    option("深色", UiTheme::DARK);
    ImGui::PopID();
    return changed;
}

bool slider_float_control(const char* id,
                          float* value,
                          float minimum,
                          float maximum,
                          const char* format) {
    const float slider_width = std::max(
        72.0f, ImGui::GetContentRegionAvail().x - 72.0f);
    ImGui::PushID(id);
    ImGui::SetNextItemWidth(slider_width);
    const bool slider_changed = ImGui::SliderFloat(
        "##slider", value, minimum, maximum, "",
        ImGuiSliderFlags_NoInput);
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::SetNextItemWidth(64.0f);
    const bool input_submitted = ImGui::InputFloat(
        "##value", value, 0.0f, 0.0f, format,
        ImGuiInputTextFlags_EnterReturnsTrue);
    const bool input_deactivated = ImGui::IsItemDeactivatedAfterEdit();
    if (input_submitted || input_deactivated) {
        *value = std::clamp(*value, minimum, maximum);
    }
    ImGui::PopID();
    return slider_changed || input_submitted || input_deactivated;
}

bool slider_int_control(const char* id,
                        int* value,
                        int minimum,
                        int maximum) {
    const float slider_width = std::max(
        72.0f, ImGui::GetContentRegionAvail().x - 72.0f);
    ImGui::PushID(id);
    ImGui::SetNextItemWidth(slider_width);
    const bool slider_changed = ImGui::SliderInt(
        "##slider", value, minimum, maximum, "",
        ImGuiSliderFlags_NoInput);
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::SetNextItemWidth(64.0f);
    const bool input_submitted = ImGui::InputInt(
        "##value", value, 0, 0,
        ImGuiInputTextFlags_EnterReturnsTrue);
    const bool input_deactivated = ImGui::IsItemDeactivatedAfterEdit();
    if (input_submitted || input_deactivated) {
        *value = std::clamp(*value, minimum, maximum);
    }
    ImGui::PopID();
    return slider_changed || input_submitted || input_deactivated;
}

void draw_timing_bar(double value,
                     double maximum,
                     const ImVec4& color) {
    const float fraction = maximum > 0.0
        ? static_cast<float>(std::clamp(value / maximum, 0.0, 1.0))
        : 0.0f;
    const ImVec2 position = ImGui::GetCursorScreenPos();
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    constexpr float kHeight = 5.0f;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(
        position, ImVec2(position.x + width, position.y + kHeight),
        ImGui::GetColorU32(rgba(0xe7eaee)), 3.0f);
    if (fraction > 0.0f) {
        draw_list->AddRectFilled(
            position,
            ImVec2(position.x + width * fraction, position.y + kHeight),
            ImGui::GetColorU32(color), 3.0f);
    }
    ImGui::Dummy(ImVec2(width, kHeight));
}

} // namespace

struct Overlay::Impl {
    UiConfig config;
    HWND window = nullptr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swap_chain;
    ComPtr<ID3D11RenderTargetView> render_target;
    HICON large_icon = nullptr;
    HICON small_icon = nullptr;
    ImFont* small_font = nullptr;
    ImFont* body_font = nullptr;
    ImFont* medium_font = nullptr;
    ImFont* title_font = nullptr;
    UiTheme applied_theme = UiTheme::LIGHT;
    WorkspacePage active_page = WorkspacePage::OVERVIEW;
    bool initialized = false;
    bool close_requested = false;

    static LRESULT CALLBACK window_proc(
            HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, message, wparam, lparam)) {
            return TRUE;
        }
        auto* self = reinterpret_cast<Impl*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(self));
        }
        switch (message) {
            case WM_NCCALCSIZE:
                // 保留可缩放窗口样式，但由客户区完整绘制标题栏与边框内表面。
                if (wparam == TRUE) return 0;
                break;
            case WM_NCHITTEST: {
                POINT point{
                    static_cast<short>(LOWORD(lparam)),
                    static_cast<short>(HIWORD(lparam))};
                ScreenToClient(hwnd, &point);
                RECT client{};
                GetClientRect(hwnd, &client);
                const int width = client.right - client.left;
                const int height = client.bottom - client.top;
                if (!IsZoomed(hwnd)) {
                    const bool left = point.x < kResizeBorder;
                    const bool right = point.x >= width - kResizeBorder;
                    const bool top = point.y < kResizeBorder;
                    const bool bottom = point.y >= height - kResizeBorder;
                    if (top && left) return HTTOPLEFT;
                    if (top && right) return HTTOPRIGHT;
                    if (bottom && left) return HTBOTTOMLEFT;
                    if (bottom && right) return HTBOTTOMRIGHT;
                    if (left) return HTLEFT;
                    if (right) return HTRIGHT;
                    if (top) return HTTOP;
                    if (bottom) return HTBOTTOM;
                }
                const int button_region =
                    static_cast<int>(kTitleButtonWidth * 3.0f);
                if (point.y >= 0 &&
                    point.y < static_cast<int>(kWindowTitleBarHeight) &&
                    point.x < width - button_region) {
                    return HTCAPTION;
                }
                return HTCLIENT;
            }
            case WM_SIZE:
                if (self && self->device && wparam != SIZE_MINIMIZED) {
                    self->destroy_render_target();
                    self->swap_chain->ResizeBuffers(
                        0, static_cast<UINT>(LOWORD(lparam)),
                        static_cast<UINT>(HIWORD(lparam)),
                        DXGI_FORMAT_UNKNOWN, 0);
                    self->create_render_target();
                }
                return 0;
            case WM_GETMINMAXINFO: {
                // 紧凑布局的最小尺寸，防止关键安全按钮与内容区互相遮挡。
                auto* minmax = reinterpret_cast<MINMAXINFO*>(lparam);
                minmax->ptMinTrackSize.x = 820;
                minmax->ptMinTrackSize.y = 600;
                MONITORINFO monitor_info{sizeof(MONITORINFO)};
                if (GetMonitorInfoW(
                        MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST),
                        &monitor_info)) {
                    const RECT& work = monitor_info.rcWork;
                    const RECT& monitor = monitor_info.rcMonitor;
                    minmax->ptMaxPosition.x = work.left - monitor.left;
                    minmax->ptMaxPosition.y = work.top - monitor.top;
                    minmax->ptMaxSize.x = work.right - work.left;
                    minmax->ptMaxSize.y = work.bottom - work.top;
                }
                return 0;
            }
            case WM_SYSCOMMAND:
                if ((wparam & 0xfff0) == SC_KEYMENU) return 0;
                break;
            case WM_CLOSE:
                if (self) self->close_requested = true;
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    bool create_device() noexcept {
        DXGI_SWAP_CHAIN_DESC swap_desc{};
        swap_desc.BufferCount = 2;
        swap_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swap_desc.OutputWindow = window;
        swap_desc.SampleDesc.Count = 1;
        swap_desc.Windowed = TRUE;
        swap_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        swap_desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

        constexpr D3D_FEATURE_LEVEL kLevels[] = {
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_0,
        };
        D3D_FEATURE_LEVEL selected{};
        const HRESULT hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            kLevels, static_cast<UINT>(std::size(kLevels)),
            D3D11_SDK_VERSION, &swap_desc, &swap_chain,
            &device, &selected, &context);
        return SUCCEEDED(hr) && create_render_target();
    }

    bool create_render_target() noexcept {
        ComPtr<ID3D11Texture2D> back_buffer;
        if (!swap_chain || FAILED(swap_chain->GetBuffer(
                0, IID_PPV_ARGS(&back_buffer)))) {
            return false;
        }
        return SUCCEEDED(device->CreateRenderTargetView(
            back_buffer.Get(), nullptr, &render_target));
    }

    void destroy_render_target() noexcept {
        render_target.Reset();
    }

    bool editable(const RuntimeSnapshot& snapshot) const noexcept {
        return snapshot.state != RuntimeState::RUNNING &&
               snapshot.state != RuntimeState::STARTING &&
               snapshot.state != RuntimeState::STOPPING;
    }

    void render_title_bar() {
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetWindowPos();
        const float width = ImGui::GetWindowWidth();
        draw_list->AddRectFilled(
            origin,
            ImVec2(origin.x + kSidebarWidth,
                   origin.y + kWindowTitleBarHeight),
            ImGui::GetColorU32(rgba(kSidebar)));
        draw_list->AddRectFilled(
            ImVec2(origin.x + kSidebarWidth, origin.y),
            ImVec2(origin.x + width,
                   origin.y + kWindowTitleBarHeight),
            ImGui::GetColorU32(rgba(kSurface)));
        draw_list->AddRectFilled(
            ImVec2(origin.x + kSidebarWidth,
                   origin.y + kWindowTitleBarHeight),
            ImVec2(origin.x + width,
                   origin.y + ImGui::GetWindowHeight()),
            ImGui::GetColorU32(rgba(kSurface)));
        draw_list->AddLine(
            ImVec2(origin.x + kSidebarWidth, origin.y),
            ImVec2(origin.x + kSidebarWidth,
                   origin.y + kWindowTitleBarHeight),
            ImGui::GetColorU32(rgba(kBorder)));
        draw_list->AddLine(
            ImVec2(origin.x, origin.y + kWindowTitleBarHeight - 1.0f),
            ImVec2(origin.x + width,
                   origin.y + kWindowTitleBarHeight - 1.0f),
            ImGui::GetColorU32(rgba(kBorder)));

        const ImVec2 mark(origin.x + 10.0f, origin.y + 9.0f);
        constexpr float kMarkSize = 18.0f;
        draw_list->AddRectFilled(
            mark, ImVec2(mark.x + kMarkSize, mark.y + kMarkSize),
            ImGui::GetColorU32(raw_rgba(
                g_active_theme == UiTheme::DARK ? 0x2b2b2b : 0x1a1c1f)),
            4.0f);
        draw_list->AddLine(
            ImVec2(mark.x + 5.0f, mark.y + 5.0f),
            ImVec2(mark.x + 13.0f, mark.y + 13.0f),
            ImGui::GetColorU32(raw_rgba(kOnAccent)), 2.0f);
        draw_list->AddLine(
            ImVec2(mark.x + 13.0f, mark.y + 5.0f),
            ImVec2(mark.x + 5.0f, mark.y + 13.0f),
            ImGui::GetColorU32(raw_rgba(kOnAccent)), 2.0f);
        draw_list->AddCircleFilled(
            ImVec2(mark.x + 9.0f, mark.y + 9.0f), 1.8f,
            ImGui::GetColorU32(raw_rgba(kAccent)));
        draw_list->AddText(
            medium_font, 14.0f,
            ImVec2(origin.x + 36.0f, origin.y + 9.0f),
            ImGui::GetColorU32(rgba(kInk)), "Xen");

        const auto title_button = [&](const char* id,
                                      int index,
                                      bool danger) {
            const float x = width -
                kTitleButtonWidth * static_cast<float>(3 - index);
            ImGui::SetCursorPos(ImVec2(x, 0.0f));
            ImGui::PushID(id);
            const bool pressed = ImGui::InvisibleButton(
                "button",
                ImVec2(kTitleButtonWidth, kWindowTitleBarHeight));
            ImGui::PopID();
            const bool hovered = ImGui::IsItemHovered();
            const ImVec2 minimum = ImGui::GetItemRectMin();
            const ImVec2 maximum = ImGui::GetItemRectMax();
            if (hovered) {
                draw_list->AddRectFilled(
                    minimum, maximum,
                    ImGui::GetColorU32(
                        danger ? rgba(kDanger) : rgba(kNavHovered)));
            }
            const ImU32 icon_color = ImGui::GetColorU32(
                danger && hovered ? raw_rgba(kOnAccent) : rgba(kInk));
            const ImVec2 center(
                (minimum.x + maximum.x) * 0.5f,
                (minimum.y + maximum.y) * 0.5f);
            if (index == 0) {
                draw_list->AddLine(
                    ImVec2(center.x - 5.0f, center.y + 3.0f),
                    ImVec2(center.x + 5.0f, center.y + 3.0f),
                    icon_color, 1.2f);
            } else if (index == 1) {
                if (IsZoomed(window)) {
                    draw_list->AddRect(
                        ImVec2(center.x - 3.0f, center.y - 5.0f),
                        ImVec2(center.x + 5.0f, center.y + 3.0f),
                        icon_color, 0.0f, 0, 1.1f);
                    draw_list->AddRect(
                        ImVec2(center.x - 5.0f, center.y - 3.0f),
                        ImVec2(center.x + 3.0f, center.y + 5.0f),
                        icon_color, 0.0f, 0, 1.1f);
                } else {
                    draw_list->AddRect(
                        ImVec2(center.x - 5.0f, center.y - 5.0f),
                        ImVec2(center.x + 5.0f, center.y + 5.0f),
                        icon_color, 0.0f, 0, 1.1f);
                }
            } else {
                draw_list->AddLine(
                    ImVec2(center.x - 4.5f, center.y - 4.5f),
                    ImVec2(center.x + 4.5f, center.y + 4.5f),
                    icon_color, 1.2f);
                draw_list->AddLine(
                    ImVec2(center.x + 4.5f, center.y - 4.5f),
                    ImVec2(center.x - 4.5f, center.y + 4.5f),
                    icon_color, 1.2f);
            }
            return pressed;
        };

        if (title_button("minimize", 0, false)) {
            ShowWindow(window, SW_MINIMIZE);
        }
        if (title_button("maximize", 1, false)) {
            ShowWindow(window, IsZoomed(window) ? SW_RESTORE : SW_MAXIMIZE);
        }
        if (title_button("close", 2, true)) {
            close_requested = true;
        }
    }

    bool nav_item(const char* label, WorkspacePage page) {
        const bool selected = active_page == page;
        const float start_x = ImGui::GetCursorPosX();
        ImGui::SetCursorPosX(start_x + 4.0f);
        const ImVec2 position = ImGui::GetCursorScreenPos();
        const ImVec2 size(
            std::max(80.0f, ImGui::GetContentRegionAvail().x - 8.0f),
            42.0f);
        ImGui::PushID(static_cast<int>(page));
        const bool pressed = ImGui::InvisibleButton("nav", size);
        ImGui::PopID();

        const bool hovered = ImGui::IsItemHovered();
        if (selected || hovered) {
            ImGui::GetWindowDrawList()->AddRectFilled(
                position,
                ImVec2(position.x + size.x, position.y + size.y),
                ImGui::GetColorU32(
                    rgba(selected ? kNavSelected : kNavHovered)),
                6.0f);
        }
        constexpr float kNavFontSize = 16.0f;
        ImGui::PushFont(medium_font);
        const ImVec2 text_size = ImGui::CalcTextSize(label);
        ImGui::PopFont();
        ImGui::GetWindowDrawList()->AddText(
            medium_font, kNavFontSize,
            ImVec2(position.x + (size.x - text_size.x) * 0.5f,
                   position.y + (size.y - text_size.y) * 0.5f),
            ImGui::GetColorU32(
                rgba(selected ? kInk : kMutedInk)),
            label);
        if (pressed) active_page = page;
        return pressed;
    }

    void render_sidebar(const RuntimeSnapshot& snapshot) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, rgba(kSidebar));
        ImGui::PushStyleVar(
            ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 12.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
        ImGui::BeginChild(
            "sidebar", ImVec2(kSidebarWidth, 0.0f),
            ImGuiChildFlags_None,
            ImGuiWindowFlags_NoScrollbar);

        nav_item("概览", WorkspacePage::OVERVIEW);
        nav_item("检测", WorkspacePage::DETECTION);
        nav_item("瞄准", WorkspacePage::AIM);
        nav_item("输入", WorkspacePage::INPUT);
        nav_item("设置", WorkspacePage::SETTINGS);

        const float footer_y = ImGui::GetWindowHeight() - 76.0f;
        if (ImGui::GetCursorPosY() < footer_y) {
            ImGui::SetCursorPosY(footer_y);
        }
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, 5.0f));
        status_dot_label(
            snapshot.output_armed ? "输出已武装" : "输出未武装",
            snapshot.output_armed ? rgba(kSuccess) : rgba(kMutedInk));
        ImGui::PushFont(small_font);
        ImGui::TextColored(rgba(kFaintInk), "P0  /  v0.1");
        ImGui::PopFont();

        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }

    void render_global_bar(const RuntimeSnapshot& snapshot,
                           OverlayActions& actions) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, rgba(kSurface));
        ImGui::PushStyleVar(
            ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 10.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
        ImGui::BeginChild(
            "global_bar", ImVec2(0.0f, kTopBarHeight),
            ImGuiChildFlags_None,
            ImGuiWindowFlags_NoScrollbar);

        ImGui::SetCursorPosY(16.0f);
        if (snapshot.state == RuntimeState::RUNNING) {
            draw_badge(runtime_label(snapshot.state),
                       kSuccess, kSuccessSoft);
        } else if (snapshot.state == RuntimeState::FAILED) {
            draw_badge(runtime_label(snapshot.state),
                       kDanger, kDangerSoft);
        } else if (snapshot.state == RuntimeState::STARTING ||
                   snapshot.state == RuntimeState::STOPPING) {
            draw_badge(runtime_label(snapshot.state),
                       kWarning, kWarningSoft);
        } else {
            draw_badge(runtime_label(snapshot.state),
                       kMutedInk, 0xeff1f3);
        }

        const float width = ImGui::GetWindowWidth();
        if (width >= 620.0f) {
            ImGui::SameLine(0.0f, 14.0f);
            ImGui::SetCursorPosY(19.0f);
            const char* provider = snapshot.provider.empty()
                ? "Provider 未就绪"
                : snapshot.provider.c_str();
            ImGui::TextColored(
                rgba(kMutedInk), "%.14s  |  %.1f FPS  |  P95 %.2f ms",
                provider, snapshot.capture_fps, snapshot.pipeline_p95_ms);
        }

        constexpr float kButtonWidth = 64.0f;
        constexpr float kButtonGap = 8.0f;
        constexpr float kControlWidth =
            kButtonWidth * 3.0f + kButtonGap * 2.0f;
        ImGui::SetCursorPos(ImVec2(
            width - kControlWidth - 14.0f, 10.0f));

        const bool running = snapshot.state == RuntimeState::RUNNING ||
                             snapshot.state == RuntimeState::STARTING;
        if (running) {
            if (ImGui::Button("停止", ImVec2(kButtonWidth, 32.0f))) {
                actions.stop_requested = true;
            }
        } else {
            ImGui::BeginDisabled(snapshot.state == RuntimeState::STOPPING);
            push_primary_button();
            if (ImGui::Button("启动", ImVec2(kButtonWidth, 32.0f))) {
                actions.start_requested = true;
            }
            pop_colored_button();
            ImGui::EndDisabled();
        }

        ImGui::SameLine(0.0f, kButtonGap);
        if (snapshot.output_armed) {
            if (ImGui::Button("解除", ImVec2(kButtonWidth, 32.0f))) {
                actions.runtime_intents.push_back(
                    {RuntimeIntentType::DISARM_OUTPUT, false});
            }
        } else {
            ImGui::BeginDisabled(
                !running || snapshot.emergency_stopped ||
                !snapshot.output_allowed_by_config);
            if (ImGui::Button("武装", ImVec2(kButtonWidth, 32.0f))) {
                actions.runtime_intents.push_back(
                    {RuntimeIntentType::ARM_OUTPUT, true});
            }
            ImGui::EndDisabled();
        }

        ImGui::SameLine(0.0f, kButtonGap);
        push_danger_button();
        if (ImGui::Button("急停", ImVec2(kButtonWidth, 32.0f))) {
            actions.runtime_intents.push_back(
                {RuntimeIntentType::EMERGENCY_STOP, true});
        }
        pop_colored_button();

        const ImVec2 window_position = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(window_position.x,
                   window_position.y + kTopBarHeight - 1.0f),
            ImVec2(window_position.x + width,
                   window_position.y + kTopBarHeight - 1.0f),
            ImGui::GetColorU32(rgba(kBorder)));

        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }

    void render_page_heading(bool can_edit, OverlayActions& actions) {
        ImGui::PushFont(small_font);
        ImGui::TextColored(rgba(kFaintInk), "%s", page_context(active_page));
        ImGui::PopFont();
        ImGui::PushFont(title_font);
        ImGui::TextUnformatted(page_title(active_page));
        ImGui::PopFont();

        if (active_page != WorkspacePage::OVERVIEW) {
            const float button_width = 96.0f;
            ImGui::SetCursorPos(ImVec2(
                ImGui::GetWindowWidth() - button_width - 22.0f, 12.0f));
            ImGui::BeginDisabled(!can_edit);
            push_primary_button();
            if (ImGui::Button(
                    "保存配置", ImVec2(button_width, 34.0f))) {
                actions.save_config_requested = true;
            }
            pop_colored_button();
            ImGui::EndDisabled();
        }
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
    }

    void render_notice(const char* id,
                       const std::string& message,
                       unsigned int foreground,
                       unsigned int background) {
        if (message.empty()) return;
        const float text_width =
            std::max(80.0f, ImGui::GetContentRegionAvail().x - 24.0f);
        const float text_height = ImGui::CalcTextSize(
            message.c_str(), nullptr, false, text_width).y;
        const float notice_height = std::max(38.0f, text_height + 18.0f);

        ImGui::PushStyleColor(ImGuiCol_ChildBg, rgba(background));
        ImGui::PushStyleColor(ImGuiCol_Border, rgba(foreground, 0.20f));
        ImGui::PushStyleVar(
            ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 9.0f));
        ImGui::BeginChild(
            id, ImVec2(0.0f, notice_height), ImGuiChildFlags_Borders,
            ImGuiWindowFlags_NoScrollbar);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + text_width);
        ImGui::TextColored(rgba(foreground), "%s", message.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
    }

    void metric_tile(const char* id,
                     const char* label,
                     const char* value,
                     const ImVec4& value_color) {
        begin_surface(id, ImVec2(0.0f, 76.0f));
        ImGui::TextColored(rgba(kMutedInk), "%s", label);
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        ImGui::PushFont(medium_font);
        ImGui::TextColored(value_color, "%s", value);
        ImGui::PopFont();
        end_surface();
    }

    void render_latency_panel(const RuntimeSnapshot& snapshot, float height) {
        begin_surface("latency_panel", ImVec2(0.0f, height));
        ImGui::PushFont(medium_font);
        ImGui::TextUnformatted("延迟链路");
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextColored(rgba(kFaintInk), "最近成功帧");
        ImGui::Dummy(ImVec2(0.0f, 5.0f));

        const PipelineProfile& profile = snapshot.last_profile;
        const std::array<StageTiming, 10> stages{{
            {"Capture", profile.capture_ms},
            {"Queue", profile.queue_ms},
            {"Preprocess", profile.detector.preprocess_ms},
            {"H2D", profile.detector.h2d_ms},
            {"Execution", profile.detector.execution_ms},
            {"D2H", profile.detector.d2h_ms},
            {"Postprocess", profile.detector.postprocess_ms},
            {"Aim", profile.aim.total_ms},
            {"Mouse", profile.mouse_ms},
            {"Total", profile.total_ms},
        }};
        double maximum = 0.0;
        for (std::size_t index = 0; index + 1 < stages.size(); ++index) {
            maximum = std::max(maximum, stages[index].milliseconds);
        }

        if (ImGui::BeginTable(
                "latency_rows", 3, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn(
                "阶段", ImGuiTableColumnFlags_WidthFixed, 88.0f);
            ImGui::TableSetupColumn(
                "条形图", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(
                "耗时", ImGuiTableColumnFlags_WidthFixed, 72.0f);
            for (std::size_t index = 0; index < stages.size(); ++index) {
                const StageTiming& stage = stages[index];
                const bool total = index + 1 == stages.size();
                ImGui::TableNextRow(ImGuiTableRowFlags_None, 27.0f);
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(
                    total ? rgba(kInk) : rgba(kMutedInk),
                    "%s", stage.label);
                ImGui::TableSetColumnIndex(1);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 7.0f);
                draw_timing_bar(
                    stage.milliseconds,
                    total ? std::max(maximum, stage.milliseconds) : maximum,
                    total ? rgba(kSkill) : rgba(kAccent));
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.3f ms", stage.milliseconds);
            }
            ImGui::EndTable();
        }
        end_surface();
    }

    void render_module_panel(const RuntimeSnapshot& snapshot, float height) {
        begin_surface("module_panel", ImVec2(0.0f, height));
        ImGui::PushFont(medium_font);
        ImGui::TextUnformatted("运行检查");
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0.0f, 5.0f));

        if (ImGui::BeginTable(
                "module_rows", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn(
                "模块", ImGuiTableColumnFlags_WidthStretch, 0.9f);
            ImGui::TableSetupColumn(
                "状态", ImGuiTableColumnFlags_WidthStretch, 1.1f);
#define XEN_MODULE_ROW(name, value, color) \
            ImGui::TableNextRow(ImGuiTableRowFlags_None, 26.0f); \
            ImGui::TableSetColumnIndex(0); \
            ImGui::TextColored(rgba(kMutedInk), "%s", name); \
            ImGui::TableSetColumnIndex(1); \
            status_dot_label(value, color)
            XEN_MODULE_ROW(
                "Capture", CaptureStatusName(snapshot.capture_status),
                capture_color(snapshot.capture_status));
            XEN_MODULE_ROW(
                "Detector", DetectionStatusName(snapshot.detection_status),
                detector_color(snapshot.detection_status));
            XEN_MODULE_ROW(
                "Aim", AimStatusName(snapshot.aim_status),
                aim_color(snapshot.aim_status));
            XEN_MODULE_ROW(
                "Mouse", MouseStatusName(snapshot.mouse_status),
                mouse_color(snapshot.mouse_status));
#undef XEN_MODULE_ROW
            ImGui::EndTable();
        }
        end_surface();
    }

    void render_activity_panel(const RuntimeSnapshot& snapshot, float height) {
        begin_surface("activity_panel", ImVec2(0.0f, height));
        ImGui::PushFont(medium_font);
        ImGui::TextUnformatted("帧与目标");
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0.0f, 4.0f));

        if (ImGui::BeginTable(
                "activity_rows", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn(
                "项目", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(
                "数值", ImGuiTableColumnFlags_WidthFixed, 88.0f);
#define XEN_COUNTER_ROW(label, value) \
            ImGui::TableNextRow(ImGuiTableRowFlags_None, 23.0f); \
            ImGui::TableSetColumnIndex(0); \
            ImGui::TextColored(rgba(kMutedInk), "%s", label); \
            ImGui::TableSetColumnIndex(1); \
            ImGui::Text("%llu", static_cast<unsigned long long>(value))
            XEN_COUNTER_ROW("已采集", snapshot.captured_frames);
            XEN_COUNTER_ROW("已处理", snapshot.processed_frames);
            XEN_COUNTER_ROW("覆盖丢帧", snapshot.overwritten_frames);
            XEN_COUNTER_ROW("失败帧", snapshot.failed_frames);
            XEN_COUNTER_ROW("鼠标命令", snapshot.mouse_commands);
#undef XEN_COUNTER_ROW
            ImGui::EndTable();
        }

        ImGui::Separator();
        if (snapshot.last_aim.has_target) {
            ImGui::Text(
                "Track %llu  /  %.3f",
                static_cast<unsigned long long>(
                    snapshot.last_aim.target.track_id),
                snapshot.last_aim.target.confidence);
            ImGui::TextColored(
                rgba(kMutedInk), "%.1f, %.1f  /  %s",
                snapshot.last_aim.target.aim_x,
                snapshot.last_aim.target.aim_y,
                snapshot.last_aim.target.predicted ? "预测" : "观测");
        } else {
            ImGui::TextColored(rgba(kFaintInk), "当前无目标");
        }
        end_surface();
    }

    void render_overview(const RuntimeSnapshot& snapshot) {
        char fps[32]{};
        char p95[32]{};
        std::snprintf(fps, std::size(fps), "%.1f FPS", snapshot.capture_fps);
        std::snprintf(
            p95, std::size(p95), "%.2f ms", snapshot.pipeline_p95_ms);
        const char* output = snapshot.emergency_stopped
            ? "急停锁定"
            : snapshot.output_armed
                ? "已武装"
                : snapshot.output_allowed_by_config
                    ? "等待武装"
                    : "配置禁用";
        const ImVec4 output_color = snapshot.emergency_stopped
            ? rgba(kDanger)
            : snapshot.output_armed
                ? rgba(kSuccess)
                : snapshot.output_allowed_by_config
                    ? rgba(kWarning)
                    : rgba(kMutedInk);

        const int columns =
            ImGui::GetContentRegionAvail().x >= 610.0f ? 4 : 2;
        if (ImGui::BeginTable(
                "metric_grid", columns,
                ImGuiTableFlags_SizingStretchSame)) {
            const std::array<const char*, 4> ids{{
                "metric_runtime", "metric_fps",
                "metric_latency", "metric_output"}};
            const std::array<const char*, 4> labels{{
                "运行状态", "采集速率", "P95 端到端", "物理输出"}};
            const std::array<const char*, 4> values{{
                runtime_label(snapshot.state), fps, p95, output}};
            const std::array<ImVec4, 4> colors{{
                runtime_color(snapshot.state),
                snapshot.capture_fps > 0.0 ? rgba(kSuccess) : rgba(kMutedInk),
                snapshot.pipeline_p95_ms > 0.0
                    ? rgba(kAccentStrong) : rgba(kMutedInk),
                output_color}};
            for (std::size_t index = 0; index < ids.size(); ++index) {
                if (index % static_cast<std::size_t>(columns) == 0) {
                    ImGui::TableNextRow();
                }
                ImGui::TableSetColumnIndex(
                    static_cast<int>(index %
                                     static_cast<std::size_t>(columns)));
                metric_tile(
                    ids[index], labels[index], values[index], colors[index]);
            }
            ImGui::EndTable();
        }
        ImGui::Dummy(ImVec2(0.0f, 6.0f));

        const bool two_columns =
            ImGui::GetContentRegionAvail().x >= 650.0f;
        if (two_columns && ImGui::BeginTable(
                "overview_body", 2,
                ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn(
                "延迟", ImGuiTableColumnFlags_WidthStretch, 1.65f);
            ImGui::TableSetupColumn(
                "状态", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            render_latency_panel(snapshot, 356.0f);
            ImGui::TableSetColumnIndex(1);
            render_module_panel(snapshot, 150.0f);
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            render_activity_panel(snapshot, 198.0f);
            ImGui::EndTable();
        } else {
            render_latency_panel(snapshot, 356.0f);
            ImGui::Dummy(ImVec2(0.0f, 8.0f));
            render_module_panel(snapshot, 150.0f);
            ImGui::Dummy(ImVec2(0.0f, 8.0f));
            render_activity_panel(snapshot, 198.0f);
        }
    }

    void begin_config_panel(const char* id,
                            const char* title,
                            float height) {
        ImGui::PushFont(small_font);
        ImGui::TextColored(rgba(kFaintInk), "%s", title);
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0.0f, 3.0f));
        begin_surface(id, ImVec2(0.0f, height));
    }

    void end_config_panel() {
        end_surface();
    }

    void render_detection_config(AppConfig& app_config, bool can_edit) {
        ImGui::BeginDisabled(!can_edit);
        const bool two_columns =
            ImGui::GetContentRegionAvail().x >= 650.0f;
        if (two_columns && ImGui::BeginTable(
                "detector_columns", 2,
                ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            render_detector_form(app_config);
            ImGui::TableSetColumnIndex(1);
            render_capture_form(app_config);
            ImGui::EndTable();
        } else {
            render_detector_form(app_config);
            ImGui::Dummy(ImVec2(0.0f, 8.0f));
            render_capture_form(app_config);
        }
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        render_detector_tuning(app_config);
        ImGui::EndDisabled();
    }

    void render_detector_form(AppConfig& app_config) {
        begin_config_panel("detector_panel", "推理", 232.0f);
        if (begin_form("detector_form", 126.0f)) {
            form_row("模型路径");
            ImGui::InputText("##model_path", &app_config.detector.model_path);

            const char* backends[] = {
                "CUDA", "TensorRT", "DirectML", "CPU"};
            int backend = static_cast<int>(app_config.detector.backend);
            form_row("推理后端");
            if (ImGui::Combo(
                    "##backend", &backend, backends,
                    static_cast<int>(std::size(backends)))) {
                app_config.detector.backend =
                    static_cast<BackendType>(backend);
            }

            form_row("设备索引");
            ImGui::InputInt(
                "##device_id", &app_config.detector.device_id);
            const char* formats[] = {
                "Auto", "Channel first",
                "Anchor + objectness", "End to end"};
            int format =
                static_cast<int>(app_config.detector.output_format);
            form_row("输出契约");
            if (ImGui::Combo(
                    "##output_format", &format, formats,
                    static_cast<int>(std::size(formats)))) {
                app_config.detector.output_format =
                    static_cast<OutputFormat>(format);
            }
            form_row("FP16");
            toggle_switch(
                "##enable_fp16", &app_config.detector.enable_fp16);
            form_row("CUDA Graph");
            toggle_switch(
                "##enable_trt_cuda_graph",
                &app_config.detector.enable_trt_cuda_graph);
            ImGui::EndTable();
        }
        end_config_panel();
    }

    void render_detector_tuning(AppConfig& app_config) {
        begin_config_panel("tuning_panel", "检测参数", 124.0f);
        if (begin_form("tuning_form", 144.0f)) {
            form_row("检测阈值");
            slider_float_control(
                "conf_threshold",
                &app_config.detector.conf_threshold,
                0.01f, 1.0f, "%.2f");
            form_row("NMS 阈值");
            slider_float_control(
                "nms_threshold",
                &app_config.detector.nms_threshold,
                0.01f, 1.0f, "%.2f");
            form_row("最大候选数");
            ImGui::InputInt("##top_k", &app_config.detector.top_k);
            ImGui::EndTable();
        }
        end_config_panel();
    }

    void render_capture_form(AppConfig& app_config) {
        const float panel_height =
            app_config.capture.center_roi ? 232.0f : 304.0f;
        begin_config_panel("capture_panel", "画面", panel_height);
        if (begin_form("capture_form", 126.0f)) {
            form_row("适配器");
            ImGui::InputInt(
                "##adapter_index", &app_config.capture.adapter_index);
            form_row("显示输出");
            ImGui::InputInt(
                "##output_index", &app_config.capture.output_index);
            form_row("中心 ROI");
            toggle_switch(
                "##center_roi", &app_config.capture.center_roi);
            form_row("ROI 宽度");
            ImGui::InputInt(
                "##roi_width", &app_config.capture.roi_width);
            form_row("ROI 高度");
            ImGui::InputInt(
                "##roi_height", &app_config.capture.roi_height);
            if (!app_config.capture.center_roi) {
                form_row("ROI X");
                ImGui::InputInt("##roi_x", &app_config.capture.roi_x);
                form_row("ROI Y");
                ImGui::InputInt("##roi_y", &app_config.capture.roi_y);
            }
            form_row("超时 / ms");
            ImGui::InputInt(
                "##acquire_timeout_ms",
                &app_config.capture.acquire_timeout_ms);
            ImGui::EndTable();
        }
        end_config_panel();
    }

    void render_aim_config(AppConfig& app_config, bool can_edit) {
        ImGui::BeginDisabled(!can_edit);
        const bool two_columns =
            ImGui::GetContentRegionAvail().x >= 650.0f;
        if (two_columns && ImGui::BeginTable(
                "aim_columns", 2,
                ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            render_tracking_form(app_config);
            ImGui::TableSetColumnIndex(1);
            render_selection_form(app_config);
            ImGui::EndTable();
        } else {
            render_tracking_form(app_config);
            ImGui::Dummy(ImVec2(0.0f, 8.0f));
            render_selection_form(app_config);
        }
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        render_control_form(app_config);
        ImGui::EndDisabled();
    }

    void render_tracking_form(AppConfig& app_config) {
        begin_config_panel("tracking_panel", "轨迹确认", 232.0f);
        if (begin_form("tracking_form", 126.0f)) {
            form_row("高置信阈值");
            slider_float_control(
                "aim_high_confidence",
                &app_config.aim.high_confidence,
                0.01f, 1.0f, "%.2f");
            form_row("低置信阈值");
            slider_float_control(
                "aim_low_confidence",
                &app_config.aim.low_confidence,
                0.01f, 1.0f, "%.2f");
            form_row("确认帧数");
            ImGui::InputInt(
                "##min_confirmed_hits",
                &app_config.aim.min_confirmed_hits);
            form_row("最大丢失帧");
            ImGui::InputInt(
                "##max_lost_frames",
                &app_config.aim.max_lost_frames);
            form_row("最小 IoU");
            slider_float_control(
                "min_iou", &app_config.aim.min_iou,
                0.0f, 1.0f, "%.2f");
            form_row("中心距离");
            slider_float_control(
                "max_center_distance",
                &app_config.aim.max_center_distance,
                0.0f, 1.0f, "%.2f");
            ImGui::EndTable();
        }
        end_config_panel();
    }

    void render_selection_form(AppConfig& app_config) {
        begin_config_panel("selection_panel", "目标选择", 196.0f);
        if (begin_form("selection_form", 126.0f)) {
            form_row("切换优势");
            slider_float_control(
                "switch_margin", &app_config.aim.switch_margin,
                0.0f, 1.0f, "%.2f");
            form_row("切换确认帧");
            ImGui::InputInt(
                "##switch_confirm_frames",
                &app_config.aim.switch_confirm_frames);
            form_row("切换冷却帧");
            ImGui::InputInt(
                "##switch_cooldown_frames",
                &app_config.aim.switch_cooldown_frames);
            form_row("身体瞄准高度");
            slider_float_control(
                "body_aim_height_ratio",
                &app_config.aim.body_aim_height_ratio,
                0.0f, 1.0f, "%.2f");
            form_row("预测增益");
            slider_float_control(
                "predicted_gain", &app_config.aim.predicted_gain,
                0.0f, 1.0f, "%.2f");
            ImGui::EndTable();
        }
        end_config_panel();
    }

    void render_control_form(AppConfig& app_config) {
        begin_config_panel("control_panel", "移动控制", 196.0f);
        if (begin_form("control_form")) {
            form_row("死区");
            slider_float_control(
                "deadzone", &app_config.aim.deadzone_pixels,
                0.0f, 20.0f, "%.1f px");
            form_row("平滑系数");
            slider_float_control(
                "smoothing", &app_config.aim.smoothing,
                0.0f, 1.0f, "%.2f");
            form_row("水平 counts / px");
            slider_float_control(
                "counts_per_pixel_x",
                &app_config.aim.counts_per_pixel_x,
                0.01f, 4.0f, "%.2f");
            form_row("垂直 counts / px");
            slider_float_control(
                "counts_per_pixel_y",
                &app_config.aim.counts_per_pixel_y,
                0.01f, 4.0f, "%.2f");
            form_row("单帧最大 counts");
            slider_float_control(
                "max_counts",
                &app_config.aim.max_counts_per_frame,
                1.0f, 200.0f, "%.0f");
            ImGui::EndTable();
        }
        end_config_panel();
    }

    void safety_gate_column(const char* label,
                            const char* value,
                            const ImVec4& color) {
        ImGui::TextColored(rgba(kMutedInk), "%s", label);
        status_dot_label(value, color);
    }

    void render_input_config(const RuntimeSnapshot& snapshot,
                             AppConfig& app_config,
                             bool can_edit,
                             OverlayActions& actions) {
        begin_config_panel("safety_panel", "安全门", 72.0f);
        if (ImGui::BeginTable(
                "safety_grid", 3,
                ImGuiTableFlags_SizingStretchSame |
                ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            safety_gate_column(
                "配置门",
                snapshot.output_allowed_by_config ? "允许" : "禁止",
                snapshot.output_allowed_by_config
                    ? rgba(kSuccess) : rgba(kMutedInk));
            ImGui::TableSetColumnIndex(1);
            safety_gate_column(
                "运行门",
                snapshot.output_armed ? "已武装" : "未武装",
                snapshot.output_armed
                    ? rgba(kSuccess) : rgba(kWarning));
            ImGui::TableSetColumnIndex(2);
            safety_gate_column(
                "急停门",
                snapshot.emergency_stopped ? "已锁定" : "正常",
                snapshot.emergency_stopped
                    ? rgba(kDanger) : rgba(kSuccess));
            ImGui::EndTable();
        }
        end_config_panel();
        ImGui::Dummy(ImVec2(0.0f, 8.0f));

        if (snapshot.emergency_stopped) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, rgba(kDangerSoft));
            ImGui::PushStyleColor(ImGuiCol_Border, rgba(kDanger, 0.35f));
            ImGui::PushStyleVar(
                ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 10.0f));
            ImGui::BeginChild(
                "emergency_recovery", ImVec2(0.0f, 56.0f),
                ImGuiChildFlags_Borders);
            ImGui::TextColored(rgba(kDanger), "急停已锁定");
            ImGui::SetCursorPos(ImVec2(
                ImGui::GetWindowWidth() - 110.0f, 9.0f));
            ImGui::BeginDisabled(snapshot.aim_hold_active);
            if (ImGui::Button("复位急停", ImVec2(96.0f, 34.0f))) {
                actions.runtime_intents.push_back(
                    {RuntimeIntentType::RESET_EMERGENCY, false});
            }
            ImGui::EndDisabled();
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
            ImGui::Dummy(ImVec2(0.0f, 8.0f));
        }

        ImGui::BeginDisabled(!can_edit);
        const bool two_columns =
            ImGui::GetContentRegionAvail().x >= 650.0f;
        if (two_columns && ImGui::BeginTable(
                "input_columns", 2,
                ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            render_mouse_form(app_config);
            ImGui::TableSetColumnIndex(1);
            render_keyboard_form(app_config);
            ImGui::EndTable();
        } else {
            render_mouse_form(app_config);
            ImGui::Dummy(ImVec2(0.0f, 8.0f));
            render_keyboard_form(app_config);
        }
        ImGui::EndDisabled();
    }

    void render_mouse_form(AppConfig& app_config) {
        begin_config_panel("mouse_panel", "鼠标输出", 88.0f);
        if (begin_form("mouse_form", 126.0f)) {
            form_row("后端");
            ImGui::TextColored(rgba(kMutedInk), "Win32 SendInput");
            form_row("物理输出");
            toggle_switch(
                "##allow_send_input",
                &app_config.mouse.allow_send_input);
            ImGui::EndTable();
        }
        end_config_panel();
    }

    void render_keyboard_form(AppConfig& app_config) {
        begin_config_panel("keyboard_panel", "全局按键", 88.0f);
        if (begin_form("keyboard_form", 126.0f)) {
            form_row("按住启用 / VK");
            ImGui::InputInt(
                "##aim_hold_virtual_key",
                &app_config.keyboard.aim_hold_virtual_key);
            form_row("急停 / VK");
            ImGui::InputInt(
                "##emergency_virtual_key",
                &app_config.keyboard.emergency_virtual_key);
            ImGui::EndTable();
        }
        end_config_panel();
    }

    void render_settings(AppConfig& app_config, bool can_edit) {
        ImGui::BeginDisabled(!can_edit);
        const bool two_columns =
            ImGui::GetContentRegionAvail().x >= 650.0f;
        if (two_columns && ImGui::BeginTable(
                "settings_columns", 2,
                ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            render_runtime_settings(app_config);
            ImGui::TableSetColumnIndex(1);
            render_ui_settings(app_config);
            ImGui::EndTable();
        } else {
            render_runtime_settings(app_config);
            ImGui::Dummy(ImVec2(0.0f, 8.0f));
            render_ui_settings(app_config);
        }
        ImGui::EndDisabled();
    }

    void render_runtime_settings(AppConfig& app_config) {
        begin_config_panel("runtime_panel", "运行统计", 52.0f);
        if (begin_form("runtime_form", 126.0f)) {
            form_row("分位数窗口");
            slider_int_control(
                "profile_window",
                &app_config.runtime.profile_window,
                64, 4096);
            ImGui::EndTable();
        }
        end_config_panel();
    }

    void render_ui_settings(AppConfig& app_config) {
        begin_config_panel("ui_panel", "窗口与外观", 160.0f);
        if (begin_form("ui_form", 126.0f)) {
            form_row("外观主题");
            theme_selector("ui_theme", &app_config.ui.theme);
            form_row("宽度");
            ImGui::InputInt("##ui_width", &app_config.ui.width);
            form_row("高度");
            ImGui::InputInt("##ui_height", &app_config.ui.height);
            form_row("垂直同步");
            toggle_switch(
                "##enable_vsync", &app_config.ui.enable_vsync);
            ImGui::EndTable();
        }
        end_config_panel();
    }

    void render_workspace(const RuntimeSnapshot& snapshot,
                          AppConfig& app_config,
                          const std::string& app_message,
                          OverlayActions& actions) {
        const bool can_edit = editable(snapshot);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, rgba(kSurface));
        ImGui::PushStyleVar(
            ImGuiStyleVar_WindowPadding, ImVec2(28.0f, 16.0f));
        ImGui::BeginChild(
            "content", ImVec2(0.0f, 0.0f),
            ImGuiChildFlags_None);

        render_page_heading(can_edit, actions);
        render_notice(
            "app_notice", app_message, kAccentStrong, kAccentSoft);
        render_notice(
            "runtime_error", snapshot.last_error, kDanger, kDangerSoft);

        switch (active_page) {
            case WorkspacePage::OVERVIEW:
                render_overview(snapshot);
                break;
            case WorkspacePage::DETECTION:
                render_detection_config(app_config, can_edit);
                break;
            case WorkspacePage::AIM:
                render_aim_config(app_config, can_edit);
                break;
            case WorkspacePage::INPUT:
                render_input_config(
                    snapshot, app_config, can_edit, actions);
                break;
            case WorkspacePage::SETTINGS:
                render_settings(app_config, can_edit);
                break;
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }
};

Overlay::Overlay() : impl_(std::make_unique<Impl>()) {}
Overlay::~Overlay() { shutdown(); }

bool Overlay::init(const UiConfig& config) noexcept {
    if (!impl_) return false;
    try {
        Log::register_module("overlay", LogLevel::INFO);
        impl_->config = config;
        ImGui_ImplWin32_EnableDpiAwareness();
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW window_class{
            sizeof(WNDCLASSEXW), CS_CLASSDC, Impl::window_proc, 0L, 0L,
            instance, nullptr, nullptr, nullptr, nullptr,
            L"XenCodexOverlay", nullptr};
        RegisterClassExW(&window_class);
        impl_->window = CreateWindowExW(
            WS_EX_APPWINDOW,
            window_class.lpszClassName, L"Xen Precision Runtime",
            WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX |
                WS_MAXIMIZEBOX | WS_SYSMENU,
            CW_USEDEFAULT, CW_USEDEFAULT, config.width, config.height,
            nullptr, nullptr, instance, impl_.get());
        if (!impl_->window) return false;
        impl_->large_icon = static_cast<HICON>(LoadImageW(
            instance, MAKEINTRESOURCEW(kAppIconResourceId), IMAGE_ICON,
            32, 32, LR_DEFAULTCOLOR));
        impl_->small_icon = static_cast<HICON>(LoadImageW(
            instance, MAKEINTRESOURCEW(kAppIconResourceId), IMAGE_ICON,
            16, 16, LR_DEFAULTCOLOR));
        if (impl_->large_icon) {
            SendMessageW(
                impl_->window, WM_SETICON, ICON_BIG,
                reinterpret_cast<LPARAM>(impl_->large_icon));
        }
        if (impl_->small_icon) {
            SendMessageW(
                impl_->window, WM_SETICON, ICON_SMALL,
                reinterpret_cast<LPARAM>(impl_->small_icon));
        }
        apply_window_theme(impl_->window, config.theme);
        if (!impl_->create_device()) return false;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename = nullptr;
        impl_->body_font = io.Fonts->AddFontFromFileTTF(
            "C:\\Windows\\Fonts\\msyh.ttc", 15.0f, nullptr,
            io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        if (!impl_->body_font) {
            impl_->body_font = io.Fonts->AddFontDefault();
        }
        impl_->small_font = io.Fonts->AddFontFromFileTTF(
            "C:\\Windows\\Fonts\\msyh.ttc", 12.0f, nullptr,
            io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        if (!impl_->small_font) {
            impl_->small_font = impl_->body_font;
        }
        impl_->medium_font = io.Fonts->AddFontFromFileTTF(
            "C:\\Windows\\Fonts\\msyhbd.ttc", 15.0f, nullptr,
            io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        if (!impl_->medium_font) {
            impl_->medium_font = impl_->body_font;
        }
        impl_->title_font = io.Fonts->AddFontFromFileTTF(
            "C:\\Windows\\Fonts\\msyhbd.ttc", 20.0f, nullptr,
            io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        if (!impl_->title_font) {
            impl_->title_font = impl_->medium_font;
        }
        io.FontDefault = impl_->body_font;
        apply_codex_theme(config.theme);
        impl_->applied_theme = config.theme;
        if (!ImGui_ImplWin32_Init(impl_->window) ||
            !ImGui_ImplDX11_Init(
                impl_->device.Get(), impl_->context.Get())) {
            return false;
        }
        ShowWindow(impl_->window, SW_SHOWNORMAL);
        UpdateWindow(impl_->window);
        impl_->initialized = true;
        LOG_INFO("overlay", "Codex 双主题控制台 Overlay 已初始化");
        return true;
    } catch (...) {
        return false;
    }
}

bool Overlay::pump_messages() noexcept {
    if (!impl_ || !impl_->initialized) return false;
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
        if (message.message == WM_QUIT) {
            impl_->close_requested = true;
        }
    }
    return !impl_->close_requested;
}

bool Overlay::render(const RuntimeSnapshot& snapshot,
                     AppConfig& config,
                     const std::string& app_message,
                     OverlayActions& actions) noexcept {
    if (!impl_ || !impl_->initialized) return false;
    try {
        actions = {};
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        if (impl_->applied_theme != config.ui.theme) {
            apply_codex_theme(config.ui.theme);
            apply_window_theme(impl_->window, config.ui.theme);
            impl_->applied_theme = config.ui.theme;
        }
        ImGui::NewFrame();

        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(io.DisplaySize);
        constexpr ImGuiWindowFlags kWindowFlags =
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse;
        ImGui::Begin("XenRoot", nullptr, kWindowFlags);

        impl_->render_title_bar();
        ImGui::SetCursorPos(ImVec2(0.0f, kWindowTitleBarHeight));
        impl_->render_sidebar(snapshot);
        ImGui::SetCursorPos(ImVec2(
            kSidebarWidth + kWorkspaceInset,
            kWindowTitleBarHeight + kWorkspaceInset));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, rgba(kSurface));
        ImGui::PushStyleColor(ImGuiCol_Border, rgba(kBorder));
        ImGui::PushStyleVar(
            ImGuiStyleVar_ChildRounding, kPanelRounding);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
        ImGui::BeginChild(
            "workspace",
            ImVec2(-kWorkspaceInset, -kWorkspaceInset),
            ImGuiChildFlags_Borders,
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse);
        impl_->render_global_bar(snapshot, actions);
        impl_->render_workspace(snapshot, config, app_message, actions);
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
        ImGui::End();

        ImGui::Render();
        const ImVec4 clear_color = rgba(kCanvas);
        const float kClearColor[4] = {
            clear_color.x, clear_color.y,
            clear_color.z, clear_color.w};
        impl_->context->OMSetRenderTargets(
            1, impl_->render_target.GetAddressOf(), nullptr);
        impl_->context->ClearRenderTargetView(
            impl_->render_target.Get(), kClearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        impl_->swap_chain->Present(
            config.ui.enable_vsync ? 1 : 0, 0);
        return true;
    } catch (...) {
        return false;
    }
}

void Overlay::shutdown() noexcept {
    if (!impl_) return;
    if (impl_->initialized) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    impl_->initialized = false;
    impl_->destroy_render_target();
    impl_->swap_chain.Reset();
    impl_->context.Reset();
    impl_->device.Reset();
    if (impl_->window) {
        DestroyWindow(impl_->window);
        impl_->window = nullptr;
    }
    if (impl_->large_icon) {
        DestroyIcon(impl_->large_icon);
        impl_->large_icon = nullptr;
    }
    if (impl_->small_icon) {
        DestroyIcon(impl_->small_icon);
        impl_->small_icon = nullptr;
    }
    UnregisterClassW(
        L"XenCodexOverlay", GetModuleHandleW(nullptr));
}
