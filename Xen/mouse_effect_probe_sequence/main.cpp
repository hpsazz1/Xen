#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "mouse_effect_probe/mouse_effect_probe.h"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace {

bool wide_to_utf8(std::wstring_view input, std::string& output) noexcept {
    if (input.empty() || input.size() > static_cast<std::size_t>(
            std::numeric_limits<int>::max())) {
        return false;
    }
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) return false;
    output.resize(static_cast<std::size_t>(required));
    return WideCharToMultiByte(
               CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
               static_cast<int>(input.size()), output.data(), required,
               nullptr, nullptr) == required;
}

bool parse_u64(std::wstring_view input, std::uint64_t& output) noexcept {
    std::string utf8;
    if (!wide_to_utf8(input, utf8)) return false;
    std::uint64_t candidate = 0;
    const auto [end, result] = std::from_chars(
        utf8.data(), utf8.data() + utf8.size(), candidate);
    if (result != std::errc{} || end != utf8.data() + utf8.size() ||
        candidate == 0) {
        return false;
    }
    output = candidate;
    return true;
}

void print_usage() {
    std::cout
        << "XenMouseEffectProbeSequence 只生成离线 sparse-pulse A 序列，"
           "不打开 Capture 或 Mouse。\n\n"
        << "用法:\n"
        << "  XenMouseEffectProbeSequence --output <new-json> "
           "--baseline-samples <n> --response-samples <n> "
           "--guard-samples <n>\n";
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc == 2 &&
        (std::wstring_view(argv[1]) == L"--help" ||
         std::wstring_view(argv[1]) == L"-h")) {
        print_usage();
        return 0;
    }
    std::filesystem::path output_path;
    mouse_effect_probe::SparsePulseSequenceRequest request;
    bool seen_output = false;
    bool seen_baseline = false;
    bool seen_response = false;
    bool seen_guard = false;
    for (int index = 1; index < argc; ++index) {
        if (index + 1 >= argc) {
            std::cerr << "参数缺少值。\n";
            print_usage();
            return 2;
        }
        const std::wstring_view argument = argv[index];
        const std::wstring_view value = argv[++index];
        if (argument == L"--output" && !seen_output) {
            output_path = std::filesystem::path(value);
            seen_output = true;
        } else if (argument == L"--baseline-samples" && !seen_baseline &&
                   parse_u64(value, request.baseline_sample_count)) {
            seen_baseline = true;
        } else if (argument == L"--response-samples" && !seen_response &&
                   parse_u64(value, request.response_sample_count)) {
            seen_response = true;
        } else if (argument == L"--guard-samples" && !seen_guard &&
                   parse_u64(value, request.guard_sample_count)) {
            seen_guard = true;
        } else {
            std::cerr << "未知、重复或非法参数。\n";
            print_usage();
            return 2;
        }
    }
    if (!seen_output || !seen_baseline || !seen_response || !seen_guard ||
        output_path.empty() || !output_path.is_absolute()) {
        std::cerr << "缺少必填参数，且 output 必须是绝对路径。\n";
        print_usage();
        return 2;
    }

    mouse_effect_probe::MouseEffectProbeSequence sequence;
    std::string error;
    if (!mouse_effect_probe::make_sparse_pulse_sequence(
            request, sequence, error) ||
        !mouse_effect_probe::write_mouse_effect_probe_sequence(
            output_path, sequence, error)) {
        std::cerr << "生成 sparse-pulse A 序列失败: " << error << '\n';
        return 1;
    }
    std::cout << "序列已原子发布: path=" << output_path
              << ", samples=" << sequence.samples.size()
              << ", net_x_counts=" << sequence.net_x_counts
              << ", max_abs_prefix_x_counts="
              << sequence.max_abs_prefix_x_counts
              << ", sequence_sha256=" << sequence.sequence_sha256 << '\n';
    return 0;
}
