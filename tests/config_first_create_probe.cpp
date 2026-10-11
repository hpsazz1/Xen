#include "config/config.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <SimpleIni.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
bool pause_publication = false;
std::filesystem::path ready_path;
std::filesystem::path resume_path;

BOOL test_move(LPCWSTR from, LPCWSTR to, DWORD flags) {
    if (pause_publication) {
        {
            std::ofstream ready(ready_path);
            ready << "ready";
            ready.close();
            if (!ready) {
                SetLastError(ERROR_WRITE_FAULT);
                return FALSE;
            }
        }
        const auto start = GetTickCount64();
        while (!std::filesystem::exists(resume_path)) {
            if (GetTickCount64() - start > 15000) {
                SetLastError(ERROR_TIMEOUT);
                return FALSE;
            }
            Sleep(5);
        }
    }
    return ::MoveFileExW(from, to, flags);
}
} // namespace

// Pause only this test process immediately before the real filesystem rename.
#define MoveFileExW test_move
#include "../Xen/config/config.cpp"
#undef MoveFileExW

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return 2;
    const std::wstring mode = argv[1];
    if (mode == L"slow-create") {
        if (argc != 5) return 2;
        ready_path = argv[3];
        resume_path = argv[4];
        pause_publication = true;
    } else if (argc != 3 ||
               (mode != L"create" && mode != L"edit" && mode != L"replace")) {
        return 2;
    }
    const auto utf8 = std::filesystem::path(argv[2]).u8string();
    const std::string path(utf8.begin(), utf8.end());
    AppConfig config;
    std::string error;
    bool created = false;
    bool ok = mode == L"replace" ||
              load_or_create_app_config(path, config, created, error);
    if (ok && (mode == L"edit" || mode == L"replace")) {
        config.ui.width = 1234;
        config.detector.model_path = "concurrent-user-choice.onnx";
        ok = save_app_config(path, config, error);
    }
    std::printf(
        "{\"pid\":%lu,\"ok\":%s,\"created\":%s,\"width\":%d,"
        "\"edited_model\":%s,\"error_empty\":%s}\n",
        GetCurrentProcessId(), ok ? "true" : "false",
        created ? "true" : "false", config.ui.width,
        config.detector.model_path == "concurrent-user-choice.onnx" ? "true" : "false",
        error.empty() ? "true" : "false");
    if (!error.empty()) std::fprintf(stderr, "%s\n", error.c_str());
    return 0;
}
