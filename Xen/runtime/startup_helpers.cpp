#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "runtime/startup_helpers.h"

#include "Xen.h"
#include "other_tools.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <vector>

namespace
{
constexpr const char* kConsoleThemeSequence =
    "\x1b]4;0;rgb:ffff/ffff/ffff\x07"
    "\x1b]4;7;rgb:1a1c/1c1c/1f1f\x07"
    "\x1b]4;8;rgb:6065/6565/6d6d\x07"
    "\x1b]4;9;rgb:3333/9c9c/ffff\x07"
    "\x1b]4;10;rgb:0000/a2a2/4040\x07"
    "\x1b]4;12;rgb:baba/2626/2323\x07"
    "\x1b[38;5;7;48;5;0m";
}

void ApplyConsoleTheme()
{
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD consoleMode = 0;
    if (output == nullptr || output == INVALID_HANDLE_VALUE || !GetConsoleMode(output, &consoleMode))
        return;

    // Windows Terminal 等伪控制台不一定支持修改 Win32 调色板，优先使用 ANSI
    // 主题序列；传统 conhost 会忽略该序列，后面的 Win32 调色板路径继续生效。
    DWORD ansiMode = consoleMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    if (SetConsoleMode(output, ansiMode))
        std::cout << kConsoleThemeSequence << std::flush;

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
