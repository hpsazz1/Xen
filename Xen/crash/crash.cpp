#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "crash/crash.h"

#include "log/log.h"

#include <Windows.h>

#ifdef ERROR
#undef ERROR
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <mutex>
#include <string>

namespace {

constexpr std::size_t kMaximumReportPath = 32'768;
constexpr DWORD kTerminateExceptionCode = 0xE0000001U;
constexpr UINT kTerminateExitCode = 0xE0000001U;

static_assert(std::atomic<bool>::is_always_lock_free,
              "崩溃处理器状态必须使用无锁原子操作");

std::mutex install_mutex;
CrashHandler* installed_owner = nullptr;
std::array<wchar_t, kMaximumReportPath> report_path{};
LPTOP_LEVEL_EXCEPTION_FILTER previous_exception_filter = nullptr;
std::terminate_handler previous_terminate_handler = nullptr;
std::atomic<bool> handler_active{false};
std::atomic_flag handling_crash = ATOMIC_FLAG_INIT;

struct FixedTextWriter {
    char* data = nullptr;
    std::size_t capacity = 0;
    std::size_t size = 0;

    void append(const char* text) noexcept {
        if (!text) return;
        while (*text != '\0' && size < capacity) {
            data[size++] = *text++;
        }
    }

    void append_character(char value) noexcept {
        if (size < capacity) data[size++] = value;
    }

    void append_decimal(std::uint64_t value,
                        std::size_t minimum_digits = 1U) noexcept {
        char digits[20]{};
        std::size_t count = 0;
        do {
            digits[count++] = static_cast<char>('0' + (value % 10U));
            value /= 10U;
        } while (value != 0U && count < sizeof(digits));
        while (count < minimum_digits && count < sizeof(digits)) {
            digits[count++] = '0';
        }
        while (count > 0U) append_character(digits[--count]);
    }

    void append_hexadecimal(std::uint64_t value,
                            std::size_t digits) noexcept {
        constexpr char kHexDigits[] = "0123456789ABCDEF";
        append("0x");
        for (std::size_t index = digits; index > 0U; --index) {
            const auto shift = static_cast<unsigned>((index - 1U) * 4U);
            append_character(kHexDigits[(value >> shift) & 0x0FU]);
        }
    }
};

bool write_file_bytes(void* context, const char* data,
                      std::size_t size) noexcept {
    if (!context || (!data && size != 0U)) return false;
    const auto file = static_cast<HANDLE>(context);
    while (size > 0U) {
        const auto chunk = size > static_cast<std::size_t>(MAXDWORD)
            ? MAXDWORD
            : static_cast<DWORD>(size);
        DWORD written = 0;
        if (!WriteFile(file, data, chunk, &written, nullptr) ||
            written == 0U) {
            return false;
        }
        data += written;
        size -= written;
    }
    return true;
}

std::size_t build_report_header(
        char* output, std::size_t capacity, const char* kind,
        DWORD exception_code, const void* exception_address) noexcept {
    SYSTEMTIME utc{};
    GetSystemTime(&utc);

    FixedTextWriter writer{output, capacity, 0U};
    writer.append("\r\n=== Xen 崩溃诊断 ===\r\nkind=");
    writer.append(kind);
    writer.append("\r\nutc=");
    writer.append_decimal(utc.wYear, 4U);
    writer.append_character('-');
    writer.append_decimal(utc.wMonth, 2U);
    writer.append_character('-');
    writer.append_decimal(utc.wDay, 2U);
    writer.append_character('T');
    writer.append_decimal(utc.wHour, 2U);
    writer.append_character(':');
    writer.append_decimal(utc.wMinute, 2U);
    writer.append_character(':');
    writer.append_decimal(utc.wSecond, 2U);
    writer.append_character('.');
    writer.append_decimal(utc.wMilliseconds, 3U);
    writer.append("Z\r\npid=");
    writer.append_decimal(GetCurrentProcessId());
    writer.append("\r\ntid=");
    writer.append_decimal(GetCurrentThreadId());
    writer.append("\r\nexception_code=");
    writer.append_hexadecimal(exception_code, 8U);
    writer.append("\r\nexception_address=");
    writer.append_hexadecimal(
        reinterpret_cast<std::uintptr_t>(exception_address),
        sizeof(std::uintptr_t) * 2U);
    writer.append("\r\nemergency_tail_begin\r\n");
    return writer.size;
}

bool write_crash_report(const char* kind,
                        EXCEPTION_POINTERS* exception) noexcept {
    if (handling_crash.test_and_set(std::memory_order_acq_rel)) {
        return false;
    }

    const auto file = CreateFileW(
        report_path.data(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    DWORD exception_code = 0U;
    const void* exception_address = nullptr;
    if (exception && exception->ExceptionRecord) {
        exception_code = exception->ExceptionRecord->ExceptionCode;
        exception_address = exception->ExceptionRecord->ExceptionAddress;
    }

    char header[1'024]{};
    const auto header_size = build_report_header(
        header, sizeof(header), kind, exception_code, exception_address);
    bool success = write_file_bytes(file, header, header_size);
    if (success) {
        success = Log::dump_emergency_tail(file, write_file_bytes);
    }
    constexpr char kReportEnd[] = "emergency_tail_end\r\n";
    if (success) {
        success = write_file_bytes(
            file, kReportEnd, sizeof(kReportEnd) - 1U);
    }
    if (!FlushFileBuffers(file)) success = false;
    CloseHandle(file);
    return success;
}

LONG WINAPI unhandled_exception_filter(
        EXCEPTION_POINTERS* exception) noexcept {
    if (!handler_active.load(std::memory_order_acquire)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    write_crash_report("unhandled_exception", exception);
    return EXCEPTION_EXECUTE_HANDLER;
}

[[noreturn]] void xen_terminate_handler() noexcept {
    CONTEXT context{};
    RtlCaptureContext(&context);

    void* address = nullptr;
#if defined(_M_X64) || defined(__x86_64__)
    address = reinterpret_cast<void*>(context.Rip);
#elif defined(_M_IX86) || defined(__i386__)
    address = reinterpret_cast<void*>(context.Eip);
#elif defined(_M_ARM64) || defined(__aarch64__)
    address = reinterpret_cast<void*>(context.Pc);
#endif

    EXCEPTION_RECORD record{};
    record.ExceptionCode = kTerminateExceptionCode;
    record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    record.ExceptionAddress = address;
    EXCEPTION_POINTERS exception{&record, &context};
    if (handler_active.load(std::memory_order_acquire)) {
        write_crash_report("terminate", &exception);
    }
    TerminateProcess(GetCurrentProcess(), kTerminateExitCode);
    ExitProcess(kTerminateExitCode);
}

bool prepare_report_path(const std::string& log_dir) {
    if (log_dir.empty()) return false;

    std::error_code error;
    auto directory = std::filesystem::absolute(
        std::filesystem::u8path(log_dir), error);
    if (error) return false;
    directory = directory.lexically_normal();
    std::filesystem::create_directories(directory, error);
    if (error || !std::filesystem::is_directory(directory, error) || error) {
        return false;
    }

    const auto native = (directory / "crash_tail.log").native();
    if (native.empty() || native.size() >= report_path.size()) return false;
    std::fill(report_path.begin(), report_path.end(), L'\0');
    std::copy(native.begin(), native.end(), report_path.begin());
    return true;
}

} // namespace

CrashHandler::~CrashHandler() {
    uninstall();
}

bool CrashHandler::install(const std::string& log_dir) noexcept {
    Log::register_module("crash", LogLevel::INFO);
    try {
        std::lock_guard<std::mutex> lock(install_mutex);
        if (installed_owner != nullptr) {
            LOG_ERROR("crash", "进程中已经安装了崩溃处理器");
            return false;
        }
        if (!prepare_report_path(log_dir)) {
            LOG_ERROR("crash", "崩溃报告目录无效: {}", log_dir);
            return false;
        }

        handling_crash.clear(std::memory_order_release);
        previous_exception_filter =
            SetUnhandledExceptionFilter(unhandled_exception_filter);
        previous_terminate_handler =
            std::set_terminate(xen_terminate_handler);
        installed_owner = this;
        installed_.store(true, std::memory_order_release);
        handler_active.store(true, std::memory_order_release);
        LOG_INFO("crash", "崩溃诊断已安装: {}", log_dir);
        return true;
    } catch (const std::exception& exception) {
        LOG_ERROR("crash", "崩溃诊断安装失败: {}", exception.what());
    } catch (...) {
        LOG_ERROR("crash", "崩溃诊断安装失败: 未知异常");
    }
    return false;
}

void CrashHandler::uninstall() noexcept {
    if (!installed_.load(std::memory_order_acquire)) return;
    try {
        std::lock_guard<std::mutex> lock(install_mutex);
        if (installed_owner != this) {
            installed_.store(false, std::memory_order_release);
            return;
        }

        handler_active.store(false, std::memory_order_release);
        const auto observed_filter =
            SetUnhandledExceptionFilter(previous_exception_filter);
        if (observed_filter != unhandled_exception_filter) {
            SetUnhandledExceptionFilter(observed_filter);
        }
        const auto observed_terminate =
            std::set_terminate(previous_terminate_handler);
        if (observed_terminate != xen_terminate_handler) {
            std::set_terminate(observed_terminate);
        }
        installed_owner = nullptr;
        installed_.store(false, std::memory_order_release);
        LOG_INFO("crash", "崩溃诊断已卸载");
    } catch (...) {
        // 关闭阶段不得抛出异常；进程级回调即使恢复失败也不访问已释放成员。
        handler_active.store(false, std::memory_order_release);
        installed_owner = nullptr;
        installed_.store(false, std::memory_order_release);
    }
}

bool CrashHandler::installed() const noexcept {
    return installed_.load(std::memory_order_acquire);
}
