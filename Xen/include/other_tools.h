#ifndef OTHER_TOOLS_H
#define OTHER_TOOLS_H

#include <string>
#include <vector>
#include <d3d11.h>

struct CaptureWindowInfo
{
    HWND hwnd = nullptr;
    std::string title;
    std::string displayName;
    bool minimized = false;
};

namespace OtherTools
{
inline int MaxInt(int a, int b) noexcept
{
    return (a > b) ? a : b;
}

inline float MaxFloat(float a, float b) noexcept
{
    return (a > b) ? a : b;
}

std::string TrimAscii(std::string s);
std::string ToLowerAscii(std::string s);
bool ContainsCaseInsensitive(const std::string& haystack, const std::string& needle);
bool HasExtensionCaseInsensitive(const std::string& path, const char* ext);
}

std::vector<unsigned char> Base64Decode(const std::string& encoded_string);
bool fileExists(const std::string& path);
std::string replace_extension(const std::string& filename, const std::string& new_extension);

std::vector<std::string> getEngineFiles();
std::vector<std::string> getModelFiles();
std::vector<std::string> getOnnxFiles();
std::vector<std::string>::difference_type getModelIndex(const std::vector<std::string>& engine_models);
std::vector<std::string> getAvailableDepthModels();

bool LoadTextureFromFile(const char* filename, ID3D11Device* device, ID3D11ShaderResourceView** out_srv, int* out_width, int* out_height);
bool LoadTextureFromMemory(const std::string& imageBase64, ID3D11Device* device, ID3D11ShaderResourceView** out_srv, int* out_width, int* out_height);

std::string get_ghub_version();
int get_active_monitors();
HMONITOR GetMonitorHandleByIndex(int monitorIndex);
double GetMonitorRefreshRateByIndex(int monitorIndex);
void SetRandomConsoleTitle();
bool IsValidImageFile(const std::wstring& wpath, UINT& outW, UINT& outH, std::string& outErr);
std::vector<CaptureWindowInfo> EnumerateCaptureWindows();
HWND FindCaptureWindowByTitle(const std::string& title);

std::vector<std::string> getAvailableModels();

void welcome_message();
bool checkwin1903();
std::string WideToUtf8(const std::wstring& ws);
#endif // OTHER_TOOLS_H
