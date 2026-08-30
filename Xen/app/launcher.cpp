#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "app/launcher_decision_internal.h"
#include "app/release_contract_internal.h"
#include "app/model_catalog_internal.h"
#include "config/config.h"

#include <Windows.h>

#include <filesystem>
#include <string>

namespace {

void show_error(const std::string& message) {
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, message.data(),
        static_cast<int>(message.size()), nullptr, 0);
    std::wstring wide(length > 0 ? static_cast<std::size_t>(length) : 0, L'\0');
    if (length > 0) {
        MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, message.data(),
            static_cast<int>(message.size()), wide.data(), length);
    } else {
        wide = L"发布入口发生未知错误";
    }
    MessageBoxW(nullptr, wide.c_str(), L"Xen", MB_OK | MB_ICONERROR);
}

void show_decision_error(void*, const std::string& message) {
    show_error(message);
}

std::filesystem::path executable_path() {
    std::wstring buffer(512, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size() - 1) {
            buffer.resize(length);
            return std::filesystem::path(buffer);
        }
        if (buffer.size() >= 32'768) return {};
        buffer.resize(std::min<std::size_t>(buffer.size() * 2, 32'768), L'\0');
    }
}

std::wstring backend_list(
        const std::vector<BackendType>& backends) {
    std::wstring result;
    for (const BackendType backend : backends) {
        if (!result.empty()) result += L',';
        const std::string name = app::detail::backend_config_name(backend);
        result.append(name.begin(), name.end());
    }
    return result;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const auto launcher = executable_path();
    if (launcher.empty()) {
        show_error("无法确定 XenLauncher.exe 路径");
        return 1;
    }
    const auto root = launcher.parent_path();
    app::detail::ReleaseManifest manifest;
    std::string error;
    if (!app::detail::load_release_manifest(
            root / L"manifest.json", manifest, error) ||
        !app::detail::validate_release_manifest(root, manifest, error)) {
        show_error(error);
        return 1;
    }

    for (;;) {
        const app::detail::ReleaseRuntimeEntry* runtime = nullptr;
        const app::detail::LauncherDecisionAdapter decision_adapter{
            nullptr, show_decision_error};
        const int decision_exit = app::detail::resolve_launcher_runtime(
            manifest,
            app::detail::path_to_utf8(root / L"config.ini"),
            decision_adapter, runtime);
        if (decision_exit != 0) return decision_exit;

        const std::wstring root_value = root.wstring();
        const std::wstring runtime_value(
            runtime->id.begin(), runtime->id.end());
        std::vector<BackendType> all_backends;
        for (const auto& entry : manifest.runtimes) {
            all_backends.insert(
                all_backends.end(), entry.backends.begin(), entry.backends.end());
        }
        const std::wstring backends_value = backend_list(all_backends);
        SetEnvironmentVariableW(L"XEN_RELEASE_ROOT", root_value.c_str());
        SetEnvironmentVariableW(L"XEN_RUNTIME_ID", runtime_value.c_str());
        SetEnvironmentVariableW(
            L"XEN_RELEASE_BACKENDS", backends_value.c_str());

        const auto worker = root / runtime->executable;
        std::wstring command = L"\"" + worker.wstring() + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(
                worker.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
                nullptr, root.c_str(), &startup, &process)) {
            show_error("无法启动发布运行时 Worker");
            return 1;
        }
        CloseHandle(process.hThread);
        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD exit_code = 1;
        GetExitCodeProcess(process.hProcess, &exit_code);
        CloseHandle(process.hProcess);
        if (exit_code != app::detail::kReleaseRestartExitCode) {
            return static_cast<int>(exit_code);
        }
    }
}
