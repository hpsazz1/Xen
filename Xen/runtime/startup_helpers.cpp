#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "runtime/startup_helpers.h"

#include "Xen.h"
#include "other_tools.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <sstream>
#include <vector>

namespace
{
bool g_consoleVirtualTerminal = false;
std::mutex g_consoleOutputMutex;

constexpr const char* kConsoleThemeSequence =
    "\x1b[38;2;26;28;31m\x1b[48;2;255;255;255m\x1b[2J\x1b[H";
constexpr const char* kConsoleThemeBase =
    "\x1b[38;2;26;28;31m\x1b[48;2;255;255;255m";

const char* ConsoleToneSequence(ConsoleTone tone)
{
    switch (tone)
    {
    case ConsoleTone::Accent: return "\x1b[38;2;51;156;255m\x1b[48;2;255;255;255m";
    case ConsoleTone::Success: return "\x1b[38;2;0;162;64m\x1b[48;2;255;255;255m";
    case ConsoleTone::Warning: return "\x1b[38;2;170;104;0m\x1b[48;2;255;255;255m";
    case ConsoleTone::Error: return "\x1b[38;2;186;38;35m\x1b[48;2;255;255;255m";
    case ConsoleTone::Muted: return "\x1b[38;2;96;101;109m\x1b[48;2;255;255;255m";
    case ConsoleTone::Normal: return "\x1b[38;2;26;28;31m\x1b[48;2;255;255;255m";
    }
    return kConsoleThemeBase;
}

WORD ConsoleToneAttributes(ConsoleTone tone)
{
    switch (tone)
    {
    case ConsoleTone::Accent: return FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    case ConsoleTone::Success: return FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    case ConsoleTone::Warning: return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    case ConsoleTone::Error: return FOREGROUND_RED | FOREGROUND_INTENSITY;
    case ConsoleTone::Muted: return FOREGROUND_INTENSITY;
    case ConsoleTone::Normal: return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    }
    return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
}
}

void ApplyConsoleTheme()
{
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD consoleMode = 0;
    if (output == nullptr || output == INVALID_HANDLE_VALUE || !GetConsoleMode(output, &consoleMode))
        return;

    // Windows Terminal 等伪控制台不一定支持修改 Win32 调色板，优先使用真彩色
    // ANSI 主题序列；传统 conhost 会在启用 VT 后同样处理该序列。
    DWORD ansiMode = consoleMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    if (SetConsoleMode(output, ansiMode))
    {
        g_consoleVirtualTerminal = true;
        std::cout << kConsoleThemeSequence << std::flush;
        return;
    }

    CONSOLE_SCREEN_BUFFER_INFOEX info{};
    info.cbSize = sizeof(info);
    if (!GetConsoleScreenBufferInfoEx(output, &info))
        return;

    // 将 Windows 的基础调色板映射到 Codex Light 主题令牌。保留颜色索引语义，
    // 使第三方库使用标准蓝、绿、红输出时也能与叠加层保持一致。
    info.ColorTable[0] = RGB(255, 255, 255); // surface
    info.ColorTable[7] = RGB(26, 28, 31);    // ink
    info.ColorTable[8] = RGB(96, 101, 109);  // muted ink
    info.ColorTable[9] = RGB(51, 156, 255);  // accent
    info.ColorTable[10] = RGB(0, 162, 64);   // success
    info.ColorTable[12] = RGB(186, 38, 35);  // error

    constexpr WORD kThemeAttributes =
        FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    info.wAttributes = kThemeAttributes;
    info.wPopupAttributes = kThemeAttributes;

    if (!SetConsoleScreenBufferInfoEx(output, &info))
        return; // ANSI 路径已经覆盖支持虚拟终端的控制台。

    SetConsoleTextAttribute(output, kThemeAttributes);

    // 新进程启动时同步已有缓冲区单元的背景色，避免只在新输出行出现白底。
    const DWORD cellCount = static_cast<DWORD>(info.dwSize.X) * static_cast<DWORD>(info.dwSize.Y);
    DWORD cellsUpdated = 0;
    FillConsoleOutputAttribute(output, kThemeAttributes, cellCount, COORD{ 0, 0 }, &cellsUpdated);
}

void WriteConsoleLine(ConsoleTone tone, const std::string& message)
{
    std::lock_guard<std::mutex> lock(g_consoleOutputMutex);
    if (g_consoleVirtualTerminal)
        std::cout << ConsoleToneSequence(tone) << message << kConsoleThemeBase << std::endl;
    else
    {
        HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO info{};
        const bool hasConsole = output != nullptr && output != INVALID_HANDLE_VALUE &&
            GetConsoleScreenBufferInfo(output, &info);
        if (hasConsole)
            SetConsoleTextAttribute(output, ConsoleToneAttributes(tone));
        std::cout << message << std::endl;
        if (hasConsole)
            SetConsoleTextAttribute(output, info.wAttributes);
    }
}

int FatalExit(const std::string& message)
{
    std::cerr << message << std::endl;
    std::cout << "Press Enter to exit...";
    std::cin.get();
    return -1;
}

void SetWorkingDirectoryToExecutable()
{
    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
        return;

    const auto exeDir = std::filesystem::path(exePath).parent_path();
    std::error_code ec;
    std::filesystem::current_path(exeDir, ec);
    if (ec && config.verbose)
    {
        std::cout << "[Config] Failed to set working dir: " << exeDir.u8string()
                  << " (" << ec.message() << ")" << std::endl;
    }
}

bool SelectCompatibleAiModel()
{
    const std::vector<std::string> availableModels = getAvailableModels();
    if (!config.ai_model.empty())
    {
        const std::string modelPath = "models/" + config.ai_model;
        if (!std::filesystem::exists(modelPath))
        {
            std::cerr << "[MAIN] Specified model does not exist: " << modelPath << std::endl;
        }
        else if (std::find(availableModels.begin(), availableModels.end(), config.ai_model) != availableModels.end())
        {
            return true;
        }
        else
        {
            std::cerr << "[MAIN] Specified model is not compatible with backend "
                      << config.backend << ": " << config.ai_model << std::endl;
        }
    }

    if (availableModels.empty())
    {
        std::cerr << "[MAIN] No compatible AI models found in 'models' directory for backend "
                  << config.backend << "." << std::endl;
        return false;
    }

    config.ai_model = availableModels.front();
    config.saveConfig("config.ini");
    std::cout << "[MAIN] Loaded first compatible " << config.backend
              << " model: " << config.ai_model << std::endl;
    return true;
}
