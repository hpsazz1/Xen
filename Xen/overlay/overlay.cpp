#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "overlay/overlay.h"
#include "overlay/overlay_internal.h"

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
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <sstream>
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
constexpr float kWindowTitleBarHeight = 36.0f;
constexpr float kTitleButtonWidth = 44.0f;
constexpr int kResizeBorder = 6;
constexpr std::size_t kMetricHistoryCapacity = 256;
// 与 Xen/app/xen.rc 保持一致，用于标题栏和任务栏图标。
constexpr int kAppIconResourceId = 101;
constexpr wchar_t kMainWindowClass[] = L"XenCodexOverlay";
constexpr wchar_t kDetachedPreviewWindowClass[] =
    L"XenDetachedDetectionPreview";
constexpr int kDetachedPreviewMinimumWidth = 320;
constexpr int kDetachedPreviewMinimumHeight = 280;

using MetricHistory =
    overlay::detail::MetricHistory<kMetricHistoryCapacity>;

enum class WorkspacePage {
    OVERVIEW,
    DETECTION,
    AIM,
    INPUT,
    SETTINGS,
};

enum class HotkeyBindingTarget {
    NONE,
    RUNTIME_TOGGLE,
    AIM_HOLD,
    EMERGENCY,
};

std::array<bool, 256> current_virtual_key_state() noexcept {
    std::array<bool, 256> result{};
    for (int virtual_key = 1; virtual_key <= 0xFF; ++virtual_key) {
        result[static_cast<std::size_t>(virtual_key)] =
            (GetAsyncKeyState(virtual_key) & 0x8000) != 0;
    }
    return result;
}

std::string virtual_key_name(int virtual_key) {
    switch (virtual_key) {
        case VK_LBUTTON: return "鼠标左键";
        case VK_RBUTTON: return "鼠标右键";
        case VK_MBUTTON: return "鼠标中键";
        case VK_XBUTTON1: return "鼠标侧键 1";
        case VK_XBUTTON2: return "鼠标侧键 2";
        case VK_ESCAPE: return "Esc";
        default: break;
    }
    UINT scan_code = MapVirtualKeyW(
        static_cast<UINT>(virtual_key), MAPVK_VK_TO_VSC);
    if (virtual_key == VK_LEFT || virtual_key == VK_UP ||
        virtual_key == VK_RIGHT || virtual_key == VK_DOWN ||
        virtual_key == VK_PRIOR || virtual_key == VK_NEXT ||
        virtual_key == VK_END || virtual_key == VK_HOME ||
        virtual_key == VK_INSERT || virtual_key == VK_DELETE ||
        virtual_key == VK_DIVIDE || virtual_key == VK_NUMLOCK) {
        scan_code |= 0xE000U;
    }
    wchar_t name[64]{};
    const LONG key_data = static_cast<LONG>(scan_code << 16U);
    if (GetKeyNameTextW(
            key_data, name, static_cast<int>(std::size(name))) > 0) {
        const int utf8_size = WideCharToMultiByte(
            CP_UTF8, 0, name, -1, nullptr, 0, nullptr, nullptr);
        if (utf8_size > 1) {
            std::string utf8(static_cast<std::size_t>(utf8_size), '\0');
            WideCharToMultiByte(
                CP_UTF8, 0, name, -1, utf8.data(), utf8_size,
                nullptr, nullptr);
            utf8.pop_back();
            return utf8;
        }
    }
    return "VK " + std::to_string(virtual_key);
}

std::string format_hotkey_binding(const std::vector<int>& virtual_keys) {
    if (virtual_keys.empty()) return "未绑定";
    std::ostringstream stream;
    for (std::size_t index = 0; index < virtual_keys.size(); ++index) {
        if (index > 0) stream << " | ";
        stream << virtual_key_name(virtual_keys[index]);
    }
    return stream.str();
}

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

const char* detector_reload_label(DetectorReloadState state) noexcept {
    switch (state) {
        case DetectorReloadState::IDLE: return "待命";
        case DetectorReloadState::LOADING: return "加载中";
        case DetectorReloadState::SUCCEEDED: return "已切换";
        case DetectorReloadState::FAILED: return "加载失败";
    }
    return "未知";
}

ImVec4 detector_reload_color(DetectorReloadState state) noexcept {
    switch (state) {
        case DetectorReloadState::SUCCEEDED: return rgba(kSuccess);
        case DetectorReloadState::LOADING: return rgba(kWarning);
        case DetectorReloadState::FAILED: return rgba(kDanger);
        case DetectorReloadState::IDLE: return rgba(kMutedInk);
    }
    return rgba(kMutedInk);
}

ImVec4 aim_color(AimStatus status) noexcept {
    if (status == AimStatus::SUCCESS) return rgba(kSuccess);
    if (status == AimStatus::NOT_RUN) return rgba(kMutedInk);
    return rgba(kDanger);
}

const char* track_state_label(TrackState state) noexcept {
    switch (state) {
        case TrackState::TENTATIVE: return "待确认";
        case TrackState::CONFIRMED: return "已确认";
        case TrackState::LOST: return "短时丢失";
    }
    return "未知";
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

void show_help_tooltip(const char* help) {
    if (!help || *help == '\0' ||
        !ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        return;
    }
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
    ImGui::TextUnformatted(help);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

void help_marker(const char* help) {
    ImGui::SameLine(0.0f, 5.0f);
    ImGui::TextColored(rgba(kFaintInk), "?");
    show_help_tooltip(help);
}

void form_row(const char* label, const char* help) {
    ImGui::TableNextRow(ImGuiTableRowFlags_None, 36.0f);
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(rgba(kMutedInk), "%s", label);
    help_marker(help);
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
    HWND detached_preview_window = nullptr;
    HDC detached_memory_dc = nullptr;
    HBITMAP detached_memory_bitmap = nullptr;
    HGDIOBJ detached_memory_previous_bitmap = nullptr;
    int detached_memory_width = 0;
    int detached_memory_height = 0;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swap_chain;
    ComPtr<ID3D11RenderTargetView> render_target;
    ComPtr<ID3D11Texture2D> preview_texture;
    ComPtr<ID3D11ShaderResourceView> preview_srv;
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
    bool show_log_panel = false;
    bool preview_requested = false;
    bool detached_preview_requested = false;
    std::shared_ptr<const RuntimePreviewFrame> detached_preview_frame;
    // 独立窗口的 WM_PAINT 不接收 AppConfig，因此缓存每帧 UI 已确认的类别映射。
    // 仅在配置变化时更新，避免预览热路径反复复制动态数组。
    std::vector<int> person_class_ids{0};
    std::vector<int> head_class_ids{1};
    int preview_texture_width = 0;
    int preview_texture_height = 0;
    std::uint64_t preview_uploaded_sequence = 0;
    std::string last_log_tail;
    MetricHistory capture_fps_history;
    MetricHistory total_latency_history;
    MetricHistory target_confidence_history;
    std::uint64_t history_sequence = 0;
    bool history_runtime_active = false;
    HotkeyBindingTarget hotkey_binding_target = HotkeyBindingTarget::NONE;
    overlay::detail::HotkeyCaptureState hotkey_capture_state;
    std::string hotkey_capture_message;

    static LRESULT CALLBACK window_proc(
            HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<Impl*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(self));
        }
        if (message == WM_KEYDOWN && wparam == VK_F9 &&
            (lparam & (1LL << 30)) == 0 && self) {
            self->show_log_panel = !self->show_log_panel;
            if (self->show_log_panel) self->last_log_tail.clear();
            return 0;
        }
        if (ImGui_ImplWin32_WndProcHandler(hwnd, message, wparam, lparam)) {
            return TRUE;
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
                minmax->ptMinTrackSize.x = kMinimumUiWidth;
                minmax->ptMinTrackSize.y = kMinimumUiHeight;
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

    static LRESULT CALLBACK detached_preview_window_proc(
            HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<Impl*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(
                hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        switch (message) {
            case WM_PAINT:
                if (self) {
                    self->paint_detached_preview(hwnd);
                    return 0;
                }
                break;
            case WM_ERASEBKGND:
                // WM_PAINT 会覆盖完整客户区，避免 10 FPS 更新时背景闪烁。
                return 1;
            case WM_GETMINMAXINFO: {
                auto* minmax = reinterpret_cast<MINMAXINFO*>(lparam);
                minmax->ptMinTrackSize.x = kDetachedPreviewMinimumWidth;
                minmax->ptMinTrackSize.y = kDetachedPreviewMinimumHeight;
                return 0;
            }
            case WM_CLOSE:
                if (self) {
                    self->detached_preview_requested = false;
                    self->detached_preview_frame.reset();
                    self->release_detached_paint_buffer();
                }
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            case WM_DESTROY:
                if (self && self->detached_preview_window == hwnd) {
                    self->detached_preview_window = nullptr;
                }
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

    void release_preview_texture() noexcept {
        preview_srv.Reset();
        preview_texture.Reset();
        preview_texture_width = 0;
        preview_texture_height = 0;
        preview_uploaded_sequence = 0;
    }

    bool ensure_preview_texture(int width, int height) noexcept {
        if (width <= 0 || height <= 0 || !device || !context) return false;
        if (preview_texture && preview_srv &&
            preview_texture_width == width &&
            preview_texture_height == height) {
            return true;
        }

        release_preview_texture();
        D3D11_TEXTURE2D_DESC texture_desc{};
        texture_desc.Width = static_cast<UINT>(width);
        texture_desc.Height = static_cast<UINT>(height);
        texture_desc.MipLevels = 1;
        texture_desc.ArraySize = 1;
        texture_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        texture_desc.SampleDesc.Count = 1;
        texture_desc.Usage = D3D11_USAGE_DYNAMIC;
        texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateTexture2D(
                &texture_desc, nullptr, &preview_texture)) ||
            FAILED(device->CreateShaderResourceView(
                preview_texture.Get(), nullptr, &preview_srv))) {
            release_preview_texture();
            return false;
        }
        preview_texture_width = width;
        preview_texture_height = height;
        return true;
    }

    bool upload_preview(
            const std::shared_ptr<const RuntimePreviewFrame>& preview) noexcept {
        if (!preview || preview->sequence == 0 || preview->width <= 0 ||
            preview->height <= 0 ||
            preview->width > kRuntimePreviewMaxDimension ||
            preview->height > kRuntimePreviewMaxDimension) {
            return false;
        }
        const std::size_t row_bytes =
            static_cast<std::size_t>(preview->width) * 4;
        const std::size_t required_bytes =
            row_bytes * static_cast<std::size_t>(preview->height);
        if (preview->bgra.size() < required_bytes ||
            !ensure_preview_texture(preview->width, preview->height)) {
            return false;
        }
        if (preview_uploaded_sequence == preview->sequence) return true;

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(
                preview_texture.Get(), 0, D3D11_MAP_WRITE_DISCARD,
                0, &mapped))) {
            return false;
        }
        if (!mapped.pData || mapped.RowPitch < row_bytes) {
            context->Unmap(preview_texture.Get(), 0);
            return false;
        }
        const auto* source = preview->bgra.data();
        auto* destination = static_cast<std::uint8_t*>(mapped.pData);
        for (int row = 0; row < preview->height; ++row) {
            std::memcpy(
                destination + static_cast<std::size_t>(row) * mapped.RowPitch,
                source + static_cast<std::size_t>(row) * row_bytes,
                row_bytes);
        }
        context->Unmap(preview_texture.Get(), 0);
        preview_uploaded_sequence = preview->sequence;
        return true;
    }

    bool ensure_detached_preview_window() noexcept {
        if (detached_preview_window) return true;
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        detached_preview_window = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            kDetachedPreviewWindowClass, L"Xen 实时检测",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
            CW_USEDEFAULT, CW_USEDEFAULT, 560, 620,
            nullptr, nullptr, instance, this);
        if (!detached_preview_window) return false;
        if (large_icon) {
            SendMessageW(
                detached_preview_window, WM_SETICON, ICON_BIG,
                reinterpret_cast<LPARAM>(large_icon));
        }
        if (small_icon) {
            SendMessageW(
                detached_preview_window, WM_SETICON, ICON_SMALL,
                reinterpret_cast<LPARAM>(small_icon));
        }
        apply_window_theme(detached_preview_window, applied_theme);
        return true;
    }

    void sync_detached_preview(
            const std::shared_ptr<const RuntimePreviewFrame>& preview) noexcept {
        if (!detached_preview_requested) {
            detached_preview_frame.reset();
            if (detached_preview_window) {
                ShowWindow(detached_preview_window, SW_HIDE);
            }
            return;
        }
        if (!ensure_detached_preview_window()) {
            detached_preview_requested = false;
            detached_preview_frame.reset();
            LOG_ERROR("overlay", "独立置顶检测窗口创建失败");
            return;
        }

        const bool preview_changed =
            overlay::detail::detached_preview_content_changed(
                static_cast<bool>(detached_preview_frame),
                detached_preview_frame
                    ? detached_preview_frame->sequence
                    : 0,
                static_cast<bool>(preview),
                preview ? preview->sequence : 0);
        detached_preview_frame = preview;
        const bool was_visible = IsWindowVisible(detached_preview_window) != FALSE;
        if (!was_visible) {
            // WS_EX_TOPMOST 提供持续置顶语义；只在首次显示时调用 SetWindowPos，
            // 避免主 UI 高频循环反复改变 Z 序和触发非必要窗口工作。
            SetWindowPos(
                detached_preview_window, HWND_TOPMOST,
                0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                    SWP_SHOWWINDOW);
        }
        if (!was_visible || preview_changed) {
            InvalidateRect(detached_preview_window, nullptr, FALSE);
        }
    }

    void release_detached_paint_buffer() noexcept {
        if (detached_memory_dc && detached_memory_previous_bitmap) {
            SelectObject(
                detached_memory_dc, detached_memory_previous_bitmap);
        }
        if (detached_memory_bitmap) {
            DeleteObject(detached_memory_bitmap);
        }
        if (detached_memory_dc) DeleteDC(detached_memory_dc);
        detached_memory_dc = nullptr;
        detached_memory_bitmap = nullptr;
        detached_memory_previous_bitmap = nullptr;
        detached_memory_width = 0;
        detached_memory_height = 0;
    }

    bool ensure_detached_paint_buffer(
            HDC window_dc, int width, int height) noexcept {
        if (!window_dc || width <= 0 || height <= 0) return false;
        if (detached_memory_dc && detached_memory_bitmap &&
            detached_memory_width == width &&
            detached_memory_height == height) {
            return true;
        }
        release_detached_paint_buffer();
        detached_memory_dc = CreateCompatibleDC(window_dc);
        detached_memory_bitmap = detached_memory_dc
            ? CreateCompatibleBitmap(window_dc, width, height)
            : nullptr;
        if (!detached_memory_dc || !detached_memory_bitmap) {
            release_detached_paint_buffer();
            return false;
        }
        detached_memory_previous_bitmap = SelectObject(
            detached_memory_dc, detached_memory_bitmap);
        if (!detached_memory_previous_bitmap) {
            release_detached_paint_buffer();
            return false;
        }
        detached_memory_width = width;
        detached_memory_height = height;
        return true;
    }

    void paint_detached_preview(HWND hwnd) noexcept {
        PAINTSTRUCT paint{};
        HDC window_dc = BeginPaint(hwnd, &paint);
        if (!window_dc) return;

        RECT client{};
        GetClientRect(hwnd, &client);
        const int client_width = std::max(0L, client.right - client.left);
        const int client_height = std::max(0L, client.bottom - client.top);
        const bool double_buffered = ensure_detached_paint_buffer(
            window_dc, client_width, client_height);
        HDC dc = double_buffered ? detached_memory_dc : window_dc;
        const auto finish_paint = [&]() noexcept {
            if (double_buffered) {
                BitBlt(
                    window_dc, 0, 0,
                    client_width, client_height,
                    detached_memory_dc, 0, 0, SRCCOPY);
            }
            EndPaint(hwnd, &paint);
        };
        HBRUSH background = CreateSolidBrush(RGB(16, 16, 16));
        FillRect(dc, &client, background);
        DeleteObject(background);

        const auto& preview = detached_preview_frame;
        const std::size_t required_bytes = preview
            ? static_cast<std::size_t>(std::max(0, preview->width)) *
                  static_cast<std::size_t>(std::max(0, preview->height)) * 4
            : 0;
        const bool has_image = preview && preview->sequence != 0 &&
            preview->width > 0 && preview->height > 0 &&
            preview->bgra.size() >= required_bytes;
        constexpr int kPadding = 10;
        constexpr int kStatusHeight = 30;
        if (!has_image) {
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(180, 184, 190));
            RECT message_rect{
                kPadding, kPadding,
                std::max(kPadding, client_width - kPadding),
                std::max(kPadding, client_height - kPadding)};
            DrawTextW(
                dc, L"等待实时检测画面",
                -1, &message_rect,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            finish_paint();
            return;
        }

        const float maximum_width = static_cast<float>(
            std::max(1, client_width - kPadding * 2));
        const float maximum_height = static_cast<float>(
            std::max(1, client_height - kPadding * 2 - kStatusHeight));
        const auto display = overlay::detail::fit_detached_preview_size(
            preview->width, preview->height,
            maximum_width, maximum_height);
        const int display_width = std::max(
            1, static_cast<int>(std::lround(display.width)));
        const int display_height = std::max(
            1, static_cast<int>(std::lround(display.height)));
        const int image_x = (client_width - display_width) / 2;
        const int image_y = kPadding +
            (static_cast<int>(maximum_height) - display_height) / 2;

        BITMAPINFO preview_bitmap{};
        preview_bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        preview_bitmap.bmiHeader.biWidth = preview->width;
        preview_bitmap.bmiHeader.biHeight = -preview->height;
        preview_bitmap.bmiHeader.biPlanes = 1;
        preview_bitmap.bmiHeader.biBitCount = 32;
        preview_bitmap.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchDIBits(
            dc, image_x, image_y, display_width, display_height,
            0, 0, preview->width, preview->height,
            preview->bgra.data(), &preview_bitmap,
            DIB_RGB_COLORS, SRCCOPY);

        const float coordinate_scale_x = preview->scale_x *
            display.width / static_cast<float>(preview->width);
        const float coordinate_scale_y = preview->scale_y *
            display.height / static_cast<float>(preview->height);
        HPEN person_pen = CreatePen(PS_SOLID, 2, RGB(66, 190, 195));
        HPEN head_pen = CreatePen(PS_SOLID, 2, RGB(245, 185, 66));
        HPEN other_pen = CreatePen(PS_SOLID, 2, RGB(51, 156, 255));
        HGDIOBJ old_pen = SelectObject(dc, other_pen);
        HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        SetBkMode(dc, TRANSPARENT);
        const std::size_t detection_count = std::min(
            preview->detection_count, preview->detections.size());
        for (std::size_t index = 0; index < detection_count; ++index) {
            const Detection& detection = preview->detections[index];
            const auto role = overlay::detail::classify_detection_role(
                detection.class_id, person_class_ids, head_class_ids);
            SelectObject(
                dc, role == overlay::detail::DetectionRole::HEAD
                        ? head_pen
                        : role == overlay::detail::DetectionRole::PERSON
                            ? person_pen
                            : other_pen);
            const auto rectangle = overlay::detail::map_preview_rect(
                detection.x1, detection.y1, detection.x2, detection.y2,
                coordinate_scale_x, coordinate_scale_y,
                display.width, display.height);
            if (rectangle.x2 - rectangle.x1 < 1.0f ||
                rectangle.y2 - rectangle.y1 < 1.0f) {
                continue;
            }
            const int left = image_x +
                static_cast<int>(std::lround(rectangle.x1));
            const int top = image_y +
                static_cast<int>(std::lround(rectangle.y1));
            const int right = image_x +
                static_cast<int>(std::lround(rectangle.x2));
            const int bottom = image_y +
                static_cast<int>(std::lround(rectangle.y2));
            Rectangle(dc, left, top, right, bottom);
        }

        // 重叠框的顶部标签会相互覆盖。固定信息区最多列出 8 项，保留一位小数，
        // 让人工验收能够同时读取身体和头部置信度而不改变框的几何位置。
        constexpr std::size_t kMaximumLabelRows = 8;
        const std::size_t label_count = std::min(
            detection_count, kMaximumLabelRows);
        if (label_count > 0) {
            constexpr int kLabelRowHeight = 20;
            constexpr int kLabelWidth = 152;
            RECT panel{
                image_x + 6, image_y + 6,
                std::min(image_x + display_width - 6,
                         image_x + 6 + kLabelWidth),
                std::min(image_y + display_height - 6,
                         image_y + 10 +
                             static_cast<int>(label_count) * kLabelRowHeight)};
            HBRUSH panel_brush = CreateSolidBrush(RGB(16, 16, 16));
            FillRect(dc, &panel, panel_brush);
            DeleteObject(panel_brush);
            for (std::size_t index = 0; index < label_count; ++index) {
                const Detection& detection = preview->detections[index];
                const auto role = overlay::detail::classify_detection_role(
                    detection.class_id, person_class_ids, head_class_ids);
                const wchar_t* role_name =
                    role == overlay::detail::DetectionRole::HEAD
                        ? L"头部"
                        : role == overlay::detail::DetectionRole::PERSON
                            ? L"身体"
                            : L"类别";
                SetTextColor(
                    dc, role == overlay::detail::DetectionRole::HEAD
                            ? RGB(245, 185, 66)
                            : role == overlay::detail::DetectionRole::PERSON
                                ? RGB(66, 190, 195)
                                : RGB(51, 156, 255));
                wchar_t label[48]{};
                swprintf_s(
                    label, L"%s C%d  %.1f%%", role_name,
                    detection.class_id,
                    std::clamp(detection.confidence, 0.0f, 1.0f) * 100.0f);
                TextOutW(
                    dc, panel.left + 6,
                    panel.top + 2 + static_cast<int>(index) * kLabelRowHeight,
                    label, static_cast<int>(wcslen(label)));
            }
        }

        if (preview->has_target) {
            const AimTargetSnapshot& target = preview->target;
            const auto target_rect = overlay::detail::map_preview_rect(
                target.x1, target.y1, target.x2, target.y2,
                coordinate_scale_x, coordinate_scale_y,
                display.width, display.height);
            HPEN target_pen = CreatePen(
                PS_SOLID, 3,
                target.predicted ? RGB(240, 173, 78) : RGB(64, 201, 119));
            SelectObject(dc, target_pen);
            Rectangle(
                dc,
                image_x + static_cast<int>(std::lround(target_rect.x1)),
                image_y + static_cast<int>(std::lround(target_rect.y1)),
                image_x + static_cast<int>(std::lround(target_rect.x2)),
                image_y + static_cast<int>(std::lround(target_rect.y2)));
            const auto aim_point = overlay::detail::map_preview_point(
                target.aim_x, target.aim_y,
                coordinate_scale_x, coordinate_scale_y);
            const int aim_x = image_x +
                static_cast<int>(std::lround(aim_point.x));
            const int aim_y = image_y +
                static_cast<int>(std::lround(aim_point.y));
            MoveToEx(dc, aim_x - 7, aim_y, nullptr);
            LineTo(dc, aim_x + 8, aim_y);
            MoveToEx(dc, aim_x, aim_y - 7, nullptr);
            LineTo(dc, aim_x, aim_y + 8);
            SelectObject(dc, other_pen);
            DeleteObject(target_pen);
        }

        SelectObject(dc, old_brush);
        SelectObject(dc, old_pen);
        DeleteObject(person_pen);
        DeleteObject(head_pen);
        DeleteObject(other_pen);
        wchar_t status[128]{};
        swprintf_s(
            status, L"帧 %llu    检测 %zu    Detector %S",
            static_cast<unsigned long long>(preview->sequence),
            detection_count, DetectionStatusName(preview->detection_status));
        SetTextColor(dc, RGB(190, 194, 200));
        TextOutW(
            dc, kPadding, client_height - kStatusHeight + 7,
            status, static_cast<int>(wcslen(status)));
        finish_paint();
    }

    bool editable(const RuntimeSnapshot& snapshot) const noexcept {
        return snapshot.state != RuntimeState::RUNNING &&
               snapshot.state != RuntimeState::STARTING &&
               snapshot.state != RuntimeState::STOPPING;
    }

    void clear_metric_history() noexcept {
        capture_fps_history.clear();
        total_latency_history.clear();
        target_confidence_history.clear();
        history_sequence = 0;
    }

    void update_metric_history(const RuntimeSnapshot& snapshot) noexcept {
        const bool runtime_active =
            snapshot.state == RuntimeState::STARTING ||
            snapshot.state == RuntimeState::RUNNING;
        if (runtime_active && !history_runtime_active) {
            clear_metric_history();
        }
        history_runtime_active = runtime_active;
        if (!runtime_active || snapshot.last_sequence == 0 ||
            snapshot.last_sequence == history_sequence) {
            return;
        }
        if (history_sequence != 0 &&
            snapshot.last_sequence < history_sequence) {
            clear_metric_history();
        }
        history_sequence = snapshot.last_sequence;

        // 与正式报告保持一致：失败帧不进入成功样本曲线，避免把错误折叠为零耗时。
        if (snapshot.detection_status != DetectionStatus::SUCCESS ||
            snapshot.aim_status != AimStatus::SUCCESS) {
            return;
        }
        const auto nonnegative = [](double value) noexcept {
            return std::isfinite(value) && value > 0.0
                ? static_cast<float>(value)
                : 0.0f;
        };
        capture_fps_history.push(nonnegative(snapshot.capture_fps));
        total_latency_history.push(
            nonnegative(snapshot.last_profile.total_ms));
        const float confidence = snapshot.last_aim.has_target &&
                                 std::isfinite(
                                     snapshot.last_aim.target.confidence)
            ? std::clamp(snapshot.last_aim.target.confidence, 0.0f, 1.0f)
            : 0.0f;
        target_confidence_history.push(confidence);
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
                                      bool danger,
                                      const char* help) {
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
            show_help_tooltip(help);
            return pressed;
        };

        if (title_button("minimize", 0, false, "最小化 Xen 主控制台。")) {
            ShowWindow(window, SW_MINIMIZE);
        }
        if (title_button(
                "maximize", 1, false,
                "最大化窗口；已最大化时恢复到先前尺寸。")) {
            ShowWindow(window, IsZoomed(window) ? SW_RESTORE : SW_MAXIMIZE);
        }
        if (title_button(
                "close", 2, true,
                "关闭 Xen；退出前会停止 Runtime、收口报告并解除物理输出。")) {
            close_requested = true;
        }
    }

    bool nav_item(const char* label,
                  WorkspacePage page,
                  const char* help) {
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
        show_help_tooltip(help);

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

        nav_item(
            "概览", WorkspacePage::OVERVIEW,
            "查看 Runtime 状态、分段延迟、模块健康、帧统计和最近成功样本历史。");
        nav_item(
            "检测", WorkspacePage::DETECTION,
            "配置模型、推理后端、采集来源与检测阈值，并查看同帧 ROI 检测预览。");
        nav_item(
            "瞄准", WorkspacePage::AIM,
            "配置轨迹确认、目标切换、瞄点位置和相对鼠标移动控制参数。");
        nav_item(
            "输入", WorkspacePage::INPUT,
            "配置物理鼠标后端、安全门和全局快捷键，并在急停后执行受控复位。");
        nav_item(
            "设置", WorkspacePage::SETTINGS,
            "配置统计窗口、控制台尺寸、主题和垂直同步；不改变推理热路径。");

        const float footer_y = ImGui::GetWindowHeight() - 72.0f;
        if (ImGui::GetCursorPosY() < footer_y) {
            ImGui::SetCursorPosY(footer_y);
        }
        ImGui::SetCursorPosX(16.0f);
        ImGui::PushFont(medium_font);
        status_dot_label(
            snapshot.output_armed ? "输出已武装" : "输出未武装",
            snapshot.output_armed ? rgba(kSuccess) : rgba(kMutedInk));
        ImGui::PopFont();
        ImGui::SetCursorPosX(16.0f);
        ImGui::PushFont(small_font);
        ImGui::TextColored(rgba(kFaintInk), "Runtime P0  /  v0.1");
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
            ImGuiChildFlags_AlwaysUseWindowPadding,
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
            show_help_tooltip(
                "停止整个 Runtime 管线并结束当前 Debug 报告；物理输出会立即解除。快捷键默认为 F8。");
        } else {
            ImGui::BeginDisabled(snapshot.state == RuntimeState::STOPPING);
            push_primary_button();
            if (ImGui::Button("启动", ImVec2(kButtonWidth, 32.0f))) {
                actions.start_requested = true;
            }
            show_help_tooltip(
                "使用当前已保存/编辑配置启动 Capture、Detector、Aim 和安全门；不会自动武装物理输出。快捷键默认为 F8。");
            pop_colored_button();
            ImGui::EndDisabled();
        }

        ImGui::SameLine(0.0f, kButtonGap);
        if (snapshot.output_armed) {
            if (ImGui::Button("解除", ImVec2(kButtonWidth, 32.0f))) {
                actions.runtime_intents.push_back(
                    {RuntimeIntentType::DISARM_OUTPUT, false});
            }
            show_help_tooltip(
                "解除本次 Runtime 会话的物理输出武装；检测和瞄准计算继续运行，但 Mouse 不再提交移动。");
        } else {
            ImGui::BeginDisabled(
                !running || snapshot.emergency_stopped ||
                !snapshot.output_allowed_by_config ||
                snapshot.detector_reload_state ==
                    DetectorReloadState::LOADING);
            if (ImGui::Button("武装", ImVec2(kButtonWidth, 32.0f))) {
                actions.runtime_intents.push_back(
                    {RuntimeIntentType::ARM_OUTPUT, true});
            }
            show_help_tooltip(
                "在配置允许、Runtime 运行且急停正常时武装物理输出；仍需按住瞄准输出键才会发送移动。");
            ImGui::EndDisabled();
        }

        ImGui::SameLine(0.0f, kButtonGap);
        push_danger_button();
        if (ImGui::Button("急停", ImVec2(kButtonWidth, 32.0f))) {
            actions.runtime_intents.push_back(
                {RuntimeIntentType::EMERGENCY_STOP, true});
        }
        show_help_tooltip(
            "立即锁存物理输出急停并解除武装；不会停止截图和检测，释放急停键后仍需在输入页手动复位。");
        pop_colored_button();

        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }

    void render_page_heading(bool can_save, OverlayActions& actions) {
        ImGui::PushFont(small_font);
        ImGui::TextColored(
            rgba(kFaintInk), "%s",
            show_log_panel ? "LOG / RING BUFFER" : page_context(active_page));
        ImGui::PopFont();
        ImGui::PushFont(title_font);
        ImGui::TextUnformatted(
            show_log_panel ? "最近日志" : page_title(active_page));
        ImGui::PopFont();

        if (!show_log_panel && active_page != WorkspacePage::OVERVIEW) {
            const float button_width = 96.0f;
            ImGui::SetCursorPos(ImVec2(
                ImGui::GetWindowWidth() - button_width - 22.0f, 12.0f));
            ImGui::BeginDisabled(!can_save);
            push_primary_button();
            if (ImGui::Button(
                    "保存配置", ImVec2(button_width, 34.0f))) {
                actions.save_config_requested = true;
            }
            show_help_tooltip(
                "严格校验并保存当前配置；快捷键会立即重载，跨隔离运行时切换会先停止 Runtime 再重启目标 Worker。");
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
        const std::array<StageTiming, 11> stages{{
            {"Capture", profile.capture_ms},
            {"Queue", profile.queue_ms},
            {"Preprocess", profile.detector.preprocess_ms},
            {"H2D", profile.detector.h2d_ms},
            {"GPU preprocess", profile.detector.gpu_preprocess_ms},
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
            ImGui::TableNextRow(ImGuiTableRowFlags_None, 18.0f); \
            ImGui::TableSetColumnIndex(0); \
            ImGui::TextColored(rgba(kMutedInk), "%s", label); \
            ImGui::TableSetColumnIndex(1); \
            ImGui::Text("%llu", static_cast<unsigned long long>(value))
            XEN_COUNTER_ROW("已采集", snapshot.captured_frames);
            XEN_COUNTER_ROW("已处理", snapshot.processed_frames);
            XEN_COUNTER_ROW("采集端淘汰", snapshot.source_dropped_frames);
            XEN_COUNTER_ROW("DXGI 重建", snapshot.duplication_recoveries);
            XEN_COUNTER_ROW("传输丢帧", snapshot.transport_dropped_frames);
            XEN_COUNTER_ROW("传输异常", snapshot.transport_invalid_packets);
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

    static float metric_history_value(void* data, int index) noexcept {
        const auto* history = static_cast<const MetricHistory*>(data);
        return history->at(static_cast<std::size_t>(index));
    }

    void render_metric_plot(const char* id,
                            const char* label,
                            const char* value_format,
                            MetricHistory& history,
                            float scale_floor,
                            float fixed_scale_max,
                            unsigned int color) {
        ImGui::PushID(id);
        const float column_right =
            ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        ImGui::TextColored(rgba(kMutedInk), "%s", label);
        char value[32]{};
        std::snprintf(
            value, std::size(value), value_format, history.latest());
        const float value_width = ImGui::CalcTextSize(value).x;
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(
            ImGui::GetCursorPosX(),
            column_right - value_width));
        ImGui::Text("%s", value);

        if (history.empty()) {
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const ImVec2 size(ImGui::GetContentRegionAvail().x, 62.0f);
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            draw_list->AddRectFilled(
                origin, ImVec2(origin.x + size.x, origin.y + size.y),
                ImGui::GetColorU32(rgba(kFieldSurface)), 6.0f);
            draw_list->AddRect(
                origin, ImVec2(origin.x + size.x, origin.y + size.y),
                ImGui::GetColorU32(rgba(kBorder)), 6.0f);
            const char* empty_text = "等待成功帧";
            const ImVec2 text_size = ImGui::CalcTextSize(empty_text);
            draw_list->AddText(
                ImVec2(origin.x + (size.x - text_size.x) * 0.5f,
                       origin.y + (size.y - text_size.y) * 0.5f),
                ImGui::GetColorU32(rgba(kFaintInk)), empty_text);
            ImGui::Dummy(size);
        } else {
            const float scale_max = fixed_scale_max > 0.0f
                ? fixed_scale_max
                : history.maximum(scale_floor) * 1.10f;
            ImGui::PushStyleColor(ImGuiCol_PlotLines, rgba(color));
            ImGui::PlotLines(
                "##history", metric_history_value,
                &history,
                static_cast<int>(history.size()), 0, nullptr,
                0.0f, scale_max,
                ImVec2(ImGui::GetContentRegionAvail().x, 62.0f));
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
    }

    void render_history_panel() {
        begin_surface("metric_history_panel", ImVec2(0.0f, 148.0f));
        ImGui::PushFont(medium_font);
        ImGui::TextUnformatted("实时历史");
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::PushFont(small_font);
        ImGui::TextColored(
            rgba(kFaintInk), "最近 256 个成功帧 / UI 观测");
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0.0f, 3.0f));

        if (ImGui::BeginTable(
                "metric_history_columns", 3,
                ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            render_metric_plot(
                "capture_fps", "采集 FPS", "%.1f",
                capture_fps_history, 10.0f, 0.0f, kSuccess);
            ImGui::TableSetColumnIndex(1);
            render_metric_plot(
                "total_latency", "端到端延迟", "%.2f ms",
                total_latency_history, 1.0f, 0.0f, kSkill);
            ImGui::TableSetColumnIndex(2);
            render_metric_plot(
                "target_confidence", "目标置信度", "%.3f",
                target_confidence_history, 1.0f, 1.0f, kAccentStrong);
            ImGui::EndTable();
        }
        end_surface();
    }

    void render_log_panel() {
        const auto lines = Log::get_ring_buffer(64);
        const bool log_changed = lines.empty()
            ? !last_log_tail.empty()
            : lines.back() != last_log_tail;
        if (lines.empty()) {
            last_log_tail.clear();
        } else {
            last_log_tail = lines.back();
        }

        const float height = std::max(
            160.0f, ImGui::GetContentRegionAvail().y);
        begin_surface("recent_log_panel", ImVec2(0.0f, height));
        ImGui::PushFont(medium_font);
        ImGui::TextUnformatted("运行日志");
        ImGui::PopFont();
        ImGui::SameLine(0.0f, 10.0f);
        ImGui::PushFont(small_font);
        ImGui::TextColored(rgba(kFaintInk), "最近 %zu 条", lines.size());
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 40.0f);
        if (ImGui::Button("×##close_log_panel", ImVec2(24.0f, 22.0f))) {
            show_log_panel = false;
        }
        show_help_tooltip("关闭最近日志面板并返回当前功能页；不会清空日志环形缓冲区。");
        ImGui::Dummy(ImVec2(0.0f, 4.0f));

        ImGui::BeginChild(
            "log_entries", ImVec2(0.0f, 0.0f),
            ImGuiChildFlags_None);
        if (lines.empty()) {
            ImGui::TextColored(rgba(kFaintInk), "暂无可显示日志");
        } else {
            for (const auto& line : lines) {
                ImGui::TextWrapped("%s", line.c_str());
            }
            if (log_changed) {
                ImGui::SetScrollHereY(1.0f);
            }
        }
        ImGui::EndChild();
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
            render_latency_panel(snapshot, 379.0f);
            ImGui::TableSetColumnIndex(1);
            render_module_panel(snapshot, 150.0f);
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            render_activity_panel(snapshot, 221.0f);
            ImGui::EndTable();
        } else {
            render_latency_panel(snapshot, 379.0f);
            ImGui::Dummy(ImVec2(0.0f, 8.0f));
            render_module_panel(snapshot, 150.0f);
            ImGui::Dummy(ImVec2(0.0f, 8.0f));
            render_activity_panel(snapshot, 221.0f);
        }
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        render_history_panel();
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

    void render_capture_geometry_panel(const RuntimeSnapshot& snapshot) {
        begin_config_panel("capture_geometry_panel", "坐标契约", 136.0f);
        if (ImGui::BeginTable(
                "capture_geometry", 2,
                ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn(
                "项目", ImGuiTableColumnFlags_WidthStretch, 0.8f);
            ImGui::TableSetupColumn(
                "数值", ImGuiTableColumnFlags_WidthStretch, 1.2f);
#define XEN_GEOMETRY_ROW(label, format, ...) \
            ImGui::TableNextRow(ImGuiTableRowFlags_None, 24.0f); \
            ImGui::TableSetColumnIndex(0); \
            ImGui::TextColored(rgba(kMutedInk), "%s", label); \
            ImGui::TableSetColumnIndex(1); \
            ImGui::Text(format, __VA_ARGS__)
            XEN_GEOMETRY_ROW(
                "编码画面", "%d x %d",
                snapshot.encoded_width, snapshot.encoded_height);
            XEN_GEOMETRY_ROW(
                "主机 FOV", "%d x %d",
                snapshot.source_width, snapshot.source_height);
            XEN_GEOMETRY_ROW(
                "主机 ROI", "%.1f, %.1f / %d x %d",
                snapshot.capture_roi_x, snapshot.capture_roi_y,
                snapshot.capture_roi_width, snapshot.capture_roi_height);
            XEN_GEOMETRY_ROW(
                "主机像素比例", "%.3f x %.3f",
                snapshot.source_pixels_per_pixel_x,
                snapshot.source_pixels_per_pixel_y);
#undef XEN_GEOMETRY_ROW
            ImGui::EndTable();
        }
        end_config_panel();
    }

    void render_roi_preview(
            const RuntimeSnapshot& snapshot,
            const std::shared_ptr<const RuntimePreviewFrame>& preview) {
        const float maximum_width =
            std::max(1.0f, ImGui::GetContentRegionAvail().x - 26.0f);
        const auto display = preview
            ? overlay::detail::fit_preview_size(
                preview->width, preview->height, maximum_width, 320.0f)
            : overlay::detail::PreviewSize{};
        const bool has_image = preview_requested && preview &&
                               display.width > 0.0f &&
                               display.height > 0.0f;
        begin_config_panel(
            "roi_preview_panel", "实时 ROI 诊断",
            has_image ? display.height + 126.0f : 92.0f);

        const bool preview_supported =
            !snapshot.d3d11_cuda_interop &&
            !snapshot.d3d11_directml_interop;
        if (!preview_supported) {
            preview_requested = false;
            detached_preview_requested = false;
        }
        if (ImGui::BeginTable(
                "roi_preview_header", 3,
                ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn(
                "状态", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(
                "页面", ImGuiTableColumnFlags_WidthFixed, 58.0f);
            ImGui::TableSetupColumn(
                "置顶", ImGuiTableColumnFlags_WidthFixed, 58.0f);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted("同帧图像、检测框与 Aim 目标");
            ImGui::TextColored(
                rgba(kFaintInk), "最长边 512 / 最高 10 FPS / 三槽最新帧");
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(rgba(kFaintInk), "页面");
            ImGui::BeginDisabled(!preview_supported);
            toggle_switch("##roi_preview_enabled", &preview_requested);
            show_help_tooltip(
                "在检测页显示同帧 ROI 图像、检测框和 Aim 目标；关闭后若无独立窗口，Runtime 停止预览颜色转换与复制。");
            ImGui::EndDisabled();
            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(rgba(kFaintInk), "置顶");
            ImGui::BeginDisabled(!preview_supported);
            toggle_switch(
                "##detached_preview_enabled",
                &detached_preview_requested);
            show_help_tooltip(
                "打开独立置顶检测预览窗口；切换主控制台页面后仍保持订阅，关闭独立窗口不会退出 Xen。");
            ImGui::EndDisabled();
            ImGui::EndTable();
        }

        if (!preview_supported) {
            ImGui::TextColored(
                rgba(kWarning),
                "GPU 互操作会话不执行 CPU 预览回读，实时画面不可用。");
            end_config_panel();
            return;
        }

        if (!preview_requested) {
            ImGui::TextColored(
                rgba(kMutedInk),
                detached_preview_requested
                    ? "页面预览已关闭，独立置顶窗口继续显示。"
                    : "预览已关闭，Runtime 不执行颜色转换或图像复制。");
            end_config_panel();
            return;
        }
        if (!snapshot.preview_enabled) {
            ImGui::TextColored(rgba(kWarning), "预览通道切换中。");
            end_config_panel();
            return;
        }
        if (!has_image) {
            // Runtime 会话可能从序号 1 重新开始；无图像阶段清除上传序号，
            // 防止新会话首帧与旧序号相同时跳过纹理更新。
            preview_uploaded_sequence = 0;
            ImGui::TextColored(
                rgba(kMutedInk),
                snapshot.state == RuntimeState::RUNNING
                    ? "等待下一帧 ROI 预览。"
                    : "Runtime 未运行，启动后将发布新会话首帧。");
            end_config_panel();
            return;
        }
        if (!upload_preview(preview)) {
            ImGui::TextColored(rgba(kDanger), "D3D11 预览纹理上传失败。");
            end_config_panel();
            return;
        }

        const ImTextureID texture_id = static_cast<ImTextureID>(
            reinterpret_cast<std::uintptr_t>(preview_srv.Get()));
        ImGui::Image(
            ImTextureRef(texture_id), ImVec2(display.width, display.height));
        const ImVec2 image_min = ImGui::GetItemRectMin();
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        const float display_scale_x =
            display.width / static_cast<float>(preview->width);
        const float display_scale_y =
            display.height / static_cast<float>(preview->height);
        const float coordinate_scale_x =
            preview->scale_x * display_scale_x;
        const float coordinate_scale_y =
            preview->scale_y * display_scale_y;

        const std::size_t detection_count = std::min(
            preview->detection_count, preview->detections.size());
        for (std::size_t index = 0; index < detection_count; ++index) {
            const Detection& detection = preview->detections[index];
            const auto role = overlay::detail::classify_detection_role(
                detection.class_id, person_class_ids, head_class_ids);
            const auto rectangle = overlay::detail::map_preview_rect(
                detection.x1, detection.y1, detection.x2, detection.y2,
                coordinate_scale_x, coordinate_scale_y,
                display.width, display.height);
            if (rectangle.x2 - rectangle.x1 < 1.0f ||
                rectangle.y2 - rectangle.y1 < 1.0f) {
                continue;
            }
            const ImVec2 minimum(
                image_min.x + rectangle.x1, image_min.y + rectangle.y1);
            const ImVec2 maximum(
                image_min.x + rectangle.x2, image_min.y + rectangle.y2);
            const ImU32 color = ImGui::GetColorU32(
                role == overlay::detail::DetectionRole::HEAD
                    ? raw_rgba(0xf5b942)
                    : role == overlay::detail::DetectionRole::PERSON
                        ? raw_rgba(0x42bebf)
                        : rgba(kAccent));
            draw_list->AddRect(minimum, maximum, color, 0.0f, 0, 1.5f);
        }

        constexpr std::size_t kMaximumLabelRows = 8;
        const std::size_t label_count = std::min(
            detection_count, kMaximumLabelRows);
        if (label_count > 0) {
            float panel_width = 0.0f;
            std::array<std::array<char, 48>, kMaximumLabelRows> labels{};
            for (std::size_t index = 0; index < label_count; ++index) {
                const Detection& detection = preview->detections[index];
                const auto role = overlay::detail::classify_detection_role(
                    detection.class_id, person_class_ids, head_class_ids);
                const char* role_name =
                    role == overlay::detail::DetectionRole::HEAD
                        ? "头部"
                        : role == overlay::detail::DetectionRole::PERSON
                            ? "身体"
                            : "类别";
                std::snprintf(
                    labels[index].data(), labels[index].size(),
                    "%s C%d  %.1f%%", role_name, detection.class_id,
                    std::clamp(detection.confidence, 0.0f, 1.0f) * 100.0f);
                panel_width = std::max(
                    panel_width,
                    ImGui::CalcTextSize(labels[index].data()).x);
            }
            const float row_height = ImGui::GetTextLineHeight() + 4.0f;
            const ImVec2 panel_min(image_min.x + 6.0f, image_min.y + 6.0f);
            const ImVec2 panel_max(
                std::min(
                    image_min.x + display.width - 6.0f,
                    panel_min.x + panel_width + 12.0f),
                std::min(
                    image_min.y + display.height - 6.0f,
                    panel_min.y + row_height * static_cast<float>(label_count) +
                        4.0f));
            draw_list->AddRectFilled(
                panel_min, panel_max,
                ImGui::GetColorU32(raw_rgba(0x101010, 0.82f)), 2.0f);
            for (std::size_t index = 0; index < label_count; ++index) {
                const Detection& detection = preview->detections[index];
                const auto role = overlay::detail::classify_detection_role(
                    detection.class_id, person_class_ids, head_class_ids);
                const ImU32 color = ImGui::GetColorU32(
                    role == overlay::detail::DetectionRole::HEAD
                        ? raw_rgba(0xf5b942)
                        : role == overlay::detail::DetectionRole::PERSON
                            ? raw_rgba(0x42bebf)
                            : rgba(kAccent));
                draw_list->AddText(
                    ImVec2(
                        panel_min.x + 6.0f,
                        panel_min.y + 2.0f +
                            static_cast<float>(index) * row_height),
                    color, labels[index].data());
            }
        }

        const auto crosshair = overlay::detail::map_preview_point(
            preview->control_center_x, preview->control_center_y,
            coordinate_scale_x, coordinate_scale_y);
        if (crosshair.x >= 0.0f && crosshair.x <= display.width &&
            crosshair.y >= 0.0f && crosshair.y <= display.height) {
            const ImVec2 center(
                image_min.x + crosshair.x, image_min.y + crosshair.y);
            const ImU32 color = ImGui::GetColorU32(rgba(kAccent));
            draw_list->AddLine(
                ImVec2(center.x - 8.0f, center.y),
                ImVec2(center.x + 8.0f, center.y), color, 1.5f);
            draw_list->AddLine(
                ImVec2(center.x, center.y - 8.0f),
                ImVec2(center.x, center.y + 8.0f), color, 1.5f);
        }

        if (preview->has_target) {
            const AimTargetSnapshot& target = preview->target;
            const auto rectangle = overlay::detail::map_preview_rect(
                target.x1, target.y1, target.x2, target.y2,
                coordinate_scale_x, coordinate_scale_y,
                display.width, display.height);
            const ImU32 target_color = ImGui::GetColorU32(
                target.predicted ? rgba(kWarning) : rgba(kSuccess));
            draw_list->AddRect(
                ImVec2(image_min.x + rectangle.x1,
                       image_min.y + rectangle.y1),
                ImVec2(image_min.x + rectangle.x2,
                       image_min.y + rectangle.y2),
                target_color, 0.0f, 0, 3.0f);
            const auto aim_point = overlay::detail::map_preview_point(
                target.aim_x, target.aim_y,
                coordinate_scale_x, coordinate_scale_y);
            if (aim_point.x >= 0.0f && aim_point.x <= display.width &&
                aim_point.y >= 0.0f && aim_point.y <= display.height) {
                const ImVec2 point(
                    image_min.x + aim_point.x, image_min.y + aim_point.y);
                draw_list->AddCircleFilled(point, 4.5f, target_color);
                draw_list->AddCircle(
                    point, 8.0f, target_color, 16, 1.5f);
            }
        }

        ImGui::PushFont(small_font);
        ImGui::TextColored(
            rgba(kFaintInk),
            "帧 %llu  /  ROI %d x %d -> %d x %d  /  样本 %llu  丢弃 %llu",
            static_cast<unsigned long long>(preview->sequence),
            preview->roi_width, preview->roi_height,
            preview->width, preview->height,
            static_cast<unsigned long long>(snapshot.preview_sampled_frames),
            static_cast<unsigned long long>(snapshot.preview_dropped_frames));
        ImGui::TextColored(
            rgba(kFaintInk), "Detector %s  /  Aim %s  /  目标 %s",
            DetectionStatusName(preview->detection_status),
            AimStatusName(preview->aim_status),
            preview->has_target
                ? (preview->target.predicted
                    ? "预测"
                    : track_state_label(preview->target.state))
                : "无");
        if (preview->has_target) {
            ImGui::TextColored(
                rgba(kFaintInk), "轨迹 %llu  /  置信度 %.1f%%  /  瞄点 %.1f, %.1f",
                static_cast<unsigned long long>(preview->target.track_id),
                std::clamp(preview->target.confidence, 0.0f, 1.0f) *
                    100.0f,
                preview->target.aim_x, preview->target.aim_y);
        }
        ImGui::PopFont();
        end_config_panel();
    }

    void render_detection_config(
            const RuntimeSnapshot& snapshot,
            const std::shared_ptr<const RuntimePreviewFrame>& preview,
            const OverlayModelCatalog& model_catalog,
            const OverlayBackendCatalog& backend_catalog,
            AppConfig& app_config,
            bool can_edit,
            OverlayActions& actions) {
        render_roi_preview(snapshot, preview);
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        render_detector_reload_panel(snapshot, actions);
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        const bool detector_editable =
            (can_edit || snapshot.state == RuntimeState::RUNNING) &&
            snapshot.detector_reload_state != DetectorReloadState::LOADING;
        const bool two_columns =
            ImGui::GetContentRegionAvail().x >= 650.0f;
        if (two_columns && ImGui::BeginTable(
                "detector_columns", 2,
                ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::BeginDisabled(!detector_editable);
            render_detector_form(
                app_config, model_catalog, backend_catalog, actions);
            ImGui::EndDisabled();
            ImGui::TableSetColumnIndex(1);
            ImGui::BeginDisabled(!can_edit);
            render_capture_form(app_config);
            ImGui::EndDisabled();
            ImGui::EndTable();
        } else {
            ImGui::BeginDisabled(!detector_editable);
            render_detector_form(
                app_config, model_catalog, backend_catalog, actions);
            ImGui::EndDisabled();
            ImGui::Dummy(ImVec2(0.0f, 8.0f));
            ImGui::BeginDisabled(!can_edit);
            render_capture_form(app_config);
            ImGui::EndDisabled();
        }
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::BeginDisabled(!detector_editable);
        render_detector_tuning(app_config);
        ImGui::EndDisabled();
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        render_capture_geometry_panel(snapshot);
    }

    void render_detector_reload_panel(const RuntimeSnapshot& snapshot,
                                      OverlayActions& actions) {
        begin_config_panel("detector_reload_panel", "运行模型", 68.0f);
        if (ImGui::BeginTable(
                "detector_reload", 2,
                ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn(
                "状态", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(
                "操作", ImGuiTableColumnFlags_WidthFixed, 112.0f);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            status_dot_label(
                detector_reload_label(snapshot.detector_reload_state),
                detector_reload_color(snapshot.detector_reload_state));
            ImGui::SameLine(0.0f, 10.0f);
            ImGui::TextColored(
                rgba(kFaintInk), "第 %llu 代",
                static_cast<unsigned long long>(
                    snapshot.detector_generation));
            const char* active_model = snapshot.active_model_path.empty()
                ? "未加载"
                : snapshot.active_model_path.c_str();
            ImGui::TextUnformatted(active_model);
            if (ImGui::IsItemHovered() &&
                !snapshot.active_model_path.empty()) {
                ImGui::SetTooltip("%s", snapshot.active_model_path.c_str());
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.0f);
            ImGui::BeginDisabled(
                snapshot.state != RuntimeState::RUNNING ||
                snapshot.detector_reload_state ==
                    DetectorReloadState::LOADING);
            push_primary_button();
            if (ImGui::Button("重载模型", ImVec2(104.0f, 34.0f))) {
                actions.reload_detector_requested = true;
            }
            show_help_tooltip(
                "Runtime 运行时在后台加载当前所选模型；成功后原子切换 Session，失败时旧模型继续服务。");
            pop_colored_button();
            ImGui::EndDisabled();
            ImGui::EndTable();
        }
        end_config_panel();
    }

    void render_detector_form(AppConfig& app_config,
                              const OverlayModelCatalog& model_catalog,
                              const OverlayBackendCatalog& backend_catalog,
                              OverlayActions& actions) {
        const bool openvino =
            app_config.detector.backend == BackendType::OPENVINO;
        begin_config_panel(
            "detector_panel", "推理", openvino ? 300.0f : 264.0f);
        if (begin_form("detector_form", 126.0f)) {
            form_row(
                "模型",
                "从发布根 models 目录选择 ONNX 模型；保存配置或运行时重载前会再次校验规范路径和文件存在性。");
            const auto selected = std::find(
                model_catalog.model_names.begin(),
                model_catalog.model_names.end(),
                app_config.detector.model_path);
            const bool model_available =
                selected != model_catalog.model_names.end();
            std::string preview = app_config.detector.model_path.empty()
                ? "未选择"
                : app_config.detector.model_path;
            if (!app_config.detector.model_path.empty() &&
                !model_available) {
                preview += "（不可用）";
            }

            constexpr float kRefreshWidth = 56.0f;
            const float model_combo_width = std::max(
                96.0f,
                ImGui::GetContentRegionAvail().x - kRefreshWidth - 8.0f);
            ImGui::SetNextItemWidth(model_combo_width);
            ImGui::BeginDisabled(model_catalog.model_names.empty());
            if (ImGui::BeginCombo("##model_name", preview.c_str())) {
                for (const std::string& model_name :
                     model_catalog.model_names) {
                    const bool is_selected =
                        model_name == app_config.detector.model_path;
                    if (ImGui::Selectable(
                            model_name.c_str(), is_selected)) {
                        app_config.detector.model_path = model_name;
                    }
                    if (is_selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered() &&
                !model_catalog.directory.empty()) {
                ImGui::SetTooltip("%s", model_catalog.directory.c_str());
            }
            ImGui::SameLine(0.0f, 8.0f);
            if (ImGui::Button(
                    "刷新", ImVec2(kRefreshWidth, 0.0f))) {
                actions.refresh_models_requested = true;
            }
            show_help_tooltip(
                "重新扫描发布根 models 目录中的常规 ONNX 文件；链接、目录穿越和目录外模型不会进入清单。");

            std::vector<const char*> backend_names;
            backend_names.reserve(backend_catalog.backends.size());
            int backend_index = -1;
            for (std::size_t index = 0;
                 index < backend_catalog.backends.size(); ++index) {
                const BackendType candidate = backend_catalog.backends[index];
                switch (candidate) {
                    case BackendType::CUDA: backend_names.push_back("CUDA"); break;
                    case BackendType::TENSORRT:
                        backend_names.push_back("TensorRT"); break;
                    case BackendType::DIRECTML:
                        backend_names.push_back("DirectML"); break;
                    case BackendType::OPENVINO:
                        backend_names.push_back("OpenVINO"); break;
                    case BackendType::CPU: backend_names.push_back("CPU"); break;
                }
                if (candidate == app_config.detector.backend) {
                    backend_index = static_cast<int>(index);
                }
            }
            form_row(
                "推理后端",
                "选择发布清单授权的执行后端；跨 NVIDIA、DirectML 或 OpenVINO 运行时保存配置时会安全停止并重启 Worker。");
            ImGui::BeginDisabled(backend_index < 0 || backend_names.empty());
            if (ImGui::Combo(
                    "##backend", &backend_index, backend_names.data(),
                    static_cast<int>(backend_names.size()))) {
                app_config.detector.backend =
                    backend_catalog.backends[
                        static_cast<std::size_t>(backend_index)];
            }
            ImGui::EndDisabled();

            if (openvino) {
                const char* devices[] = {"GPU", "CPU", "NPU"};
                int device = static_cast<int>(
                    app_config.detector.openvino_device);
                form_row(
                    "OpenVINO 设备",
                    "选择 OpenVINO Execution Provider 使用的 GPU、CPU 或 NPU 设备类型；严格后端失败时不会静默回退 CPU。");
                if (ImGui::Combo(
                        "##openvino_device", &device, devices,
                        static_cast<int>(std::size(devices)))) {
                    app_config.detector.openvino_device =
                        static_cast<OpenVinoDevice>(device);
                    if (app_config.detector.openvino_device !=
                        OpenVinoDevice::GPU) {
                        app_config.detector.device_id = 0;
                    }
                }
            }

            form_row(
                "设备索引",
                "选择 CUDA、TensorRT、DirectML 或 OpenVINO GPU 使用的设备编号；OpenVINO CPU/NPU 固定使用 0。");
            ImGui::BeginDisabled(
                openvino && app_config.detector.openvino_device !=
                    OpenVinoDevice::GPU);
            ImGui::InputInt(
                "##device_id", &app_config.detector.device_id);
            ImGui::EndDisabled();
            const char* formats[] = {
                "Auto", "Channel first",
                "Anchor + objectness", "End to end"};
            int format =
                static_cast<int>(app_config.detector.output_format);
            form_row(
                "输出契约",
                "指定模型输出张量的解析方式；Auto 会按张量形状识别，明确选择可在歧义模型上固定契约。");
            if (ImGui::Combo(
                    "##output_format", &format, formats,
                    static_cast<int>(std::size(formats)))) {
                app_config.detector.output_format =
                    static_cast<OutputFormat>(format);
            }
            form_row(
                "FP16",
                "请求后端以半精度执行可支持的算子；实际执行能力仍由所选 Provider、模型和设备决定。");
            toggle_switch(
                "##enable_fp16", &app_config.detector.enable_fp16);
            form_row(
                "CUDA Graph",
                "TensorRT 固定 shape 会话复用稳定设备地址并重放 CUDA Graph；变化输入正确性仍由正式回归门禁保证。");
            toggle_switch(
                "##enable_trt_cuda_graph",
                &app_config.detector.enable_trt_cuda_graph);
            form_row(
                "GPU 前处理",
                "在支持的 CUDA/TensorRT 路径把颜色转换、缩放和归一化放到 GPU；不支持时按显式契约失败或使用既定 CPU 路径。");
            toggle_switch(
                "##enable_gpu_preprocess",
                &app_config.detector.enable_gpu_preprocess);
            ImGui::EndTable();
        }
        end_config_panel();
    }

    void render_detector_tuning(AppConfig& app_config) {
        begin_config_panel("tuning_panel", "检测参数", 124.0f);
        if (begin_form("tuning_form", 144.0f)) {
            form_row(
                "检测阈值",
                "过滤低于该置信度的候选框；提高可减少弱检测，降低会增加候选数量和误检风险。");
            slider_float_control(
                "conf_threshold",
                &app_config.detector.conf_threshold,
                0.01f, 1.0f, "%.2f");
            form_row(
                "NMS 阈值",
                "控制同类候选框非极大值抑制的 IoU 阈值；越低越容易合并重叠框。");
            slider_float_control(
                "nms_threshold",
                &app_config.detector.nms_threshold,
                0.01f, 1.0f, "%.2f");
            form_row(
                "最大候选数",
                "限制进入 NMS 和后处理的最高置信候选数量，避免异常输出放大延迟和内存占用。");
            ImGui::InputInt("##top_k", &app_config.detector.top_k);
            ImGui::EndTable();
        }
        end_config_panel();
    }

    void render_capture_form(AppConfig& app_config) {
        const bool udp =
            app_config.capture.backend == CaptureBackend::UDP_MJPEG;
        const bool xudp =
            app_config.capture.backend == CaptureBackend::XUDP_JPEG;
        const bool ndi = app_config.capture.backend == CaptureBackend::NDI;
        const bool network = udp || xudp || ndi;
        const bool network_source_required =
            (ndi && app_config.capture.ndi_frame_layout !=
                        NetworkFrameLayout::FULL_FRAME_1_TO_1) ||
            (udp && app_config.capture.udp_frame_layout !=
                        NetworkFrameLayout::FULL_FRAME_1_TO_1);
        const bool ndi_metadata_optional_source = ndi &&
            app_config.capture.ndi_require_frame_metadata;
        const bool source_fields_visible =
            network_source_required && !ndi_metadata_optional_source;
        const bool udp_source_required = udp &&
            app_config.capture.udp_frame_layout !=
                UdpFrameLayout::FULL_FRAME_1_TO_1;
        float panel_height = network
            ? (app_config.capture.center_roi ? 304.0f : 376.0f)
            : (app_config.capture.center_roi ? 268.0f : 340.0f);
        if (udp) panel_height += udp_source_required ? 108.0f : 36.0f;
        if (ndi) panel_height += source_fields_visible ? 220.0f : 188.0f;
        begin_config_panel("capture_panel", "画面", panel_height);
        if (begin_form("capture_form", 126.0f)) {
            const char* backends[] = {
                "本机 DXGI", "UDP MJPEG", "XUDP JPEG", "NDI"};
            int backend = static_cast<int>(app_config.capture.backend);
            form_row(
                "采集后端",
                "选择本机 DXGI 或网络 UDP、XUDP、NDI 画面来源；不同后端使用各自的连接和几何契约。");
            if (ImGui::Combo(
                    "##capture_backend", &backend, backends,
                    static_cast<int>(std::size(backends)))) {
                app_config.capture.backend =
                    static_cast<CaptureBackend>(backend);
            }
            if (udp || xudp) {
                form_row(
                    "UDP 地址",
                    "填写 UDP/XUDP 监听地址，例如 udp://0.0.0.0:5000；启动时会严格解析并绑定端口。");
                ImGui::InputText(
                    "##udp_url", &app_config.capture.udp_url);
                form_row(
                    "读取超时 / ms",
                    "单次等待网络数据的最长时间；超时用于让采集线程及时检查停止和连接状态。");
                ImGui::InputInt(
                    "##udp_read_timeout_ms",
                    &app_config.capture.udp_read_timeout_ms);
                form_row(
                    "断流判定 / ms",
                    "连续未收到完整有效帧达到该时长后判定网络源断开，并向 Runtime 报告明确失败状态。");
                ImGui::InputInt(
                    "##udp_disconnect_timeout_ms",
                    &app_config.capture.udp_disconnect_timeout_ms);
                if (udp) {
                    const char* layouts[] = {
                        "完整画面 1:1",
                        "完整画面已缩放",
                        "主机中心 1:1 裁剪"};
                    int layout = static_cast<int>(
                        app_config.capture.udp_frame_layout);
                    form_row(
                        "网络画面语义",
                        "声明接收图像与主机 FOV/中心 ROI 的坐标关系；错误选择会导致检测框到主机坐标映射错误。");
                    if (ImGui::Combo(
                            "##udp_frame_layout", &layout, layouts,
                            static_cast<int>(std::size(layouts)))) {
                        app_config.capture.udp_frame_layout =
                            static_cast<UdpFrameLayout>(layout);
                        if (app_config.capture.udp_frame_layout ==
                            UdpFrameLayout::FULL_FRAME_1_TO_1) {
                            app_config.capture.udp_source_width = 0;
                            app_config.capture.udp_source_height = 0;
                        }
                    }
                    if (udp_source_required) {
                        form_row(
                            "主机 FOV 宽度",
                            "发送端主机完整画面的像素宽度，用于把已缩放或中心裁剪网络帧映射回主机坐标。");
                        ImGui::InputInt(
                            "##udp_source_width",
                            &app_config.capture.udp_source_width);
                        form_row(
                            "主机 FOV 高度",
                            "发送端主机完整画面的像素高度，用于把已缩放或中心裁剪网络帧映射回主机坐标。");
                        ImGui::InputInt(
                            "##udp_source_height",
                            &app_config.capture.udp_source_height);
                    }
                }
            } else if (ndi) {
                form_row(
                    "NDI 源名称",
                    "指定唯一 NDI 发送源；留空或匹配不唯一时不会猜测连接目标。");
                ImGui::InputText(
                    "##ndi_source_name", &app_config.capture.ndi_source_name);
                form_row(
                    "发现超时 / ms",
                    "等待目标 NDI 源出现在发现列表中的最长时间，超时后启动失败并报告原因。");
                ImGui::InputInt(
                    "##ndi_discovery_timeout_ms",
                    &app_config.capture.ndi_discovery_timeout_ms);
                form_row(
                    "接收超时 / ms",
                    "单次等待 NDI 视频帧的最长时间；用于保持停止请求和断流检测有界。");
                ImGui::InputInt(
                    "##ndi_receive_timeout_ms",
                    &app_config.capture.ndi_receive_timeout_ms);
                form_row(
                    "断流判定 / ms",
                    "连续未收到有效 NDI 视频帧达到该时长后判定断流，并向 Runtime 报告明确失败。");
                ImGui::InputInt(
                    "##ndi_disconnect_timeout_ms",
                    &app_config.capture.ndi_disconnect_timeout_ms);
                const char* layouts[] = {
                    "完整画面 1:1",
                    "完整画面已缩放",
                    "主机中心 1:1 裁剪"};
                int layout = static_cast<int>(
                    app_config.capture.ndi_frame_layout);
                form_row(
                    "网络画面语义",
                    "声明 NDI 图像与主机 FOV/中心 ROI 的坐标关系；可由 Xen metadata 提供或使用下方显式尺寸。");
                if (ImGui::Combo(
                        "##ndi_frame_layout", &layout, layouts,
                        static_cast<int>(std::size(layouts)))) {
                    app_config.capture.ndi_frame_layout =
                        static_cast<NetworkFrameLayout>(layout);
                    if (app_config.capture.ndi_frame_layout ==
                        NetworkFrameLayout::FULL_FRAME_1_TO_1) {
                        app_config.capture.ndi_source_width = 0;
                        app_config.capture.ndi_source_height = 0;
                    }
                }
                form_row(
                    "要求 Xen metadata",
                    "开启后每帧必须携带 Xen 几何 metadata，并以其作为主机 FOV 与 ROI 的事实源；缺失或非法时拒绝该帧。");
                toggle_switch(
                    "##ndi_require_frame_metadata",
                    &app_config.capture.ndi_require_frame_metadata);
                if (source_fields_visible) {
                    form_row(
                        "主机 FOV 宽度",
                        "未要求 Xen metadata 时，显式填写发送端主机完整画面的像素宽度以完成坐标映射。");
                    ImGui::InputInt(
                        "##ndi_source_width",
                        &app_config.capture.ndi_source_width);
                    form_row(
                        "主机 FOV 高度",
                        "未要求 Xen metadata 时，显式填写发送端主机完整画面的像素高度以完成坐标映射。");
                    ImGui::InputInt(
                        "##ndi_source_height",
                        &app_config.capture.ndi_source_height);
                }
            } else {
                form_row(
                    "适配器",
                    "选择 DXGI 显卡适配器索引；多 GPU 环境应与目标显示输出所属适配器一致。");
                ImGui::InputInt(
                    "##adapter_index", &app_config.capture.adapter_index);
                form_row(
                    "显示输出",
                    "选择所用适配器下的 DXGI 显示输出索引，即实际需要截取的桌面显示器。");
                ImGui::InputInt(
                    "##output_index", &app_config.capture.output_index);
            }
            form_row(
                "中心 ROI",
                "开启后按主机 FOV 中心自动计算 ROI 起点；关闭后使用下方 ROI X/Y 显式定位。");
            toggle_switch(
                "##center_roi", &app_config.capture.center_roi);
            form_row(
                "ROI 宽度",
                "检测输入区域的主机像素宽度；必须与模型和所选 Provider 的 shape 约束兼容。");
            ImGui::InputInt(
                "##roi_width", &app_config.capture.roi_width);
            form_row(
                "ROI 高度",
                "检测输入区域的主机像素高度；必须与模型和所选 Provider 的 shape 约束兼容。");
            ImGui::InputInt(
                "##roi_height", &app_config.capture.roi_height);
            if (!app_config.capture.center_roi) {
                form_row(
                    "ROI X",
                    "关闭中心 ROI 时，检测区域左上角相对主机 FOV 的水平像素坐标。");
                ImGui::InputInt("##roi_x", &app_config.capture.roi_x);
                form_row(
                    "ROI Y",
                    "关闭中心 ROI 时，检测区域左上角相对主机 FOV 的垂直像素坐标。");
                ImGui::InputInt("##roi_y", &app_config.capture.roi_y);
            }
            form_row(
                "超时 / ms",
                "DXGI 获取下一帧的最长等待时间；网络后端也用它约束通用采集轮询等待。");
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
            form_row(
                "高置信阈值",
                "达到该置信度的观测可直接参与轨迹确认；应不低于低置信阈值。");
            slider_float_control(
                "aim_high_confidence",
                &app_config.aim.high_confidence,
                0.01f, 1.0f, "%.2f");
            form_row(
                "低置信阈值",
                "保留弱观测用于与已有轨迹关联；低于该值的检测不会进入 Aim 观测集。");
            slider_float_control(
                "aim_low_confidence",
                &app_config.aim.low_confidence,
                0.01f, 1.0f, "%.2f");
            form_row(
                "确认帧数",
                "新轨迹至少连续命中该帧数后才进入确认状态，减少单帧误检触发。");
            ImGui::InputInt(
                "##min_confirmed_hits",
                &app_config.aim.min_confirmed_hits);
            form_row(
                "最大丢失帧",
                "已确认轨迹允许连续缺少观测的帧数；超过后删除轨迹，不再沿用旧目标。");
            ImGui::InputInt(
                "##max_lost_frames",
                &app_config.aim.max_lost_frames);
            form_row(
                "最小 IoU",
                "观测与预测框关联时要求的最小交并比；与中心距离门槛共同限制错误匹配。");
            slider_float_control(
                "min_iou", &app_config.aim.min_iou,
                0.0f, 1.0f, "%.2f");
            form_row(
                "中心距离",
                "观测与预测中心允许的最大归一化距离；越小越保守，快速移动时可能更易断轨。");
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
            form_row(
                "切换优势",
                "候选目标评分必须比当前目标至少高出该幅度，才进入切换确认，避免相近目标抖动。");
            slider_float_control(
                "switch_margin", &app_config.aim.switch_margin,
                0.0f, 1.0f, "%.2f");
            form_row(
                "切换确认帧",
                "新候选连续保持优势达到该帧数后才切换目标。");
            ImGui::InputInt(
                "##switch_confirm_frames",
                &app_config.aim.switch_confirm_frames);
            form_row(
                "切换冷却帧",
                "完成一次目标切换后，在该帧数内抑制再次切换，降低多目标来回跳变。");
            ImGui::InputInt(
                "##switch_cooldown_frames",
                &app_config.aim.switch_cooldown_frames);
            form_row(
                "身体瞄准高度",
                "身体框内从顶部向下的瞄点比例；0 为顶部，1 为底部，头部类别使用自身瞄点契约。");
            slider_float_control(
                "body_aim_height_ratio",
                &app_config.aim.body_aim_height_ratio,
                0.0f, 1.0f, "%.2f");
            form_row(
                "预测增益",
                "目标暂时丢失但轨迹仍有效时，对预测控制量施加的比例；0 表示预测帧不移动。");
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
            form_row(
                "死区",
                "瞄点误差绝对值落在该像素范围内时不输出对应轴移动，减少准星附近抖动。");
            slider_float_control(
                "deadzone", &app_config.aim.deadzone_pixels,
                0.0f, 20.0f, "%.1f px");
            form_row(
                "平滑系数",
                "当前控制量在指数平滑中的权重；越大响应越快，越小移动越平缓。");
            slider_float_control(
                "smoothing", &app_config.aim.smoothing,
                0.0f, 1.0f, "%.2f");
            form_row(
                "水平 counts / px",
                "把主机 FOV 水平像素误差换算为相对鼠标 counts 的比例，不使用辅机桌面分辨率缩放。");
            slider_float_control(
                "counts_per_pixel_x",
                &app_config.aim.counts_per_pixel_x,
                0.01f, 4.0f, "%.2f");
            form_row(
                "垂直 counts / px",
                "把主机 FOV 垂直像素误差换算为相对鼠标 counts 的比例，不使用辅机桌面分辨率缩放。");
            slider_float_control(
                "counts_per_pixel_y",
                &app_config.aim.counts_per_pixel_y,
                0.01f, 4.0f, "%.2f");
            form_row(
                "单帧最大 counts",
                "限制每帧每个轴提交的相对移动绝对值，避免异常目标或参数产生突跳。");
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
            show_help_tooltip(
                "仅在所有瞄准输出按键已释放时清除急停锁存；复位后仍保持未武装，必须再次手动武装。");
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
            render_keyboard_form(app_config, actions);
            ImGui::EndTable();
        } else {
            render_mouse_form(app_config);
            ImGui::Dummy(ImVec2(0.0f, 8.0f));
            render_keyboard_form(app_config, actions);
        }
        ImGui::EndDisabled();
    }

    void render_mouse_form(AppConfig& app_config) {
        const bool kmbox =
            app_config.mouse.backend == MouseBackend::KMBOX_NET;
        const bool makcu =
            app_config.mouse.backend == MouseBackend::MAKCU;
        begin_config_panel(
            "mouse_panel", "鼠标输出",
            kmbox ? 268.0f : (makcu ? 228.0f : 88.0f));
        if (begin_form("mouse_form", 126.0f)) {
            const char* backends[] = {
                "Win32 SendInput", "KMBOX NET", "MAKCU"};
            int backend = static_cast<int>(app_config.mouse.backend);
            form_row(
                "后端",
                "选择物理鼠标命令的提交方式；Win32 使用 SendInput，KMBOX NET 和 MAKCU 使用外部设备并等待协议确认。");
            if (ImGui::Combo(
                    "##mouse_backend", &backend, backends,
                    static_cast<int>(std::size(backends)))) {
                app_config.mouse.backend =
                    static_cast<MouseBackend>(backend);
            }
            if (kmbox) {
                form_row(
                    "设备 IPv4",
                    "KMBOX NET 设备的固定 IPv4 地址；命令 ACK 只接受来自该地址和配置端口的响应。");
                ImGui::InputText(
                    "##kmbox_ip", &app_config.mouse.kmbox_ip);
                form_row(
                    "设备端口",
                    "KMBOX NET 设备监听的 UDP 端口；超出有效端口范围时配置会被拒绝。");
                ImGui::InputInt(
                    "##kmbox_port", &app_config.mouse.kmbox_port);
                form_row(
                    "设备 UUID",
                    "KMBOX 屏幕显示的 8 位十六进制 UUID；仓库不提供或保存设备凭据默认值。");
                ImGui::InputText(
                    "##kmbox_uuid", &app_config.mouse.kmbox_uuid,
                    ImGuiInputTextFlags_CharsHexadecimal);
                form_row(
                    "连接超时 / ms",
                    "等待 KMBOX connect 确认的最长时间；失败时设备保持不可用且不发送移动。");
                ImGui::InputInt(
                    "##kmbox_connect_timeout_ms",
                    &app_config.mouse.kmbox_connect_timeout_ms);
                form_row(
                    "命令超时 / ms",
                    "等待 KMBOX move ACK 的最长时间；超时或 ACK 校验失败会触发 Runtime 物理输出急停。");
                ImGui::InputInt(
                    "##kmbox_command_timeout_ms",
                    &app_config.mouse.kmbox_command_timeout_ms);
            } else if (makcu) {
                form_row(
                    "串口",
                    "MAKCU 设备所在的 Windows 串口名称，例如 COM3；打开失败时不会允许物理输出。");
                ImGui::InputText(
                    "##makcu_port", &app_config.mouse.makcu_port);
                const char* baud_rates[] = {"115200", "4000000"};
                int baud_index =
                    app_config.mouse.makcu_baud_rate == 4000000 ? 1 : 0;
                form_row(
                    "波特率",
                    "选择 MAKCU 串口通信速率；必须与设备固件当前配置一致。");
                if (ImGui::Combo(
                        "##makcu_baud_rate", &baud_index, baud_rates,
                        static_cast<int>(std::size(baud_rates)))) {
                    app_config.mouse.makcu_baud_rate =
                        baud_index == 1 ? 4000000 : 115200;
                }
                form_row(
                    "连接超时 / ms",
                    "打开并握手 MAKCU 串口的最长时间；失败时设备保持不可用。");
                ImGui::InputInt(
                    "##makcu_connect_timeout_ms",
                    &app_config.mouse.makcu_connect_timeout_ms);
                form_row(
                    "命令超时 / ms",
                    "等待 MAKCU 移动命令确认的最长时间；超时或畸形响应会触发 Runtime 物理输出急停。");
                ImGui::InputInt(
                    "##makcu_command_timeout_ms",
                    &app_config.mouse.makcu_command_timeout_ms);
            }
            form_row(
                "物理输出",
                "配置级总开关。开启后仍必须启动 Runtime、手动武装、按住瞄准输出键且急停未锁定，Mouse 才会收到命令。");
            toggle_switch(
                "##allow_send_input",
                &app_config.mouse.allow_send_input);
            ImGui::EndTable();
        }
        end_config_panel();
    }

    std::vector<int>* hotkey_binding(AppConfig& app_config) noexcept {
        switch (hotkey_binding_target) {
            case HotkeyBindingTarget::RUNTIME_TOGGLE:
                return &app_config.keyboard.runtime_toggle_virtual_keys;
            case HotkeyBindingTarget::AIM_HOLD:
                return &app_config.keyboard.aim_hold_virtual_keys;
            case HotkeyBindingTarget::EMERGENCY:
                return &app_config.keyboard.emergency_virtual_keys;
            case HotkeyBindingTarget::NONE:
                return nullptr;
        }
        return nullptr;
    }

    bool virtual_key_assigned_elsewhere(
            const AppConfig& app_config,
            const std::vector<int>* current_binding,
            int virtual_key) const noexcept {
        const std::array<const std::vector<int>*, 3> bindings{{
            &app_config.keyboard.runtime_toggle_virtual_keys,
            &app_config.keyboard.aim_hold_virtual_keys,
            &app_config.keyboard.emergency_virtual_keys}};
        return std::any_of(
            bindings.begin(), bindings.end(),
            [&](const std::vector<int>* binding) {
                return binding != current_binding &&
                    std::find(binding->begin(), binding->end(), virtual_key) !=
                        binding->end();
            });
    }

    void begin_hotkey_binding(
            HotkeyBindingTarget target,
            const std::array<bool, 256>& key_active) noexcept {
        hotkey_binding_target = target;
        hotkey_capture_message.clear();
        overlay::detail::begin_hotkey_capture(
            hotkey_capture_state, key_active);
    }

    void render_hotkey_row(
            const char* label,
            const char* id,
            const char* help,
            HotkeyBindingTarget target,
            const std::vector<int>& binding,
            const std::array<bool, 256>& key_active) {
        form_row(label, help);
        const bool capturing = hotkey_capture_state.active &&
            hotkey_binding_target == target;
        const std::string text = capturing
            ? "等待按键（Esc 清空）"
            : format_hotkey_binding(binding);
        if (ImGui::Button(id, ImVec2(-1.0f, 30.0f))) {
            begin_hotkey_binding(target, key_active);
        }
        const ImVec2 minimum = ImGui::GetItemRectMin();
        const ImVec2 maximum = ImGui::GetItemRectMax();
        const ImVec2 text_size = ImGui::CalcTextSize(text.c_str());
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(
                minimum.x + 10.0f,
                minimum.y + (maximum.y - minimum.y - text_size.y) * 0.5f),
            ImGui::GetColorU32(rgba(capturing ? kAccentStrong : kInk)),
            text.c_str());
        show_help_tooltip(help);
    }

    void render_keyboard_form(
            AppConfig& app_config,
            OverlayActions& actions) {
        const auto key_active = current_virtual_key_state();
        const bool capture_was_active = hotkey_capture_state.active;
        const auto capture_result = overlay::detail::update_hotkey_capture(
            hotkey_capture_state, key_active);
        if (capture_was_active) actions.hotkey_capture_consumed = true;
        if (capture_result.type !=
                overlay::detail::HotkeyCaptureResultType::NONE) {
            std::vector<int>* binding = hotkey_binding(app_config);
            if (binding && capture_result.type ==
                    overlay::detail::HotkeyCaptureResultType::CLEARED) {
                binding->clear();
                hotkey_capture_message = "绑定已清空，该功能已禁用";
            } else if (binding && capture_result.type ==
                    overlay::detail::HotkeyCaptureResultType::ASSIGNED) {
                const int virtual_key = capture_result.virtual_key;
                if (virtual_key_assigned_elsewhere(
                        app_config, binding, virtual_key)) {
                    hotkey_capture_message = "该按键已被其他功能占用";
                } else if (std::find(
                        binding->begin(), binding->end(), virtual_key) ==
                        binding->end()) {
                    binding->push_back(virtual_key);
                    hotkey_capture_message = "按键已追加";
                } else {
                    hotkey_capture_message = "该按键已在当前绑定中";
                }
            }
            hotkey_binding_target = HotkeyBindingTarget::NONE;
        }

        begin_config_panel("keyboard_panel", "全局按键", 148.0f);
        if (begin_form("keyboard_form", 126.0f)) {
            render_hotkey_row(
                "运行管线启停", "##runtime_toggle_virtual_keys",
                "按一次启动或停止整个 Runtime 管线；停止会结束截图、检测、瞄准和物理输出。",
                HotkeyBindingTarget::RUNTIME_TOGGLE,
                app_config.keyboard.runtime_toggle_virtual_keys, key_active);
            render_hotkey_row(
                "瞄准输出（按住）", "##aim_hold_virtual_keys",
                "仅在 Runtime 已运行、物理输出已允许且已武装时生效；按住期间放行瞄准鼠标输出，全部释放后立即关闭。",
                HotkeyBindingTarget::AIM_HOLD,
                app_config.keyboard.aim_hold_virtual_keys, key_active);
            render_hotkey_row(
                "物理输出急停", "##emergency_virtual_keys",
                "按下后立即锁定物理输出，但不会停止截图和检测 Runtime；释放急停键后仍需在界面中手动复位。",
                HotkeyBindingTarget::EMERGENCY,
                app_config.keyboard.emergency_virtual_keys, key_active);
            ImGui::EndTable();
        }
        if (!hotkey_capture_message.empty()) {
            ImGui::TextColored(
                rgba(hotkey_capture_message.find("占用") != std::string::npos
                    ? kDanger : kMutedInk),
                "%s", hotkey_capture_message.c_str());
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
            form_row(
                "分位数窗口",
                "Runtime 用于计算滚动延迟分位数的最近成功样本数量；失败帧单独计数，不进入 P50/P95。");
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
            form_row(
                "外观主题",
                "切换控制台浅色或深色配色；只影响界面显示，不改变 Runtime 或检测参数。");
            theme_selector("ui_theme", &app_config.ui.theme);
            form_row(
                "宽度",
                "主控制台启动时的客户区宽度；仍受最小窗口尺寸约束并可在运行时拖动缩放。");
            ImGui::InputInt("##ui_width", &app_config.ui.width);
            form_row(
                "高度",
                "主控制台启动时的客户区高度；仍受最小窗口尺寸约束并可在运行时拖动缩放。");
            ImGui::InputInt("##ui_height", &app_config.ui.height);
            form_row(
                "垂直同步",
                "控制 Overlay 交换链 Present 是否等待显示器垂直同步；不改变 Capture、Detector 或控制线程频率。");
            toggle_switch(
                "##enable_vsync", &app_config.ui.enable_vsync);
            ImGui::EndTable();
        }
        end_config_panel();
    }

    void render_workspace(
            const RuntimeSnapshot& snapshot,
            const std::shared_ptr<const RuntimePreviewFrame>& preview,
            const OverlayModelCatalog& model_catalog,
            const OverlayBackendCatalog& backend_catalog,
            AppConfig& app_config,
            const std::string& app_message,
            OverlayActions& actions) {
        const bool can_edit = editable(snapshot);
        const bool can_save = can_edit ||
            (active_page == WorkspacePage::DETECTION &&
             snapshot.state == RuntimeState::RUNNING &&
             snapshot.detector_reload_state !=
                 DetectorReloadState::LOADING);
        if (hotkey_capture_state.active &&
            (active_page != WorkspacePage::INPUT || !can_edit ||
             show_log_panel)) {
            hotkey_capture_state = {};
            hotkey_binding_target = HotkeyBindingTarget::NONE;
            hotkey_capture_message.clear();
        }
        ImGui::PushStyleColor(ImGuiCol_ChildBg, rgba(kSurface));
        ImGui::PushStyleVar(
            ImGuiStyleVar_WindowPadding, ImVec2(28.0f, 16.0f));
        ImGui::BeginChild(
            "content", ImVec2(0.0f, 0.0f),
            ImGuiChildFlags_AlwaysUseWindowPadding);

        render_page_heading(can_save, actions);
        render_notice(
            "app_notice", app_message, kAccentStrong, kAccentSoft);
        render_notice(
            "runtime_error", snapshot.last_error, kDanger, kDangerSoft);
        render_notice(
            "detector_reload_error", snapshot.detector_reload_error,
            kDanger, kDangerSoft);

        if (show_log_panel) {
            render_log_panel();
        } else {
            switch (active_page) {
                case WorkspacePage::OVERVIEW:
                    render_overview(snapshot);
                    break;
                case WorkspacePage::DETECTION:
                    render_detection_config(
                        snapshot, preview, model_catalog, backend_catalog,
                        app_config, can_edit, actions);
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
        impl_->detached_preview_requested =
            config.open_detached_preview_on_start;
        ImGui_ImplWin32_EnableDpiAwareness();
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW window_class{
            sizeof(WNDCLASSEXW), CS_CLASSDC, Impl::window_proc, 0L, 0L,
            instance, nullptr, nullptr, nullptr, nullptr,
            kMainWindowClass, nullptr};
        if (!RegisterClassExW(&window_class) &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
        WNDCLASSEXW detached_window_class = window_class;
        detached_window_class.style = CS_HREDRAW | CS_VREDRAW;
        detached_window_class.lpfnWndProc =
            Impl::detached_preview_window_proc;
        detached_window_class.lpszClassName =
            kDetachedPreviewWindowClass;
        if (!RegisterClassExW(&detached_window_class) &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            UnregisterClassW(kMainWindowClass, instance);
            return false;
        }
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

bool Overlay::render(
        const RuntimeSnapshot& snapshot,
        const std::shared_ptr<const RuntimePreviewFrame>& preview,
        const OverlayModelCatalog& model_catalog,
        const OverlayBackendCatalog& backend_catalog,
        AppConfig& config,
        const std::string& app_message,
        OverlayActions& actions) noexcept {
    if (!impl_ || !impl_->initialized) return false;
    try {
        actions = {};
        impl_->update_metric_history(snapshot);
        if (impl_->person_class_ids != config.aim.person_class_ids ||
            impl_->head_class_ids != config.aim.head_class_ids) {
            impl_->person_class_ids = config.aim.person_class_ids;
            impl_->head_class_ids = config.aim.head_class_ids;
            if (impl_->detached_preview_window) {
                InvalidateRect(impl_->detached_preview_window, nullptr, FALSE);
            }
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        if (impl_->applied_theme != config.ui.theme) {
            apply_codex_theme(config.ui.theme);
            apply_window_theme(impl_->window, config.ui.theme);
            apply_window_theme(
                impl_->detached_preview_window, config.ui.theme);
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
            kSidebarWidth, kWindowTitleBarHeight));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, rgba(kSurface));
        ImGui::PushStyleVar(
            ImGuiStyleVar_ChildRounding, 0.0f);
        ImGui::BeginChild(
            "workspace",
            ImVec2(0.0f, 0.0f),
            ImGuiChildFlags_None,
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse);
        impl_->render_global_bar(snapshot, actions);
        impl_->render_workspace(
            snapshot, preview, model_catalog, backend_catalog,
            config, app_message, actions);
        const bool detection_page_active =
            impl_->active_page == WorkspacePage::DETECTION;
        const bool preview_enabled =
            overlay::detail::preview_subscription_required(
                detection_page_active,
                impl_->preview_requested,
                impl_->detached_preview_requested);
        actions.preview_enabled_changed =
            preview_enabled != snapshot.preview_enabled;
        actions.preview_enabled = preview_enabled;
        if (!detection_page_active || !impl_->preview_requested) {
            impl_->release_preview_texture();
        }
        impl_->sync_detached_preview(preview);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
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
    impl_->release_preview_texture();
    impl_->detached_preview_frame.reset();
    if (impl_->detached_preview_window) {
        DestroyWindow(impl_->detached_preview_window);
        impl_->detached_preview_window = nullptr;
    }
    impl_->release_detached_paint_buffer();
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
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    UnregisterClassW(kDetachedPreviewWindowClass, instance);
    UnregisterClassW(kMainWindowClass, instance);
}
