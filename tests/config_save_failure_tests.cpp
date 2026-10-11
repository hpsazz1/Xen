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
// 此独立测试进程的固定容量台账；不改变生产配置库的分配器。
struct Allocation { void* pointer; std::size_t bytes; };
Allocation allocations[16384]{};
int live_count = 0;
std::size_t live_bytes = 0;
bool injecting = false;
int fail_at = 0, attempts = 0;
bool failed_nothrow = false;
std::size_t failed_size = 0;
int failures = 0, cases = 0, injected_calls = 0;

void* allocate(std::size_t size, bool nothrow) {
    if (injecting && ++attempts == fail_at) {
        failed_nothrow = nothrow;
        failed_size = size;
        return nullptr;
    }
    void* value = std::malloc(size ? size : 1);
    if (!value) return nullptr;
    for (auto& item : allocations) {
        if (item.pointer) continue;
        item = {value, size};
        ++live_count;
        live_bytes += size;
        return value;
    }
    std::abort();
}

void release(void* value) noexcept {
    if (!value) return;
    for (auto& item : allocations) {
        if (item.pointer != value) continue;
        --live_count;
        live_bytes -= item.bytes;
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

std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void expect_no_temporary_files(const std::filesystem::path& path) {
    for (const auto& entry : std::filesystem::directory_iterator(path.parent_path())) {
        expect(entry.path() == path, "保存结束后不能残留临时文件");
    }
}

DWORD handles() {
    DWORD value = 0;
    expect(GetProcessHandleCount(GetCurrentProcess(), &value) != FALSE, "可读取句柄数");
    return value;
}

void check_case(const std::filesystem::path& path, const std::string& input,
                const AppConfig& config, const std::string& expected,
                int point, int repeats, bool existing) {
    constexpr const char* original = "[ui]\nwidth=1234\n";
    if (existing) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << original;
        expect(output.good(), "预置旧文件成功");
    } else {
        std::filesystem::remove(path);
    }
    const int before_count = live_count;
    const auto before_bytes = live_bytes;
    const auto before_handles = handles();
    const int before_failures = failures;
    bool false_success = false;
    int faulty_list_size = -1;
    {
        std::string error;
        for (int repeat = 0; repeat < repeats; ++repeat) {
            fail_at = point;
            attempts = 0;
            failed_size = 0;
            injecting = true;
            const bool saved = save_app_config(input, config, error);
            injecting = false;
            ++injected_calls;
            false_success |= saved;
            if (saved) {
                AppConfig partial;
                std::string read_error;
                if (load_app_config(input, partial, read_error)) {
                    faulty_list_size = static_cast<int>(partial.aim.person_class_ids.size());
                }
            }
            expect(failed_size > 0, "命中指定分配失败点");
            expect(!saved && !error.empty(), "分配失败必须明确拒绝保存");
            expect(existing ? read_bytes(path) == original : !std::filesystem::exists(path),
                   "失败不得覆盖旧文件或发布新文件");
            expect_no_temporary_files(path);
        }
        expect(save_app_config(input, config, error) && error.empty(), "失败后重试成功并清除错误");
        expect(read_bytes(path) == expected, "重试写出完整且相同的配置字节");
        AppConfig loaded;
        expect(load_app_config(input, loaded, error), "重试结果可读取");
        expect(loaded.ui.width == config.ui.width &&
               loaded.movement.enabled == config.movement.enabled &&
               loaded.aim.person_class_ids == config.aim.person_class_ids &&
               loaded.log.module_levels == config.log.module_levels,
               "读取结果保留标量、长列表与模块设置");
        expect_no_temporary_files(path);
    }
    expect(live_count == before_count && live_bytes == before_bytes, "保存失败及恢复不泄漏配置分配");
    expect(handles() == before_handles, "保存失败及恢复不泄漏文件句柄");
    ++cases;
    std::printf("{\"point\":%d,\"repeats\":%d,\"existing\":%s,\"failed_nothrow\":%s,"
                "\"failed_size\":%zu,\"false_success\":%s,\"allocation_delta\":%d,"
                "\"byte_delta\":%lld,\"faulty_list_size\":%d,\"failures\":%d}\n",
                point, repeats, existing ? "true" : "false", failed_nothrow ? "true" : "false",
                failed_size, false_success ? "true" : "false", live_count - before_count,
                static_cast<long long>(live_bytes) - static_cast<long long>(before_bytes),
                faulty_list_size,
                failures - before_failures);
}

void check_path(const std::filesystem::path& path) {
    const auto input = utf8(path);
    AppConfig config;
    config.ui.width = 1280;
    config.movement.enabled = true; // 只序列化，绝不启动 Runtime 或设备。
    config.aim.person_class_ids.clear();
    for (int id = 0; id < 300; ++id) config.aim.person_class_ids.push_back(id);
    config.log.module_levels = {{"fixture", LogLevel::WARN}};
    std::string error;
    expect(save_app_config(input, config, error), "预热保存成功");
    const auto expected = read_bytes(path);
    attempts = 0;
    fail_at = 0;
    injecting = true;
    const bool control = save_app_config(input, config, error);
    injecting = false;
    const int points = attempts;
    expect(control && points > 0 && read_bytes(path) == expected, "无注入控制组完整保存");
    for (int point = 1; point <= points; ++point) {
        check_case(path, input, config, expected, point, 1, true);
    }
    // 缺失文件和重复失败另覆盖首、中、末分配点，避免只测覆盖旧文件。
    for (const int point : {1, points / 2, points}) {
        check_case(path, input, config, expected, point, 3, false);
        check_case(path, input, config, expected, point, 3, true);
    }
    std::filesystem::remove(path);
}
} // namespace

void* operator new(std::size_t size) { if (auto p = allocate(size, false)) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return allocate(size, true); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return allocate(size, true); }
void operator delete(void* p) noexcept { release(p); }
void operator delete[](void* p) noexcept { release(p); }
void operator delete(void* p, std::size_t) noexcept { release(p); }
void operator delete[](void* p, std::size_t) noexcept { release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { release(p); }

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
        (L"xen-config-save-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    if (!std::filesystem::create_directory(directory)) return 2;
    check_path(directory / L"ascii.ini");
    check_path(directory / L"配置-\U0001F9EA.ini");
    if (!failures) std::filesystem::remove(directory);
    std::printf("config_save_failure_tests: cases=%d injected_calls=%d failures=%d\n", cases, injected_calls, failures);
    return failures ? 1 : 0;
}
