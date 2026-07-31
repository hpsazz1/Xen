#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "crash/crash.h"
#include "log/log.h"

#include <Windows.h>

#ifdef ERROR
#undef ERROR
#endif

#include <array>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr DWORD kChildExceptionCode = 0xE0424242U;
int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "[FAIL] " << message << '\n';
        ++failures;
    }
}

class TempDirectory {
public:
    explicit TempDirectory(std::string_view suffix) {
        const auto nonce = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("xen_crash_tests_" + std::string(suffix) + "_" +
             std::to_string(GetCurrentProcessId()) + "_" +
             std::to_string(nonce));
        std::filesystem::create_directories(path_);
    }

    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

std::string path_to_utf8(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    return std::string(
        reinterpret_cast<const char*>(encoded.data()), encoded.size());
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream),
                       std::istreambuf_iterator<char>());
}

bool collect_emergency_tail(void* context, const char* data,
                            std::size_t size) noexcept {
    if (!context || (!data && size != 0U)) return false;
    try {
        static_cast<std::string*>(context)->append(data, size);
        return true;
    } catch (...) {
        return false;
    }
}

bool reject_emergency_tail(void*, const char*, std::size_t) noexcept {
    return false;
}

LogConfig minimal_sink_config() {
    LogConfig config;
    config.enable_console = false;
    config.enable_file = false;
    config.enable_debug_file = false;
    config.enable_ringbuf = true;
    config.ringbuf_capacity = 16;
    return config;
}

void test_emergency_tail_contract() {
    Log::shutdown();
    Log::init(minimal_sink_config());
    expect(Log::initialized(), "紧急尾部测试的 Log 初始化失败");
    Log::register_module("emergency", LogLevel::WARN);
    LOG_INFO("emergency", "filtered-info-marker");
    LOG_ERROR("emergency", "accepted-error-marker");
    Log::register_module("emergency", LogLevel::INFO);
    for (int index = 0; index < 140; ++index) {
        LOG_INFO("emergency", "emergency-seq={:03}", index);
    }
    Log::write("emergency", LogLevel::INFO, std::string(700, 'x'));
    Log::shutdown();

    std::string tail;
    expect(Log::dump_emergency_tail(&tail, collect_emergency_tail),
           "Log 关闭后紧急尾部仍应可读取");
    expect(tail.find("filtered-info-marker") == std::string::npos,
           "等级过滤前的日志不得进入紧急尾部");
    expect(tail.find("accepted-error-marker") == std::string::npos,
           "固定 128 槽应覆盖更早的已接收日志");
    expect(tail.find("emergency-seq=000\r\n") == std::string::npos,
           "紧急尾部超容量后必须覆盖最旧记录");
    expect(tail.find("emergency-seq=139\r\n") != std::string::npos,
           "紧急尾部必须保留最新记录");
    expect(!tail.empty() && tail.ends_with("\r\n"),
           "紧急尾部每条记录必须以 CRLF 结束");
    bool found_truncated_record = false;
    std::size_t offset = 0U;
    while (offset < tail.size()) {
        const auto end = tail.find("\r\n", offset);
        expect(end != std::string::npos,
               "紧急尾部不得出现缺少 CRLF 的记录");
        if (end == std::string::npos) break;
        expect((end + 2U) - offset <= 512U,
               "单条紧急日志不得超过固定槽大小");
        const std::string_view line(tail.data() + offset, end - offset);
        if (line.starts_with("[INFO] [emergency] xxxxxxxx")) {
            found_truncated_record = true;
            expect((end + 2U) - offset == 512U,
                   "超长消息必须截断到固定槽上限");
        }
        offset = end + 2U;
    }
    expect(found_truncated_record,
           "紧急尾部必须保留超长消息的截断记录");
    expect(!Log::dump_emergency_tail(nullptr, nullptr),
           "空写入回调必须被拒绝");
    expect(!Log::dump_emergency_tail(nullptr, reject_emergency_tail),
           "写入回调失败时必须立即报告失败");
}

void test_concurrent_emergency_tail_is_not_torn() {
    Log::init(minimal_sink_config());
    Log::register_module("emergency", LogLevel::INFO);
    std::vector<std::thread> writers;
    for (int writer = 0; writer < 8; ++writer) {
        writers.emplace_back([writer] {
            for (int index = 0; index < 1'000; ++index) {
                LOG_INFO("emergency", "concurrent writer={} index={}",
                         writer, index);
            }
        });
    }
    for (auto& writer : writers) writer.join();
    Log::shutdown();

    std::string tail;
    expect(Log::dump_emergency_tail(&tail, collect_emergency_tail),
           "并发写入后的紧急尾部必须可读取");
    std::size_t offset = 0U;
    std::size_t line_count = 0U;
    while (offset < tail.size()) {
        const auto end = tail.find("\r\n", offset);
        expect(end != std::string::npos,
               "并发紧急日志不得出现缺少 CRLF 的撕裂记录");
        if (end == std::string::npos) break;
        const std::string_view line(tail.data() + offset, end - offset);
        expect(line.starts_with("[INFO] [emergency] concurrent writer="),
               "并发紧急日志不得拼接不同槽的内容");
        ++line_count;
        offset = end + 2U;
    }
    expect(line_count > 0U && line_count <= 128U,
           "并发紧急尾部记录数必须位于固定容量内");
}

void test_single_process_handler_ownership() {
    TempDirectory directory("ownership");
    Log::init(minimal_sink_config());
    CrashHandler first;
    CrashHandler second;
    expect(first.install(path_to_utf8(directory.path())),
           "首个崩溃处理器应安装成功");
    expect(first.installed(), "安装后状态必须为 true");
    expect(!second.install(path_to_utf8(directory.path())),
           "同一进程不得同时安装两个崩溃处理器");
    first.uninstall();
    expect(!first.installed(), "卸载后状态必须为 false");
    expect(second.install(path_to_utf8(directory.path())),
           "原处理器卸载后新实例应可安装");
    second.uninstall();
    Log::shutdown();
}

bool run_child_process(const std::wstring& mode,
                       const std::filesystem::path& directory,
                       DWORD& exit_code) {
    std::array<wchar_t, 32'768> executable{};
    const DWORD executable_length = GetModuleFileNameW(
        nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (executable_length == 0U ||
        executable_length >= executable.size()) {
        return false;
    }

    const std::wstring executable_path(
        executable.data(), executable_length);
    std::wstring command_line = L"\"" + executable_path + L"\" " +
        mode + L" \"" + directory.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            executable_path.c_str(), command_line.data(), nullptr, nullptr,
            FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        return false;
    }

    const DWORD wait_result = WaitForSingleObject(process.hProcess, 30'000U);
    bool success = wait_result == WAIT_OBJECT_0;
    if (!success) {
        TerminateProcess(process.hProcess, 0xDEADU);
        WaitForSingleObject(process.hProcess, 5'000U);
    }
    if (!GetExitCodeProcess(process.hProcess, &exit_code)) success = false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return success;
}

void test_real_crash_child(const std::wstring& mode,
                           std::string_view expected_kind,
                           std::string_view expected_code,
                           std::string_view marker) {
    TempDirectory directory(
        mode == L"--seh-child" ? "seh" : "terminate");
    DWORD exit_code = 0U;
    expect(run_child_process(mode, directory.path(), exit_code),
           "真实崩溃子进程必须在超时前结束");
    expect(exit_code != 0U && exit_code != STILL_ACTIVE,
           "真实崩溃子进程必须以非零状态退出");

    const auto report = read_file(directory.path() / "crash_tail.log");
    expect(!report.empty(), "真实崩溃必须生成 crash_tail.log");
    expect(report.find(std::string("kind=") + std::string(expected_kind)) !=
               std::string::npos,
           "崩溃报告必须记录处理入口类型");
    expect(report.find(std::string("exception_code=") +
                       std::string(expected_code)) != std::string::npos,
           "崩溃报告必须记录异常码");
    expect(report.find(marker) != std::string::npos,
           "崩溃报告必须包含崩溃前唯一日志 marker");
    expect(report.find("emergency_tail_begin\r\n") != std::string::npos &&
               report.find("emergency_tail_end\r\n") != std::string::npos,
           "崩溃报告必须完整标记紧急尾部边界");
}

[[noreturn]] void run_crashing_child(
        const std::filesystem::path& directory, bool use_terminate) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    Log::init(minimal_sink_config());
    Log::register_module("crash_child", LogLevel::INFO);
    CrashHandler handler;
    if (!handler.install(path_to_utf8(directory))) {
        ExitProcess(70U);
    }

    if (use_terminate) {
        LOG_ERROR("crash_child", "terminate-child-unique-marker");
        std::terminate();
    }

    LOG_ERROR("crash_child", "seh-child-unique-marker");
    RaiseException(
        kChildExceptionCode, EXCEPTION_NONCONTINUABLE, 0U, nullptr);
    ExitProcess(71U);
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc == 3 && std::wstring_view(argv[1]) == L"--seh-child") {
        run_crashing_child(std::filesystem::path(argv[2]), false);
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--terminate-child") {
        run_crashing_child(std::filesystem::path(argv[2]), true);
    }

    try {
        test_emergency_tail_contract();
        test_concurrent_emergency_tail_is_not_torn();
        test_single_process_handler_ownership();
        test_real_crash_child(
            L"--seh-child", "unhandled_exception", "0xE0424242",
            "seh-child-unique-marker");
        test_real_crash_child(
            L"--terminate-child", "terminate", "0xE0000001",
            "terminate-child-unique-marker");
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] 测试出现未捕获异常: "
                  << exception.what() << '\n';
        ++failures;
    } catch (...) {
        std::cerr << "[FAIL] 测试出现未知未捕获异常\n";
        ++failures;
    }

    Log::shutdown();
    if (failures != 0) {
        std::cerr << failures << " 个 Crash 测试失败\n";
        return 1;
    }
    std::cout << "Crash 测试全部通过\n";
    return 0;
}
