#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "runtime/startup_helpers.h"

#include "Xen.h"
#include "other_tools.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <vector>

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

