#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <Windows.h>

#include <string>
#include <iostream>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <algorithm>
#include <array>
#include <fstream>
#include <random>
#include <set>
#include <utility>
#include <vector>

#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <comdef.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <VersionHelpers.h>

#include "other_tools.h"
#include "config.h"
#include "Xen.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

// Base64 编码字符表（标准 RFC 4648）
static const std::string base64_chars =
"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
"abcdefghijklmnopqrstuvwxyz"
"0123456789+/";

// 判断字符是否为有效的 Base64 字符（字母数字或 '+' 或 '/'）
static inline bool is_base64(unsigned char c)
{
    return (isalnum(c) || (c == '+') || (c == '/'));
}

// COM 初始化/清理的 RAII 包装类
// 构造函数调用 CoInitializeEx，析构函数调用 CoUninitialize
class ScopedComInit
{
public:
    explicit ScopedComInit(DWORD coinit) noexcept
        : hr(CoInitializeEx(nullptr, coinit))
    {
    }

    ~ScopedComInit()
    {
        if (hr == S_OK || hr == S_FALSE)
        {
            CoUninitialize();
        }
    }

    bool ok() const noexcept
    {
        return hr == S_OK || hr == S_FALSE || hr == RPC_E_CHANGED_MODE;
    }

    HRESULT hr;
};

// 将 UTF-8 字符串转换为 std::filesystem::path
// 使用 C++17 的 u8path，失败时回退到普通构造
static std::filesystem::path MakePathFromUtf8(const char* filename)
{
    if (!filename || !*filename)
    {
        return {};
    }

    try
    {
        return std::filesystem::u8path(filename);
    }
    catch (...)
    {
        return std::filesystem::path(filename);
    }
}

// 将整个文件读入字节缓冲区
// 用于后续加载模型文件或图像文件
static bool ReadFileToBuffer(const std::filesystem::path& path, std::vector<unsigned char>& out)
{
    out.clear();

    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec)
    {
        return false;
    }

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        return false;
    }

    std::streamsize size = file.tellg();
    if (size <= 0)
    {
        return false;
    }

    out.resize(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(out.data()), size))
    {
        out.clear();
        return false;
    }

    return true;
}

namespace OtherTools
{
// 去除 ASCII 字符串首尾的空白字符（空格、制表符、回车、换行）
std::string TrimAscii(std::string s)
{
    const size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
        return {};

    const size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end == std::string::npos ? std::string::npos : (end - start + 1));
}

// 将字符串转换为小写（仅 ASCII 字符）
std::string ToLowerAscii(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// 不区分大小写地检查 haystack 中是否包含 needle
bool ContainsCaseInsensitive(const std::string& haystack, const std::string& needle)
{
    const std::string lowerHaystack = ToLowerAscii(haystack);
    const std::string lowerNeedle = ToLowerAscii(needle);
    return lowerHaystack.find(lowerNeedle) != std::string::npos;
}

// 不区分大小写地检查文件路径的扩展名是否匹配
bool HasExtensionCaseInsensitive(const std::string& path, const char* ext)
{
    if (!ext || !*ext)
        return false;

    const std::string current = ToLowerAscii(std::filesystem::path(path).extension().string());
    const std::string expected = ToLowerAscii(ext);
    return current == expected;
}
}

// 在指定目录中查找所有具有给定扩展名的模型文件
// 返回按文件名排序的文件列表
static std::vector<std::string> GetModelFilesByExtInDir(const std::string& dir, const std::vector<std::string>& exts)
{
    std::vector<std::string> files;

    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    if (ec)
    {
        return files;
    }

    for (const auto& entry : it)
    {
        if (!entry.is_regular_file())
        {
            continue;
        }

        std::string ext = OtherTools::ToLowerAscii(entry.path().extension().string());

        const bool match = std::any_of(exts.begin(), exts.end(),
            [&ext](const std::string& e) { return ext == e; });
        if (match)
        {
            files.push_back(entry.path().filename().string());
        }
    }

    std::sort(files.begin(), files.end());
    return files;
}

// 在默认 "models/" 目录中查找模型文件
static std::vector<std::string> GetModelFilesByExt(const std::vector<std::string>& exts)
{
    return GetModelFilesByExtInDir("models/", exts);
}

// 将宽字符串（UTF-16）转换为 UTF-8 字符串
// 使用 Windows API WideCharToMultiByte
std::string WideToUtf8(const std::wstring& ws)
{
    if (ws.empty())
        return {};

    int len = WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()),
        nullptr, 0, nullptr, nullptr);
    if (len <= 0)
        return {};

    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()),
        out.data(), len, nullptr, nullptr);
    return out;
}

// 获取指定窗口所属进程的名称
// 通过进程 ID 查询进程可执行文件名
static std::string GetWindowProcessName(HWND hwnd)
{
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == 0)
        return {};

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process)
        return {};

    wchar_t imagePath[MAX_PATH]{};
    DWORD imagePathLen = MAX_PATH;
    const BOOL ok = QueryFullProcessImageNameW(process, 0, imagePath, &imagePathLen);
    CloseHandle(process);

    if (!ok || imagePathLen == 0)
        return {};

    std::filesystem::path path(std::wstring(imagePath, imagePathLen));
    return WideToUtf8(path.filename().wstring());
}

// 构造窗口的显示名称
// 格式：窗口标题 - 进程名 [minimized] [hwnd地址]
static std::string MakeCaptureWindowDisplayName(HWND hwnd, const std::string& title, bool minimized)
{
    std::string display = title;

    const std::string processName = GetWindowProcessName(hwnd);
    if (!processName.empty())
        display += " - " + processName;

    if (minimized)
        display += " [minimized]";

    char hwndText[32]{};
    std::snprintf(hwndText, sizeof(hwndText), "%p", reinterpret_cast<void*>(hwnd));
    display += " [" + std::string(hwndText) + "]";

    return display;
}

// 枚举所有可用于捕获的窗口
// 遍历所有顶层窗口，过滤掉不可见、被隐藏（cloaked）和无标题的窗口
std::vector<CaptureWindowInfo> EnumerateCaptureWindows()
{
    std::vector<CaptureWindowInfo> windows;

    ::EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* out = reinterpret_cast<std::vector<CaptureWindowInfo>*>(lParam);
        if (!out || !::IsWindowVisible(hwnd))
            return TRUE;

        BOOL isCloaked = FALSE;
        if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &isCloaked, sizeof(isCloaked))) && isCloaked)
            return TRUE;

        const int titleLength = ::GetWindowTextLengthW(hwnd);
        if (titleLength <= 0)
            return TRUE;

        std::vector<wchar_t> titleBuffer(static_cast<size_t>(titleLength) + 1);
        const int copied = ::GetWindowTextW(hwnd, titleBuffer.data(), static_cast<int>(titleBuffer.size()));
        if (copied <= 0)
            return TRUE;

        const std::string title = OtherTools::TrimAscii(WideToUtf8(std::wstring(titleBuffer.data(), copied)));
        if (title.empty())
            return TRUE;

        CaptureWindowInfo info;
        info.hwnd = hwnd;
        info.title = title;
        info.minimized = ::IsIconic(hwnd) != FALSE;
        info.displayName = MakeCaptureWindowDisplayName(hwnd, title, info.minimized);
        out->push_back(std::move(info));

        return TRUE;
    }, reinterpret_cast<LPARAM>(&windows));

    return windows;
}

// 根据标题查找捕获窗口
// 搜索策略：精确匹配 -> 子串匹配 -> 不区分大小写匹配
HWND FindCaptureWindowByTitle(const std::string& title)
{
    const std::string needle = OtherTools::TrimAscii(title);
    if (needle.empty())
        return nullptr;

    const std::vector<CaptureWindowInfo> windows = EnumerateCaptureWindows();

    for (const auto& window : windows)
        if (window.title == needle)
            return window.hwnd;

    for (const auto& window : windows)
        if (window.title.find(needle) != std::string::npos)
            return window.hwnd;

    for (const auto& window : windows)
        if (OtherTools::ContainsCaseInsensitive(window.title, needle))
            return window.hwnd;

    return nullptr;
}

// 将 HRESULT 错误码转换为人类可读的字符串
static std::string hr_to_string(HRESULT hr)
{
    _com_error err(hr);
    const wchar_t* ws = err.ErrorMessage();
    int len = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    std::string s(len > 0 ? len - 1 : 0, '\0');
    if (len > 0)
        WideCharToMultiByte(CP_UTF8, 0, ws, -1, s.data(), len, nullptr, nullptr);
    return s;
}

// 验证图像文件的有效性
// 使用 WIC（Windows Imaging Component）解码图像文件，检查尺寸是否合理
// 返回图像宽度、高度；若无效则返回错误描述
bool IsValidImageFile(const std::wstring& wpath, UINT& outW, UINT& outH, std::string& outErr)
{
    outW = outH = 0;
    outErr.clear();

    ScopedComInit com(COINIT_MULTITHREADED);
    if (!com.ok())
    {
        outErr = "CoInitializeEx failed: " + hr_to_string(com.hr);
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { outErr = "WIC factory error: " + hr_to_string(hr); return false; }

    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(wpath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) { outErr = "DecoderFromFilename failed: " + hr_to_string(hr); return false; }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) { outErr = "GetFrame(0) failed: " + hr_to_string(hr); return false; }

    UINT w = 0, h = 0;
    hr = frame->GetSize(&w, &h);
    if (FAILED(hr)) { outErr = "GetSize failed: " + hr_to_string(hr); return false; }

    const UINT MAX_DIM = 16384;
    if (w == 0 || h == 0 || w > MAX_DIM || h > MAX_DIM)
    {
        outErr = "Invalid image size: " + std::to_string(w) + "x" + std::to_string(h);
        return false;
    }

    ComPtr<IWICFormatConverter> conv;
    hr = factory->CreateFormatConverter(&conv);
    if (FAILED(hr)) { outErr = "CreateFormatConverter failed: " + hr_to_string(hr); return false; }

    hr = conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) { outErr = "Converter Initialize failed: " + hr_to_string(hr); return false; }

    const UINT probe_rows = (std::min)(h, 8u);
    std::vector<uint8_t> probe;
    probe.resize((size_t)w * probe_rows * 4);
    WICRect rect{ 0, 0, (INT)w, (INT)probe_rows };
    hr = conv->CopyPixels(&rect, (UINT)(w * 4), (UINT)probe.size(), probe.data());
    if (FAILED(hr)) { outErr = "CopyPixels failed: " + hr_to_string(hr); return false; }

    outW = w; outH = h;
    return true;
}

// Base64 解码
// 支持 data: URI 格式（自动跳过 "base64," 前缀），过滤空白字符
// 返回解码后的字节数组
std::vector<unsigned char> Base64Decode(const std::string& encoded_string)
{
    std::vector<unsigned char> ret;

    if (encoded_string.empty())
    {
        return ret;
    }

    size_t start = 0;
    size_t base64_pos = encoded_string.find("base64,");
    if (base64_pos != std::string::npos)
    {
        start = base64_pos + 7;
    }

    std::string filtered;
    filtered.reserve(encoded_string.size() - start);
    for (size_t idx = start; idx < encoded_string.size(); ++idx)
    {
        unsigned char c = static_cast<unsigned char>(encoded_string[idx]);
        if (c == '=')
        {
            filtered.push_back('=');
            continue;
        }
        if (is_base64(c))
        {
            filtered.push_back(static_cast<char>(c));
            continue;
        }
        if (std::isspace(c))
        {
            continue;
        }
    }

    if (filtered.empty())
    {
        return ret;
    }

    int in_len = static_cast<int>(filtered.size());
    int i = 0;
    int in_ = 0;
    unsigned char char_array_4[4], char_array_3[3];

    while (in_len-- && (filtered[in_] != '=') && is_base64(static_cast<unsigned char>(filtered[in_])))
    {
        char_array_4[i++] = static_cast<unsigned char>(filtered[in_]); in_++;
        if (i == 4)
        {
            for (i = 0; i < 4; i++)
                char_array_4[i] = static_cast<unsigned char>(base64_chars.find(char_array_4[i]));

            char_array_3[0] = static_cast<unsigned char>((char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4));
            char_array_3[1] = static_cast<unsigned char>(((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2));
            char_array_3[2] = static_cast<unsigned char>(((char_array_4[2] & 0x3) << 6) + char_array_4[3]);

            for (i = 0; (i < 3); i++)
                ret.push_back(char_array_3[i]);
            i = 0;
        }
    }

    if (i)
    {
        int j;
        for (j = i; j < 4; j++)
            char_array_4[j] = 0;

        for (j = 0; j < 4; j++)
            char_array_4[j] = static_cast<unsigned char>(base64_chars.find(char_array_4[j]));

        char_array_3[0] = static_cast<unsigned char>((char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4));
        char_array_3[1] = static_cast<unsigned char>(((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2));
        char_array_3[2] = static_cast<unsigned char>(((char_array_4[2] & 0x3) << 6) + char_array_4[3]);

        for (j = 0; (j < i - 1); j++)
            ret.push_back(char_array_3[j]);
    }

    return ret;
}

// 检查文件是否存在（支持 UTF-8 路径）
bool fileExists(const std::string& path)
{
    std::filesystem::path fsPath;
    try
    {
        fsPath = std::filesystem::u8path(path);
    }
    catch (...)
    {
        fsPath = std::filesystem::path(path);
    }

    std::error_code ec;
    return std::filesystem::exists(fsPath, ec) && !ec;
}

// 替换文件名的扩展名
// 例如 "image.jpg" + ".png" → "image.png"
std::string replace_extension(const std::string& filename, const std::string& new_extension)
{
    size_t last_dot = filename.find_last_of(".");
    if (last_dot == std::string::npos)
    {
        return filename + new_extension;
    }
    else
    {
        return filename.substr(0, last_dot) + new_extension;
    }
}

// 获取所有可用的模型文件（.engine 和 .onnx）
std::vector<std::string> getModelFiles()
{
    return GetModelFilesByExt({ ".engine", ".onnx" });
}

// 获取所有 TensorRT engine 文件
std::vector<std::string> getEngineFiles()
{
    return GetModelFilesByExt({ ".engine" });
}

// 获取所有 ONNX 模型文件
std::vector<std::string> getOnnxFiles()
{
    return GetModelFilesByExt({ ".onnx" });
}

// 获取可用的深度模型文件（在 depth_models 目录中）
std::vector<std::string> getAvailableDepthModels()
{
    return GetModelFilesByExtInDir("depth_models", { ".engine", ".onnx", ".trt", ".plan" });
}

// 获取当前 AI 模型在模型列表中的索引
std::vector<std::string>::difference_type getModelIndex(const std::vector<std::string>& engine_models)
{
    auto it = std::find(engine_models.begin(), engine_models.end(), config.ai_model);

    if (it != engine_models.end())
    {
        return std::distance(engine_models.begin(), it);
    }
    else
    {
        return -1;
    }
}

// 从图像文件加载 D3D11 纹理
// 使用 stb_image 解码图像，创建 D3D11 Texture2D 并生成着色器资源视图（SRV）
bool LoadTextureFromFile(const char* filename, ID3D11Device* device, ID3D11ShaderResourceView** out_srv, int* out_width, int* out_height)
{
    if (out_srv)
    {
        *out_srv = nullptr;
    }
    if (out_width)
    {
        *out_width = 0;
    }
    if (out_height)
    {
        *out_height = 0;
    }

    if (!filename || !device || !out_srv || !out_width || !out_height)
        return false;

    int image_width = 0;
    int image_height = 0;
    int channels = 0;
    const std::filesystem::path path = MakePathFromUtf8(filename);
    std::vector<unsigned char> fileData;
    if (path.empty() || !ReadFileToBuffer(path, fileData))
    {
        return false;
    }

    unsigned char* image_data = stbi_load_from_memory(
        fileData.data(),
        static_cast<int>(fileData.size()),
        &image_width,
        &image_height,
        &channels,
        4);
    if (image_data == NULL)
        return false;

    D3D11_TEXTURE2D_DESC desc;
    ZeroMemory(&desc, sizeof(desc));
    desc.Width = image_width;
    desc.Height = image_height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;

    ID3D11Texture2D* pTexture = NULL;
    D3D11_SUBRESOURCE_DATA subResource;
    subResource.pSysMem = image_data;
    subResource.SysMemPitch = desc.Width * 4;
    subResource.SysMemSlicePitch = 0;
    HRESULT hr = device->CreateTexture2D(&desc, &subResource, &pTexture);
    if (FAILED(hr))
    {
        stbi_image_free(image_data);
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
    ZeroMemory(&srvDesc, sizeof(srvDesc));
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = desc.MipLevels;
    srvDesc.Texture2D.MostDetailedMip = 0;
    hr = device->CreateShaderResourceView(pTexture, &srvDesc, out_srv);
    if (FAILED(hr))
    {
        pTexture->Release();
        stbi_image_free(image_data);
        return false;
    }
    pTexture->Release();

    *out_width = image_width;
    *out_height = image_height;
    stbi_image_free(image_data);

    return true;
}

// 从 Base64 编码的内存数据加载 D3D11 纹理
// 与 LoadTextureFromFile 类似，但输入是 Base64 字符串而非文件路径
bool LoadTextureFromMemory(const std::string& imageBase64, ID3D11Device* device, ID3D11ShaderResourceView** out_srv, int* out_width, int* out_height)
{
    if (out_srv)
    {
        *out_srv = nullptr;
    }
    if (out_width)
    {
        *out_width = 0;
    }
    if (out_height)
    {
        *out_height = 0;
    }

    if (!device || !out_srv || !out_width || !out_height)
        return false;

    std::vector<unsigned char> decodedData = Base64Decode(imageBase64);
    if (decodedData.empty())
    {
        return false;
    }

    int image_width = 0;
    int image_height = 0;
    int channels = 0;

    unsigned char* image_data = stbi_load_from_memory(
        decodedData.data(),
        static_cast<int>(decodedData.size()),
        &image_width, &image_height, &channels, 4);

    if (image_data == NULL)
    {
        std::cerr << "Can't load image from memory." << std::endl;
        return false;
    }

    D3D11_TEXTURE2D_DESC desc;
    ZeroMemory(&desc, sizeof(desc));
    desc.Width = image_width;
    desc.Height = image_height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;

    ID3D11Texture2D* pTexture = NULL;
    D3D11_SUBRESOURCE_DATA subResource;
    subResource.pSysMem = image_data;
    subResource.SysMemPitch = desc.Width * 4;
    subResource.SysMemSlicePitch = 0;
    HRESULT hr = device->CreateTexture2D(&desc, &subResource, &pTexture);
    if (FAILED(hr))
    {
        stbi_image_free(image_data);
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
    ZeroMemory(&srvDesc, sizeof(srvDesc));
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = desc.MipLevels;
    srvDesc.Texture2D.MostDetailedMip = 0;
    hr = device->CreateShaderResourceView(pTexture, &srvDesc, out_srv);
    if (FAILED(hr))
    {
        pTexture->Release();
        stbi_image_free(image_data);
        return false;
    }
    pTexture->Release();

    *out_width = image_width;
    *out_height = image_height;
    stbi_image_free(image_data);

    return true;
}

// 获取 Logitech G HUB 版本号
// 从 "C:\\Program Files\\LGHUB\\version" 文件中读取
std::string get_ghub_version()
{
    std::string line;
    std::ifstream in("C:\\Program Files\\LGHUB\\version");
    if (in.is_open())
    {
        while (std::getline(in, line)) { }
    }
    in.close();

    if (!line.empty())
    {
        return line;
    }

	return "unknown";
}

// 检查路径字符串中是否包含 "tensorrt"（不区分大小写）
bool contains_tensorrt(const std::string& path)
{
    const std::string lowercase_path = OtherTools::ToLowerAscii(path);
    return lowercase_path.find("tensorrt") != std::string::npos;
}

// 获取当前活动的显示器数量
// 通过 DXGI 枚举适配器和输出
int get_active_monitors()
{
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)))
    {
        return -1;
    }

    int monitorCount = 0;

    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        IDXGIOutput* output = nullptr;
        for (UINT j = 0; adapter->EnumOutputs(j, &output) != DXGI_ERROR_NOT_FOUND; ++j)
        {
            monitorCount++;
            output->Release();
        }
        adapter->Release();
    }

    factory->Release();
    return monitorCount;
}

// 根据索引获取显示器句柄（HMONITOR）
// 通过 EnumDisplayMonitors 遍历显示器，返回指定索引的句柄
HMONITOR GetMonitorHandleByIndex(int monitorIndex)
{
    monitorIndex = std::max(0, monitorIndex);

    struct MonitorSearch
    {
        int targetIndex;
        int currentIndex;
        HMONITOR targetMonitor;
    };

    MonitorSearch search = { monitorIndex, 0, nullptr };

    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR hMonitor, HDC, LPRECT, LPARAM lParam) -> BOOL
        {
            MonitorSearch* search = reinterpret_cast<MonitorSearch*>(lParam);
            if (search->currentIndex == search->targetIndex)
            {
                search->targetMonitor = hMonitor;
                return FALSE;
            }
            search->currentIndex++;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));

    return search.targetMonitor;
}

namespace
{
// 将 DISPLAYCONFIG_RATIONAL 刷新率转换为 double
double RefreshRateFromRational(const DISPLAYCONFIG_RATIONAL& refreshRate)
{
    if (refreshRate.Numerator == 0 || refreshRate.Denominator == 0)
        return 0.0;

    return static_cast<double>(refreshRate.Numerator) /
        static_cast<double>(refreshRate.Denominator);
}

// 通过 DisplayConfig API 获取指定显示设备的刷新率
// 使用 QueryDisplayConfig 查询活动显示路径的刷新率
double GetDisplayConfigRefreshRateForDevice(const wchar_t* gdiDeviceName)
{
    if (!gdiDeviceName || gdiDeviceName[0] == L'\0')
        return 0.0;

    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    LONG result = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
    if (result != ERROR_SUCCESS || pathCount == 0 || modeCount == 0)
        return 0.0;

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    result = QueryDisplayConfig(
        QDC_ONLY_ACTIVE_PATHS,
        &pathCount,
        paths.data(),
        &modeCount,
        modes.data(),
        nullptr);
    if (result != ERROR_SUCCESS)
        return 0.0;

    paths.resize(pathCount);
    for (const auto& path : paths)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName{};
        sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sourceName.header.size = sizeof(sourceName);
        sourceName.header.adapterId = path.sourceInfo.adapterId;
        sourceName.header.id = path.sourceInfo.id;

        if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS)
            continue;

        if (wcscmp(sourceName.viewGdiDeviceName, gdiDeviceName) == 0)
        {
            const double refreshHz = RefreshRateFromRational(path.targetInfo.refreshRate);
            if (refreshHz > 0.0)
                return refreshHz;
        }
    }

    return 0.0;
}
}

// 根据索引获取显示器刷新率（Hz）
// 优先使用 DisplayConfig API，回退到 EnumDisplaySettings
double GetMonitorRefreshRateByIndex(int monitorIndex)
{
    HMONITOR monitor = GetMonitorHandleByIndex(monitorIndex);
    if (!monitor)
        return 0.0;

    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo))
        return 0.0;

    double refreshHz = GetDisplayConfigRefreshRateForDevice(monitorInfo.szDevice);
    if (refreshHz > 0.0)
        return refreshHz;

    DEVMODEW devMode{};
    devMode.dmSize = sizeof(devMode);
    if (EnumDisplaySettingsW(monitorInfo.szDevice, ENUM_CURRENT_SETTINGS, &devMode) &&
        devMode.dmDisplayFrequency > 1)
    {
        return static_cast<double>(devMode.dmDisplayFrequency);
    }

    return 0.0;
}

// 获取所有可用模型列表
// 如果后端为 TRT，优先使用 .engine 文件，同名的 ONNX 文件被过滤掉
// 否则返回所有 ONNX 文件
std::vector<std::string> getAvailableModels()
{
    std::vector<std::string> availableModels;

    std::vector<std::string> engineFiles = getEngineFiles();
    std::vector<std::string> onnxFiles = getOnnxFiles();

    if (config.backend == "TRT")
    {
        std::set<std::string> engineModels;
        for (const auto& file : engineFiles)
        {
            engineModels.insert(std::filesystem::path(file).stem().string());
        }

        for (const auto& file : engineFiles)
        {
            availableModels.push_back(file);
        }

        for (const auto& file : onnxFiles)
        {
            std::string modelName = std::filesystem::path(file).stem().string();
            if (engineModels.find(modelName) == engineModels.end())
            {
                availableModels.push_back(file);
            }
        }
    }
    else
    {
        for (const auto& file : onnxFiles)
        {
            availableModels.push_back(file);
        }
    }

    return availableModels;
}

// 设置随机控制台标题，用于隐藏程序身份
// 从预定义的常见 Windows 程序名列表中随机选择一个
void SetRandomConsoleTitle()
{
    static constexpr std::array<const wchar_t*, 10> kTitles = {
        L"Microsoft Edge",
        L"Google Chrome",
        L"Notepad",
        L"Windows Terminal",
        L"PowerShell",
        L"Visual Studio Code",
        L"Task Manager",
        L"File Explorer",
        L"Calculator",
        L"Command Prompt",
    };

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<size_t> dist(0, kTitles.size() - 1);

    ::SetConsoleTitleW(kTitles[dist(gen)]);
}

// 检查当前 Windows 版本是否为 1903（19H1）或更高
bool checkwin1903()
{
    return IsWindows10OrGreater() && IsWindowsVersionOrGreater(10, 0, 18362);
}

// 打印欢迎/启动消息
// 显示自瞄启动提示及按键绑定信息
void welcome_message()
{
    std::cout <<
    "\n\n辅助瞄准已启动！\n" <<
    config.joinStrings(config.button_targeting) << " -> 瞄准\n" <<
    config.joinStrings(config.button_exit) << " -> 退出\n" <<
    config.joinStrings(config.button_pause) << " -> 暂停瞄准\n" <<
    config.joinStrings(config.button_reload_config) << " -> 重载配置\n" <<
    config.joinStrings(config.button_open_overlay) << " -> 打开叠加层（选项）" <<
    std::endl;
}
