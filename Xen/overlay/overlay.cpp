#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <Windows.h>

#include <tchar.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <dcomp.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <chrono>
#include <cmath>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include "overlay.h"
#include "overlay/draw_settings.h"
#include "overlay/config_dirty.h"
#include "include/other_tools.h"
#include "config.h"
#include "keycodes.h"
#include "keyboard_listener.h"

#ifdef USE_CUDA
#include "trt_detector.h"
#endif

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "d3d11.lib")

// D3D11 设备指针，用于创建资源和渲染
ID3D11Device* g_pd3dDevice = NULL;
// D3D11 设备上下文，执行渲染命令
ID3D11DeviceContext* g_pd3dDeviceContext = NULL;
// DXGI 交换链（用于 DirectComposition 的 Flip 模式）
IDXGISwapChain1* g_pSwapChain = NULL;
// DirectComposition 设备，用于将渲染内容合成到窗口
IDCompositionDevice* g_dcompDevice = NULL;
// DirectComposition 合成目标，绑定到 HWND
IDCompositionTarget* g_dcompTarget = NULL;
// DirectComposition 视觉对象，承载交换链内容
IDCompositionVisual* g_dcompVisual = NULL;
// D3D11 渲染目标视图，对应交换链后台缓冲区
ID3D11RenderTargetView* g_mainRenderTargetView = NULL;
// 叠加层窗口句柄
HWND g_hwnd = NULL;

extern Config config;
extern std::mutex configMutex;
extern std::atomic<bool> shouldExit;

bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();

// D3D11 混合状态对象，用于支持透明叠加渲染
ID3D11BlendState* g_pBlendState = nullptr;

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// 叠加层窗口基础尺寸（宽×高）
const int BASE_OVERLAY_WIDTH = 860;
const int BASE_OVERLAY_HEIGHT = 526;
// 编辑器最小不透明度值，保证内容始终可读
static const int MIN_EDITOR_OPACITY = 252;

// 当前叠加层窗口的宽高（逻辑像素）
int overlayWidth = 0;
int overlayHeight = 0;

// 拖拽标题栏的高度（像素）
static const int DRAG_BAR_HEIGHT_PX = 34;
// 叠加层最小宽高限制
static const int MIN_OVERLAY_W = 560;
static const int MIN_OVERLAY_H = 340;
// 窗口边缘缩放热区宽度
static const int RESIZE_BORDER_PX = 8;
// 窗口距工作区边缘的最小间距
static const int WORKAREA_MARGIN_PX = 20;

// 是否启用自动尺寸调整（用于内容自适应）
static bool g_autoResizeEnabled = true;
// 基础 ImGui 样式快照，用于运行时缩放
static ImGuiStyle g_baseStyle{};
// 基础样式是否已捕获
static bool g_baseStyleReady = false;
// 当前运行时 UI 缩放比例（DPI 感知）
static float g_runtimeUiScale = -1.0f;
// 叠加层是否可见（窗口是否显示）
static bool g_overlayVisible = false;
// 是否正在渲染叠加帧（防止重入）
static bool g_renderingOverlayFrame = false;
// 是否正在执行手动窗口拖拽
static bool g_manualWindowDragActive = false;
// 手动拖拽的命中测试类型（HTCAPTION / HTLEFT 等）
static WPARAM g_manualWindowDragHit = HTNOWHERE;
// 手动拖拽起始鼠标位置
static POINT g_manualWindowDragStartPoint{};
// 手动拖拽起始窗口矩形
static RECT g_manualWindowDragStartRect{};
// 当前活跃的叠加层选项卡索引
static int g_activeOverlayTab = 0;
// 待处理的窗口几何脏标记（在 ImGui 初始化前设置）
static bool g_pendingOverlayGeometryDirty = false;

// 可用 AI 模型名称列表
std::vector<std::string> availableModels;
// 所有按键名称列表（用于设置热键绑定）
std::vector<std::string> key_names;
// 按键名称的 C 字符串数组（供 ImGui Combo 使用）
std::vector<const char*> key_names_cstrs;

// 人体轮廓预览纹理（用于目标偏移设置界面）
ID3D11ShaderResourceView* body_texture = nullptr;

// 函数前向声明
static UINT GetDpiForWindowSafe(HWND hwnd);
static RECT GetOverlayWorkArea(HWND hwnd);
static void ClampOverlayToWorkArea(HWND hwnd, int& x, int& y, int& w, int& h);
static void EnsureOverlayInsideWorkArea(HWND hwnd, bool persistGeometry = false);
static bool StoreOverlayWindowGeometry(HWND hwnd, bool markDirty);
static bool ResizeOverlayBackBuffer(UINT width, UINT height);
static HRESULT RenderOverlayFrame(bool allowAutoResize, bool allowConfigSave);

void load_body_texture();
void release_body_texture();
std::vector<std::string> getAvailableModels();

// 将整数值限制在 [lo, hi] 闭区间内
// v: 输入值
// lo: 下限
// hi: 上限
// 返回裁剪后的值
static inline int ClampInt(int v, int lo, int hi)
{
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

// 叠加层线程配置快照结构体
// 用于在每次循环中原子地读取配置，避免锁竞争
struct OverlayThreadConfigSnapshot
{
    std::vector<std::string> buttonOpenOverlay;  // 打开/关闭叠加层的热键列表
    bool excludeFromCapture = true;               // 是否排除在录屏/截图之外
};

// 获取叠加层配置的快照（线程安全）
// 在锁保护下复制配置到快照结构体，供叠加层线程使用
static OverlayThreadConfigSnapshot SnapshotOverlayThreadConfig()
{
    std::lock_guard<std::mutex> lock(configMutex);
    OverlayThreadConfigSnapshot snapshot;
    snapshot.buttonOpenOverlay = config.button_open_overlay;
    snapshot.excludeFromCapture = config.overlay_exclude_from_capture;
    return snapshot;
}

// 计算运行时 UI 缩放比例
// 从配置读取 overlay_ui_scale，限制在 [0.85, 1.35] 范围内
static float ComputeRuntimeUiScale()
{
    return std::clamp(config.overlay_ui_scale, 0.85f, 1.35f);
}

// 应用运行时 UI 缩放
// 如果缩放比例相对于当前值有显著变化（>0.01），
// 则重置样式到基础样式并按新比例整体缩放
// 同时设置 FontScaleMain 让字体也按比例缩放
static void ApplyRuntimeUiScale()
{
    if (!g_baseStyleReady)
        return;

    const float targetScale = ComputeRuntimeUiScale();
    ImGuiStyle& style = ImGui::GetStyle();
    if (std::fabs(targetScale - g_runtimeUiScale) > 0.01f)
    {
        style = g_baseStyle;
        style.ScaleAllSizes(targetScale);
        g_runtimeUiScale = targetScale;
    }
    style.FontScaleMain = targetScale;
}

// 尝试自动调整叠加层窗口大小
// 当前实现为空（保持尺寸稳定），仅保留扩展点。
// extraContentWidth: 内容区域水平滚动宽度（预留参数）

// 设置叠加层窗口的不透明度
// opacity255: 不透明度值（0-255），实际强制下限为 MIN_EDITOR_OPACITY=252
// 确保窗口具有 WS_EX_LAYERED 扩展样式，然后设置透明度
void Overlay_SetOpacity(int opacity255)
{
    if (!g_hwnd) return;

    opacity255 = ClampInt(opacity255, MIN_EDITOR_OPACITY, 255);

    // 确保窗口是分层窗口（支持透明度）
    LONG exStyle = GetWindowLong(g_hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_LAYERED) == 0)
        SetWindowLong(g_hwnd, GWL_EXSTYLE, exStyle | WS_EX_LAYERED);

    // 设置整体透明度（LWA_ALPHA 模式）
    SetLayeredWindowAttributes(g_hwnd, 0, (BYTE)opacity255, LWA_ALPHA);
}

// 设置窗口的显示亲和性（是否被录屏/截图捕获）
// 设置 WDA_EXCLUDEFROMCAPTURE 可将此窗口从系统级捕获中排除。
// 如果该 API 失败，回退到 WDA_MONITOR（禁止捕获，但全屏也可能不可见）。
// hwnd: 窗口句柄
// excludeFromCapture: true=排除捕获，false=允许捕获
static void Overlay_SetDisplayAffinity(HWND hwnd, bool excludeFromCapture)
{
    if (!hwnd)
        return;

    const DWORD wanted = excludeFromCapture ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE;
    if (SetWindowDisplayAffinity(hwnd, wanted))
        return;

    // 设置失败时的回退处理
    if (excludeFromCapture)
    {
        const DWORD err = GetLastError();
        std::cerr << "[OverlayUI] SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) failed, err=" << err
                  << ". Trying WDA_MONITOR fallback." << std::endl;
        // WDA_MONITOR：窗口在捕获中显示为黑屏/不可见
        if (!SetWindowDisplayAffinity(hwnd, WDA_MONITOR))
        {
            std::cerr << "[OverlayUI] SetWindowDisplayAffinity(WDA_MONITOR) failed, err="
                      << GetLastError() << std::endl;
        }
    }
}

// 应用捕获排除设置（从配置读取）
// 根据当前配置的 overlay_exclude_from_capture 决定是否将叠加层
// 排除在系统录屏/截图之外
void Overlay_ApplyCaptureExclusion()
{
    Overlay_SetDisplayAffinity(g_hwnd, config.overlay_exclude_from_capture);
}

// 将浮点 RGBA 颜色值（0-255 范围）转换为 ImVec4（0.0-1.0 范围）
// r, g, b, a: 颜色分量，取值范围 0-255，Alpha 默认为 255
static inline ImVec4 RGBA(int r, int g, int b, int a = 255)
{
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

// 应用 Windows 11 深色主题
// 覆盖 ImGui 默认样式的所有颜色和尺寸参数，打造深色半透明 UI 风格。
// 包含完整的配色方案：
//   底色（surfaceBase / surfaceRaised）→ 深灰色半透明
//   控件（control / controlHover / controlActive）→ 不同层次的灰色
//   强调色（accent）→ 亮蓝色
//   文字（text / textDim）→ 近乎纯白 / 浅灰
//   边框（stroke / strokeHi）→ 半透明灰色
// 同时配置圆角半径、内边距、间距等布局参数。
// === Neon Void 科幻主题 ===
// 深空黑基底 + 霓虹青(#00e5ff)强调 + 电光紫(#7c4dff)辅色
// 更锐利的圆角、更宽松的间距、高对比度文字
static void ApplyTheme_NeonVoid()
{
    ImGuiStyle& style = ImGui::GetStyle();
    style.Alpha = 1.0f;
    style.DisabledAlpha = 0.40f;

    style.WindowRounding = 6.0f;
    style.ChildRounding = 6.0f;
    style.PopupRounding = 4.0f;
    style.FrameRounding = 4.0f;
    style.TabRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.ImageRounding = 4.0f;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.TabBorderSize = 1.0f;

    style.WindowPadding = ImVec2(18.0f, 14.0f);
    style.FramePadding = ImVec2(10.0f, 7.0f);
    style.ItemSpacing = ImVec2(10.0f, 10.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    style.CellPadding = ImVec2(8.0f, 6.0f);
    style.ScrollbarSize = 7.0f;
    style.ScrollbarPadding = 1.0f;
    style.GrabMinSize = 12.0f;
    style.IndentSpacing = 20.0f;
    style.ColumnsMinSpacing = 10.0f;
    style.TabBarBorderSize = 1.0f;
    style.TabBarOverlineSize = 2.0f;
    style.SeparatorSize = 1.0f;
    style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign = ImVec2(0.0f, 0.5f);
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);

    ImVec4* c = style.Colors;

    const ImVec4 voidBg      = RGBA(8,  12, 20, 255);
    const ImVec4 surface     = RGBA(16, 20, 30, 250);
    const ImVec4 surfaceAlt  = RGBA(20, 26, 38, 248);
    const ImVec4 ctrlBg      = RGBA(22, 27, 40, 255);
    const ImVec4 ctrlHov     = RGBA(28, 35, 53, 255);
    const ImVec4 ctrlAct     = RGBA(36, 48, 74, 255);
    const ImVec4 cyan        = RGBA(0,  229, 255, 245);
    const ImVec4 purple      = RGBA(124, 77,  255, 245);
    const ImVec4 green       = RGBA(0,  230, 118, 245);
    const ImVec4 amber       = RGBA(255, 171, 0,   245);
    const ImVec4 txtMain     = RGBA(228, 231, 236, 255);
    const ImVec4 txtSub      = RGBA(136, 146, 164, 255);
    const ImVec4 borderDim   = RGBA(30,  38,  54,  155);
    const ImVec4 borderGlow  = RGBA(0,   229, 255, 68);

    c[ImGuiCol_Text] = txtMain;
    c[ImGuiCol_TextDisabled] = txtSub;
    c[ImGuiCol_WindowBg] = RGBA(0, 0, 0, 0);
    c[ImGuiCol_ChildBg] = RGBA(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = RGBA(14, 18, 28, 252);
    c[ImGuiCol_Border] = borderDim;
    c[ImGuiCol_BorderShadow] = RGBA(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = ctrlBg;
    c[ImGuiCol_FrameBgHovered] = ctrlHov;
    c[ImGuiCol_FrameBgActive] = ctrlAct;
    c[ImGuiCol_TitleBg] = surface;
    c[ImGuiCol_TitleBgActive] = surfaceAlt;
    c[ImGuiCol_TitleBgCollapsed] = voidBg;
    c[ImGuiCol_MenuBarBg] = surface;
    c[ImGuiCol_ScrollbarBg] = RGBA(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = RGBA(60, 70, 90, 120);
    c[ImGuiCol_ScrollbarGrabHovered] = RGBA(0, 229, 255, 90);
    c[ImGuiCol_ScrollbarGrabActive] = cyan;
    c[ImGuiCol_CheckMark] = cyan;
    c[ImGuiCol_CheckboxSelectedBg] = RGBA(0, 180, 210, 255);
    c[ImGuiCol_SliderGrab] = cyan;
    c[ImGuiCol_SliderGrabActive] = RGBA(50, 240, 255, 255);
    c[ImGuiCol_Button] = ctrlBg;
    c[ImGuiCol_ButtonHovered] = ctrlHov;
    c[ImGuiCol_ButtonActive] = ctrlAct;
    c[ImGuiCol_Header] = surface;
    c[ImGuiCol_HeaderHovered] = surfaceAlt;
    c[ImGuiCol_HeaderActive] = RGBA(0, 229, 255, 60);
    c[ImGuiCol_Separator] = borderDim;
    c[ImGuiCol_SeparatorHovered] = borderGlow;
    c[ImGuiCol_SeparatorActive] = cyan;
    c[ImGuiCol_Tab] = RGBA(20, 26, 38, 245);
    c[ImGuiCol_TabHovered] = RGBA(28, 35, 53, 255);
    c[ImGuiCol_TabSelected] = RGBA(32, 42, 62, 255);
    c[ImGuiCol_TabSelectedOverline] = cyan;
    c[ImGuiCol_TabDimmed] = RGBA(14, 18, 28, 235);
    c[ImGuiCol_TabDimmedSelected] = RGBA(22, 30, 44, 245);
    c[ImGuiCol_TabDimmedSelectedOverline] = RGBA(0, 229, 255, 120);
    c[ImGuiCol_ResizeGrip] = RGBA(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = RGBA(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripActive] = RGBA(0, 0, 0, 0);
    c[ImGuiCol_InputTextCursor] = cyan;
    c[ImGuiCol_PlotLines] = cyan;
    c[ImGuiCol_PlotLinesHovered] = RGBA(80, 245, 255, 255);
    c[ImGuiCol_PlotHistogram] = purple;
    c[ImGuiCol_PlotHistogramHovered] = RGBA(150, 110, 255, 255);
    c[ImGuiCol_TableHeaderBg] = surfaceAlt;
    c[ImGuiCol_TableBorderStrong] = borderDim;
    c[ImGuiCol_TableBorderLight] = RGBA(40, 50, 70, 90);
    c[ImGuiCol_TableRowBg] = RGBA(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = RGBA(0, 229, 255, 8);
    c[ImGuiCol_NavCursor] = cyan;
    c[ImGuiCol_NavWindowingHighlight] = RGBA(0, 229, 255, 60);
    c[ImGuiCol_NavWindowingDimBg] = RGBA(0, 0, 0, 120);
    c[ImGuiCol_TextLink] = cyan;
    c[ImGuiCol_TextSelectedBg] = RGBA(0, 229, 255, 90);
    c[ImGuiCol_TreeLines] = RGBA(60, 70, 90, 90);
    c[ImGuiCol_DragDropTarget] = cyan;
    c[ImGuiCol_DragDropTargetBg] = RGBA(0, 229, 255, 50);
    c[ImGuiCol_UnsavedMarker] = amber;
    c[ImGuiCol_ModalWindowDimBg] = RGBA(0, 0, 0, 160);
}

// 侧边栏图标种类枚举
// 对应叠加层配置界面中每个选项卡的图标
enum class SidebarIconKind
{
    Camera,    // 摄像头/采集
    Chip,      // AI芯片/模型
    Layers,    // 图层/深度
    Crosshair, // 准星/目标
    Move,      // 移动/鼠标移动
    Curve,     // 曲线/预测
    Spark,     // 火花/辅助功能
    User,      // 用户/配置文件
    Mouse,     // 鼠标/输入设备
    Keyboard,  // 键盘/热键
    Sliders,   // 滑块/叠加层设置
    Monitor,   // 显示器/游戏内覆盖
    Palette,   // 调色板/渲染样式
    Image,     // 图片/图标叠加
    Bars,      // 柱状图/统计
    Debug      // 调试/诊断
};

// 叠加层选项卡项结构体
// label: 选项卡显示名称
// group: 所属分组（Vision / Aim / Control / Visuals / Monitor）
// description: 选项卡用途说明
// draw: 指向选项卡内容绘制函数的函数指针
// icon: 选项卡在侧边栏显示的图标种类
struct OverlayTabItem
{
    const char* label;
    const char* group;
    const char* description;
    void (*draw)();
    SidebarIconKind icon;
};

// 叠加层选项卡定义表
// 按功能分组排列：感知 → 瞄准 → 控制 → 显示 → 系统
static const OverlayTabItem kOverlayTabs[] = {
    { "采集源",        "Perception",  "DXGI/WinRT/UDP/NDI/虚拟摄像头等画面采集源。",  draw_capture_settings,        SidebarIconKind::Camera },
    { "检测参数",      "Perception",  "置信度/NMS 阈值、最大检测数。",                  draw_ai,                      SidebarIconKind::Chip },
    { "深度感知",      "Perception",  "深度推理、遮罩过滤、深度叠加层调试。",            draw_depth,                   SidebarIconKind::Layers },

    { "目标偏移",      "Aim",         "身体/头部 Y 偏移量、自动瞄准开关。",             draw_target,                  SidebarIconKind::Crosshair },
    { "追踪状态",      "Aim",         "活跃追踪列表、ID/类别/丢失帧统计。",             draw_tracker,                 SidebarIconKind::Crosshair },
    { "移动参数",      "Aim",         "FOV、速度倍率、吸附半径、跟踪强度。",            draw_mouse_movement,          SidebarIconKind::Move },
    { "轨迹模拟",      "Aim",         "Wind Mouse / Bezier / EMA 轨迹参数。",           draw_mouse_trajectory,        SidebarIconKind::Curve },
    { "预判补偿",      "Aim",         "卡尔曼预测 + 检测延迟补偿。",                    draw_mouse_prediction,        SidebarIconKind::Curve },
    { "射击辅助",      "Aim",         "自动射击、压枪、急停、解锁Y轴。",                draw_mouse_assist,            SidebarIconKind::Spark },
    { "灵敏度配置",    "Aim",         "游戏灵敏度、偏航/俯仰、FOV缩放预设。",           draw_mouse_profiles,          SidebarIconKind::User },

    { "设备连接",      "Control",     "鼠标后端选择及连接参数。",                       draw_mouse_input,             SidebarIconKind::Mouse },
    { "按键绑定",      "Control",     "瞄准/射击/缩放等操作的按键映射。",              draw_buttons,                 SidebarIconKind::Keyboard },
    { "面板外观",      "Control",     "透明度、UI缩放、录屏隐藏。",                     draw_overlay,                 SidebarIconKind::Sliders },

    { "游戏叠加",      "Display",     "游戏内覆盖层开关与绘制项。",                     draw_game_overlay_general,    SidebarIconKind::Monitor },
    { "绘制样式",      "Display",     "方框/捕获框/未来点的颜色与粗细。",              draw_game_overlay_visuals,    SidebarIconKind::Palette },
    { "目标标记",      "Display",     "图标路径/尺寸/锚点/类别筛选。",                 draw_game_overlay_icon,       SidebarIconKind::Image },

    { "性能统计",      "System",      "预处理/推理/NMS 耗时图表与采集详情。",          draw_stats,                   SidebarIconKind::Bars },
    { "流水线追踪",    "System",      "各处理阶段坐标记录与对比分析。",                  draw_pipeline_tracer,         SidebarIconKind::Bars },
    { "开发工具",      "System",      "截图、数据收集、YOLO 自动标注。",               draw_debug,                   SidebarIconKind::Debug },
};

// 绘制主面板背景 — 深空底色 + 发光边框 + HUD 角标
static void DrawMainPanelBackground(const ImVec2& pos, const ImVec2& size)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 max(pos.x + size.x, pos.y + size.y);
    const float r = 8.0f;

    // 底层深空填充
    draw->AddRectFilled(pos, max, IM_COL32(8, 12, 20, 252), r);

    // 渐变叠加：面板表面微妙的色温变化
    draw->AddRectFilledMultiColor(
        ImVec2(pos.x + 1.0f, pos.y + 1.0f),
        ImVec2(max.x - 1.0f, max.y - 1.0f),
        IM_COL32(18, 24, 36, 60),
        IM_COL32(12, 16, 26, 30),
        IM_COL32(8, 10, 16, 50),
        IM_COL32(14, 20, 32, 45));

    // 主边框 — 暗色基础边框
    draw->AddRect(pos, max, IM_COL32(30, 38, 54, 180), r, 0, 1.2f);

    // 外圈青色辉光 — 多层叠加实现发光效果
    draw->AddRect(ImVec2(pos.x - 1.0f, pos.y - 1.0f), ImVec2(max.x + 1.0f, max.y + 1.0f),
        IM_COL32(0, 229, 255, 22), r + 1.0f, 0, 2.0f);
    draw->AddRect(ImVec2(pos.x - 2.0f, pos.y - 2.0f), ImVec2(max.x + 2.0f, max.y + 2.0f),
        IM_COL32(0, 229, 255, 10), r + 2.0f, 0, 2.5f);

    // 四角 HUD 角度装饰线 (8 条短斜线)
    const float cl = 18.0f;  // 角线长度
    const float co = 6.0f;   // 角线距边偏移
    const ImU32 cornerCol = IM_COL32(0, 229, 255, 90);
    const float cs = 1.5f;   // 线宽
    // 左上角
    draw->AddLine(ImVec2(pos.x + co, pos.y + r), ImVec2(pos.x + co, pos.y + co + cl), cornerCol, cs);
    draw->AddLine(ImVec2(pos.x + r, pos.y + co), ImVec2(pos.x + co + cl, pos.y + co), cornerCol, cs);
    // 右上角
    draw->AddLine(ImVec2(max.x - co, pos.y + r), ImVec2(max.x - co, pos.y + co + cl), cornerCol, cs);
    draw->AddLine(ImVec2(max.x - r, pos.y + co), ImVec2(max.x - co - cl, pos.y + co), cornerCol, cs);
    // 左下角
    draw->AddLine(ImVec2(pos.x + co, max.y - r), ImVec2(pos.x + co, max.y - co - cl), cornerCol, cs);
    draw->AddLine(ImVec2(pos.x + r, max.y - co), ImVec2(pos.x + co + cl, max.y - co), cornerCol, cs);
    // 右下角
    draw->AddLine(ImVec2(max.x - co, max.y - r), ImVec2(max.x - co, max.y - co - cl), cornerCol, cs);
    draw->AddLine(ImVec2(max.x - r, max.y - co), ImVec2(max.x - co - cl, max.y - co), cornerCol, cs);
}

// 绘制侧边栏标题区域 — Xen 项目名称 + 版本号
static void DrawSidebarTitle()
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float availX = ImGui::GetContentRegionAvail().x;

    // Xen 主标题 — 霓虹青大字
    const char* title = "Xen";
    const float titleSize = 24.0f;
    // 用较大的字体大小绘制 (手动缩放文字)
    const ImVec2 titleDim = ImGui::CalcTextSize(title);
    const float titleX = cursor.x + 8.0f;
    const float titleY = cursor.y + 4.0f;

    // 标题发光背景 (微弱的青色光晕)
    draw->AddRectFilled(
        ImVec2(titleX - 4.0f, titleY - 2.0f),
        ImVec2(titleX + titleDim.x + 4.0f, titleY + titleDim.y + 2.0f),
        IM_COL32(0, 229, 255, 15), 3.0f);

    // 标题文字
    draw->AddText(ImVec2(titleX, titleY), IM_COL32(0, 229, 255, 255), title);

    // 版本副标题
    const char* version = "v2.0";
    const ImVec2 verDim = ImGui::CalcTextSize(version);
    draw->AddText(
        ImVec2(titleX + titleDim.x + 6.0f, titleY + titleDim.y - verDim.y),
        IM_COL32(100, 110, 128, 200), version);

    // 标题下方分隔线 — 青色渐变
    const float sepY = titleY + titleDim.y + 8.0f;
    draw->AddLine(
        ImVec2(cursor.x + 6.0f, sepY),
        ImVec2(cursor.x + availX - 6.0f, sepY),
        IM_COL32(0, 229, 255, 50), 1.0f);

    // 占位空间
    ImGui::Dummy(ImVec2(availX, titleDim.y + 18.0f));
}

// 绘制侧边栏图标（使用 ImDrawList 矢量绘制）
// 每个图标是 18×18 像素大小，用线条和形状组成简笔画风格
// 不同类型的图标使用不同的颜色（按分组区分）：
//   Vision → 蓝色, Aim → 橙色, Control → 青色, Visuals → 紫色, Monitor → 灰色
// 选中状态下的图标使用高亮蓝色
// draw: ImDrawList 指针
// icon: 图标种类
// group: 所属分组（用于确定默认颜色）
// pos: 图标左上角位置
// selected: 是否处于选中状态
static void DrawSidebarIcon(ImDrawList* draw, SidebarIconKind icon, const char* group, const ImVec2& pos, bool selected)
{
    // 颜色选择：选中时使用高亮青，否则按分组分配颜色
    const ImU32 color = selected ? IM_COL32(0, 229, 255, 255) :
        (std::strcmp(group, "Perception") == 0 ? IM_COL32(0, 210, 240, 230) :
         std::strcmp(group, "Aim") == 0 ? IM_COL32(255, 180, 60, 230) :
         std::strcmp(group, "Control") == 0 ? IM_COL32(0, 220, 180, 230) :
         std::strcmp(group, "Display") == 0 ? IM_COL32(170, 130, 255, 230) :
         IM_COL32(180, 190, 210, 230));

    const float x = pos.x;
    const float y = pos.y;
    const float s = 18.0f;
    const ImVec2 c(x + s * 0.5f, y + s * 0.5f);
    const float stroke = 1.8f;
    const ImU32 soft = (color & IM_COL32_A_MASK) ? (color & 0x88FFFFFFu) : color;

    // 根据图标种类绘制不同的矢量图形
    switch (icon)
    {
    case SidebarIconKind::Camera:        // 相机图标
        draw->AddRect(ImVec2(x + 3.0f, y + 5.5f), ImVec2(x + 15.0f, y + 13.5f), color, 2.5f, 0, stroke);
        draw->AddRectFilled(ImVec2(x + 6.0f, y + 3.8f), ImVec2(x + 10.0f, y + 5.8f), color, 1.0f);
        draw->AddCircle(ImVec2(c.x + 1.0f, c.y + 0.5f), 2.4f, color, 20, stroke);
        break;
    case SidebarIconKind::Chip:          // AI芯片/模型图标
        draw->AddRect(ImVec2(x + 4.5f, y + 4.5f), ImVec2(x + 13.5f, y + 13.5f), color, 2.0f, 0, stroke);
        draw->AddCircleFilled(c, 2.0f, soft, 16);
        for (int i = 0; i < 3; ++i)
        {
            const float p = y + 5.5f + i * 3.5f;
            draw->AddLine(ImVec2(x + 2.0f, p), ImVec2(x + 4.5f, p), color, stroke);
            draw->AddLine(ImVec2(x + 13.5f, p), ImVec2(x + 16.0f, p), color, stroke);
        }
        break;
    case SidebarIconKind::Layers:        // 图层/深度图标
    {
        const ImVec2 points[] = { ImVec2(c.x, y + 3.0f), ImVec2(x + 15.0f, y + 7.0f), ImVec2(c.x, y + 11.0f), ImVec2(x + 3.0f, y + 7.0f) };
        draw->AddPolyline(points, 4, color, ImDrawFlags_Closed, stroke);
        draw->AddLine(ImVec2(x + 4.0f, y + 11.5f), ImVec2(c.x, y + 15.0f), color, stroke);
        draw->AddLine(ImVec2(x + 14.0f, y + 11.5f), ImVec2(c.x, y + 15.0f), color, stroke);
        break;
    }
    case SidebarIconKind::Crosshair:     // 准星/目标图标
        draw->AddCircle(c, 5.8f, color, 28, stroke);
        draw->AddLine(ImVec2(c.x - 8.0f, c.y), ImVec2(c.x - 4.0f, c.y), color, stroke);
        draw->AddLine(ImVec2(c.x + 4.0f, c.y), ImVec2(c.x + 8.0f, c.y), color, stroke);
        draw->AddLine(ImVec2(c.x, c.y - 8.0f), ImVec2(c.x, c.y - 4.0f), color, stroke);
        draw->AddLine(ImVec2(c.x, c.y + 4.0f), ImVec2(c.x, c.y + 8.0f), color, stroke);
        break;
    case SidebarIconKind::Move:          // 移动图标（十字箭头）
        draw->AddLine(ImVec2(c.x, y + 3.0f), ImVec2(c.x, y + 15.0f), color, stroke);
        draw->AddLine(ImVec2(x + 3.0f, c.y), ImVec2(x + 15.0f, c.y), color, stroke);
        draw->AddTriangleFilled(ImVec2(c.x, y + 2.0f), ImVec2(c.x - 2.5f, y + 5.0f), ImVec2(c.x + 2.5f, y + 5.0f), color);
        draw->AddTriangleFilled(ImVec2(c.x, y + 16.0f), ImVec2(c.x - 2.5f, y + 13.0f), ImVec2(c.x + 2.5f, y + 13.0f), color);
        break;
    case SidebarIconKind::Curve:         // 曲线/预测图标
        draw->AddBezierCubic(ImVec2(x + 3.0f, y + 13.5f), ImVec2(x + 6.5f, y + 5.0f), ImVec2(x + 11.5f, y + 14.0f), ImVec2(x + 15.0f, y + 4.0f), color, stroke, 18);
        draw->AddCircleFilled(ImVec2(x + 15.0f, y + 4.0f), 2.0f, color, 12);
        break;
    case SidebarIconKind::Spark:         // 火花/辅助图标（十字+叉号变体）
        draw->AddLine(ImVec2(c.x, y + 3.0f), ImVec2(c.x, y + 15.0f), color, stroke);
        draw->AddLine(ImVec2(x + 3.0f, c.y), ImVec2(x + 15.0f, c.y), color, stroke);
        draw->AddLine(ImVec2(x + 5.0f, y + 5.0f), ImVec2(x + 13.0f, y + 13.0f), color, stroke);
        draw->AddLine(ImVec2(x + 13.0f, y + 5.0f), ImVec2(x + 5.0f, y + 13.0f), color, stroke);
        break;
    case SidebarIconKind::User:          // 用户/配置文件图标（头像轮廓）
        draw->AddCircle(ImVec2(c.x, y + 6.0f), 3.0f, color, 18, stroke);
        draw->AddBezierCubic(ImVec2(x + 4.0f, y + 15.0f), ImVec2(x + 5.0f, y + 10.5f), ImVec2(x + 13.0f, y + 10.5f), ImVec2(x + 14.0f, y + 15.0f), color, stroke, 14);
        break;
    case SidebarIconKind::Mouse:         // 鼠标图标
        draw->AddRect(ImVec2(x + 5.0f, y + 2.5f), ImVec2(x + 13.0f, y + 15.5f), color, 4.0f, 0, stroke);
        draw->AddLine(ImVec2(c.x, y + 3.0f), ImVec2(c.x, y + 7.0f), color, stroke);
        break;
    case SidebarIconKind::Keyboard:      // 键盘图标
        draw->AddRect(ImVec2(x + 2.5f, y + 5.0f), ImVec2(x + 15.5f, y + 13.0f), color, 2.0f, 0, stroke);
        for (int i = 0; i < 3; ++i)
            draw->AddCircleFilled(ImVec2(x + 5.0f + i * 4.0f, y + 8.0f), 0.9f, color, 8);
        draw->AddLine(ImVec2(x + 5.0f, y + 11.0f), ImVec2(x + 13.0f, y + 11.0f), color, stroke);
        break;
    case SidebarIconKind::Sliders:       // 滑块/设置图标（三条带调节点的横线）
        for (int i = 0; i < 3; ++i)
        {
            const float yy = y + 5.0f + i * 4.0f;
            draw->AddLine(ImVec2(x + 3.0f, yy), ImVec2(x + 15.0f, yy), color, stroke);
            draw->AddCircleFilled(ImVec2(x + 6.0f + i * 3.0f, yy), 1.8f, color, 14);
        }
        break;
    case SidebarIconKind::Monitor:       // 显示器/游戏内覆盖图标
        draw->AddRect(ImVec2(x + 3.0f, y + 4.0f), ImVec2(x + 15.0f, y + 12.0f), color, 2.0f, 0, stroke);
        draw->AddLine(ImVec2(c.x, y + 12.0f), ImVec2(c.x, y + 15.0f), color, stroke);
        draw->AddLine(ImVec2(x + 6.0f, y + 15.0f), ImVec2(x + 12.0f, y + 15.0f), color, stroke);
        break;
    case SidebarIconKind::Palette:       // 调色板/渲染样式图标
        draw->AddCircle(c, 6.5f, color, 24, stroke);
        draw->AddCircleFilled(ImVec2(x + 6.0f, y + 7.0f), 1.1f, color, 8);
        draw->AddCircleFilled(ImVec2(x + 9.0f, y + 5.5f), 1.1f, color, 8);
        draw->AddCircleFilled(ImVec2(x + 12.0f, y + 8.0f), 1.1f, color, 8);
        break;
    case SidebarIconKind::Image:         // 图片/图标叠加图标（带山的风景画）
    {
        draw->AddRect(ImVec2(x + 3.0f, y + 4.0f), ImVec2(x + 15.0f, y + 14.0f), color, 2.0f, 0, stroke);
        draw->AddCircleFilled(ImVec2(x + 6.0f, y + 7.0f), 1.3f, color, 10);
        const ImVec2 points[] = { ImVec2(x + 4.0f, y + 13.0f), ImVec2(x + 8.0f, y + 9.0f), ImVec2(x + 11.0f, y + 12.0f), ImVec2(x + 14.0f, y + 9.0f) };
        draw->AddPolyline(points, 4, color, 0, stroke);
        break;
    }
    case SidebarIconKind::Bars:          // 柱状图/统计图标（三个不同高度的矩形）
        draw->AddRectFilled(ImVec2(x + 4.0f, y + 10.0f), ImVec2(x + 6.5f, y + 15.0f), color, 1.0f);
        draw->AddRectFilled(ImVec2(x + 8.0f, y + 6.5f), ImVec2(x + 10.5f, y + 15.0f), color, 1.0f);
        draw->AddRectFilled(ImVec2(x + 12.0f, y + 3.5f), ImVec2(x + 14.5f, y + 15.0f), color, 1.0f);
        break;
    case SidebarIconKind::Debug:         // 调试/诊断图标
        draw->AddRect(ImVec2(x + 5.0f, y + 5.0f), ImVec2(x + 13.0f, y + 13.5f), color, 3.0f, 0, stroke);
        draw->AddLine(ImVec2(x + 3.0f, y + 8.0f), ImVec2(x + 15.0f, y + 8.0f), color, stroke);
        draw->AddLine(ImVec2(x + 3.0f, y + 11.5f), ImVec2(x + 15.0f, y + 11.5f), color, stroke);
        break;
    }
}

// 绘制侧边栏选项卡按钮 — 科幻风格
// 选中态：左侧青色发光指示条 + 半透明青色背景
// 悬停态：淡紫色覆盖
static bool DrawSidebarTabButton(const OverlayTabItem& tab, bool selected)
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImGuiStyle& style = ImGui::GetStyle();
    ImVec2 size = ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight() + style.ItemSpacing.y * 0.30f);
    if (size.x < 1.0f)
        size.x = 1.0f;

    const std::string id = std::string("##nav_") + tab.label;
    const bool pressed = ImGui::InvisibleButton(id.c_str(), size);
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 max(pos.x + size.x, pos.y + size.y);

    // 背景
    ImU32 rowBg = IM_COL32(0, 0, 0, 0);
    if (selected)
        rowBg = IM_COL32(0, 229, 255, 28);   // 青色半透明
    else if (hovered)
        rowBg = IM_COL32(124, 77, 255, 22);  // 紫色半透明

    if (selected || hovered)
        draw->AddRectFilled(pos, max, rowBg, 4.0f);

    // 选中态装饰
    if (selected)
    {
        // 左侧青色发光指示条 (多层叠加实现辉光)
        const float mx0 = pos.x + 3.0f, mx1 = pos.x + 5.0f;
        const float my0 = pos.y + 6.0f, my1 = max.y - 6.0f;
        draw->AddRectFilled(ImVec2(mx0 - 1.0f, my0), ImVec2(mx1 + 1.0f, my1),
            IM_COL32(0, 229, 255, 50), 2.0f);
        draw->AddRectFilled(ImVec2(mx0, my0), ImVec2(mx1, my1),
            IM_COL32(0, 229, 255, 220), 2.0f);
        // 弱边框
        draw->AddRect(pos, max, IM_COL32(0, 229, 255, 15), 4.0f, 0, 1.0f);
    }

    // 文字
    const float textY = pos.y + (size.y - ImGui::GetTextLineHeight()) * 0.5f;
    const ImU32 textCol = selected
        ? IM_COL32(0, 229, 255, 255)
        : (hovered ? IM_COL32(220, 225, 235, 255) : IM_COL32(160, 170, 185, 230));

    // 图标
    DrawSidebarIcon(draw, tab.icon, tab.group,
        ImVec2(pos.x + style.FramePadding.x + 4.0f, pos.y + (size.y - 18.0f) * 0.5f), selected);

    // 标签
    draw->AddText(ImVec2(pos.x + style.FramePadding.x + 31.0f, textY), textCol, tab.label);

    return pressed;
}

// 安全地获取窗口的 DPI 值
// 通过动态加载 GetDpiForWindow() 函数（Windows 10 1607+），
// 避免对旧版本 Windows 的链接依赖。如果 API 不可用则返回默认 96。
// hwnd: 目标窗口句柄
// 返回 DPI 值（默认为 96）
static UINT GetDpiForWindowSafe(HWND hwnd)
{
    UINT dpi = 96;
    HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    if (user32)
    {
        auto pGetDpiForWindow = (UINT(WINAPI*)(HWND))::GetProcAddress(user32, "GetDpiForWindow");
        if (pGetDpiForWindow)
            dpi = pGetDpiForWindow(hwnd);
    }
    return dpi;
}

// 获取窗口所在显示器的工作区矩形（排除任务栏等占用区域）
// 如果 hwnd 为 NULL，则使用鼠标光标位置确定显示器
// hwnd: 窗口句柄，可以为 NULL
// 返回工作区 RECT，失败时返回主显示器全屏尺寸
static RECT GetOverlayWorkArea(HWND hwnd)
{
    RECT work{};
    HMONITOR monitor = nullptr;

    if (hwnd)
    {
        // 使用窗口所在显示器
        monitor = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    }
    else
    {
        // 无窗口时用鼠标位置确定显示器
        POINT pt{};
        ::GetCursorPos(&pt);
        monitor = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    }

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (monitor && ::GetMonitorInfo(monitor, &mi))
        return mi.rcWork;

    // 回退到主显示器的全屏幕尺寸
    work.left = 0;
    work.top = 0;
    work.right = ::GetSystemMetrics(SM_CXSCREEN);
    work.bottom = ::GetSystemMetrics(SM_CYSCREEN);
    return work;
}

// 根据给定矩形所在显示器获取工作区
// 用于在窗口尚未创建时（或使用虚拟矩形）确定工作区范围
// rect: 参考矩形，用于定位显示器
// 返回工作区 RECT
static RECT GetOverlayWorkAreaForRect(const RECT& rect)
{
    HMONITOR monitor = ::MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (monitor && ::GetMonitorInfo(monitor, &mi))
        return mi.rcWork;

    return GetOverlayWorkArea(nullptr);
}

// 将窗口位置和尺寸裁剪到工作区范围内
// 确保窗口不小于最小尺寸（考虑 DPI 缩放），
// 且不超出工作区边界（留出 WORKAREA_MARGIN_PX 边距）
// hwnd: 窗口句柄（可以为 NULL）
// x, y: 输入输出，窗口左上角坐标
// w, h: 输入输出，窗口宽度和高度
static void ClampOverlayToWorkArea(HWND hwnd, int& x, int& y, int& w, int& h)
{
    const RECT desiredRect = {
        x,
        y,
        x + OtherTools::MaxInt(1, w),
        y + OtherTools::MaxInt(1, h)
    };
    const RECT work = hwnd ? GetOverlayWorkArea(hwnd) : GetOverlayWorkAreaForRect(desiredRect);
    const UINT dpi = hwnd ? GetDpiForWindowSafe(hwnd) : 96;

    // 根据 DPI 缩放最小尺寸
    const int minW = ::MulDiv(MIN_OVERLAY_W, (int)dpi, 96);
    const int minH = ::MulDiv(MIN_OVERLAY_H, (int)dpi, 96);

    const int workW = OtherTools::MaxInt(1, static_cast<int>(work.right - work.left - WORKAREA_MARGIN_PX));
    const int workH = OtherTools::MaxInt(1, static_cast<int>(work.bottom - work.top - WORKAREA_MARGIN_PX));

    const int maxW = OtherTools::MaxInt(minW, workW);
    const int maxH = OtherTools::MaxInt(minH, workH);

    // 尺寸限制在最小和最大之间
    w = ClampInt(w, minW, maxW);
    h = ClampInt(h, minH, maxH);

    // 位置限制在工作区内（窗口需完整可见）
    const int maxX = OtherTools::MaxInt(static_cast<int>(work.left), static_cast<int>(work.right - w));
    const int maxY = OtherTools::MaxInt(static_cast<int>(work.top), static_cast<int>(work.bottom - h));
    x = ClampInt(x, static_cast<int>(work.left), maxX);
    y = ClampInt(y, static_cast<int>(work.top), maxY);
}

// 标记叠加层窗口几何信息为脏（需要保存）
// 如果 ImGui 已初始化，直接标记配置为脏；
// 否则设置挂起标记，待 SetupImGui 时处理
static void MarkOverlayGeometryDirty()
{
    if (ImGui::GetCurrentContext())
    {
        OverlayConfig_MarkDirty();
    }
    else
    {
        g_pendingOverlayGeometryDirty = true;
    }
}

// 将窗口当前几何信息（位置和尺寸）存储到配置中
// 只有当值实际发生变化时才标记脏
// hwnd: 窗口句柄
// markDirty: 是否在变化时标记配置脏
// 返回 true 表示几何信息已更新
static bool StoreOverlayWindowGeometry(HWND hwnd, bool markDirty)
{
    if (!hwnd)
        return false;

    RECT wndRect{};
    if (!::GetWindowRect(hwnd, &wndRect))
        return false;

    const int x = wndRect.left;
    const int y = wndRect.top;
    const int w = wndRect.right - wndRect.left;
    const int h = wndRect.bottom - wndRect.top;

    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(configMutex);
        changed = config.overlay_x != x ||
                  config.overlay_y != y ||
                  config.overlay_width != w ||
                  config.overlay_height != h;

        if (changed)
        {
            // 更新配置中的位置和尺寸
            config.overlay_x = x;
            config.overlay_y = y;
            config.overlay_width = w;
            config.overlay_height = h;
        }
    }

    if (changed && markDirty)
        MarkOverlayGeometryDirty();

    return changed;
}

// 确保窗口完全在显示器工作区范围内
// 如果窗口超出工作区或尺寸过小，调整位置和尺寸
// 可选择在调整后持久化几何信息
// hwnd: 窗口句柄
// persistGeometry: 是否在调整后将新几何信息保存到配置
static void EnsureOverlayInsideWorkArea(HWND hwnd, bool persistGeometry)
{
    if (!hwnd)
        return;

    RECT wndRect{};
    ::GetWindowRect(hwnd, &wndRect);

    const int oldW = overlayWidth;
    const int oldH = overlayHeight;

    int x = wndRect.left;
    int y = wndRect.top;
    int w = overlayWidth;
    int h = overlayHeight;
    ClampOverlayToWorkArea(hwnd, x, y, w, h);

    overlayWidth = w;
    overlayHeight = h;

    // 如果位置或尺寸有变化，应用新的窗口矩形
    if (x != wndRect.left || y != wndRect.top || w != oldW || h != oldH)
        ::SetWindowPos(hwnd, NULL, x, y, w, h, SWP_NOZORDER);

    // 可选：将当前几何信息持久化到配置
    if (persistGeometry)
        StoreOverlayWindowGeometry(hwnd, true);
}

// 初始化 D3D11 混合状态，支持透明叠加层渲染
// 使用标准 Alpha 混合：源颜色 × SrcAlpha + 目标颜色 × (1 - SrcAlpha)
// Alpha 通道使用 ONE / ZERO 保留源 Alpha 值
// 渲染目标写入掩码为全通道（RGBA）
// 返回 true 表示成功，false 表示失败
bool InitializeBlendState()
{
    D3D11_BLEND_DESC blendDesc;
    ZeroMemory(&blendDesc, sizeof(blendDesc));

    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    HRESULT hr = g_pd3dDevice->CreateBlendState(&blendDesc, &g_pBlendState);
    if (FAILED(hr))
        return false;

    float blendFactor[4] = { 0.f, 0.f, 0.f, 0.f };
    g_pd3dDeviceContext->OMSetBlendState(g_pBlendState, blendFactor, 0xffffffff);
    return true;
}

// 创建 D3D11 设备并初始化 DirectComposition 合成链
// 这是叠加层渲染管线的核心初始化函数：
// 1. 创建 D3D11 设备和设备上下文（支持 D3D 10.0 回退）
// 2. 通过 DXGI 适配器获取 IDXGIFactory2 以创建交换链
// 3. 创建用于 DirectComposition 的翻转模式交换链
//    (DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, 预乘 Alpha)
// 4. 初始化 DirectComposition 设备、合成目标和视觉对象
// 5. 将交换链设置为视觉对象的内容并提交
// 6. 创建透明混合状态对象
// 7. 创建初始渲染目标视图
// hWnd: 叠加层窗口句柄，用于 DirectComposition 绑定
// 返回 true 表示成功，false 表示失败
bool CreateDeviceD3D(HWND hWnd)
{
    UINT createDeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };

    // 创建 D3D11 硬件设备（优先使用 11.0，回退到 10.0）
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createDeviceFlags,
        featureLevelArray,
        ARRAYSIZE(featureLevelArray),
        D3D11_SDK_VERSION,
        &g_pd3dDevice,
        &featureLevel,
        &g_pd3dDeviceContext);

    if (FAILED(hr))
        return false;

    // 获取 DXGI 设备接口
    IDXGIDevice* dxgiDev = nullptr;
    hr = g_pd3dDevice->QueryInterface(IID_PPV_ARGS(&dxgiDev));
    if (FAILED(hr) || !dxgiDev)
        return false;

    // 获取 DXGI 适配器
    IDXGIAdapter* adapter = nullptr;
    hr = dxgiDev->GetAdapter(&adapter);
    if (FAILED(hr) || !adapter)
    {
        dxgiDev->Release();
        return false;
    }

    // 获取 DXGI Factory 2（用于创建 Composition 交换链）
    IDXGIFactory2* factory2 = nullptr;
    {
        IDXGIFactory* baseFactory = nullptr;
        hr = adapter->GetParent(IID_PPV_ARGS(&baseFactory));
        if (FAILED(hr) || !baseFactory)
        {
            adapter->Release();
            dxgiDev->Release();
            return false;
        }
        hr = baseFactory->QueryInterface(IID_PPV_ARGS(&factory2));
        baseFactory->Release();
    }

    if (FAILED(hr) || !factory2)
    {
        adapter->Release();
        dxgiDev->Release();
        return false;
    }

    // 配置交换链：B8G8R8A8 格式、双缓冲、翻转模式、预乘 Alpha
    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width = overlayWidth;
    scd.Height = overlayHeight;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    scd.Scaling = DXGI_SCALING_STRETCH;

    // 创建用于 DirectComposition 的交换链
    hr = factory2->CreateSwapChainForComposition(
        g_pd3dDevice,
        &scd,
        nullptr,
        &g_pSwapChain);

    factory2->Release();
    adapter->Release();

    if (FAILED(hr) || !g_pSwapChain)
    {
        dxgiDev->Release();
        return false;
    }

    // 创建 DirectComposition 设备
    hr = DCompositionCreateDevice(dxgiDev, IID_PPV_ARGS(&g_dcompDevice));
    dxgiDev->Release();
    if (FAILED(hr) || !g_dcompDevice)
        return false;

    // 创建绑定到窗口的合成目标
    hr = g_dcompDevice->CreateTargetForHwnd(hWnd, TRUE, &g_dcompTarget);
    if (FAILED(hr) || !g_dcompTarget)
        return false;

    // 创建视觉对象并关联交换链
    hr = g_dcompDevice->CreateVisual(&g_dcompVisual);
    if (FAILED(hr) || !g_dcompVisual)
        return false;

    hr = g_dcompVisual->SetContent(g_pSwapChain);
    if (FAILED(hr))
        return false;

    // 将视觉对象设置为合成目标的根节点
    hr = g_dcompTarget->SetRoot(g_dcompVisual);
    if (FAILED(hr))
        return false;

    // 提交合成树到 DWM
    g_dcompDevice->Commit();

    // 初始化透明混合状态（支持 Alpha 混合）
    if (!InitializeBlendState())
        return false;

    // 创建初始渲染目标视图
    CreateRenderTarget();
    return true;
}

// 创建 D3D11 渲染目标视图
// 从交换链获取后台缓冲区纹理，并创建对应的渲染目标视图
void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer = NULL;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, NULL, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

// 清理 D3D11 渲染目标视图
// 先解绑设备上下文中的渲染目标，然后释放并置空
void CleanupRenderTarget()
{
    if (g_pd3dDeviceContext)
    {
        ID3D11RenderTargetView* nullRenderTarget = nullptr;
        g_pd3dDeviceContext->OMSetRenderTargets(1, &nullRenderTarget, NULL);
    }
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = NULL; }
}

// 调整叠加层交换链后台缓冲区尺寸
// 当窗口大小变化时调用，先清理旧的渲染目标视图，
// 然后按新尺寸重置交换链缓冲区，最后重建渲染目标视图。
// width, height: 新的缓冲区尺寸（像素）
// 返回 true 表示成功，false 表示失败
static bool ResizeOverlayBackBuffer(UINT width, UINT height)
{
    if (width == 0 || height == 0)
        return false;

    overlayWidth = static_cast<int>(width);
    overlayHeight = static_cast<int>(height);

    // 如果 D3D11 设备或交换链尚未就绪，仅更新变量即可
    if (!g_pd3dDevice || !g_pSwapChain)
        return true;

    CleanupRenderTarget();
    // ResizeBuffers 使用 DXGI_FORMAT_UNKNOWN 保持原有格式
    const HRESULT hr = g_pSwapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr))
        return false;

    CreateRenderTarget();
    // 通知 DirectComposition 更新合成内容
    if (g_dcompDevice)
        g_dcompDevice->Commit();

    return true;
}

// 清理所有 D3D11 和 DirectComposition 资源
// 按依赖关系的逆序释放：渲染目标 → 混合状态 → 交换链 → 合成视觉/目标/设备 → 设备上下文 → 设备
void CleanupDeviceD3D()
{
    CleanupRenderTarget();

    if (g_dcompVisual) { g_dcompVisual->Release(); g_dcompVisual = NULL; }
    if (g_dcompTarget) { g_dcompTarget->Release(); g_dcompTarget = NULL; }
    if (g_dcompDevice) { g_dcompDevice->Release(); g_dcompDevice = NULL; }

    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = NULL; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = NULL; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = NULL; }
    if (g_pBlendState) { g_pBlendState->Release(); g_pBlendState = nullptr; }
}

// 判断命中测试值是否表示窗口边缘缩放
static bool IsOverlayResizeHit(WPARAM hit)
{
    return hit == HTLEFT || hit == HTRIGHT || hit == HTTOP || hit == HTBOTTOM ||
           hit == HTTOPLEFT || hit == HTTOPRIGHT || hit == HTBOTTOMLEFT || hit == HTBOTTOMRIGHT;
}

// 判断命中测试值是否表示窗口移动或缩放
static bool IsOverlayMoveOrResizeHit(WPARAM hit)
{
    return hit == HTCAPTION || IsOverlayResizeHit(hit);
}

// 开始手动窗口拖拽（移动或缩放）
// 捕获鼠标，记录起始状态（光标位置和窗口矩形）
// 如果是缩放操作，禁用自动尺寸调整
// hwnd: 叠加层窗口句柄
// hit: WM_NCHITTEST 返回的命中测试类型（HTCAPTION 或边缘值）
static void BeginManualOverlayWindowDrag(HWND hwnd, WPARAM hit)
{
    if (!hwnd || !IsOverlayMoveOrResizeHit(hit))
        return;

    g_manualWindowDragActive = true;
    g_manualWindowDragHit = hit;
    ::GetCursorPos(&g_manualWindowDragStartPoint);
    ::GetWindowRect(hwnd, &g_manualWindowDragStartRect);
    ::SetCapture(hwnd);

    // 缩放操作时禁用自动尺寸调整，避免自动缩放干扰手动调整
    if (IsOverlayResizeHit(hit))
        g_autoResizeEnabled = false;
}

// 结束手动窗口拖拽
// 清除拖拽状态，释放鼠标捕获，并确保窗口在工作区内
// hwnd: 叠加层窗口句柄
static void EndManualOverlayWindowDrag(HWND hwnd)
{
    if (!g_manualWindowDragActive)
        return;

    g_manualWindowDragActive = false;
    g_manualWindowDragHit = HTNOWHERE;

    if (::GetCapture() == hwnd)
        ::ReleaseCapture();

    // 确保窗口不超出工作区，并持久化几何信息
    EnsureOverlayInsideWorkArea(hwnd, true);
}

// 更新手动窗口拖拽（每帧鼠标移动时调用）
// 根据拖拽类型（移动 vs 缩放）计算新的窗口位置和尺寸
// 然后通过 SetWindowPos 应用新几何
// 移动：根据鼠标偏移平移窗口
// 缩放：根据鼠标偏移和拖拽方向调整宽高，保持对边/对角固定
// hwnd: 叠加层窗口句柄
static void UpdateManualOverlayWindowDrag(HWND hwnd)
{
    if (!hwnd || !g_manualWindowDragActive)
        return;

    POINT pt{};
    ::GetCursorPos(&pt);

    // 从起始位置到当前光标的偏移量
    const int dx = pt.x - g_manualWindowDragStartPoint.x;
    const int dy = pt.y - g_manualWindowDragStartPoint.y;
    const int startW = g_manualWindowDragStartRect.right - g_manualWindowDragStartRect.left;
    const int startH = g_manualWindowDragStartRect.bottom - g_manualWindowDragStartRect.top;

    int x = g_manualWindowDragStartRect.left;
    int y = g_manualWindowDragStartRect.top;
    int w = startW;
    int h = startH;

    if (g_manualWindowDragHit == HTCAPTION)
    {
        // ---- 移动模式：平移整个窗口 ----
        x += dx;
        y += dy;
        ClampOverlayToWorkArea(hwnd, x, y, w, h);
    }
    else
    {
        // ---- 缩放模式：根据拖拽方向调整窗口 ----
        const UINT dpi = GetDpiForWindowSafe(hwnd);
        const int minW = ::MulDiv(MIN_OVERLAY_W, (int)dpi, 96);
        const int minH = ::MulDiv(MIN_OVERLAY_H, (int)dpi, 96);
        const RECT work = GetOverlayWorkArea(hwnd);
        const int maxW = OtherTools::MaxInt(minW, static_cast<int>((work.right - work.left) - WORKAREA_MARGIN_PX));
        const int maxH = OtherTools::MaxInt(minH, static_cast<int>((work.bottom - work.top) - WORKAREA_MARGIN_PX));

        const bool left = g_manualWindowDragHit == HTLEFT ||
                          g_manualWindowDragHit == HTTOPLEFT ||
                          g_manualWindowDragHit == HTBOTTOMLEFT;
        const bool right = g_manualWindowDragHit == HTRIGHT ||
                           g_manualWindowDragHit == HTTOPRIGHT ||
                           g_manualWindowDragHit == HTBOTTOMRIGHT;
        const bool top = g_manualWindowDragHit == HTTOP ||
                         g_manualWindowDragHit == HTTOPLEFT ||
                         g_manualWindowDragHit == HTTOPRIGHT;
        const bool bottom = g_manualWindowDragHit == HTBOTTOM ||
                            g_manualWindowDragHit == HTBOTTOMLEFT ||
                            g_manualWindowDragHit == HTBOTTOMRIGHT;

        // 左边缘缩放：向右拖拽减少宽度
        if (left)
        {
            w = ClampInt(startW - dx, minW, maxW);
            x = g_manualWindowDragStartRect.right - w;
        }
        else if (right)
        {
            w = ClampInt(startW + dx, minW, maxW);
        }

        // 上边缘缩放：向下拖拽减少高度
        if (top)
        {
            h = ClampInt(startH - dy, minH, maxH);
            y = g_manualWindowDragStartRect.bottom - h;
        }
        else if (bottom)
        {
            h = ClampInt(startH + dy, minH, maxH);
        }

        // 缩放后确保窗口不超出工作区
        ClampOverlayToWorkArea(hwnd, x, y, w, h);
    }

    // 应用新的窗口位置和尺寸
    ::SetWindowPos(hwnd, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

// 窗口过程函数：处理叠加层窗口的所有 Windows 消息
// 消息处理：
//   WM_NCHITTEST — 自定义非客户区命中测试，将窗口边缘映射为可调整大小的区域
//                   (HTLEFT/HTRIGHT/HTTOP/HTBOTTOM 等)，标题栏区域映射为 HTCAPTION
//   WM_GETMINMAXINFO — 限制窗口最小/最大尺寸（基于 DPI 和可用工作区）
//   WM_NCLBUTTONDOWN — 在标题栏或边缘按下鼠标时启动手动窗口拖拽/调整
//   WM_MOUSEMOVE — 拖拽过程中持续更新窗口位置/尺寸
//   WM_LBUTTONUP / WM_NCLBUTTONUP — 释放鼠标时结束拖拽
//   WM_CAPTURECHANGED — 捕获丢失时结束拖拽
//   WM_EXITSIZEMOVE — 窗口移动/缩放结束后确保在工作区内并保存位置
//   WM_DISPLAYCHANGE — 显示器分辨率或布局变更时重新适配
//   WM_DPICHANGED — DPI 变化时重新适配
//   WM_SIZE — 窗口尺寸变化时调整交换链后台缓冲区
//   WM_DESTROY — 设置退出标志并投递退出消息
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        // ---- 自定义非客户区命中测试 ----
        case WM_NCHITTEST:
        {
            POINT pt = { (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam) };
            ::ScreenToClient(hWnd, &pt);

            RECT rc;
            ::GetClientRect(hWnd, &rc);

            // 根据 DPI 计算边缘热区宽度
            const UINT dpi = GetDpiForWindowSafe(hWnd);
            const int border = ::MulDiv(RESIZE_BORDER_PX, (int)dpi, 96);
            const bool left = pt.x < rc.left + border;
            const bool right = pt.x >= rc.right - border;
            const bool top = pt.y < rc.top + border;
            const bool bottom = pt.y >= rc.bottom - border;

            // 四个角优先判定，然后判定四条边
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;

            // 顶部标题栏区域（用于拖拽移动窗口）
            if (pt.y >= rc.top && pt.y < rc.top + MulDiv(DRAG_BAR_HEIGHT_PX, dpi, 96))
                return HTCAPTION;

            return HTCLIENT;
        }

        // ---- 窗口最小/最大尺寸约束 ----
        case WM_GETMINMAXINFO:
        {
            MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            const UINT dpi = GetDpiForWindowSafe(hWnd);
            const int minW = ::MulDiv(MIN_OVERLAY_W, (int)dpi, 96);
            const int minH = ::MulDiv(MIN_OVERLAY_H, (int)dpi, 96);
            const RECT work = GetOverlayWorkArea(hWnd);
            const int maxW = OtherTools::MaxInt(minW, static_cast<int>((work.right - work.left) - WORKAREA_MARGIN_PX));
            const int maxH = OtherTools::MaxInt(minH, static_cast<int>((work.bottom - work.top) - WORKAREA_MARGIN_PX));
            mmi->ptMinTrackSize.x = minW;
            mmi->ptMinTrackSize.y = minH;
            if (maxW > 0) mmi->ptMaxTrackSize.x = maxW;
            if (maxH > 0) mmi->ptMaxTrackSize.y = maxH;
            return 0;
        }

        // ---- 非客户区鼠标左键按下：启动手动拖拽 ----
        case WM_NCLBUTTONDOWN:
            if (IsOverlayMoveOrResizeHit(wParam))
            {
                BeginManualOverlayWindowDrag(hWnd, wParam);
                return 0;
            }
            break;

        // ---- 鼠标移动：手动拖拽更新 ----
        case WM_MOUSEMOVE:
            if (g_manualWindowDragActive)
            {
                UpdateManualOverlayWindowDrag(hWnd);
                return 0;
            }
            break;

        // ---- 鼠标释放：结束手动拖拽 ----
        case WM_LBUTTONUP:
        case WM_NCLBUTTONUP:
            if (g_manualWindowDragActive)
            {
                EndManualOverlayWindowDrag(hWnd);
                return 0;
            }
            break;

        // ---- 捕获丢失：清理拖拽状态 ----
        case WM_CAPTURECHANGED:
            if (g_manualWindowDragActive)
            {
                g_manualWindowDragActive = false;
                g_manualWindowDragHit = HTNOWHERE;
                EnsureOverlayInsideWorkArea(hWnd, true);
                return 0;
            }
            break;
    }

    // 将消息传递给 ImGui 的 Win32 消息处理器（处理输入、焦点等）
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    // 窗口移动/缩放完成后：确保在工作区内并持久化位置
    case WM_EXITSIZEMOVE:
        g_autoResizeEnabled = false;
        EnsureOverlayInsideWorkArea(hWnd, true);
        return 0;

    // 显示器变更 / DPI 变更：重新适配窗口
    case WM_DISPLAYCHANGE:
        EnsureOverlayInsideWorkArea(hWnd, true);
        return 0;

    case WM_DPICHANGED:
        EnsureOverlayInsideWorkArea(hWnd, true);
        return 0;

    // 窗口大小变更：调整交换链后台缓冲区并重新渲染
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
        {
            const UINT width = (UINT)LOWORD(lParam);
            const UINT height = (UINT)HIWORD(lParam);

            if (ResizeOverlayBackBuffer(width, height) && g_overlayVisible)
                RenderOverlayFrame(false, false);
        }
        return 0;

    // 窗口销毁：触发退出流程
    case WM_DESTROY:
        shouldExit = true;
        ::PostQuitMessage(0);
        return 0;

    default:
        return ::DefWindowProc(hWnd, msg, wParam, lParam);
    }
}

// 初始化 ImGui 上下文和后端
// 1. 创建 ImGui 上下文
// 2. 加载字体（首选 Segoe UI Semibold，回退到 Segoe UI 或默认字体）
// 3. 初始化 Win32 和 D3D11 后端
// 4. 应用深色主题并捕获基础样式快照，供运行时缩放使用
// 5. 加载人体轮廓预览纹理
// 6. 处理在 ImGui 初始化前设置的挂起几何脏标记
void SetupImGui()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    ImGui::GetStyle().FontScaleMain = 1.0f;
    // 配置字体：过采样提高小字号清晰度，16.5pt
    ImFontConfig fontConfig{};
    fontConfig.OversampleH = 2;
    fontConfig.OversampleV = 1;
    fontConfig.PixelSnapH = true;

    // 加载中文字体（微软雅黑），同时支持英文和中文字符
    // Windows 11 上的中文字体通常位于 C:\Windows\Fonts\msyh.ttc
    // 但有些系统可能路径不同或使用 msyh.ttc 不同的版本
    ImFont* font = nullptr;

    // 方法1：直接路径
    font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 16.5f, &fontConfig);
    if (!font) // 方法2：试试 msyhbd
        font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyhbd.ttc", 16.5f, &fontConfig);
    if (!font) // 方法3：simsun
        font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\simsun.ttc", 16.0f, &fontConfig);
    if (!font) // 方法4：seguisb
        font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisb.ttf", 16.5f, &fontConfig);
    if (!font)
        io.Fonts->AddFontDefault();

    // 输出字体加载状态到控制台（用于调试）
    if (font)
        std::cout << "[Overlay] Font loaded successfully." << std::endl;
    else
        std::cout << "[Overlay] WARNING: No system font loaded, using default font." << std::endl;

    // 禁用 ImGui 的 .ini 文件读写
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    // 初始化 ImGui 平台/渲染后端
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // 应用 Neon Void 科幻主题，保存基础样式用于缩放
    ApplyTheme_NeonVoid();
    g_baseStyle = ImGui::GetStyle();
    g_baseStyleReady = true;
    g_runtimeUiScale = -1.0f;
    // 加载目标偏移设置界面中的人体轮廓预览图
    load_body_texture();

    // 如果窗口几何在 ImGui 初始化前被标记为脏，现在处理
    if (g_pendingOverlayGeometryDirty)
    {
        g_pendingOverlayGeometryDirty = false;
        OverlayConfig_MarkDirty();
    }
}

// 创建叠加层窗口
// 1. 从配置读取或使用默认值初始化窗口位置与尺寸
// 2. 裁剪窗口到工作区范围内
// 3. 注册窗口类（类名 "Chrome"），创建 WS_POPUP 无边框窗口
// 4. 启用 DWM 透明玻璃效果（扩展客户区边框）
// 5. 设置窗口透明度
// 6. 初始化 D3D11 设备、交换链和 DirectComposition
// 7. 应用捕获排除设置
// 返回 true 表示成功，false 表示失败
bool CreateOverlayWindow()
{
    // 从配置读取初始位置和尺寸
    int overlayX = config.overlay_x;
    int overlayY = config.overlay_y;
    overlayWidth = config.overlay_width > 0 ? config.overlay_width : BASE_OVERLAY_WIDTH;
    overlayHeight = config.overlay_height > 0 ? config.overlay_height : BASE_OVERLAY_HEIGHT;

    // 将初始尺寸限制在工作区范围内
    {
        int x = overlayX;
        int y = overlayY;
        int w = overlayWidth;
        int h = overlayHeight;
        ClampOverlayToWorkArea(nullptr, x, y, w, h);
        overlayX = x;
        overlayY = y;
        overlayWidth = w;
        overlayHeight = h;
    }

    // 注册窗口类，窗口过程为 WndProc
    WNDCLASSEX wc = {
        sizeof(WNDCLASSEX),
        CS_CLASSDC,
        WndProc,
        0L,
        0L,
        GetModuleHandle(NULL),
        NULL,
        NULL,
        NULL,
        NULL,
        _T("Chrome"),
        NULL
    };
    ::RegisterClassEx(&wc);

    // 扩展样式：置顶、工具窗口（不在任务栏显示）、分层（支持透明度）
    const DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED;
    const DWORD style = WS_POPUP;

    // 调整窗口矩形以包含扩展样式所需尺寸
    RECT wr = { overlayX, overlayY, overlayX + overlayWidth, overlayY + overlayHeight };
    ::AdjustWindowRectEx(&wr, style, FALSE, exStyle);

    const int wndW = wr.right - wr.left;
    const int wndH = wr.bottom - wr.top;

    // 创建窗口
    g_hwnd = ::CreateWindowEx(
        exStyle,
        wc.lpszClassName, _T("Chrome"),
        style,
        wr.left, wr.top, wndW, wndH,
        NULL, NULL, wc.hInstance, NULL);

    if (g_hwnd == NULL)
        return false;

    // 确保窗口在工作区内
    EnsureOverlayInsideWorkArea(g_hwnd, true);

    // 启用 DWM 透明：扩展客户区边框到标题栏区域，实现无边框透明效果
    BOOL dwm = FALSE;
    if (SUCCEEDED(DwmIsCompositionEnabled(&dwm)) && dwm)
    {
        MARGINS m = { -1, -1, -1, -1 };
        DwmExtendFrameIntoClientArea(g_hwnd, &m);
    }

    // 校验并修复不透明度值
    if (config.overlay_opacity < MIN_EDITOR_OPACITY)  config.overlay_opacity = MIN_EDITOR_OPACITY;
    if (config.overlay_opacity >= 256) config.overlay_opacity = 255;

    // 设置窗口透明度
    Overlay_SetOpacity(config.overlay_opacity);

    // 初始化 D3D11 设备和 DirectComposition
    if (!CreateDeviceD3D(g_hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClass(wc.lpszClassName, wc.hInstance);
        return false;
    }

    // 应用捕获排除（防止被录屏/截图捕获）
    Overlay_ApplyCaptureExclusion();

    return true;
}

// 渲染每一帧叠加层 UI
// 这是主要的绘制函数，每帧调用一次：
// 1. 检查可见性及 D3D11 资源是否就绪，防止重入
// 2. 应用运行时 UI 缩放
// 3. 开始 ImGui 帧，创建根窗口作为画布
// 4. 绘制主面板背景
// 5. 在左侧绘制侧边栏导航（含分组标题和选项卡按钮）
// 6. 在右侧绘制当前选项卡内容（调用对应的 draw 函数）
// 7. 尝试自动调整窗口尺寸并保存配置
// 8. 结束 ImGui 帧并提交渲染数据到 D3D11 后台缓冲区
// 9. 调用 Present 显示合成结果
// allowAutoResize: 是否允许根据内容自动缩放窗口
// allowConfigSave: 是否允许在此帧尝试保存配置
// 返回 Present 的 HRESULT 值
static HRESULT RenderOverlayFrame(bool allowAutoResize, bool allowConfigSave)
{
    // 不可见、未就绪或正在渲染时跳过，防止重入
    if (!g_overlayVisible || !g_pSwapChain || !g_pd3dDeviceContext || !g_mainRenderTargetView ||
        !ImGui::GetCurrentContext() || g_renderingOverlayFrame)
    {
        return S_FALSE;
    }

    const float w = static_cast<float>(overlayWidth);
    const float h = static_cast<float>(overlayHeight);
    if (w <= 0.0f || h <= 0.0f)
        return S_FALSE;

    // 设置重入标志
    g_renderingOverlayFrame = true;

    // 根据用户配置缩放 ImGui 样式
    ApplyRuntimeUiScale();

    // 开始 ImGui 新帧
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // 侧边栏宽度：窗口宽度的 28%，限制在 230~255 像素
    const float sidebarWidth = std::clamp(w * 0.28f, 230.0f, 255.0f);

    // 创建填充整个窗口的无装饰根容器
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    // 根窗口背景完全透明（由 DrawMainPanelBackground 手动绘制）
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
    ImGui::Begin("##editor_root", nullptr,
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();

    // 绘制主面板圆角矩形背景
    DrawMainPanelBackground(ImGui::GetWindowPos(), ImGui::GetWindowSize());

    {
        {
        std::lock_guard<std::mutex> lock(configMutex);

        // 验证当前选项卡索引在有效范围内
        const int tabCount = static_cast<int>(sizeof(kOverlayTabs) / sizeof(kOverlayTabs[0]));
        if (g_activeOverlayTab < 0 || g_activeOverlayTab >= tabCount)
            g_activeOverlayTab = 0;

        // ---- 左侧：侧边栏导航 ----
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
        ImGui::BeginChild("##options_nav", ImVec2(sidebarWidth, 0.0f),
            ImGuiChildFlags_AlwaysUseWindowPadding,
            ImGuiWindowFlags_NoScrollbar);

        DrawSidebarTitle();

        // 遍历所有选项卡，按分组（Vision / Aim / Control / Visuals / Monitor）显示标题和按钮
        const char* lastGroup = nullptr;
        for (int i = 0; i < tabCount; ++i)
        {
            const char* group = kOverlayTabs[i].group;
            if (!lastGroup || std::strcmp(lastGroup, group) != 0)
            {
                // 新分组开始 → 绘制分组标题
                if (lastGroup)
                    ImGui::Dummy(ImVec2(0.0f, 5.0f));
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(160, 140, 200, 230));
                // 将英文组名映射为中文显示
                const char* groupLabel = group;
                if (std::strcmp(group, "Perception") == 0) groupLabel = "感知";
                else if (std::strcmp(group, "Aim") == 0) groupLabel = "瞄准";
                else if (std::strcmp(group, "Control") == 0) groupLabel = "控制";
                else if (std::strcmp(group, "Display") == 0) groupLabel = "显示";
                else if (std::strcmp(group, "System") == 0) groupLabel = "系统";
                ImGui::TextUnformatted(groupLabel);
                ImGui::PopStyleColor();
            }
            // 绘制选项卡按钮，点击时切换 g_activeOverlayTab
            if (DrawSidebarTabButton(kOverlayTabs[i], g_activeOverlayTab == i))
                g_activeOverlayTab = i;
            lastGroup = group;
        }
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
        } // 释放 configMutex，后续 draw() 内部可能自行加锁

        ImGui::SameLine(0.0f, 12.0f);

        // ---- 右侧：选项卡内容区域 ----
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
        // 内容区域自动填充剩余宽度，支持垂直滚动
        ImGui::BeginChild("##options_content", ImVec2(0.0f, 0.0f),
            ImGuiChildFlags_AlwaysUseWindowPadding,
            ImGuiWindowFlags_AlwaysVerticalScrollbar);

        // 调用当前选项卡的绘制函数
        kOverlayTabs[g_activeOverlayTab].draw();

        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();

        // 如果允许，尝试保存配置（仅在更改时写入）
        if (allowConfigSave)
        {
            std::lock_guard<std::mutex> lock(configMutex);
            OverlayConfig_TrySave();
        }
    }

    ImGui::End();
    // 生成 ImGui 绘制命令
    ImGui::Render();

    // 清除后台缓冲区为全透明，然后渲染 ImGui 绘制数据
    const float clear_color_with_alpha[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, NULL);
    g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    // 提交到 DXGI 交换链（不垂直同步）
    const HRESULT result = g_pSwapChain->Present(0, 0);
    g_renderingOverlayFrame = false;
    return result;
}

// 叠加层主线程函数
// 负责创建窗口、初始化 ImGui、处理消息循环、响应用户热键切换叠加层显隐、
// 每帧渲染叠加层 UI，并在退出时清理所有资源
void OverlayThread()
{
    // 创建叠加层窗口（含 D3D11 初始化）
    if (!CreateOverlayWindow())
    {
        std::cout << "[Overlay] Can't create overlay window!" << std::endl;
        return;
    }

    // 初始化 ImGui 上下文、字体、样式及 D3D11 后端
    SetupImGui();

    // 叠加层当前是否显示
    bool show_overlay = false;

    // 构建按键名列表（用于热键绑定配置）
    for (const auto& pair : KeyCodes::key_code_map)
        key_names.push_back(pair.first);

    std::sort(key_names.begin(), key_names.end());
    key_names_cstrs.reserve(key_names.size());
    for (const auto& name : key_names)
        key_names_cstrs.push_back(name.c_str());

    // 获取可用 AI 模型列表
    availableModels = getAvailableModels();

    MSG msg;
    ZeroMemory(&msg, sizeof(msg));
    // 初始快照叠加层配置（热键、捕获排除等）
    OverlayThreadConfigSnapshot overlayCfg = SnapshotOverlayThreadConfig();
    bool lastExcludeFromCapture = overlayCfg.excludeFromCapture;
    bool overlayHotkeyWasDown = false;
    // 应用初始的捕获排除设置
    Overlay_SetDisplayAffinity(g_hwnd, lastExcludeFromCapture);

    // 主消息循环，持续运行直到收到退出信号
    while (!shouldExit)
    {
        // 处理 Windows 消息队列中的所有待处理消息
        while (::PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
            {
                shouldExit = true;
                break;
            }
        }
        if (shouldExit) break;

        // 刷新配置快照，检测捕获排除设置是否变化
        overlayCfg = SnapshotOverlayThreadConfig();

        if (lastExcludeFromCapture != overlayCfg.excludeFromCapture)
        {
            lastExcludeFromCapture = overlayCfg.excludeFromCapture;
            Overlay_SetDisplayAffinity(g_hwnd, lastExcludeFromCapture);
        }

        // 检测叠加层开关键是否按下（上升沿触发，切换显隐状态）
        const bool overlayHotkeyDown = isAnyKeyPressedWin32Only(overlayCfg.buttonOpenOverlay);
        if (overlayHotkeyDown && !overlayHotkeyWasDown)
        {
            show_overlay = !show_overlay;
            g_overlayVisible = show_overlay;

            if (show_overlay)
            {
                // 显示叠加层：重置自动缩放、确保在工作区内、显示窗口并置前
                g_autoResizeEnabled = true;
                EnsureOverlayInsideWorkArea(g_hwnd, true);
                ShowWindow(g_hwnd, SW_SHOW);
                SetForegroundWindow(g_hwnd);
            }
            else
            {
                // 隐藏叠加层：保存窗口位置尺寸，立即存储配置
                StoreOverlayWindowGeometry(g_hwnd, true);
                {
                    std::lock_guard<std::mutex> lock(configMutex);
                    OverlayConfig_SaveNow();
                }
                ShowWindow(g_hwnd, SW_HIDE);
            }
        }
        overlayHotkeyWasDown = overlayHotkeyDown;

        // 叠加层隐藏时跳过渲染，降低 CPU 占用
        if (!show_overlay)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // 渲染当前帧
        HRESULT result = RenderOverlayFrame(true, true);
        // 如果被遮挡或设备丢失，降低轮询频率
        if (result == DXGI_STATUS_OCCLUDED || result == DXGI_ERROR_ACCESS_LOST)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // 每帧间隔约 10ms（约 100 FPS 上限）
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 退出前保存窗口几何信息和最终配置
    {
        StoreOverlayWindowGeometry(g_hwnd, true);
        std::lock_guard<std::mutex> lock(configMutex);
        OverlayConfig_SaveNow();
    }

    // 释放资源：纹理、ImGui、D3D11 设备、窗口和窗口类
    release_body_texture();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(g_hwnd);
    ::UnregisterClass(_T("Chrome"), GetModuleHandle(NULL));
}

// 应用程序入口点（Windows 桌面应用程序的主函数）
// 创建叠加层线程并等待其结束，返回后退出进程
// hInstance: 当前应用程序实例句柄
// hPrevInstance: 保留参数，始终为 NULL
// lpCmdLine: 命令行参数字符串
// nCmdShow: 窗口显示方式（未使用）
int APIENTRY _tWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPTSTR    lpCmdLine,
    _In_ int       nCmdShow)
{
    std::thread overlay(OverlayThread);
    overlay.join();
    return 0;
}

