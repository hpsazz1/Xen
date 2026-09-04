#include "aim_production_red/aim_production_red.h"

#include <windows.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void print_usage() {
    std::cout
        << "Usage: XenAimProductionRedProducer.exe "
           "--plan <json> --config <ini> "
           "--measured-reference-source <file> "
           "--output-directory <new-directory>\n"
        << "This tool is output-off: physical_output_capability=false, "
           "physical_dispatch_count=0.\n";
}

std::filesystem::path executable_path() {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    return std::filesystem::path(
        std::wstring_view(buffer.data(), static_cast<std::size_t>(length)));
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--help") {
        print_usage();
        return 0;
    }

    aim_production_red::ProduceOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (index + 1 >= argc) {
            std::cerr << "missing value for CLI argument\n";
            print_usage();
            return 2;
        }
        const std::filesystem::path value(argv[++index]);
        if (argument == L"--plan") {
            options.plan_path = value;
        } else if (argument == L"--config") {
            options.config_path = value;
        } else if (argument == L"--measured-reference-source") {
            options.measured_reference_source_path = value;
        } else if (argument == L"--output-directory") {
            options.output_directory = value;
        } else {
            std::cerr << "unknown CLI argument\n";
            print_usage();
            return 2;
        }
    }
    options.producer_binary_path = executable_path();
    if (options.plan_path.empty() || options.config_path.empty() ||
        options.measured_reference_source_path.empty() ||
        options.output_directory.empty() ||
        options.producer_binary_path.empty()) {
        std::cerr << "required CLI argument missing\n";
        print_usage();
        return 2;
    }

    aim_production_red::ProduceResult result;
    std::string error;
    if (!aim_production_red::produce_output_off_bundle(
            options, result, error)) {
        std::cerr << "Aim production red producer failed: " << error << '\n';
        return 2;
    }
    std::cout
        << "Aim production red producer: physical_output_capability=false, "
        << "physical_dispatch_count=0, traces=" << result.trace_count
        << ", samples=" << result.sample_count
        << ", manifest=\"" << result.manifest_path.string() << "\"\n";
    return 0;
}
