#include "config/config.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <SimpleIni.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstdio>
#include <sstream>

namespace config_test_io {
enum class Fault { NONE, WRITE, PARTIAL_WRITE, ZERO_WRITE, FLUSH, CLOSE, SHORT_SUCCESS };
Fault fault = Fault::NONE;
int write_calls = 0;
bool hit = false;

BOOL write(HANDLE handle, LPCVOID buffer, DWORD count, LPDWORD written, LPOVERLAPPED overlapped) {
    ++write_calls;
    if (fault == Fault::WRITE || (fault == Fault::PARTIAL_WRITE && write_calls > 1)) {
        hit = true;
        *written = 0;
        SetLastError(ERROR_DISK_FULL);
        return FALSE;
    }
    if (fault == Fault::ZERO_WRITE) {
        hit = true;
        *written = 0;
        SetLastError(ERROR_DISK_FULL);
        return TRUE;
    }
    if (fault == Fault::PARTIAL_WRITE || fault == Fault::SHORT_SUCCESS) {
        count = (std::min)(count, DWORD{37});
    }
    return ::WriteFile(handle, buffer, count, written, overlapped);
}

BOOL flush(HANDLE handle) {
    if (fault == Fault::FLUSH) {
        hit = true;
        SetLastError(ERROR_WRITE_FAULT);
        return FALSE;
    }
    return ::FlushFileBuffers(handle);
}

BOOL close(HANDLE handle) {
    const BOOL closed = ::CloseHandle(handle);
    if (fault == Fault::CLOSE && closed) {
        // 仅模拟调用方收到失败；真实句柄已释放，不伪造 OS 句柄泄漏。
        hit = true;
        SetLastError(ERROR_GEN_FAILURE);
        return FALSE;
    }
    return closed;
}
} // namespace config_test_io

// 单独编译实际保存实现，只替换这三个调用；生产 config target 不含注入。
#define WriteFile config_test_io::write
#define FlushFileBuffers config_test_io::flush
#define CloseHandle config_test_io::close
#include "../Xen/config/config.cpp"
#undef WriteFile
#undef FlushFileBuffers
#undef CloseHandle

namespace {
int io_failures = 0;
void expect_io(bool condition, const char* message) {
    if (condition) return;
    ++io_failures;
    std::fprintf(stderr, "[失败] %s\n", message);
}

std::string path_utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {value.begin(), value.end()};
}

std::string file_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

DWORD process_handles() {
    DWORD count = 0;
    expect_io(GetProcessHandleCount(GetCurrentProcess(), &count) != FALSE, "句柄数可读取");
    return count;
}

void check_directory(const std::filesystem::path& path, bool exists) {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(path.parent_path())) {
        ++count;
        expect_io(exists && entry.path() == path, "无多余临时文件");
    }
    expect_io(count == (exists ? 1 : 0), "目录只包含已发布的目标文件");
}

void check_io_path(const std::filesystem::path& path) {
    using config_test_io::Fault;
    const auto input = path_utf8(path);
    const std::string original = "[ui]\nwidth=1234\n";
    AppConfig config;
    config.ui.width = 1280;
    std::string error;
    expect_io(save_app_config(input, config, error), "无故障控制组成功");
    const auto expected = file_bytes(path);
    for (const auto fault : {Fault::WRITE, Fault::PARTIAL_WRITE, Fault::ZERO_WRITE, Fault::FLUSH, Fault::CLOSE}) {
        for (const bool exists : {false, true}) {
            std::filesystem::remove(path);
            if (exists) {
                std::ofstream output(path, std::ios::binary);
                output << original;
                expect_io(output.good(), "预置旧配置成功");
            }
            const DWORD before = process_handles();
            for (int repeat = 0; repeat < 3; ++repeat) {
                config_test_io::fault = fault;
                config_test_io::hit = false;
                config_test_io::write_calls = 0;
                const bool saved = save_app_config(input, config, error);
                config_test_io::fault = Fault::NONE;
                expect_io(config_test_io::hit && !saved && !error.empty(), "I/O 失败必须被命中并上报");
                expect_io(exists ? file_bytes(path) == original : !std::filesystem::exists(path),
                          "I/O 失败不得替换旧文件或发布新文件");
                check_directory(path, exists);
                expect_io(process_handles() == before, "I/O 失败不遗留句柄");
            }
            expect_io(save_app_config(input, config, error) && error.empty(), "关闭故障注入后重试成功");
            expect_io(file_bytes(path) == expected, "重试结果字节完整");
            AppConfig loaded;
            expect_io(load_app_config(input, loaded, error) && loaded.ui.width == 1280, "重试结果可正确读取");
            check_directory(path, true);
            std::printf("io fault=%d existing=%d repeats=3 failures=%d\n", static_cast<int>(fault), exists, io_failures);
        }
    }
    config_test_io::fault = Fault::SHORT_SUCCESS;
    config_test_io::write_calls = 0;
    const bool short_saved = save_app_config(input, config, error);
    config_test_io::fault = Fault::NONE;
    expect_io(short_saved && config_test_io::write_calls > 1 && file_bytes(path) == expected,
              "短写成功时继续写完所有字节");
    std::filesystem::remove(path);
}
} // namespace

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
        (L"xen-config-save-io-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    if (!std::filesystem::create_directory(directory)) return 2;
    check_io_path(directory / L"ascii.ini");
    check_io_path(directory / L"配置-\U0001F9EA.ini");
    if (!io_failures) std::filesystem::remove(directory);
    std::printf("config_save_io_failure_tests: cases=20 injected_calls=60 short_writes=2 failures=%d\n", io_failures);
    return io_failures ? 1 : 0;
}
