#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <dcomp.h>
#include <wrl/client.h>

#include <atomic>
#include <thread>
#include <mutex>
#include <memory>
#include <vector>
#include <unordered_map>
#include <string>
#include <cstdint>
#include <chrono>
#include <iostream>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dcomp.lib")

#include "Game_overlay.h"

using Microsoft::WRL::ComPtr;

// 获取虚拟屏幕的坐标和尺寸（多显示器环境下包含所有显示器）
// 参数 x, y: 虚拟屏幕左上角坐标; w, h: 虚拟屏幕的宽度和高度
static void GetVirtualScreen(int& x, int& y, int& w, int& h)
{
    x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

// 设置每显示器高DPI感知 v2（Per-Monitor Aware V2）
// 使窗口能够根据每个显示器的独立DPI进行缩放，确保渲染清晰
static void SetPerMonitorV2DpiAwareness()
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return;
    using Fn = BOOL(WINAPI*)(HANDLE);
    if (auto p = reinterpret_cast<Fn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")))
        p(reinterpret_cast<HANDLE>(-4)); // PER_MONITOR_AWARE_V2
}

// 将32位ARGB颜色值转换为Direct2D所需的 D2D1_COLOR_F 结构
// 参数 argb: 格式为 0xAARRGGBB 的32位颜色值
// 返回: 包含归一化浮点数颜色分量的 D2D1_COLOR_F 结构
static D2D1_COLOR_F ToD2DColor(OverlayColor argb)
{
    float a = float((argb >> 24) & 0xFF) / 255.f;
    float r = float((argb >> 16) & 0xFF) / 255.f;
    float g = float((argb >> 8) & 0xFF) / 255.f;
    float b = float((argb >> 0) & 0xFF) / 255.f;
    return D2D1::ColorF(r, g, b, a);
}

// 绘制命令结构体，表示一条独立的绘制指令
// 包含命令类型、颜色、线条粗细以及联合体参数
struct DrawCmd
{
    // 命令类型枚举：线段、矩形、填充矩形、圆、填充圆、文本、图像
    enum Type { Line, Rect, RectFilled, Circle, CircleFilled, Text, Image } type;
    OverlayColor color = 0;          // 绘制颜色（ARGB格式）
    float thickness = 1.0f;          // 线条宽度（仅对描边类型有效）
    union
    {
        OverlayLine line;            // 线段参数（起点和终点坐标）
        OverlayRect rect;            // 矩形参数（位置和尺寸）
        OverlayCircle circle;        // 圆参数（圆心和半径）
        struct { float x, y, w, h, opacity; int imageId; } image;   // 图像参数
        struct { float x, y, size; } textPos;   // 文本位置参数
    };
    std::wstring text;               // 文本内容（仅文本命令使用）
    std::wstring fontName;           // 字体名称（仅文本命令使用）
};

// 绘制列表结构体，包含一组待渲染的绘制命令
// 通过双缓冲机制在主线程和渲染线程之间传递
struct DrawList
{
    std::vector<DrawCmd> cmds;       // 绘制命令数组
};

// 图像资源结构体，管理加载的图像数据及其D2D位图
struct ImageRes
{
    ComPtr<ID2D1Bitmap1> bmp;        // Direct2D位图对象
    float w = 0.f;                   // 图像宽度（像素）
    float h = 0.f;                   // 图像高度（像素）
    std::vector<uint8_t> pending;    // 待更新的像素数据缓冲区
    int pendingW = 0;                // 待更新图像的宽度
    int pendingH = 0;                // 待更新图像的高度
    int pendingStride = 0;           // 待更新图像的行步幅（每行字节数）
    bool pendingDirty = false;       // 是否有待处理的像素数据更新
};

// Game_overlay 内部实现类，封装所有渲染细节
// 使用Pimpl（Pointer to Implementation）惯用法隐藏实现
struct Game_overlay::Impl
{
    HINSTANCE hinst = nullptr;       // 应用程序实例句柄
    HWND hwnd = nullptr;             // 覆盖层窗口句柄
    int winX = 0, winY = 0, winW = 0, winH = 0;  // 窗口位置和尺寸
    bool useVirtual = true;          // 是否使用虚拟屏幕尺寸

    // Direct3D 11 设备和资源
    ComPtr<ID3D11Device>           d3d;           // D3D11 设备
    ComPtr<ID3D11DeviceContext>    d3dCtx;        // D3D11 设备上下文
    ComPtr<IDXGISwapChain1>        swapChain;     // DXGI 交换链
    ComPtr<ID3D11RenderTargetView> rtv;           // D3D11 渲染目标视图

    // Direct2D 1.1 设备和资源
    ComPtr<ID2D1Factory1>          d2dFactory;    // D2D 工厂
    ComPtr<ID2D1Device>            d2dDevice;     // D2D 设备
    ComPtr<ID2D1DeviceContext>     d2dCtx;        // D2D 设备上下文
    ComPtr<ID2D1Bitmap1>           d2dTarget;     // D2D 渲染目标位图
    ComPtr<ID2D1SolidColorBrush>   brush;         // D2D 单色画刷

    // DirectWrite 文本渲染资源
    ComPtr<IDWriteFactory>         dwFactory;     // DirectWrite 工厂
    std::unordered_map<uint64_t, ComPtr<IDWriteTextFormat>> textFormatCache;  // 文本格式缓存

    ComPtr<IWICImagingFactory>     wic;           // WIC 图像处理工厂

    // DirectComposition 组合引擎资源
    ComPtr<IDCompositionDevice>    dcompDevice;   // DirectComposition 设备
    ComPtr<IDCompositionTarget>    dcompTarget;   // DirectComposition 渲染目标
    ComPtr<IDCompositionVisual>    dcompRoot;     // DirectComposition 根视觉对象

    std::mutex d2dMutex;                           // D2D 渲染互斥锁
    std::thread thread;                            // 渲染线程
    std::atomic<bool> running{ false };            // 渲染线程运行标志
    std::atomic<bool> visible{ true };             // 窗口可见性标志
    std::atomic<bool> excludeFromCapture{ true };  // 是否从屏幕捕获中排除
    bool appliedExcludeFromCapture = true;         // 当前生效的捕获排除状态
    std::atomic<unsigned> maxFps{ 0 };             // 最大帧率限制（0为不限制）

    std::mutex pendingMutex;                       // 待处理绘制命令的互斥锁
    DrawList pending;                              // 待处理的绘制列表
    std::shared_ptr<DrawList> current;             // 当前正在渲染的绘制列表（线程安全共享指针）

    std::mutex imgMutex;                           // 图像资源表的互斥锁
    int nextImageId = 1;                           // 下一个可用的图像ID
    std::unordered_map<int, ImageRes> images;      // 图像资源表

    void UseVirtualScreen() { useVirtual = true; }                 // 设置为使用虚拟屏幕尺寸
    void SetBounds(int x, int y, int w, int h) { useVirtual = false; winX = x; winY = y; winW = w; winH = h; }  // 设置固定窗口边界

    bool Start();          // 启动渲染线程
    void Stop();           // 停止渲染线程

    void BeginFrame();     // 开始一帧的绘制（清空待处理命令列表）
    void EndFrame();       // 结束一帧的绘制（将待处理命令提交到渲染线程）
    void AddCmd(const DrawCmd&);  // 添加一个绘制命令到待处理列表

    int  LoadImageFromFile(const std::wstring& path);       // 从文件加载图像
    void UnloadImage(int id);                                // 卸载指定ID的图像
    void DrawImage(int id, float x, float y, float w, float h, float opacity);  // 绘制图像
    int  UpdateImageFromBGRA(const void* data, int width, int height, int strideBytes, int imageId);  // 从BGRA数据更新图像

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);  // 窗口消息处理函数
    bool    CreateWindowAndDevices();      // 创建窗口和所有图形设备
    void    DestroyWindowAndDevices();     // 销毁窗口和释放所有图形设备
    void    RenderLoop();                  // 渲染主循环
    void    RenderOne();                   // 渲染一帧

    HRESULT EnsureWic();                   // 确保WIC工厂已创建
    HRESULT CreateTextFormat(const std::wstring& font, float size, IDWriteTextFormat** out);  // 创建文本格式（带缓存）
    HRESULT CreateTargets();               // 创建D2D/D3D渲染目标
    void    ReleaseTargets();              // 释放D2D/D3D渲染目标
    void    ApplyDisplayAffinity();        // 应用窗口显示亲和性（捕获排除）
};

// Game_overlay 构造函数，创建实现对象
Game_overlay::Game_overlay() : impl_(new Impl) {}
// Game_overlay 析构函数，确保渲染线程停止
Game_overlay::~Game_overlay() { Stop(); }

// ----- Game_overlay 公有包装方法 -----
// 这些方法将外部调用转发到内部实现对象

bool Game_overlay::Start() { return impl_->Start(); }               // 启动覆盖层
void Game_overlay::Stop() { impl_->Stop(); }                       // 停止覆盖层
bool Game_overlay::IsRunning() const { return impl_->running.load(); }  // 检查覆盖层是否在运行
void Game_overlay::SetVisible(bool v) { impl_->visible.store(v); }     // 设置可见性
bool Game_overlay::GetVisible() const { return impl_->visible.load(); } // 获取可见性
void Game_overlay::SetExcludeFromCapture(bool exclude) { impl_->excludeFromCapture.store(exclude); }  // 设置捕获排除
bool Game_overlay::GetExcludeFromCapture() const { return impl_->excludeFromCapture.load(); }         // 获取捕获排除状态
void Game_overlay::UseVirtualScreen() { impl_->UseVirtualScreen(); }      // 使用虚拟屏幕
void Game_overlay::SetWindowBounds(int x, int y, int w, int h) { impl_->SetBounds(x, y, w, h); }  // 设置窗口尺寸
void Game_overlay::SetMaxFPS(unsigned f) { impl_->maxFps.store(f); }     // 设置最大帧率

void Game_overlay::BeginFrame() { impl_->BeginFrame(); }  // 开始一帧
void Game_overlay::EndFrame() { impl_->EndFrame(); }      // 结束一帧

// 添加线段绘制命令
void Game_overlay::AddLine(const OverlayLine& l, OverlayColor c, float t)
{
    DrawCmd cmd{}; cmd.type = DrawCmd::Line; cmd.color = c; cmd.thickness = t; cmd.line = l;
    impl_->AddCmd(cmd);
}
// 添加矩形边框绘制命令
void Game_overlay::AddRect(const OverlayRect& r, OverlayColor c, float t)
{
    DrawCmd cmd{}; cmd.type = DrawCmd::Rect; cmd.color = c; cmd.thickness = t; cmd.rect = r;
    impl_->AddCmd(cmd);
}
// 添加填充矩形绘制命令
void Game_overlay::FillRect(const OverlayRect& r, OverlayColor c)
{
    DrawCmd cmd{}; cmd.type = DrawCmd::RectFilled; cmd.color = c; cmd.rect = r;
    impl_->AddCmd(cmd);
}
// 添加圆边框绘制命令
void Game_overlay::AddCircle(const OverlayCircle& c0, OverlayColor c, float t)
{
    DrawCmd cmd{}; cmd.type = DrawCmd::Circle; cmd.color = c; cmd.thickness = t; cmd.circle = c0;
    impl_->AddCmd(cmd);
}
// 添加填充圆绘制命令
void Game_overlay::FillCircle(const OverlayCircle& c0, OverlayColor c)
{
    DrawCmd cmd{}; cmd.type = DrawCmd::CircleFilled; cmd.color = c; cmd.circle = c0;
    impl_->AddCmd(cmd);
}
// 添加文本绘制命令，支持自定义字体和字号
void Game_overlay::AddText(float x, float y, const std::wstring& text,
    float sizePx, OverlayColor c, const std::wstring& font)
{
    DrawCmd cmd{}; cmd.type = DrawCmd::Text; cmd.color = c; cmd.text = text;
    cmd.textPos = { x, y, sizePx }; cmd.fontName = font;
    impl_->AddCmd(cmd);
}
int  Game_overlay::LoadImageFromFile(const std::wstring& path) { return impl_->LoadImageFromFile(path); }  // 从文件加载图像
void Game_overlay::UnloadImage(int id) { impl_->UnloadImage(id); }      // 卸载图像
void Game_overlay::DrawImage(int id, float x, float y, float w, float h, float op) { impl_->DrawImage(id, x, y, w, h, op); }  // 绘制图像
int  Game_overlay::UpdateImageFromBGRA(const void* data, int width, int height, int strideBytes, int imageId)
{
    return impl_->UpdateImageFromBGRA(data, width, height, strideBytes, imageId);
}

// Impl::Start - 启动渲染线程
// 初始化COM，创建窗口和设备，进入渲染循环
bool Game_overlay::Impl::Start()
{
    if (running.load()) return true;
    running = true;
    thread = std::thread([this] {
        SetPerMonitorV2DpiAwareness();           // 设置高DPI感知
        HRESULT cohr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // 初始化COM（多线程模式）
        const bool coinit_ok = SUCCEEDED(cohr);
        try
        {
            hinst = GetModuleHandleW(nullptr);   // 获取模块句柄
            if (!CreateWindowAndDevices()) {      // 创建窗口和图形设备
                running = false;
                if (coinit_ok) CoUninitialize();
                return;
            }
            RenderLoop();                         // 进入渲染主循环
        }
        catch (const std::exception& e)
        {
            std::cerr << "[GameOverlay] Render thread crashed: " << e.what() << std::endl;
        }
        catch (...)
        {
            std::cerr << "[GameOverlay] Render thread crashed: unknown exception." << std::endl;
        }
        DestroyWindowAndDevices();                // 清理资源
        if (coinit_ok) CoUninitialize();          // 反初始化COM
        running = false;
        });
    return true;
}

// Impl::Stop - 停止渲染线程
// 通过 atomic 标志通知线程退出，并发送关闭消息确保消息循环能退出
void Game_overlay::Impl::Stop()
{
    if (!running.exchange(false)) return;
    if (hwnd) PostMessageW(hwnd, WM_CLOSE, 0, 0);
    if (thread.joinable()) thread.join();
}

// Impl::BeginFrame - 开始一帧的绘制
// 清空待处理的绘制命令列表，准备新的绘制帧
void Game_overlay::Impl::BeginFrame()
{
    std::lock_guard<std::mutex> lk(pendingMutex);
    pending.cmds.clear();
}

// Impl::EndFrame - 结束一帧的绘制
// 将当前帧的所有绘制命令打包为一个 DrawList 并原子地交换给渲染线程
void Game_overlay::Impl::EndFrame()
{
    auto dl = std::make_shared<DrawList>();
    {
        std::lock_guard<std::mutex> lk(pendingMutex);
        dl->cmds = pending.cmds;
    }
    std::atomic_store_explicit(&current, dl, std::memory_order_release);
}

// Impl::AddCmd - 添加一个绘制命令到待处理列表
// 线程安全：通过 pendingMutex 保护
void Game_overlay::Impl::AddCmd(const DrawCmd& c)
{
    std::lock_guard<std::mutex> lk(pendingMutex);
    pending.cmds.push_back(c);
}

// Impl::EnsureWic - 确保 WIC（Windows Imaging Component）工厂已创建
// 返回 S_OK 表示成功，否则返回 COM 错误码
HRESULT Game_overlay::Impl::EnsureWic()
{
    if (wic) return S_OK;
    return CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
}

// Impl::LoadImageFromFile - 从图像文件加载图像
// 使用WIC解码图像文件，转换为D2D位图，并注册到图像资源表
// 参数 path: 图像文件路径
// 返回: 图像ID（失败返回 0）
int Game_overlay::Impl::LoadImageFromFile(const std::wstring& path)
{
    if (path.empty()) return 0;
    if (FAILED(EnsureWic())) return 0;

    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &dec))) return 0;

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return 0;

    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv))) return 0;

    // 首先尝试转换为预乘Alpha格式（PBGRA）
    bool premultiplied = true;
    if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.f, WICBitmapPaletteTypeCustom)))
    {
        premultiplied = false;
        // 如果预乘Alpha格式转换失败，退回到普通BGRA格式
        if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.f, WICBitmapPaletteTypeCustom)))
            return 0;
    }

    if (!d2dCtx) return 0;

    std::lock_guard<std::mutex> d2dLock(d2dMutex);
    D2D1_BITMAP_PROPERTIES1 props =
        D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                premultiplied ? D2D1_ALPHA_MODE_PREMULTIPLIED : D2D1_ALPHA_MODE_IGNORE),
            96.f, 96.f);

    ComPtr<ID2D1Bitmap1> bmp;
    if (FAILED(d2dCtx->CreateBitmapFromWicBitmap(conv.Get(), &props, &bmp))) return 0;

    auto sz = bmp->GetSize();
    ImageRes ir;
    ir.bmp = bmp;
    ir.w = sz.width;
    ir.h = sz.height;

    std::lock_guard<std::mutex> il(imgMutex);
    int id = nextImageId++;
    images.emplace(id, std::move(ir));
    return id;
}

// Impl::UnloadImage - 卸载指定ID的图像资源
// 线程安全：通过 imgMutex 保护
void Game_overlay::Impl::UnloadImage(int id)
{
    std::lock_guard<std::mutex> il(imgMutex);
    images.erase(id);
}

// Impl::DrawImage - 添加图像绘制命令到待处理列表
// 参数 id: 图像ID; x,y,w,h: 绘制位置和尺寸; opacity: 不透明度(0.0-1.0)
void Game_overlay::Impl::DrawImage(int id, float x, float y, float w, float h, float opacity)
{
    DrawCmd c{};
    c.type = DrawCmd::Image;
    c.image = { x, y, w, h, opacity, id };
    AddCmd(c);
}

// Impl::UpdateImageFromBGRA - 从BGRA像素数据更新图像
// 支持更新现有图像或创建新图像，数据会被拷贝到内部缓冲区供渲染线程使用
// 参数 data: BGRA像素数据; width,height: 图像尺寸; strideBytes: 行步幅; imageId: 已有图像的ID（0表示新建）
// 返回: 图像ID
int Game_overlay::Impl::UpdateImageFromBGRA(const void* data, int width, int height, int strideBytes, int imageId)
{
    if (!data || width <= 0 || height <= 0 || strideBytes <= 0)
        return 0;

    std::lock_guard<std::mutex> il(imgMutex);
    ImageRes* target = nullptr;
    if (imageId != 0)
    {
        auto it = images.find(imageId);
        if (it != images.end())
            target = &it->second;
    }
    // 如果未指定有效的imageId，创建一个新的图像资源
    if (!target)
    {
        imageId = nextImageId++;
        auto [it, _] = images.emplace(imageId, ImageRes{});
        target = &it->second;
    }

    target->pendingW = width;
    target->pendingH = height;
    target->pendingStride = strideBytes;
    target->pending.resize(static_cast<size_t>(strideBytes) * static_cast<size_t>(height));
    const uint8_t* src = static_cast<const uint8_t*>(data);
    for (int y = 0; y < height; ++y)
    {
        memcpy(target->pending.data() + static_cast<size_t>(y) * strideBytes,
            src + static_cast<size_t>(y) * strideBytes,
            strideBytes);
    }
    target->pendingDirty = true;  // 标记有数据需要更新到D2D位图
    return imageId;
}

// Impl::WndProc - 覆盖层窗口的窗口过程函数
// 处理窗口创建、命中测试、尺寸变化和销毁等消息
LRESULT CALLBACK Game_overlay::Impl::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    Impl* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (msg)
    {
    case WM_NCCREATE:
    {
        // 窗口创建时将 Impl 指针存入窗口的用户数据区
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return TRUE;
    }
    case WM_NCHITTEST:     return HTTRANSPARENT;  // 所有鼠标点击穿透到下层窗口
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;   // 不允许鼠标激活窗口
    case WM_SIZE:
        // 窗口大小改变时重建渲染目标和交换链缓冲区
        if (self && self->swapChain)
        {
            UINT w = LOWORD(lParam), h = HIWORD(lParam);
            if (w > 0 && h > 0)
            {
                self->ReleaseTargets();
                self->swapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
                self->CreateTargets();
                if (self->dcompDevice) self->dcompDevice->Commit();
            }
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);  // 窗口销毁时发送退出消息
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// Impl::CreateWindowAndDevices - 创建覆盖层窗口和所有图形设备
// 依次创建：窗口 -> D3D11 设备 -> DXGI 交换链 -> DirectComposition -> D2D -> DirectWrite
// 初始化成功后渲染线程将进入渲染循环
bool Game_overlay::Impl::CreateWindowAndDevices()
{
    // 确定窗口尺寸：使用虚拟屏幕或用户指定的固定尺寸
    if (useVirtual) GetVirtualScreen(winX, winY, winW, winH);
    else if (winW <= 0 || winH <= 0) GetVirtualScreen(winX, winY, winW, winH);

    // 注册覆盖层窗口类
    WNDCLASSEXW wc{ sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = Game_overlay::Impl::WndProc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"GameOverlayDCompWnd";
    wc.hbrBackground = nullptr;
    if (!RegisterClassExW(&wc))
        return false;

    // 创建透明、置顶、不可激活的覆盖窗口
    DWORD ex = WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED;
    hwnd = CreateWindowExW(ex, wc.lpszClassName, L"", WS_POPUP,
        winX, winY, winW, winH, nullptr, nullptr, hinst, this);
    if (!hwnd) return false;

    // 如果DWM合成已启用，将窗口客户区扩展到边框区域以实现透明效果
    BOOL dwm = FALSE;
    if (SUCCEEDED(DwmIsCompositionEnabled(&dwm)) && dwm)
    {
        MARGINS m = { -1, -1, -1, -1 };
        DwmExtendFrameIntoClientArea(hwnd, &m);
    }

    ShowWindow(hwnd, SW_SHOWNA);     // 显示窗口但不激活
    ApplyDisplayAffinity();           // 应用捕获排除设置
    SetWindowPos(hwnd, HWND_TOPMOST, winX, winY, winW, winH,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);

    // 创建 D3D11 设备（启用BGRA支持，调试模式下添加调试层）
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL flOut{};
    if (FAILED(D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        featureLevels, _countof(featureLevels),
        D3D11_SDK_VERSION, &d3d, &flOut, &d3dCtx)))
    {
        return false;
    }

    // 获取 DXGI 工厂，用于创建交换链
    ComPtr<IDXGIDevice> dxgiDev; d3d.As(&dxgiDev);
    ComPtr<IDXGIAdapter> adapter; dxgiDev->GetAdapter(&adapter);
    ComPtr<IDXGIFactory2> factory2;
    {
        ComPtr<IDXGIFactory> baseFactory;
        adapter->GetParent(IID_PPV_ARGS(&baseFactory));
        baseFactory.As(&factory2);
    }
    if (!factory2) return false;

    // 配置交换链描述：双缓冲、预乘Alpha、用于DirectComposition
    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = winW;
    scd.Height = winH;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    scd.Scaling = DXGI_SCALING_STRETCH;

    // 创建用于 DirectComposition 的交换链
    if (FAILED(factory2->CreateSwapChainForComposition(
        d3d.Get(), &scd, nullptr, &swapChain)))
    {
        return false;
    }

    // 创建 DirectComposition 设备并建立视觉树：交换链 -> 根视觉 -> 窗口
    if (FAILED(DCompositionCreateDevice(
        dxgiDev.Get(), IID_PPV_ARGS(&dcompDevice))))
    {
        return false;
    }
    if (FAILED(dcompDevice->CreateTargetForHwnd(hwnd, TRUE, &dcompTarget))) return false;
    if (FAILED(dcompDevice->CreateVisual(&dcompRoot))) return false;
    if (FAILED(dcompRoot->SetContent(swapChain.Get()))) return false;
    if (FAILED(dcompTarget->SetRoot(dcompRoot.Get()))) return false;
    dcompDevice->Commit();

    // 创建 D2D 渲染目标
    if (FAILED(CreateTargets())) return false;

    // 创建默认画刷并设置抗锯齿模式
    if (FAILED(d2dCtx->CreateSolidColorBrush(
        D2D1::ColorF(1.f, 1.f, 1.f, 1.f), &brush))) return false;
    d2dCtx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    d2dCtx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

    // 创建 DirectWrite 工厂
    if (FAILED(DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(dwFactory.GetAddressOf()))))
    {
        return false;
    }

    // 初始化一个空的绘制列表作为当前帧
    std::atomic_store_explicit(
        &current, std::make_shared<DrawList>(),
        std::memory_order_release);

    return true;
}

// Impl::DestroyWindowAndDevices - 销毁窗口并释放所有图形设备资源
// 按依赖关系逆序释放：D2D -> D3D -> DirectComposition -> DirectWrite -> WIC -> 窗口
void Game_overlay::Impl::DestroyWindowAndDevices()
{
    ReleaseTargets();
    brush.Reset();
    d2dCtx.Reset();
    d2dDevice.Reset();
    d2dFactory.Reset();
    rtv.Reset();
    swapChain.Reset();
    dcompRoot.Reset();
    dcompTarget.Reset();
    dcompDevice.Reset();
    dwFactory.Reset();
    wic.Reset();
    d3dCtx.Reset();
    d3d.Reset();

    if (hwnd) { DestroyWindow(hwnd); hwnd = nullptr; }
    UnregisterClassW(L"GameOverlayDCompWnd", hinst);
}

// Impl::CreateTargets - 创建 D2D 和 D3D 渲染目标
// 从交换链后台缓冲区创建 D3D 渲染目标视图和 D2D 位图渲染目标
HRESULT Game_overlay::Impl::CreateTargets()
{
    ReleaseTargets();

    // 获取交换链的后台缓冲区纹理
    ComPtr<ID3D11Texture2D> bb;
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&bb))))
        return E_FAIL;

    // 创建 D3D11 渲染目标视图
    if (FAILED(d3d->CreateRenderTargetView(bb.Get(), nullptr, &rtv)))
        return E_FAIL;

    // 按需创建 D2D 工厂（多线程模式）
    if (!d2dFactory)
    {
        D2D1_FACTORY_OPTIONS opts{};
#if defined(_DEBUG)
        opts.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
        if (FAILED(D2D1CreateFactory(
            D2D1_FACTORY_TYPE_MULTI_THREADED,
            __uuidof(ID2D1Factory1),
            &opts,
            reinterpret_cast<void**>(d2dFactory.GetAddressOf()))))
            return E_FAIL;
    }

    // 按需创建 D2D 设备
    if (!d2dDevice)
    {
        ComPtr<IDXGIDevice> dxgiDev; d3d.As(&dxgiDev);
        if (FAILED(d2dFactory->CreateDevice(dxgiDev.Get(), &d2dDevice)))
            return E_FAIL;
    }

    // 按需创建 D2D 设备上下文
    if (!d2dCtx)
    {
        if (FAILED(d2dDevice->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dCtx)))
            return E_FAIL;
    }

    // 从 DXGI 表面创建 D2D 位图渲染目标
    ComPtr<IDXGISurface> surf;
    if (FAILED(bb.As(&surf))) return E_FAIL;

    D2D1_BITMAP_PROPERTIES1 props =
        D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET |
            D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(
                DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.f, 96.f
        );

    if (FAILED(d2dCtx->CreateBitmapFromDxgiSurface(
        surf.Get(), &props, &d2dTarget)))
        return E_FAIL;

    d2dCtx->SetTarget(d2dTarget.Get());

    return S_OK;
}

// Impl::ReleaseTargets - 释放 D2D/D3D 渲染目标
// 在窗口大小改变或设备丢失时需要释放并重建
void Game_overlay::Impl::ReleaseTargets()
{
    if (d2dCtx) d2dCtx->SetTarget(nullptr);
    d2dTarget.Reset();
    rtv.Reset();
}

// Impl::CreateTextFormat - 创建文本格式对象（带缓存）
// 通过哈希表缓存已创建的 IDWriteTextFormat 对象
// 参数 font: 字体名称; size: 字号; out: 输出文本格式指针
HRESULT Game_overlay::Impl::CreateTextFormat(
    const std::wstring& font, float size,
    IDWriteTextFormat** out)
{
    // 生成缓存键：字体名称哈希和字号组合
    uint64_t key =
        (std::hash<std::wstring>{}(font) ^ (uint64_t(std::lround(size * 100)) << 1));

    // 查找缓存，命中直接返回
    auto it = textFormatCache.find(key);
    if (it != textFormatCache.end())
    {
        *out = it->second.Get();
        (*out)->AddRef();
        return S_OK;
    }

    ComPtr<IDWriteTextFormat> fmt;
    if (FAILED(dwFactory->CreateTextFormat(
        font.c_str(), nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        size, L"en-US", &fmt)))
        return E_FAIL;

    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);  // 禁止自动换行
    textFormatCache.emplace(key, fmt);
    *out = fmt.Detach();
    return S_OK;
}

// Impl::ApplyDisplayAffinity - 应用窗口显示亲和性设置
// 将窗口标记为从屏幕捕获（截图/录屏）中排除，防止覆盖层被捕获
void Game_overlay::Impl::ApplyDisplayAffinity()
{
    if (!hwnd)
        return;

    const bool wantedExclude = excludeFromCapture.load();
    appliedExcludeFromCapture = wantedExclude;
    const DWORD affinity = wantedExclude ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE;
    if (SetWindowDisplayAffinity(hwnd, affinity))
        return;

    // 如果 WDA_EXCLUDEFROMCAPTURE 失败（旧系统不支持），回退到 WDA_MONITOR
    if (wantedExclude)
    {
        const DWORD err = GetLastError();
        std::cerr << "[GameOverlay] SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) failed, err=" << err
                  << ". Trying WDA_MONITOR fallback." << std::endl;
        if (!SetWindowDisplayAffinity(hwnd, WDA_MONITOR))
        {
            std::cerr << "[GameOverlay] SetWindowDisplayAffinity(WDA_MONITOR) failed, err="
                      << GetLastError() << std::endl;
        }
    }
}

// Impl::RenderLoop - 渲染主循环
// 处理窗口消息，按帧率限制循环调用 RenderOne 渲染帧
void Game_overlay::Impl::RenderLoop()
{
    running = true;
    auto last = std::chrono::high_resolution_clock::now();
    appliedExcludeFromCapture = !excludeFromCapture.load();

    MSG msg{};
    while (running.load())
    {
        // 处理窗口消息队列
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running.load()) break;

        // 帧率限制：如果设置了最大FPS，确保帧间隔不小于 1,000,000/maxFps 微秒
        unsigned cap = maxFps.load();
        if (cap > 0)
        {
            auto now = std::chrono::high_resolution_clock::now();
            auto minDelta = std::chrono::microseconds(1'000'000 / cap);
            if (now - last < minDelta) { Sleep(1); continue; }
            last = now;
        }

        // 检查捕获排除状态是否变化，变化则重新应用
        if (appliedExcludeFromCapture != excludeFromCapture.load())
            ApplyDisplayAffinity();
        RenderOne();
    }
}

// Impl::RenderOne - 渲染一帧画面
// 处理图像更新、执行所有绘制命令、提交到交换链
void Game_overlay::Impl::RenderOne()
{
    if (!d2dCtx || !swapChain) return;

    // 可见性控制：不可见时隐藏窗口并休眠
    if (!visible.load())
    {
        ShowWindow(hwnd, SW_HIDE);
        Sleep(10);
        return;
    }
    else
    {
        ShowWindow(hwnd, SW_SHOWNA);
    }

    std::lock_guard<std::mutex> d2dLock(d2dMutex);
    d2dCtx->BeginDraw();
    d2dCtx->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 0.f));  // 清除为全透明

    // 处理所有图像的挂起更新（从BGRA数据创建或更新D2D位图）
    {
        std::lock_guard<std::mutex> il(imgMutex);
        for (auto& kv : images)
        {
            auto& img = kv.second;
            if (!img.pendingDirty || img.pending.empty())
                continue;

            D2D1_BITMAP_PROPERTIES1 props =
                D2D1::BitmapProperties1(
                    D2D1_BITMAP_OPTIONS_NONE,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                    96.f, 96.f);

            // 如果尺寸变化或尚未创建位图，重新创建；否则直接拷贝像素数据
            if (!img.bmp || img.w != img.pendingW || img.h != img.pendingH)
            {
                ComPtr<ID2D1Bitmap1> bmp;
                HRESULT hr = d2dCtx->CreateBitmap(
                    D2D1::SizeU(static_cast<UINT32>(img.pendingW), static_cast<UINT32>(img.pendingH)),
                    img.pending.data(),
                    static_cast<UINT32>(img.pendingStride),
                    props,
                    &bmp);
                if (FAILED(hr))
                    continue;

                img.bmp = bmp;
                img.w = static_cast<float>(img.pendingW);
                img.h = static_cast<float>(img.pendingH);
            }
            else
            {
                if (FAILED(img.bmp->CopyFromMemory(nullptr, img.pending.data(), static_cast<UINT32>(img.pendingStride))))
                    continue;
            }

            img.pendingDirty = false;  // 清除更新标记
        }
    }

    // 获取当前帧的绘制命令列表并执行所有绘制命令
    auto dl = std::atomic_load_explicit(&current, std::memory_order_acquire);
    if (dl)
    {
        for (const auto& c : dl->cmds)
        {
            switch (c.type)
            {
            case DrawCmd::Line:
                // 绘制线段
                brush->SetColor(ToD2DColor(c.color));
                d2dCtx->DrawLine(
                    { c.line.x1, c.line.y1 },
                    { c.line.x2, c.line.y2 },
                    brush.Get(), c.thickness);
                break;

            case DrawCmd::Rect:
                // 绘制矩形边框
                brush->SetColor(ToD2DColor(c.color));
                d2dCtx->DrawRectangle(
                    D2D1::RectF(c.rect.x, c.rect.y,
                        c.rect.x + c.rect.w, c.rect.y + c.rect.h),
                    brush.Get(), c.thickness);
                break;

            case DrawCmd::RectFilled:
                // 绘制填充矩形
                brush->SetColor(ToD2DColor(c.color));
                d2dCtx->FillRectangle(
                    D2D1::RectF(c.rect.x, c.rect.y,
                        c.rect.x + c.rect.w, c.rect.y + c.rect.h),
                    brush.Get());
                break;

            case DrawCmd::Circle:
                // 绘制圆形边框
                brush->SetColor(ToD2DColor(c.color));
                d2dCtx->DrawEllipse(
                    D2D1::Ellipse({ c.circle.cx, c.circle.cy }, c.circle.r, c.circle.r),
                    brush.Get(), c.thickness);
                break;

            case DrawCmd::CircleFilled:
                // 绘制填充圆形
                brush->SetColor(ToD2DColor(c.color));
                d2dCtx->FillEllipse(
                    D2D1::Ellipse({ c.circle.cx, c.circle.cy }, c.circle.r, c.circle.r),
                    brush.Get());
                break;

            case DrawCmd::Text:
            {
                // 绘制文本（使用默认字体或自定义字体）
                brush->SetColor(ToD2DColor(c.color));
                ComPtr<IDWriteTextFormat> fmt;
                if (SUCCEEDED(CreateTextFormat(
                    c.fontName.empty() ? L"Segoe UI" : c.fontName,
                    c.textPos.size, &fmt)))
                {
                    D2D1_RECT_F layout = D2D1::RectF(
                        c.textPos.x, c.textPos.y,
                        c.textPos.x + 4000.f, c.textPos.y + 1200.f);
                    d2dCtx->DrawTextW(
                        c.text.c_str(),
                        (UINT32)c.text.size(),
                        fmt.Get(),
                        layout,
                        brush.Get());
                }
            } break;

            case DrawCmd::Image:
            {
                // 绘制图像
                std::lock_guard<std::mutex> lk(imgMutex);
                auto it = images.find(c.image.imageId);
                if (it != images.end() && it->second.bmp)
                {
                    D2D1_RECT_F dst = D2D1::RectF(
                        c.image.x, c.image.y,
                        c.image.x + c.image.w,
                        c.image.y + c.image.h);
                    d2dCtx->DrawBitmap(
                        it->second.bmp.Get(),
                        &dst,
                        c.image.opacity,
                        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                        nullptr);
                }
            } break;
            }
        }
    }

    // 结束D2D绘制，如果失败则重建渲染目标
    HRESULT hrEnd = d2dCtx->EndDraw();
    if (FAILED(hrEnd))
    {
        CreateTargets();
    }

    // 提交渲染结果到交换链，显示到屏幕
    swapChain->Present(0, 0);
}
