#ifndef WINRT_CAPTURE_H
#define WINRT_CAPTURE_H

// WinRT 屏幕捕获类
// 使用 Windows.Graphics.Capture API 进行现代 Windows 屏幕采集（支持 UWP 和游戏窗口）

#include <opencv2/opencv.hpp>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mutex>
#include <string>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.System.Threading.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/base.h>
#include <comdef.h>

#include "capture.h"

class WinRTScreenCapture : public IScreenCapture
{
public:
    // WinRT 捕获选项
    struct Options
    {
        std::string target;            // 捕获目标标识
        std::string windowTitle;       // 目标窗口标题
        int monitorIndex = 0;          // 显示器索引
        bool captureBorders = true;    // 是否捕获窗口边框
        bool captureCursor = true;     // 是否捕获鼠标指针
    };

    WinRTScreenCapture(int desiredWidth, int desiredHeight, const Options& options);
    ~WinRTScreenCapture();

    cv::Mat GetNextFrameCpu() override;

private:
    winrt::com_ptr<ID3D11Device>         d3dDevice;
    winrt::com_ptr<ID3D11DeviceContext>  d3dContext;

    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice device{ nullptr };

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem              captureItem{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool       framePool{ nullptr };
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession           session{ nullptr };

    winrt::com_ptr<ID3D11Texture2D> sharedTexture;          // GPU 共享纹理

    winrt::com_ptr<ID3D11Texture2D> stagingTextureCPU;      // CPU 回读纹理

    bool useCuda = false;

    int screenWidth = 0;
    int screenHeight = 0;
    int desiredRegionWidth = 0;
    int desiredRegionHeight = 0;
    int regionWidth = 0;
    int regionHeight = 0;
    int regionX = 0;
    int regionY = 0;

    bool createStagingTextureCPU();

    // 创建指定显示器的捕获项
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem
        CreateCaptureItemForMonitor(HMONITOR hMonitor);

    // 创建指定窗口的捕获项
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem
        CreateCaptureItemForWindow(HWND hWnd);

    // 从 DXGI 设备创建 Direct3D 设备
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice
        CreateDirect3DDevice(IDXGIDevice* dxgiDevice);

    // 从 WinRT 对象获取 DXGI 接口
    template<typename T>
    winrt::com_ptr<T> GetDXGIInterfaceFromObject(
        winrt::Windows::Foundation::IInspectable const& object);

    // 根据窗口标题子字符串查找窗口句柄
    static HWND FindWindowByTitleSubstring(const std::string& title_substr);
};

#endif // WINRT_CAPTURE_H
