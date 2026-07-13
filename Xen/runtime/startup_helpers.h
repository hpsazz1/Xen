#pragma once

#include <string>

int FatalExit(const std::string& message);
void SetWorkingDirectoryToExecutable();
bool SelectCompatibleAiModel();

