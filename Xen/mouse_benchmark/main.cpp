#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>

#ifdef ERROR
#undef ERROR
#endif

#include "mouse_benchmark/mouse_benchmark.h"

#include <iostream>
#include <string_view>
#include <vector>

namespace {

BOOL WINAPI mouse_benchmark_control_handler(DWORD control_type) noexcept {
    if (control_type == CTRL_C_EVENT || control_type == CTRL_BREAK_EVENT ||
        control_type == CTRL_CLOSE_EVENT ||
        control_type == CTRL_SHUTDOWN_EVENT) {
        request_mouse_benchmark_stop();
        return TRUE;
    }
    return FALSE;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::wstring_view> arguments;
    arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }

    MouseBenchmarkOptions options;
    std::string error;
    const auto status = parse_mouse_benchmark_options(
        arguments, options, error);
    if (status == MouseBenchmarkParseStatus::HELP) {
        std::cout << mouse_benchmark_usage();
        return 0;
    }
    if (status == MouseBenchmarkParseStatus::INVALID) {
        std::cerr << "参数错误: " << error << "\n\n"
                  << mouse_benchmark_usage();
        return 2;
    }
    if (!SetConsoleCtrlHandler(mouse_benchmark_control_handler, TRUE)) {
        std::cerr << "无法安装控制台中止处理器，Win32Error="
                  << GetLastError() << '\n';
        return 3;
    }

    MouseBenchmarkResult result;
    const bool succeeded = run_mouse_benchmark(options, result, error);
    SetConsoleCtrlHandler(mouse_benchmark_control_handler, FALSE);
    if (!succeeded) {
        std::cerr << "鼠标基准失败: " << error
                  << ", final_status="
                  << MouseStatusName(result.final_status) << '\n';
        return 1;
    }
    std::cout << "鼠标基准通过: commands="
              << result.successful_commands
              << ", formal_commands="
              << result.formal_successful_commands
              << ", semantic="
              << mouse_benchmark_completion_semantic_name(
                     result.completion_semantic)
              << ", boundary="
              << mouse_benchmark_peer_test_boundary_name(
                     options.peer_test_boundary)
              << ", protocol_ack_observed="
              << (result.protocol_ack_observed ? "true" : "false")
              << ", physical_effect_observed="
              << (result.physical_effect_observed ? "true" : "false")
              << ", mean=" << result.command_latency.mean_ms
              << "ms, P50=" << result.command_latency.p50_ms
              << "ms, P95=" << result.command_latency.p95_ms
              << "ms, P99=" << result.command_latency.p99_ms
              << "ms, max=" << result.command_latency.max_ms
              << "ms, report=" << options.report_path << '\n';
    return 0;
}
