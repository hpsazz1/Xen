#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>

#ifdef ERROR
#undef ERROR
#endif

#include "benchmark/benchmark.h"
#include "benchmark/benchmark_internal.h"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

BOOL WINAPI benchmark_control_handler(DWORD control_type) noexcept {
    return benchmark::detail::handle_benchmark_console_control(control_type)
        ? TRUE : FALSE;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::wstring_view> arguments;
    arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }

    BenchmarkOptions options;
    std::string error;
    const BenchmarkParseStatus status = parse_benchmark_options(
        arguments, options, error);
    if (status == BenchmarkParseStatus::HELP) {
        std::cout << benchmark_usage();
        return 0;
    }
    if (status == BenchmarkParseStatus::INVALID) {
        std::cerr << "参数错误: " << error << "\n\n"
                  << benchmark_usage();
        return 2;
    }
    benchmark::detail::prepare_benchmark_console_control();
    if (!SetConsoleCtrlHandler(benchmark_control_handler, TRUE)) {
        std::cerr << "无法安装控制台中止处理器，Win32Error="
                  << GetLastError() << '\n';
        return 3;
    }
    const bool succeeded = run_runtime_benchmark(options, error);
    SetConsoleCtrlHandler(benchmark_control_handler, FALSE);
    if (!succeeded) {
        std::cerr << "基准失败: " << error << '\n';
        return 1;
    }
    std::cout << "基准通过，报告前缀=" << options.report_prefix;
    if (!options.provider_profile_path.empty()) {
        std::cout << "，Provider profile="
                  << options.provider_profile_path;
    }
    std::cout << '\n';
    return 0;
}
