#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#ifdef ERROR
#undef ERROR
#endif
#include "benchmark/benchmark_internal.h"
#include "debug/debug.h"
#include "sender/report.h"

#include <algorithm>
#include <codecvt>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <locale>
#include <string>

namespace {
int failures = 0;
void expect(bool value, const std::string& message) {
    std::cout << (value ? "PASS " : "FAIL ") << message << '\n';
    if (!value) ++failures;
}
std::string utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {value.begin(), value.end()};
}
void put(const std::filesystem::path& path, const std::string& value) {
    std::ofstream output(path, std::ios::binary);
    output << value;
    output.close();
    if (!output) throw std::runtime_error("fixture write failed");
}
std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}
struct CloseState { int fail_at = 1; int calls = 0; int write_failures = 0; };

// 不替换ofstream：真实filebuf::close调用unshift，转换终止失败才置failbit。
// 只在单线程测试进程中临时改变C++ locale；退出作用域恢复。
class CloseErrorCodecvt final : public std::codecvt<char, char, std::mbstate_t> {
public:
    explicit CloseErrorCodecvt(CloseState& state) : state_(state) {}
protected:
    result do_out(state_type&, const char* begin, const char* end,
                  const char*& next, char* to, char* limit, char*& to_next) const override {
        if (state_.fail_at == -1) {
            next = begin; to_next = to; ++state_.write_failures;
            return error;
        }
        const auto size = std::min(end - begin, limit - to);
        std::memcpy(to, begin, static_cast<std::size_t>(size));
        next = begin + size; to_next = to + size;
        return next == end ? ok : partial;
    }
    result do_in(state_type&, const char* begin, const char* end,
                 const char*& next, char* to, char* limit, char*& to_next) const override {
        const auto size = std::min(end - begin, limit - to);
        std::memcpy(to, begin, static_cast<std::size_t>(size));
        next = begin + size; to_next = to + size;
        return next == end ? ok : partial;
    }
    result do_unshift(state_type&, char* to, char*, char*& next) const override {
        next = to;
        return ++state_.calls == state_.fail_at ? error : ok;
    }
    bool do_always_noconv() const noexcept override { return false; }
    int do_encoding() const noexcept override { return 1; }
    int do_max_length() const noexcept override { return 1; }
    int do_length(state_type&, const char* from, const char* end, std::size_t max) const override {
        return static_cast<int>(std::min(static_cast<std::size_t>(end - from), max));
    }
private:
    CloseState& state_;
};
struct ScopedCloseError {
    explicit ScopedCloseError(CloseState& state)
        : previous_(std::locale::global(std::locale(std::locale(), new CloseErrorCodecvt(state)))) {}
    ~ScopedCloseError() { std::locale::global(previous_); }
    std::locale previous_;
};
bool no_temporary_files(const std::filesystem::path& root, const std::string& prefix) {
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        const auto name = entry.path().filename().string();
        if (!name.starts_with(prefix)) continue;
        if (name.find(".tmp.") != std::string::npos || name.find(".pending-") != std::string::npos ||
            name.find(".rollback.") != std::string::npos) return false;
    }
    return true;
}
sender::detail::SenderRunReport sender_report() {
    sender::detail::SenderRunReport report;
    report.destination_url = "udp://127.0.0.1:5000"; // 仅序列化字段，不创建sender/socket。
    report.stop_reason = "frame_limit";
    report.maximum_frames = 2;
    report.elapsed_seconds = 1.25;
    report.geometry = {2560, 1440, 320, 320, 1120, 560, 320, 320, 1.0, 1.0};
    report.stats.stream_id = 42;
    report.stats.last_frame_id = report.stats.frames_sent = 2;
    report.stats.datagrams_sent = 4;
    report.stats.jpeg_bytes_sent = 4000;
    report.stats.wire_bytes_sent = 4496;
    report.stats.largest_datagram_bytes = 1400;
    report.stats.last_frame_datagrams = 2;
    report.stats.last_frame_jpeg_bytes = 2000;
    report.stats.last_frame_wire_bytes = 2248;
    report.samples = {{1, .4, .2, .1, .05, .35, 2, 2000, 2248},
                      {2, .6, .4, .2, .10, .70, 2, 2000, 2248}};
    return report;
}
}

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("xen-report-close-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    if (!std::filesystem::create_directory(root)) return 2;
    {
        CloseState state;
        ScopedCloseError scope(state);
        std::ofstream output(root / "close-control.txt", std::ios::binary);
        output << "ordinary bytes";
        output.flush();
        expect(output.good() && state.calls == 0, "real ofstream write/flush succeed before close");
        output.close();
        expect(output.fail() && !output.is_open() && state.calls == 1, "real close fails through codecvt unshift");
    }
    for (int failure_at : {0, 1, 2, -1}) {
        const auto csv = root / ("debug-" + std::to_string(failure_at) + ".csv");
        const auto json = root / ("debug-" + std::to_string(failure_at) + ".json");
        put(csv, "previous CSV"); put(json, "previous JSON");
        DebugReportConfig config;
        config.csv_path = utf8(csv); config.json_path = utf8(json);
        config.session_id = "synthetic-close-test"; config.enable_lock_marker = false;
        DebugReport report; std::string error;
        expect(report.start(config, error), "DebugReport start");
        CloseState state{failure_at}; bool result;
        {
            ScopedCloseError scope(state);
            result = report.finalize({}, error);
        }
        expect(failure_at == 0 ? result : (!result && !error.empty() && read(csv) == "previous CSV" && read(json) == "previous JSON"),
            "DebugReport CSV/JSON close ordinal " + std::to_string(failure_at));
        expect(failure_at == -1 ? state.write_failures > 0 : state.calls >= (failure_at == 0 ? 2 : failure_at), "DebugReport reached selected stream failure point");
        expect(no_temporary_files(root, "debug-" + std::to_string(failure_at) + "."), "DebugReport cleans owned temporary files");
    }
    for (int failure_at : {0, 1, -1}) {
        const auto path = root / ("sender-" + std::to_string(failure_at) + ".json");
        CloseState state{failure_at}; std::string error; bool result;
        {
            ScopedCloseError scope(state);
            result = sender::detail::write_sender_run_report(utf8(path), sender_report(), error);
        }
        expect(failure_at == 0 ? result : (!result && !error.empty() && !std::filesystem::exists(path)), "Sender close " + std::to_string(failure_at));
        expect((failure_at == -1 ? state.write_failures > 0 : state.calls == 1) && no_temporary_files(root, path.filename().string()), "Sender stream result and pending cleanup");
        BenchmarkOptions options; options.ready_file_path = utf8(root / ("ready-" + std::to_string(failure_at) + ".json"));
        state.calls = 0; state.write_failures = 0;
        {
            ScopedCloseError scope(state);
            result = benchmark::detail::BenchmarkFileTestAccess::ready_lifecycle(options, {}, {}, error);
        }
        expect(result == (failure_at == 0) && (failure_at == -1 ? state.write_failures > 0 : state.calls == 1), "Ready guard stream result " + std::to_string(failure_at));
        expect(!std::filesystem::exists(std::filesystem::u8path(options.ready_file_path)) && no_temporary_files(root, "ready-" + std::to_string(failure_at) + "."), "Ready guard lifecycle cleanup");
        for (auto format : {benchmark::detail::ReportFileFormat::CSV, benchmark::detail::ReportFileFormat::JSON}) {
            const auto path = root / ("retention-" + std::to_string(failure_at) + (format == benchmark::detail::ReportFileFormat::CSV ? ".csv" : ".json"));
            const std::string original = format == benchmark::detail::ReportFileFormat::CSV ? "# report_samples_dropped,0\n" : "  \"report_samples_dropped\": 0,\n";
            put(path, original); state.calls = 0; state.write_failures = 0;
            {
                ScopedCloseError scope(state);
                result = benchmark::detail::BenchmarkFileTestAccess::rewrite_retention(utf8(path), format, 7, error);
            }
            expect(failure_at == 0 ? (result && read(path) != original) : (!result && !error.empty() && read(path) == original), "Retention actual close " + path.filename().string());
            expect((failure_at == -1 ? state.write_failures > 0 : state.calls == 1) && no_temporary_files(root, path.filename().string()), "Retention writer result and temporary cleanup");
        }
    }
    {
        std::string error;
        const auto path = root / "foreign.json";
        put(path, "foreign bytes");
        expect(!sender::detail::write_sender_run_report(utf8(path), sender_report(), error) && read(path) == "foreign bytes", "Sender preserves existing target");
        BenchmarkOptions options; options.ready_file_path = utf8(path);
        expect(!benchmark::detail::BenchmarkFileTestAccess::ready_lifecycle(options, {}, {}, error) && read(path) == "foreign bytes", "Ready guard preserves existing target");
    }
    std::cout << "Synthetic file fixtures retained: " << utf8(root) << '\n';
    return failures ? 1 : 0;
}
