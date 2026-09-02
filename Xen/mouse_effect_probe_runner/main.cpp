#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "mouse_effect_probe_runner/mouse_effect_probe_runner.h"

#include <iostream>
#include <string_view>
#include <vector>

namespace {

BOOL WINAPI mouse_effect_probe_control_handler(DWORD control_type) noexcept {
    if (control_type == CTRL_C_EVENT || control_type == CTRL_BREAK_EVENT ||
        control_type == CTRL_CLOSE_EVENT ||
        control_type == CTRL_SHUTDOWN_EVENT) {
        request_mouse_effect_probe_stop();
        return TRUE;
    }
    return FALSE;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::wstring_view> arguments;
    arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0U);
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }

    MouseEffectProbeRunOptions options;
    std::string error;
    const auto parse_status = parse_mouse_effect_probe_options(
        arguments, options, error);
    if (parse_status == MouseEffectProbeParseStatus::HELP) {
        std::cout << mouse_effect_probe_usage();
        return 0;
    }
    if (parse_status == MouseEffectProbeParseStatus::INVALID) {
        std::cerr << "参数错误: " << error << "\n\n"
                  << mouse_effect_probe_usage();
        return 2;
    }
    if (!SetConsoleCtrlHandler(mouse_effect_probe_control_handler, TRUE)) {
        std::cerr << "无法安装控制台中止处理器，Win32Error="
                  << GetLastError() << '\n';
        return 3;
    }

    MouseEffectProbeRunResult result;
    const bool completed = run_mouse_effect_probe(options, result, error);
    SetConsoleCtrlHandler(mouse_effect_probe_control_handler, FALSE);
    if (!completed) {
        std::cerr << "Mouse Effect Probe 未正常完成: " << error
                  << ", stop_reason="
                  << mouse_effect_probe::probe_stop_reason_name(
                         result.execution.stop_reason);
        if (!result.report_sha256.empty()) {
            std::cerr << ", report=" << options.report_path;
        }
        if (!result.safety_ledger_sha256.empty()) {
            std::cerr << ", safety_ledger=" << options.safety_ledger_path
                      << ", safety_ledger_sha256="
                      << result.safety_ledger_sha256;
        }
        std::cerr << '\n';
        return 1;
    }
    std::cout
        << "Mouse Effect Probe 时间线完成: mode="
        << mouse_effect_probe::probe_dispatch_mode_name(
               result.execution.dispatch_mode)
        << ", consumed_samples="
        << result.execution.consumed_sample_count
        << ", requested_x_counts="
        << result.execution.cumulative_requested_x_counts
        << ", backend_completed_x_counts="
        << result.execution.cumulative_backend_completed_x_counts
        << ", report=" << options.report_path
        << ", report_sha256=" << result.report_sha256;
    if (!result.safety_ledger_sha256.empty()) {
        std::cout << ", safety_ledger=" << options.safety_ledger_path
                  << ", safety_ledger_sha256="
                  << result.safety_ledger_sha256;
    }
    std::cout << '\n'
        << "该结果不声明 visible physical effect；必须与 sidecar manifest/"
           "背景 witness 离线对齐后再判断。\n";
    return 0;
}
