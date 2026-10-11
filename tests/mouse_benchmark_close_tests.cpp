#include "close_error_codecvt_test_support.h"
#include "mouse_benchmark/mouse_benchmark.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

namespace {
int failures = 0;
void check(bool value, const char* description) {
    std::cout << (value ? "PASS " : "FAIL ") << description << '\n';
    if (!value) ++failures;
}
std::string text(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return {bytes.begin(), bytes.end()};
}
std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}
bool no_pending(const std::filesystem::path& root) {
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.path().filename().wstring().find(L".pending-") != std::wstring::npos) return false;
    }
    return true;
}
}

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("xen-mouse-benchmark-close-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!std::filesystem::create_directory(root)) return 2;
    // 仅构造合法报告字段并调用writer，绝不调用run/Mouse factory或打开串口。
    MouseBenchmarkOptions options;
    options.backend_explicit = true;
    options.mouse.backend = MouseBackend::MAKCU;
    options.mouse.makcu_port = "COM8";
    options.mouse.makcu_baud_rate = 4000000;
    options.mouse.makcu_connect_timeout_ms = 700;
    options.mouse.makcu_command_timeout_ms = 80;
    options.run_uuid = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
    options.peer_test_boundary = MouseBenchmarkPeerTestBoundary::IN_MEMORY_SERIAL_FAKE;
    options.warmup_pairs = 0; options.sample_pairs = 1;
    options.dx_counts = 2; options.dy_counts = -1;
    options.allow_physical_output = options.physical_output_confirmed = true;
    MouseBenchmarkResult result;
    result.complete = true; result.run_uuid = options.run_uuid;
    result.completion_semantic = MouseBenchmarkCompletionSemantic::MAKCU_MATCHED_SERIAL_DEVICE_STATUS_ACK;
    result.protocol_ack_observed = true; result.physical_effect_observed = false;
    result.successful_commands = 4; result.formal_successful_commands = 2;
    result.final_status = MouseStatus::READY;
    result.command_latency = {0.2, 0.1, 0.3, 0.4, 0.5};
    result.samples = {{1, 1, 1, 2, -1, 0.1}, {2, 1, -1, -2, 1, 0.2}};
    std::string error;
    for (int fail_at : {0, 1}) {
        const auto path = root / (fail_at ? L"失败-\U0001F680.json" : L"healthy.json");
        options.report_path = text(path);
        xen::test::CloseState state{fail_at};
        bool written;
        {
            xen::test::ScopedCloseError scope(state);
            written = write_mouse_benchmark_report(options, result, error);
        }
        check(state.calls == 1 && state.write_failures == 0, "selected actual close reached after successful writing");
        check(written == (fail_at == 0), "writer propagates close result");
        if (fail_at) {
            check(!path.empty() && !std::filesystem::exists(path) && !error.empty(),
                  "close failure leaves no final report and provides an error");
            check(no_pending(root), "close failure removes owned pending file");
            check(write_mouse_benchmark_report(options, result, error), "same target can recover after close failure");
        }
        const auto bytes = read(path);
        check(bytes.find("\"physical_effect_observed\": false") != std::string::npos &&
                  bytes.find("\"peer_test_boundary\": \"in_memory_serial_fake\"") != std::string::npos,
              "healthy report preserves evidence boundary");
        check(!write_mouse_benchmark_report(options, result, error) && read(path) == bytes,
              "existing report remains byte-identical after refused overwrite");
    }
    std::cout << "Fixture directory: " << text(root) << '\n';
    return failures ? 1 : 0;
}
