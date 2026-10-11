#include "config/config.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <new>
#include <string>

namespace {
// 只链接到本测试进程；台账和失败注入本身不分配内存。
struct Allocation { void* pointer; std::size_t bytes; };
Allocation allocations[8192]{};
int allocation_count = 0;
std::size_t allocation_bytes = 0;
bool injecting = false;
int fail_at = 0;
int attempts = 0;
int failures = 0;

void* allocate(std::size_t size) {
    if (injecting && ++attempts == fail_at) return nullptr;
    void* value = std::malloc(size ? size : 1);
    if (!value) return nullptr;
    for (auto& item : allocations) {
        if (item.pointer) continue;
        item = {value, size};
        ++allocation_count;
        allocation_bytes += size;
        return value;
    }
    std::abort();
}

void release(void* value) noexcept {
    if (!value) return;
    for (auto& item : allocations) {
        if (item.pointer != value) continue;
        --allocation_count;
        allocation_bytes -= item.bytes;
        item = {};
        std::free(value);
        return;
    }
    std::abort();
}

void expect(bool condition, const char* message) {
    if (condition) return;
    ++failures;
    std::fprintf(stderr, "[失败] %s (allocation=%d)\n", message, fail_at);
}

std::string utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {value.begin(), value.end()};
}

std::string snapshot(const std::filesystem::path& path, const AppConfig& config) {
    std::string error;
    expect(save_app_config(utf8(path), config, error), "配置快照保存成功");
    std::ifstream input(path, std::ios::binary);
    expect(input.good(), "配置快照可读取");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

DWORD handle_count() {
    DWORD count = 0;
    expect(GetProcessHandleCount(GetCurrentProcess(), &count) != FALSE,
           "可以读取进程句柄数");
    return count;
}

void check_closed(const std::filesystem::path& path) {
    const auto handle = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    expect(handle != INVALID_HANDLE_VALUE, "读取结束后输入文件可独占打开");
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
}

void check_path(const std::filesystem::path& path, bool backend_only) {
    const auto input = utf8(path);
    const auto snapshot_path = path.parent_path() / L"snapshot.ini";
    // 预热 CRT / 序列化，并量出这份输入实际经历的全部分配点。
    int points = 0;
    {
        AppConfig config;
        BackendType backend{};
        std::string error;
        expect(load_app_config(input, config, error), "预热加载成功");
        snapshot(snapshot_path, config);
        attempts = 0;
        fail_at = 0;
        injecting = true;
        const bool ok = backend_only ? load_app_backend(input, backend, error)
                                     : load_app_config(input, config, error);
        injecting = false;
        points = attempts;
        expect(ok && points > 0, "无注入控制组加载成功");
    }
    check_closed(path);
    for (int point = 1; point <= points; ++point) {
        const int before_count = allocation_count;
        const auto before_bytes = allocation_bytes;
        const auto before_handles = handle_count();
        const int before_failures = failures;
        {
            AppConfig config;
            config.ui.width = 1234;
            const auto original = snapshot(snapshot_path, config);
            BackendType backend = static_cast<BackendType>(-37);
            std::string error;
            // 同一调用者连续失败三次，再使用同一配置对象成功恢复。
            for (int repeat = 0; repeat < 3; ++repeat) {
                attempts = 0;
                fail_at = point;
                injecting = true;
                const bool ok = backend_only ? load_app_backend(input, backend, error)
                                             : load_app_config(input, config, error);
                injecting = false;
                expect(!ok && !error.empty(), "分配失败必须上报错误");
                expect(backend == static_cast<BackendType>(-37), "失败保留原后端");
                expect(snapshot(snapshot_path, config) == original,
                       "失败保留完整可序列化配置");
            }
            fail_at = 0;
            const bool recovered = backend_only ? load_app_backend(input, backend, error)
                                                : load_app_config(input, config, error);
            expect(recovered && error.empty(), "连续失败后可成功恢复且清除错误");
            expect(backend_only ? backend == BackendType::DIRECTML : config.ui.width == 1280,
                   "恢复后的值来自输入文件");
        }
        expect(allocation_count == before_count && allocation_bytes == before_bytes,
               "失败和恢复后所有配置分配均释放");
        expect(handle_count() == before_handles, "失败和恢复后文件句柄均释放");
        check_closed(path);
        std::printf("{\"backend\":%s,\"point\":%d,\"repeats\":3,\"allocation_delta\":%d,"
                    "\"byte_delta\":%lld,\"handle_delta\":%lld,\"failures\":%d}\n",
                    backend_only ? "true" : "false", point, allocation_count - before_count,
                    static_cast<long long>(allocation_bytes) - static_cast<long long>(before_bytes),
                    static_cast<long long>(handle_count()) - before_handles, failures - before_failures);
    }
}
} // namespace

void* operator new(std::size_t size) { if (auto p = allocate(size)) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return allocate(size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return allocate(size); }
void operator delete(void* p) noexcept { release(p); }
void operator delete[](void* p) noexcept { release(p); }
void operator delete(void* p, std::size_t) noexcept { release(p); }
void operator delete[](void* p, std::size_t) noexcept { release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { release(p); }

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
        (L"xen-config-load-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    if (!std::filesystem::create_directory(directory)) return 2;
    const std::filesystem::path names[] = {L"ascii.ini", L"配置-\U0001F9EA.ini"};
    for (const auto& name : names) {
        const auto path = directory / name;
        {
            std::ofstream output(path, std::ios::binary);
            output << "; synthetic fixture\n\n[ui]\nwidth=1280\n[detector]\nbackend=directml\n";
            if (!output.good()) return 2;
        }
        for (const bool backend_only : {false, true}) check_path(path, backend_only);
    }
    // 只移除上面创建的三个已知文件；失败时保留反例供检查。
    if (!failures) {
        for (const auto& name : names) std::filesystem::remove(directory / name);
        std::filesystem::remove(directory / L"snapshot.ini");
        std::filesystem::remove(directory);
    }
    std::printf("config_load_failure_tests: failures=%d\n", failures);
    return failures ? 1 : 0;
}
