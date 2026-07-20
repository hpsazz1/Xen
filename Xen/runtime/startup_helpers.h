#pragma once

#include <string>

enum class ConsoleTone
{
    Normal,
    Accent,
    Success,
    Warning,
    Error,
    Muted,
};

int FatalExit(const std::string& message);
void ApplyConsoleTheme();
void WriteConsoleLine(ConsoleTone tone, const std::string& message);
void SetWorkingDirectoryToExecutable();
bool SelectCompatibleAiModel();
