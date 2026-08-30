#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>

#ifdef ERROR
#undef ERROR
#endif

#include "benchmark/benchmark.h"
#include "benchmark/benchmark_internal.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

int failures = 0;

struct Win32HandleCloser {
    void operator()(void* handle) const noexcept {
        if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
};

using ScopedWin32Handle = std::unique_ptr<void, Win32HandleCloser>;

struct OwnedBenchmarkTemporaryRoot {
    std::filesystem::path temporary_base;
    std::filesystem::path path;
    std::filesystem::path owner_path;
    std::string leaf_name;
    std::string guid;
};

constexpr std::string_view kCancellationRootPrefix =
    "xen-benchmark-cancel-tests-";
constexpr std::string_view kReportConsumerRootPrefix =
    "xen-benchmark-report-consumer-";

std::string make_test_guid() {
    std::string guid = "00000000-0000-4000-8000-000000000000";
    constexpr char hexadecimal[] = "0123456789abcdef";
    std::random_device random;
    for (char& character : guid) {
        if (character == '0') {
            character = hexadecimal[random() & 0x0fU];
        }
    }
    guid[14] = '4';
    guid[19] = hexadecimal[8U + (random() & 0x03U)];
    return guid;
}

bool non_reparse_chain(const std::filesystem::path& path,
                       std::string& error) {
    auto current = std::filesystem::absolute(path).lexically_normal();
    for (;;) {
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
            error = "临时测试路径属性不可确认或包含 reparse point: " +
                current.string();
            return false;
        }
        const auto parent = current.parent_path();
        if (parent.empty() || parent == current) return true;
        current = parent;
    }
}

bool create_owned_benchmark_temporary_root(
        OwnedBenchmarkTemporaryRoot& root,
        std::string_view leaf_prefix,
        std::string& error) {
    root = {};
    if (leaf_prefix != kCancellationRootPrefix &&
        leaf_prefix != kReportConsumerRootPrefix) {
        error = "Benchmark 临时根前缀不属于已登记用途";
        return false;
    }
    root.temporary_base = std::filesystem::absolute(
        std::filesystem::temp_directory_path()).lexically_normal();
    if (root.temporary_base.filename().empty()) {
        root.temporary_base = root.temporary_base.parent_path();
    }
    if (!non_reparse_chain(root.temporary_base, error)) return false;
    root.guid = make_test_guid();
    root.leaf_name = std::string(leaf_prefix) + root.guid;
    root.path = root.temporary_base / root.leaf_name;
    std::error_code create_error;
    if (!std::filesystem::create_directory(root.path, create_error)) {
        error = "创建本轮 Benchmark 临时根失败: " +
            create_error.message();
        return false;
    }
    root.owner_path = root.path / ".xen-benchmark-report-owner";
    std::ofstream owner(root.owner_path, std::ios::binary);
    owner << root.guid;
    owner.close();
    if (!owner || !non_reparse_chain(root.owner_path, error)) {
        if (error.empty()) error = "写入本轮 Benchmark owner 失败";
        return false;
    }
    return true;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

bool cleanup_owned_benchmark_temporary_root(
        const OwnedBenchmarkTemporaryRoot& root,
        std::span<const std::filesystem::path> owned_files,
        std::string& error) {
    if (root.temporary_base.empty() || root.path.empty() ||
        root.owner_path.empty() || root.leaf_name.empty() ||
        root.guid.empty()) {
        error = "Benchmark 临时根所有权信息不完整";
        return false;
    }
    const auto normalized =
        std::filesystem::absolute(root.path).lexically_normal();
    const auto normalized_base =
        std::filesystem::absolute(root.temporary_base).lexically_normal();
    auto expected_base = std::filesystem::absolute(
        std::filesystem::temp_directory_path()).lexically_normal();
    if (expected_base.filename().empty()) {
        expected_base = expected_base.parent_path();
    }
    const auto normalized_owner =
        std::filesystem::absolute(root.owner_path).lexically_normal();
    const bool registered_leaf =
        root.leaf_name ==
            std::string(kCancellationRootPrefix) + root.guid ||
        root.leaf_name ==
            std::string(kReportConsumerRootPrefix) + root.guid;
    if (normalized_base != expected_base ||
        normalized.parent_path() != normalized_base ||
        normalized.filename() != root.leaf_name ||
        !registered_leaf ||
        normalized_owner != normalized / ".xen-benchmark-report-owner" ||
        !non_reparse_chain(normalized, error) ||
        !non_reparse_chain(normalized_owner, error) ||
        read_file(normalized_owner) != root.guid) {
        if (error.empty()) error = "Benchmark 临时根不属于本轮";
        return false;
    }

    std::vector<std::filesystem::path> normalized_owned_files;
    normalized_owned_files.reserve(owned_files.size() + 1U);
    for (const auto& path : owned_files) {
        const auto normalized_file =
            std::filesystem::absolute(path).lexically_normal();
        if (normalized_file.parent_path() != normalized ||
            normalized_file == normalized_owner ||
            std::find(normalized_owned_files.begin(),
                      normalized_owned_files.end(), normalized_file) !=
                normalized_owned_files.end()) {
            error = "Benchmark 临时文件不属于本轮根或重复登记";
            return false;
        }
        normalized_owned_files.push_back(normalized_file);
    }
    normalized_owned_files.push_back(normalized_owner);

    for (const auto& entry : std::filesystem::directory_iterator(normalized)) {
        const auto entry_path =
            std::filesystem::absolute(entry.path()).lexically_normal();
        if (std::find(normalized_owned_files.begin(),
                      normalized_owned_files.end(), entry_path) ==
            normalized_owned_files.end()) {
            error = "Benchmark 临时根包含非本轮登记文件";
            return false;
        }
    }
    for (const auto& path : normalized_owned_files) {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const DWORD win32_error = GetLastError();
            if (win32_error == ERROR_FILE_NOT_FOUND ||
                win32_error == ERROR_PATH_NOT_FOUND) {
                continue;
            }
            error = "无法确认本轮 Benchmark 临时文件属性";
            return false;
        }
        if ((attributes & (FILE_ATTRIBUTE_DIRECTORY |
                           FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
            error = "Benchmark 临时根包含不可安全删除的目标文件";
            return false;
        }
        std::error_code remove_error;
        if (!std::filesystem::remove(path, remove_error) || remove_error) {
            error = "删除本轮 Benchmark 临时文件失败: " +
                remove_error.message();
            return false;
        }
    }
    std::error_code remove_error;
    if (!std::filesystem::remove(normalized, remove_error) || remove_error) {
        error = "删除本轮 Benchmark 临时根失败（可能存在报告残留）: " +
            remove_error.message();
        return false;
    }
    return true;
}

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << message << '\n';
}

BenchmarkParseStatus parse(
        const std::vector<std::wstring_view>& arguments,
        BenchmarkOptions& options,
        std::string& error) {
    return parse_benchmark_options(arguments, options, error);
}

class TemporaryBenchmarkRun final {
public:
    TemporaryBenchmarkRun() noexcept {
        try {
            std::string error;
            if (!create_owned_benchmark_temporary_root(
                    owned_root_, kCancellationRootPrefix, error)) {
                return;
            }
            model_path_ = owned_root_.path / "model.onnx";
            sentinel_path_ = owned_root_.path / "sentinel.txt";
            std::ofstream model(
                model_path_, std::ios::binary | std::ios::trunc);
            std::ofstream sentinel(
                sentinel_path_, std::ios::binary | std::ios::trunc);
            model << "test-model-placeholder";
            sentinel << "keep";
            valid_ = model.good() && sentinel.good();
        } catch (...) {
        }
    }

    TemporaryBenchmarkRun(const TemporaryBenchmarkRun&) = delete;
    TemporaryBenchmarkRun& operator=(const TemporaryBenchmarkRun&) = delete;

    ~TemporaryBenchmarkRun() noexcept {
        try {
            const std::array owned_files{model_path_, sentinel_path_};
            std::string ignored;
            (void)cleanup_owned_benchmark_temporary_root(
                owned_root_, owned_files, ignored);
        } catch (...) {
        }
    }

    bool valid() const noexcept { return valid_; }

    BenchmarkOptions options() const {
        BenchmarkOptions options;
        options.model_path = model_path_.string();
        options.report_prefix = (owned_root_.path / "result").string();
        options.ready_file_path =
            (owned_root_.path / "result.ready.json").string();
        options.provider_profile_path =
            (owned_root_.path / "result.provider-profile.json").string();
        options.backend = BackendType::TENSORRT;
        options.backend_explicit = true;
        options.warmup_samples = 0;
        options.minimum_samples = 1;
        options.minimum_seconds = 0;
        options.maximum_seconds = 1;
        return options;
    }

    bool outputs_clean() const noexcept {
        try {
            const auto options_value = options();
            if (std::filesystem::exists(options_value.report_prefix + ".csv") ||
                std::filesystem::exists(options_value.report_prefix + ".json") ||
                std::filesystem::exists(options_value.ready_file_path) ||
                std::filesystem::exists(options_value.provider_profile_path)) {
                return false;
            }
            for (const auto& entry :
                 std::filesystem::directory_iterator(owned_root_.path)) {
                const std::string name = entry.path().filename().string();
                if (name.find(".pending.") != std::string::npos ||
                    name.find(".tmp.") != std::string::npos) {
                    return false;
                }
            }
            std::ifstream sentinel(sentinel_path_, std::ios::binary);
            std::string contents;
            sentinel >> contents;
            return std::filesystem::is_regular_file(model_path_) &&
                contents == "keep";
        } catch (...) {
            return false;
        }
    }

private:
    OwnedBenchmarkTemporaryRoot owned_root_;
    std::filesystem::path model_path_;
    std::filesystem::path sentinel_path_;
    bool valid_ = false;
};

void test_owned_benchmark_temporary_root_requires_precise_boundary_and_owner() {
    OwnedBenchmarkTemporaryRoot root;
    std::string error;
    const bool root_created = create_owned_benchmark_temporary_root(
        root, kCancellationRootPrefix, error);
    expect(root_created,
           "Benchmark 清理测试必须创建本轮 owned 临时根: " + error);
    if (!root_created) return;

    const auto owned_path = root.path / "owned.txt";
    {
        std::ofstream owned(owned_path, std::ios::binary | std::ios::trunc);
        owned << "owned";
    }
    const auto outside_path = root.temporary_base /
        ("xen-benchmark-outside-" + root.guid + ".txt");
    const std::array out_of_boundary{owned_path, outside_path};
    error.clear();
    expect(!cleanup_owned_benchmark_temporary_root(
               root, out_of_boundary, error) &&
               std::filesystem::is_regular_file(owned_path) &&
               std::filesystem::is_directory(root.path),
           "Benchmark 临时根清理必须先拒绝越界目标且不做部分删除");

    {
        std::ofstream foreign_owner(
            root.owner_path, std::ios::binary | std::ios::trunc);
        foreign_owner << "foreign-owner";
    }
    const std::array owned_files{owned_path};
    error.clear();
    expect(!cleanup_owned_benchmark_temporary_root(
               root, owned_files, error) &&
               std::filesystem::is_regular_file(owned_path) &&
               std::filesystem::is_directory(root.path),
           "Benchmark 临时根清理必须拒绝失配 owner 且不做部分删除");

    {
        std::ofstream owner(
            root.owner_path, std::ios::binary | std::ios::trunc);
        owner << root.guid;
    }
    error.clear();
    expect(cleanup_owned_benchmark_temporary_root(
               root, owned_files, error) &&
               !std::filesystem::exists(root.path),
           "恢复精确 owner 后只应删除本轮登记文件与空根: " + error);
}

void test_owned_benchmark_temporary_root_rejects_foreign_leaf_without_deletion() {
    OwnedBenchmarkTemporaryRoot root;
    std::string error;
    const bool root_created = create_owned_benchmark_temporary_root(
        root, kCancellationRootPrefix, error);
    expect(root_created,
           "foreign leaf 测试必须创建本轮 owned 临时根: " + error);
    if (!root_created) return;

    const std::string expected_leaf = root.leaf_name;
    const auto expected_path = root.path;
    const auto expected_owned_path = expected_path / "owned.txt";
    {
        std::ofstream owned(
            expected_owned_path, std::ios::binary | std::ios::trunc);
        owned << "owned";
    }
    const std::string foreign_leaf = std::string(kCancellationRootPrefix) +
        "foreign-" + root.guid;
    const auto foreign_path = root.temporary_base / foreign_leaf;
    std::error_code rename_error;
    std::filesystem::rename(expected_path, foreign_path, rename_error);
    expect(!rename_error,
           "测试必须把 owned 根改名为 prefix+foreign+guid: " +
               rename_error.message());
    if (rename_error) {
        const std::array owned_files{expected_owned_path};
        std::string cleanup_error;
        (void)cleanup_owned_benchmark_temporary_root(
            root, owned_files, cleanup_error);
        return;
    }

    root.leaf_name = foreign_leaf;
    root.path = foreign_path;
    root.owner_path = foreign_path / ".xen-benchmark-report-owner";
    const auto foreign_owned_path = foreign_path / "owned.txt";
    const std::array foreign_owned_files{foreign_owned_path};
    error.clear();
    const bool rejected = !cleanup_owned_benchmark_temporary_root(
        root, foreign_owned_files, error);
    expect(rejected &&
               std::filesystem::is_regular_file(foreign_owned_path) &&
               std::filesystem::is_regular_file(root.owner_path) &&
               std::filesystem::is_directory(foreign_path),
           "cleanup 必须拒绝 prefix+foreign+guid 且保持零删除");

    if (std::filesystem::is_directory(foreign_path)) {
        rename_error.clear();
        std::filesystem::rename(foreign_path, expected_path, rename_error);
        expect(!rename_error,
               "foreign leaf 测试必须恢复精确 owned 根: " +
                   rename_error.message());
        if (!rename_error) {
            root.leaf_name = expected_leaf;
            root.path = expected_path;
            root.owner_path =
                expected_path / ".xen-benchmark-report-owner";
            const std::array owned_files{expected_owned_path};
            std::string cleanup_error;
            expect(cleanup_owned_benchmark_temporary_root(
                       root, owned_files, cleanup_error),
                   "foreign leaf 测试收口不得遗留 owned 根: " +
                       cleanup_error);
        }
    }
}

struct BenchmarkRunAdapterState {
    bool fail_setup = false;
    bool block_setup = false;
    bool return_runtime = false;
    bool request_ctrl_break_before_runtime_start = false;
    bool ctrl_break_handled = false;
    bool setup_entered = false;
    bool release_setup = false;
    int setup_calls = 0;
    int runtime_factory_calls = 0;
    int before_runtime_start_calls = 0;
    int runtime_start_calls = 0;
    std::mutex mutex;
    std::condition_variable condition;
};

bool fake_provider_setup(
        void* context,
        const DetectorConfig&,
        const std::string& output_path,
        const char*,
        std::string& error) {
    auto& state = *static_cast<BenchmarkRunAdapterState*>(context);
    ++state.setup_calls;
    try {
        std::ofstream output(
            std::filesystem::u8path(output_path),
            std::ios::binary | std::ios::trunc);
        output << "{\"provider_profile\":true}\n";
        output.flush();
        if (!output.good()) {
            error = "注入 setup 无法创建 Provider profile";
            return false;
        }
    } catch (...) {
        error = "注入 setup 写入 Provider profile 异常";
        return false;
    }
    {
        std::unique_lock lock(state.mutex);
        state.setup_entered = true;
        state.condition.notify_all();
        state.condition.wait(lock, [&state] {
            return !state.block_setup || state.release_setup;
        });
    }
    if (state.fail_setup) {
        error = "注入 setup 失败";
        return false;
    }
    error.clear();
    return true;
}

std::unique_ptr<Runtime> fake_runtime_factory(void* context) {
    auto& state = *static_cast<BenchmarkRunAdapterState*>(context);
    ++state.runtime_factory_calls;
    if (state.return_runtime) return std::make_unique<Runtime>();
    return {};
}

bool fake_runtime_start(
        void* context,
        Runtime&,
        const AppConfig&) noexcept {
    auto& state = *static_cast<BenchmarkRunAdapterState*>(context);
    ++state.runtime_start_calls;
    return false;
}

void fake_before_runtime_start(void* context) noexcept {
    auto& state = *static_cast<BenchmarkRunAdapterState*>(context);
    ++state.before_runtime_start_calls;
    if (state.request_ctrl_break_before_runtime_start) {
        state.ctrl_break_handled =
            benchmark::detail::handle_benchmark_console_control(
                CTRL_BREAK_EVENT);
    }
}

benchmark::detail::BenchmarkRunAdapter adapter_for(
        BenchmarkRunAdapterState& state) noexcept {
    benchmark::detail::BenchmarkRunAdapter adapter;
    adapter.context = &state;
    adapter.setup_provider_profile = fake_provider_setup;
    adapter.create_runtime = fake_runtime_factory;
    adapter.start_runtime = fake_runtime_start;
    adapter.before_runtime_start = fake_before_runtime_start;
    return adapter;
}

bool wait_for_setup(BenchmarkRunAdapterState& state) {
    std::unique_lock lock(state.mutex);
    return state.condition.wait_for(
        lock, std::chrono::seconds(2), [&state] {
            return state.setup_entered;
        });
}

void release_setup(BenchmarkRunAdapterState& state) noexcept {
    std::lock_guard lock(state.mutex);
    state.release_setup = true;
    state.condition.notify_all();
}

void test_normal_setup_reaches_runtime_factory_and_cleans_outputs() {
    TemporaryBenchmarkRun run;
    expect(run.valid(), "正常 setup fixture 必须创建成功");
    if (!run.valid()) return;

    BenchmarkRunAdapterState state;
    std::string error;
    expect(!benchmark::detail::run_runtime_benchmark_with_adapter(
               run.options(), adapter_for(state), error),
           "fake Runtime factory 拒绝后正式编排应失败");
    expect(state.setup_calls == 1 && state.runtime_factory_calls == 1 &&
               error.find("创建 Runtime") != std::string::npos,
           "正常 setup 必须恰好到达一次 Runtime factory: " + error);
    expect(run.outputs_clean(),
           "fake Runtime factory 拒绝后必须清理本轮 profile/ready/pending");
}

void test_failed_setup_skips_runtime_and_cleans_outputs() {
    TemporaryBenchmarkRun run;
    expect(run.valid(), "失败 setup fixture 必须创建成功");
    if (!run.valid()) return;

    BenchmarkRunAdapterState state;
    state.fail_setup = true;
    std::string error;
    expect(!benchmark::detail::run_runtime_benchmark_with_adapter(
               run.options(), adapter_for(state), error),
           "setup 失败必须使正式编排失败");
    expect(state.setup_calls == 1 && state.runtime_factory_calls == 0 &&
               error == "注入 setup 失败",
           "setup 失败不得进入 Runtime 且必须保留原始错误: " + error);
    expect(run.outputs_clean(),
           "setup 失败后必须清理本轮 profile/ready/pending");
}

void test_stop_during_blocking_setup_skips_runtime_and_allows_next_run() {
    TemporaryBenchmarkRun run;
    expect(run.valid(), "阻塞 setup fixture 必须创建成功");
    if (!run.valid()) return;

    BenchmarkRunAdapterState cancelled;
    cancelled.block_setup = true;
    bool cancelled_result = true;
    std::string cancelled_error;
    const auto options = run.options();
    std::thread worker([&] {
        cancelled_result =
            benchmark::detail::run_runtime_benchmark_with_adapter(
                options, adapter_for(cancelled), cancelled_error);
    });
    const bool setup_is_blocked = wait_for_setup(cancelled);
    expect(setup_is_blocked,
           "测试必须先确认 production setup seam 已经阻塞");
    if (setup_is_blocked) {
        expect(benchmark::detail::handle_benchmark_console_control(
                   CTRL_C_EVENT),
               "CLI Ctrl+C handler 必须确认已处理 stop 事件");
    }
    release_setup(cancelled);
    worker.join();

    expect(!cancelled_result && cancelled.setup_calls == 1 &&
               cancelled.runtime_factory_calls == 0 &&
               cancelled_error.find("人工中止") != std::string::npos,
           "setup 期间 stop 必须在 Runtime factory 前失败: " +
               cancelled_error);
    expect(run.outputs_clean(),
           "setup 取消后必须清理 profile/ready/pending 且保留 fixture");

    BenchmarkRunAdapterState retry;
    std::string retry_error;
    expect(!benchmark::detail::run_runtime_benchmark_with_adapter(
               options, adapter_for(retry), retry_error),
           "重复运行到 fake Runtime factory 后应按注入边界失败");
    expect(retry.setup_calls == 1 && retry.runtime_factory_calls == 1 &&
               retry_error.find("创建 Runtime") != std::string::npos,
           "取消后的下一轮正常 setup 不得继承旧 stop: " + retry_error);
    expect(run.outputs_clean(),
           "重复运行失败后仍必须清理本轮全部临时产物");
}

void test_stop_after_runtime_factory_is_observed_before_start() {
    TemporaryBenchmarkRun run;
    expect(run.valid(), "Runtime start 前 stop fixture 必须创建成功");
    if (!run.valid()) return;

    BenchmarkRunAdapterState state;
    state.return_runtime = true;
    state.request_ctrl_break_before_runtime_start = true;
    std::string error;
    expect(!benchmark::detail::run_runtime_benchmark_with_adapter(
               run.options(), adapter_for(state), error),
           "Runtime factory 期间 stop 必须使正式编排失败");
    expect(state.setup_calls == 1 && state.runtime_factory_calls == 1 &&
               state.before_runtime_start_calls == 1 &&
               state.ctrl_break_handled && state.runtime_start_calls == 0 &&
               error.find("人工中止") != std::string::npos,
           "Ctrl+Break 必须在非空 Runtime 的 start 紧邻门被观察: " +
               error);
    expect(run.outputs_clean(),
           "Runtime start 前取消必须清理 profile/ready/pending");
}

void test_console_handler_only_claims_interrupt_events() {
    expect(!benchmark::detail::handle_benchmark_console_control(
               CTRL_CLOSE_EVENT),
           "CTRL_CLOSE 不得被当作普通可清理中止事件吞掉");
    expect(!benchmark::detail::handle_benchmark_console_control(
               CTRL_LOGOFF_EVENT),
           "CTRL_LOGOFF 不得被 Benchmark handler 吞掉");
    expect(!benchmark::detail::handle_benchmark_console_control(
               CTRL_SHUTDOWN_EVENT),
           "CTRL_SHUTDOWN 不得被当作普通可清理中止事件吞掉");
    expect(benchmark::detail::handle_benchmark_console_control(
               CTRL_BREAK_EVENT),
           "Ctrl+Break 必须与 Ctrl+C 一样只登记 stop 请求");
}

void test_console_stop_before_run_is_not_reset() {
    TemporaryBenchmarkRun run;
    expect(run.valid(), "run 前 console stop fixture 必须创建成功");
    if (!run.valid()) return;

    BenchmarkRunAdapterState state;
    benchmark::detail::prepare_benchmark_console_control();
    expect(benchmark::detail::handle_benchmark_console_control(
               CTRL_C_EVENT),
           "handler 安装后的 Ctrl+C 必须被当前 run 接收");
    std::string error;
    expect(!benchmark::detail::run_runtime_benchmark_with_adapter(
               run.options(), adapter_for(state), error),
           "run 前 Ctrl+C 必须使正式编排失败");
    expect(state.setup_calls == 1 && state.runtime_factory_calls == 0 &&
               error.find("人工中止") != std::string::npos,
           "runner 不得清除 handler 已接收的当前 run stop: " + error);
    expect(run.outputs_clean(),
           "run 前 stop 也必须沿公共收口清理全部临时产物");
}

void test_main_machine_defaults() {
    const std::vector<std::wstring_view> arguments{
        L"--model", L"model.onnx",
        L"--backend", L"tensorrt",
        L"--report-prefix", L"reports/runtime",
        L"--provider-profile", L"reports/runtime.provider-profile.json"};
    BenchmarkOptions options;
    std::string error;
    expect(parse(arguments, options, error) == BenchmarkParseStatus::READY,
           "完整必选参数应解析成功: " + error);
    const auto& geometry = options.expected_geometry;
    expect(geometry.source_width == 2560 &&
               geometry.source_height == 1440 &&
               geometry.encoded_width == 2560 &&
               geometry.encoded_height == 1440 &&
               geometry.roi_x == 1120 && geometry.roi_y == 560 &&
               geometry.roi_width == 320 && geometry.roi_height == 320 &&
               geometry.source_pixels_per_pixel_x == 1.0 &&
               geometry.source_pixels_per_pixel_y == 1.0,
           "默认几何必须是 2560x1440 主机中心 320x320 ROI");
    expect(options.warmup_samples == 100 &&
               options.minimum_samples == 10000 &&
               options.minimum_seconds == 300 &&
               options.maximum_seconds == 600 && options.enable_fp16 &&
               options.enable_cuda_graph &&
               options.enable_gpu_preprocess &&
               !options.enable_d3d11_cuda_interop &&
               !options.enable_d3d11_directml_interop &&
               !options.enable_performance_probes &&
               options.provider_profile_path ==
                   "reports/runtime.provider-profile.json",
            "正式门槛和 TensorRT 优化默认值必须稳定");
    const std::string usage = benchmark_usage();
    expect(usage.find("Ctrl+C") != std::string::npos &&
               usage.find("setup") != std::string::npos &&
               usage.find("不发布") != std::string::npos,
           "CLI help 必须说明 setup/正式阶段取消且不发布本轮产物");
}

void test_network_encoded_override() {
    const std::vector<std::wstring_view> arguments{
        L"--model", L"model.onnx",
        L"--backend", L"directml",
        L"--report-prefix", L"reports/network",
        L"--ready-file", L"reports/network.ready.json",
        L"--output-format", L"channel_first",
        L"--expect-source", L"2560x1440",
        L"--expect-encoded", L"320x320",
        L"--expect-roi", L"1120,560,320,320",
        L"--expect-scale", L"1,1",
        L"--warmup-samples", L"0",
        L"--minimum-samples", L"1",
        L"--minimum-seconds", L"0",
        L"--maximum-seconds", L"10",
        L"--fp16", L"off",
        L"--cuda-graph", L"off",
        L"--gpu-preprocess", L"off"};
    BenchmarkOptions options;
    std::string error;
    expect(parse(arguments, options, error) == BenchmarkParseStatus::READY,
           "网络几何和短冒烟门槛应解析成功: " + error);
    expect(options.backend == BackendType::DIRECTML &&
               options.output_format == OutputFormat::CHANNEL_FIRST &&
               options.expected_geometry.encoded_width == 320 &&
               options.expected_geometry.encoded_height == 320 &&
               options.warmup_samples == 0 &&
               options.minimum_samples == 1 &&
               options.minimum_seconds == 0 &&
               options.maximum_seconds == 10 &&
               options.ready_file_path == "reports/network.ready.json" &&
               !options.enable_fp16 && !options.enable_cuda_graph &&
               !options.enable_gpu_preprocess &&
               !options.enable_performance_probes,
           "辅机运行必须只覆盖编码尺寸，不改变主机 FOV/ROI 契约");
}

void test_performance_probe_option() {
    BenchmarkOptions options;
    std::string error;
    expect(parse({L"--model", L"model.onnx",
                  L"--backend", L"cpu",
                  L"--report-prefix", L"report",
                  L"--performance-probes", L"on"}, options, error) ==
               BenchmarkParseStatus::READY &&
               options.enable_performance_probes,
           "性能探针必须由正式命令行独立开启");
    expect(parse({L"--model", L"model.onnx",
                  L"--backend", L"cpu",
                  L"--report-prefix", L"report",
                  L"--performance-probes", L"maybe"}, options, error) ==
               BenchmarkParseStatus::INVALID &&
               error.find("--performance-probes") != std::string::npos,
           "性能探针非法开关必须拒绝");
}

void test_stop_after_formal_gate_prevents_report_finalize() {
    OwnedBenchmarkTemporaryRoot owned_root;
    std::string error;
    const bool root_created = create_owned_benchmark_temporary_root(
        owned_root, kReportConsumerRootPrefix, error);
    expect(root_created,
           "正式门槛后 stop 测试必须创建 owned 临时根: " + error);
    if (!root_created) return;

    const auto csv_path = owned_root.path / "cancelled.csv";
    const auto json_path = owned_root.path / "cancelled.json";
    DebugReportConfig config;
    config.csv_path = csv_path.string();
    config.json_path = json_path.string();
    config.session_id = "cancelled-after-formal-gate";
    config.max_samples = 1;

    RuntimePipelineSample sample;
    sample.sequence = 1;
    sample.profile.total_ms = 1.0;
    sample.detection_status = DetectionStatus::SUCCESS;
    sample.aim_status = AimStatus::SUCCESS;
    sample.mouse_status = MouseStatus::READY;
    benchmark::detail::FormalSampleTracker formal_tracker(1);
    expect(formal_tracker.observe(sample, error) &&
               formal_tracker.gates_satisfied(1, true),
           "测试必须先确认正式样本与时间门槛均已满足: " + error);

    DebugReport report;
    expect(report.start(config, error),
           "正式门槛后 stop 测试报告必须启动: " + error);
    report.ingest(std::span<const RuntimePipelineSample>(&sample, 1));
    RuntimeSnapshot final_snapshot;
    DebugCoverageSummary coverage;
    coverage.available = true;
    coverage.formal.sample_count = 1;

    benchmark::detail::prepare_benchmark_console_control();
    expect(benchmark::detail::handle_benchmark_console_control(
               CTRL_C_EVENT),
           "正式门槛满足后 Ctrl+C 必须登记 stop");
    error.clear();
    const bool finalized = benchmark::detail::finalize_report(
        report, final_snapshot, coverage, formal_tracker.summary(), 1,
        false, CaptureBackend::DESKTOP_DUPLICATION, error);
    expect(!finalized && error.find("人工中止") != std::string::npos &&
               !std::filesystem::exists(csv_path) &&
               !std::filesystem::exists(json_path),
           "正式阶段 stop 必须在 report finalize 前失败且不生成 pair: " +
               error);
    benchmark::detail::prepare_benchmark_console_control();

    const std::array owned_files{csv_path, json_path};
    std::string cleanup_error;
    expect(cleanup_owned_benchmark_temporary_root(
               owned_root, owned_files, cleanup_error),
           "正式门槛后 stop 测试不得遗留报告文件: " + cleanup_error);
}

void test_stop_at_benchmark_report_publish_boundary_keeps_staging_pair() {
    benchmark::detail::prepare_benchmark_console_control();
    OwnedBenchmarkTemporaryRoot owned_root;
    std::string error;
    const bool root_created = create_owned_benchmark_temporary_root(
        owned_root, kReportConsumerRootPrefix, error);
    expect(root_created,
           "最终发布 stop 测试必须创建 owned 临时根: " + error);
    if (!root_created) return;

    const auto csv_staging_path = owned_root.path / "result.csv.pending";
    const auto json_staging_path = owned_root.path / "result.json.pending";
    const auto csv_path = owned_root.path / "result.csv";
    const auto json_path = owned_root.path / "result.json";
    DebugReportConfig config;
    config.csv_path = csv_staging_path.string();
    config.json_path = json_staging_path.string();
    config.session_id = "cancelled-before-final-promotion";
    config.max_samples = 1;

    RuntimePipelineSample sample;
    sample.sequence = 1;
    sample.profile.total_ms = 1.0;
    sample.detection_status = DetectionStatus::SUCCESS;
    sample.aim_status = AimStatus::SUCCESS;
    sample.mouse_status = MouseStatus::READY;
    RuntimeSnapshot final_snapshot;
    DebugCoverageSummary coverage;
    coverage.available = true;
    coverage.formal.sample_count = 1;
    benchmark::detail::FormalSampleSummary formal_summary;
    formal_summary.formal_sample_count = 1;
    formal_summary.successful_samples = 1;
    formal_summary.retained_sample_count = 1;
    DebugReport report;
    expect(report.start(config, error),
           "最终发布 stop 测试报告必须启动: " + error);
    report.ingest(std::span<const RuntimePipelineSample>(&sample, 1));
    expect(benchmark::detail::finalize_report(
               report, final_snapshot, coverage, formal_summary, 1, false,
               CaptureBackend::DESKTOP_DUPLICATION, error) &&
               std::filesystem::is_regular_file(csv_staging_path) &&
               std::filesystem::is_regular_file(json_staging_path) &&
               !std::filesystem::exists(csv_path) &&
               !std::filesystem::exists(json_path),
           "测试必须先形成完整 staging pair 且尚未发布 final: " + error);

    expect(benchmark::detail::handle_benchmark_console_control(
               CTRL_C_EVENT),
           "最终 promotion 前 Ctrl+C 必须登记 stop");
    error.clear();
    const bool published = benchmark::detail::publish_benchmark_reports(
        csv_staging_path.string(), json_staging_path.string(),
        csv_path.string(), json_path.string(), error);
    expect(!published && error.find("人工中止") != std::string::npos &&
               std::filesystem::is_regular_file(csv_staging_path) &&
               std::filesystem::is_regular_file(json_staging_path) &&
               !std::filesystem::exists(csv_path) &&
               !std::filesystem::exists(json_path),
           "最终发布提交边界必须 fail-closed 并保留完整 staging pair: " +
               error);
    benchmark::detail::prepare_benchmark_console_control();

    const std::array owned_files{
        csv_staging_path, json_staging_path, csv_path, json_path};
    std::string cleanup_error;
    expect(cleanup_owned_benchmark_temporary_root(
               owned_root, owned_files, cleanup_error),
           "最终发布 stop 测试不得遗留 staging/final 文件: " +
               cleanup_error);
}

void test_benchmark_report_consumer_pair_publication() {
    benchmark::detail::prepare_benchmark_console_control();
    OwnedBenchmarkTemporaryRoot owned_root;
    std::string error;
    const bool root_created = create_owned_benchmark_temporary_root(
        owned_root, kReportConsumerRootPrefix, error);
    expect(root_created,
           "Benchmark consumer 测试必须创建本轮 owned 临时根: " + error);
    if (!root_created) return;

    const auto csv_path = owned_root.path / "runtime.csv";
    const auto json_path = owned_root.path / "runtime.json";
    DebugReportConfig config;
    config.csv_path = csv_path.string();
    config.json_path = json_path.string();
    config.max_samples = 1;

    RuntimePipelineSample sample;
    sample.sequence = 1;
    sample.profile.total_ms = 1.0;
    sample.detection_status = DetectionStatus::SUCCESS;
    sample.aim_status = AimStatus::SUCCESS;
    sample.mouse_status = MouseStatus::READY;
    RuntimeSnapshot final_snapshot;
    DebugCoverageSummary coverage;
    coverage.available = true;
    coverage.formal.sample_count = 1;
    benchmark::detail::FormalSampleSummary formal_summary;
    formal_summary.formal_sample_count = 1;
    formal_summary.successful_samples = 1;
    formal_summary.retained_sample_count = 1;
    const auto publish = [&](std::string_view session_id) {
        DebugReport report;
        config.session_id = session_id;
        const bool started = report.start(config, error);
        expect(started, "Benchmark consumer 报告应启动: " + error);
        if (!started) return false;
        report.ingest(std::span<const RuntimePipelineSample>(&sample, 1));
        return benchmark::detail::finalize_report(
            report, final_snapshot, coverage, formal_summary, 1, false,
            CaptureBackend::DESKTOP_DUPLICATION, error);
    };

    expect(publish("old-benchmark-session"),
           "Benchmark 生产 consumer seam 应发布成功 pair: " + error);
    const std::string old_csv = read_file(csv_path);
    const std::string old_json = read_file(json_path);
    expect(old_csv.find("old-benchmark-session") != std::string::npos &&
               old_json.find("old-benchmark-session") != std::string::npos,
           "Benchmark consumer 成功路径必须发布同一旧 session pair");

    ScopedWin32Handle json_reader(CreateFileW(
        json_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    expect(json_reader.get() != INVALID_HANDLE_VALUE,
           "测试必须锁定旧 JSON 注入第二文件发布失败");
    if (json_reader.get() != INVALID_HANDLE_VALUE) {
        expect(!publish("new-benchmark-session") && !error.empty(),
               "第二文件发布失败必须由 Benchmark consumer 显式拒绝");
    }
    json_reader.reset();

    expect(read_file(csv_path) == old_csv &&
               read_file(json_path) == old_json,
           "Benchmark consumer 失败后必须保留完整旧 pair");
    std::string cleanup_error;
    const std::array owned_files{csv_path, json_path};
    expect(cleanup_owned_benchmark_temporary_root(
               owned_root, owned_files, cleanup_error),
           "Benchmark consumer 失败后不得遗留临时/回滚文件: " +
               cleanup_error);
}

RuntimePipelineSample coverage_sample(
        std::uint64_t sequence,
        std::uint64_t overwritten) {
    RuntimePipelineSample sample;
    sample.sequence = sequence;
    sample.runtime_overwritten_frames = overwritten;
    return sample;
}

RuntimeSnapshot coverage_snapshot(std::uint64_t overwritten) {
    RuntimeSnapshot snapshot;
    snapshot.overwritten_frames = overwritten;
    return snapshot;
}

void test_coverage_phase_tracker() {
    benchmark::detail::CoverageTracker tracker;
    std::string error;
    expect(tracker.observe(
               benchmark::detail::CoveragePhase::STARTUP,
               coverage_sample(3, 2), error) &&
           tracker.observe(
               benchmark::detail::CoveragePhase::WARMUP,
               coverage_sample(4, 2), error) &&
           tracker.observe(
               benchmark::detail::CoveragePhase::WARMUP,
               coverage_sample(6, 3), error) &&
           tracker.observe(
               benchmark::detail::CoveragePhase::FORMAL,
               coverage_sample(7, 3), error) &&
           tracker.observe(
               benchmark::detail::CoveragePhase::FORMAL,
               coverage_sample(9, 4), error) &&
           tracker.finish(2, 2, coverage_snapshot(6), error),
           "startup/warmup/formal 覆盖分段应按样本端点闭合: " + error);
    const auto& summary = tracker.summary();
    expect(summary.available &&
               summary.warmup_start_overwritten_frames == 2 &&
               summary.warmup_end_overwritten_frames == 3 &&
               summary.formal_end_overwritten_frames == 6 &&
               summary.startup.runtime_overwritten_frames == 2 &&
               summary.startup.sequence_gaps == 2 &&
               summary.warmup.runtime_overwritten_frames == 1 &&
               summary.warmup.sequence_gaps == 1 &&
               summary.formal.runtime_overwritten_frames == 3 &&
               summary.formal.sequence_gaps == 1 &&
               summary.formal.trailing_runtime_overwritten_frames == 2 &&
               summary.formal.counter_matches_sequence_gaps,
           "覆盖分段必须同时报告累计差和 CSV sequence 缺口");

    benchmark::detail::CoverageTracker zero_warmup;
    expect(zero_warmup.observe(
               benchmark::detail::CoveragePhase::STARTUP,
               coverage_sample(1, 0), error) &&
           zero_warmup.observe(
               benchmark::detail::CoveragePhase::FORMAL,
               coverage_sample(2, 0), error) &&
           zero_warmup.finish(0, 1, coverage_snapshot(0), error),
           "warmup=0 时必须允许 startup 直接进入 formal");

    benchmark::detail::CoverageTracker final_regression;
    expect(final_regression.observe(
               benchmark::detail::CoveragePhase::STARTUP,
               coverage_sample(1, 1), error) &&
           final_regression.observe(
               benchmark::detail::CoveragePhase::FORMAL,
               coverage_sample(2, 2), error) &&
           !final_regression.finish(
               0, 1, coverage_snapshot(1), error),
           "Runtime 停止终值小于最后样本累计值时必须拒绝");

    benchmark::detail::CoverageTracker regression;
    expect(regression.observe(
               benchmark::detail::CoveragePhase::STARTUP,
               coverage_sample(2, 1), error) &&
           !regression.observe(
               benchmark::detail::CoveragePhase::FORMAL,
               coverage_sample(3, 0), error),
           "Runtime 覆盖累计值回退必须拒绝");

    benchmark::detail::CoverageTracker capture_regression;
    RuntimePipelineSample capture_first = coverage_sample(1, 0);
    capture_first.source_dropped_frames = 2;
    capture_first.transport_dropped_frames = 3;
    capture_first.transport_invalid_packets = 4;
    RuntimePipelineSample capture_second = coverage_sample(2, 0);
    capture_second.source_dropped_frames = 1;
    capture_second.transport_dropped_frames = 2;
    capture_second.transport_invalid_packets = 3;
    expect(capture_regression.observe(
               benchmark::detail::CoveragePhase::STARTUP,
               capture_first, error) &&
           !capture_regression.observe(
               benchmark::detail::CoveragePhase::FORMAL,
               capture_second, error),
           "Capture/传输累计值回退必须拒绝");
}

RuntimePipelineSample successful_phase_sample(std::uint64_t sequence) {
    RuntimePipelineSample sample = coverage_sample(sequence, 0);
    sample.detection_status = DetectionStatus::SUCCESS;
    sample.aim_status = AimStatus::SUCCESS;
    sample.mouse_status = MouseStatus::READY;
    return sample;
}

void test_sample_phase_tracker() {
    benchmark::detail::SamplePhaseTracker tracker(2);
    std::string error;
    const std::array expected_phases{
        benchmark::detail::CoveragePhase::STARTUP,
        benchmark::detail::CoveragePhase::WARMUP,
        benchmark::detail::CoveragePhase::WARMUP,
        benchmark::detail::CoveragePhase::FORMAL};
    const std::array expected_measurement_begins{false, false, true, false};
    for (std::size_t index = 0; index < expected_phases.size(); ++index) {
        benchmark::detail::SamplePhaseObservation observation;
        expect(tracker.observe(
                   successful_phase_sample(index + 1), observation, error) &&
                   observation.phase == expected_phases[index] &&
                   observation.measurement_begins ==
                       expected_measurement_begins[index],
               "同一 drain 批次必须精确跨过 startup/warmup/formal: " +
                   error);
    }
    // 模拟门槛满足后 stop() 的 final drain；后续成功样本必须继续归 formal。
    for (std::uint64_t sequence = 5; sequence <= 6; ++sequence) {
        benchmark::detail::SamplePhaseObservation observation;
        expect(tracker.observe(
                   successful_phase_sample(sequence), observation, error) &&
                   observation.phase ==
                       benchmark::detail::CoveragePhase::FORMAL,
               "final drain 的成功样本必须继续进入 formal: " + error);
    }
    RuntimePipelineSample failed = successful_phase_sample(7);
    failed.detection_status = DetectionStatus::INFERENCE_FAILED;
    benchmark::detail::SamplePhaseObservation ignored;
    expect(!tracker.observe(failed, ignored, error) &&
               tracker.startup_successful() == 1 &&
               tracker.warmup_successful() == 2 &&
               tracker.formal_successful() == 3 &&
               tracker.finish(coverage_snapshot(0), error),
           "失败样本不得改变阶段计数或进入正式报告");

    benchmark::detail::SamplePhaseTracker zero_warmup(0);
    benchmark::detail::SamplePhaseObservation startup;
    benchmark::detail::SamplePhaseObservation formal;
    expect(zero_warmup.observe(
               successful_phase_sample(1), startup, error) &&
           startup.phase == benchmark::detail::CoveragePhase::STARTUP &&
           startup.measurement_begins &&
           zero_warmup.observe(
               successful_phase_sample(2), formal, error) &&
           formal.phase == benchmark::detail::CoveragePhase::FORMAL &&
           zero_warmup.finish(coverage_snapshot(0), error),
           "warmup=0 必须在 startup 后立即开始 formal 计时");
}

void test_formal_sample_tracker_waits_for_time_gate_after_retention_limit() {
    constexpr std::uint64_t retention_capacity = 100000;
    benchmark::detail::FormalSampleTracker tracker(retention_capacity);
    const std::size_t storage_bytes = tracker.retention_storage_bytes();
    std::string error;
    for (std::uint64_t sequence = 1;
         sequence <= retention_capacity;
         ++sequence) {
        expect(tracker.observe(
                   successful_phase_sample(sequence), error),
               "容量内 formal 成功样本必须完整聚合: " +
                   error);
    }
    const auto& short_summary = tracker.summary();
    const auto short_spans = tracker.retained_sample_spans();
    expect(short_summary.formal_sample_count == retention_capacity &&
               short_summary.retained_sample_count == retention_capacity &&
               short_summary.omitted_sample_count == 0 &&
               short_spans[0].size() == retention_capacity &&
               short_spans[1].empty() &&
               short_spans[0].front().sequence == 1 &&
               short_spans[0].back().sequence == retention_capacity,
           "不超过容量的短报告必须完整留样");

    expect(tracker.observe(
               successful_phase_sample(retention_capacity + 1), error),
           "时间门未满足时，第 100001 个 formal 成功样本仍必须继续聚合: " +
               error);

    const auto& summary = tracker.summary();
    const auto retained_spans = tracker.retained_sample_spans();
    expect(!tracker.gates_satisfied(10000, false) &&
               summary.formal_sample_count == retention_capacity + 1 &&
               summary.successful_samples == retention_capacity + 1 &&
               summary.failed_samples == 0 &&
               summary.retained_sample_count == retention_capacity &&
               summary.omitted_sample_count == 1 &&
               summary.retained_sample_count + summary.omitted_sample_count ==
                   summary.formal_sample_count &&
               retained_spans[0].size() + retained_spans[1].size() ==
                   retention_capacity &&
               retained_spans[0].front().sequence == 2 &&
               retained_spans[1].back().sequence == retention_capacity + 1 &&
               tracker.retention_capacity() == retention_capacity &&
               tracker.retention_storage_bytes() == storage_bytes,
           "formal tracker 必须等待时间门，同时保持聚合、计数守恒和固定容量尾窗");

    tracker.release_retained_storage();
    const auto released_spans = tracker.retained_sample_spans();
    expect(tracker.retention_storage_bytes() == 0 &&
               released_spans[0].empty() && released_spans[1].empty() &&
               summary.formal_sample_count == retention_capacity + 1 &&
               summary.retained_sample_count == retention_capacity &&
               summary.omitted_sample_count == 1,
           "尾窗复制后必须释放样本库存，同时保留发布所需守恒计数");
}

void test_bounded_report_metadata_line() {
    std::string rewritten;
    expect(benchmark::detail::rewrite_report_samples_dropped_line(
               "# report_samples_dropped,0",
               benchmark::detail::ReportFileFormat::CSV, 1, rewritten) &&
               rewritten == "# report_samples_dropped,1",
           "CSV staging 报告必须显式写入省略样本数");
    expect(benchmark::detail::rewrite_report_samples_dropped_line(
               "  \"report_samples_dropped\": 0,",
               benchmark::detail::ReportFileFormat::JSON, 1, rewritten) &&
               rewritten == "  \"report_samples_dropped\": 1,",
           "JSON staging 报告必须显式写入省略样本数");
    expect(!benchmark::detail::rewrite_report_samples_dropped_line(
               "# sample_count,100000",
               benchmark::detail::ReportFileFormat::CSV, 1, rewritten),
           "非省略计数行不得被 staging 补写 seam 改动");
}

void test_invalid_options() {
    BenchmarkOptions options;
    std::string error;
    expect(parse({L"--model", L"model.onnx",
                  L"--report-prefix", L"report"}, options, error) ==
               BenchmarkParseStatus::INVALID &&
               error.find("--backend") != std::string::npos,
           "正式基准必须显式选择后端");
    expect(parse({L"--model", L"model.onnx",
                  L"--backend", L"cuda",
                  L"--report-prefix", L"report",
                  L"--provider-profile", L"report.provider.json",
                  L"--minimum-seconds", L"20",
                  L"--maximum-seconds", L"10"}, options, error) ==
               BenchmarkParseStatus::INVALID,
           "最大时限短于最小时长必须拒绝");
    expect(parse({L"--model", L"model.onnx",
                  L"--backend", L"cuda",
                  L"--report-prefix", L"report",
                  L"--provider-profile", L"report.provider.json",
                  L"--warmup-samples", L"100001"}, options, error) ==
               BenchmarkParseStatus::INVALID,
           "warmup 超过固定容量上限必须拒绝");
    expect(parse({L"--model", L"model.onnx",
                  L"--backend", L"cuda",
                  L"--report-prefix", L"report",
                  L"--provider-profile", L"report.provider.json",
                  L"--maximum-seconds", L"86401"}, options, error) ==
               BenchmarkParseStatus::INVALID,
           "最大运行时长超过一天必须拒绝");
    expect(parse({L"--model", L"model.onnx",
                  L"--backend", L"cuda",
                  L"--report-prefix", L"report",
                  L"--provider-profile", L"report.provider.json",
                  L"--output-format", L"unknown",
                  L"--maximum-seconds", L"10"}, options, error) ==
               BenchmarkParseStatus::INVALID,
           "未知模型输出契约必须拒绝");
    expect(parse({L"--model", L"model.onnx",
                  L"--backend", L"cuda",
                  L"--report-prefix", L"report",
                  L"--provider-profile", L"report.provider.json",
                  L"--expect-roi", L"2500,1400,320,320"}, options,
                 error) == BenchmarkParseStatus::INVALID,
           "越过主机 FOV 的 ROI 必须拒绝");
    expect(parse({L"--model", L"model.onnx",
                  L"--backend", L"cuda",
                  L"--report-prefix", L"report"}, options, error) ==
               BenchmarkParseStatus::INVALID &&
               error.find("--provider-profile") != std::string::npos,
           "TensorRT/CUDA 缺少节点级 Provider profile 必须拒绝");
    expect(parse({L"--model", L"model.onnx",
                  L"--backend", L"directml",
                  L"--report-prefix", L"report",
                  L"--provider-profile", L"unexpected.json"}, options,
                 error) == BenchmarkParseStatus::INVALID,
           "严格 DirectML 不应接受多余的 GPU Provider profile 参数");
    expect(parse({L"--model", L"model.onnx",
                  L"--backend", L"cpu",
                  L"--report-prefix", L"report",
                  L"--ready-file", L"report.ready.json"}, options,
                 error) == BenchmarkParseStatus::READY &&
               options.ready_file_path == "report.ready.json",
           "CPU 正式基准也应支持独立 ready-file 协调");
    expect(parse({L"--model", L"model.onnx",
                  L"--backend", L"directml",
                  L"--report-prefix", L"report",
                  L"--ready-file", L"first.ready.json",
                  L"--ready-file", L"second.ready.json"}, options,
                 error) == BenchmarkParseStatus::INVALID,
           "重复 ready-file 参数必须拒绝，避免清理目标歧义");
    expect(parse({L"--model", L"model.onnx",
                  L"--backend", L"directml",
                  L"--report-prefix", L"report",
                  L"--ready-file", L""}, options, error) ==
               BenchmarkParseStatus::INVALID,
           "显式传入空 ready-file 路径必须拒绝");
    expect(parse({L"--model", L"model.onnx",
                  L"--backend", L"cuda",
                  L"--report-prefix", L"report",
                  L"--provider-profile", L"report.provider.json",
                  L"--d3d11-cuda-interop", L"on"}, options, error) ==
               BenchmarkParseStatus::INVALID,
           "D3D11/CUDA 互操作不得用于普通 CUDA EP");
    expect(parse({L"--model", L"model.onnx",
                  L"--backend", L"tensorrt",
                  L"--report-prefix", L"report",
                  L"--provider-profile", L"report.provider.json",
                  L"--d3d11-cuda-interop", L"on",
                  L"--cuda-graph", L"off"}, options, error) ==
               BenchmarkParseStatus::INVALID,
           "D3D11/CUDA 互操作关闭 CUDA Graph 时必须拒绝");
    expect(parse({L"--model", L"model.onnx",
                  L"--backend", L"tensorrt",
                  L"--report-prefix", L"report",
                  L"--provider-profile", L"report.provider.json",
                  L"--d3d11-cuda-interop", L"on"}, options, error) ==
               BenchmarkParseStatus::READY &&
               options.enable_d3d11_cuda_interop,
           "TensorRT CUDA Graph GPU 前处理应接受显式 D3D11 互操作");
    expect(parse({L"--model", L"model.onnx",
                  L"--backend", L"directml",
                  L"--report-prefix", L"report",
                  L"--d3d11-directml-interop", L"on"}, options, error) ==
               BenchmarkParseStatus::READY &&
               options.enable_d3d11_directml_interop,
           "严格 DirectML 应接受显式 D3D11 资源桥接");
    expect(parse({L"--model", L"model.onnx",
                  L"--backend", L"cpu",
                  L"--report-prefix", L"report",
                  L"--d3d11-directml-interop", L"on"}, options, error) ==
               BenchmarkParseStatus::INVALID,
           "D3D11/DirectML 互操作不得用于 CPU 后端");
    expect(parse({L"--help"}, options, error) ==
               BenchmarkParseStatus::HELP,
           "--help 不应要求其他必选参数");
}

void test_provider_mapping() {
    expect(std::string(expected_provider_name(BackendType::TENSORRT)) ==
               "TensorrtExecutionProvider" &&
               std::string(expected_provider_name(BackendType::CUDA)) ==
               "CUDAExecutionProvider" &&
               std::string(expected_provider_name(BackendType::DIRECTML)) ==
               "DmlExecutionProvider" &&
               std::string(expected_provider_name(BackendType::OPENVINO)) ==
               "OpenVINOExecutionProvider" &&
               std::string(expected_provider_name(BackendType::CPU)) ==
               "CPUExecutionProvider",
           "请求后端必须映射到 ORT 实际 Provider 名称");
}

void test_openvino_options() {
    BenchmarkOptions options;
    std::string error;
    expect(parse({L"--model", L"model.onnx",
                  L"--backend", L"openvino",
                  L"--openvino-device", L"cpu",
                  L"--report-prefix", L"report",
                  L"--provider-profile", L"report.provider.json"},
                 options, error) == BenchmarkParseStatus::READY &&
               options.backend == BackendType::OPENVINO &&
               options.openvino_device == OpenVinoDevice::CPU &&
               options.openvino_device_explicit,
           "OpenVINO 正式基准必须解析显式设备与 Provider profile: " +
               error);
    expect(parse({L"--model", L"model.onnx",
                  L"--backend", L"openvino",
                  L"--report-prefix", L"report",
                  L"--provider-profile", L"report.provider.json"},
                 options, error) == BenchmarkParseStatus::INVALID,
           "OpenVINO 缺少显式设备时必须拒绝");
    expect(parse({L"--model", L"model.onnx",
                  L"--backend", L"cpu",
                  L"--openvino-device", L"cpu",
                  L"--report-prefix", L"report"},
                 options, error) == BenchmarkParseStatus::INVALID,
           "非 OpenVINO 后端不得接受 OpenVINO 设备参数");
}

void test_per_frame_geometry_validation() {
    BenchmarkExpectedGeometry expected;
    RuntimeFrameGeometry actual;
    actual.source_width = 2560;
    actual.source_height = 1440;
    actual.encoded_width = 2560;
    actual.encoded_height = 1440;
    actual.roi_x = 1120.0;
    actual.roi_y = 560.0;
    actual.roi_width = 320;
    actual.roi_height = 320;
    actual.source_pixels_per_pixel_x = 1.0;
    actual.source_pixels_per_pixel_y = 1.0;
    std::string error;
    expect(validate_benchmark_geometry(actual, expected, error),
           "主机默认逐帧几何应通过: " + error);

    actual.encoded_width = 1920;
    actual.encoded_height = 1080;
    expect(!validate_benchmark_geometry(actual, expected, error) &&
               error.find("actual=source 2560x1440") != std::string::npos,
           "辅机显示分辨率不得被误写成编码或主机 FOV 几何");

    actual.encoded_width = 2560;
    actual.encoded_height = 1440;
    actual.source_pixels_per_pixel_x = 4.0;
    expect(!validate_benchmark_geometry(actual, expected, error),
           "任一帧比例变化都必须被正式基准拒绝");
}

} // namespace

int main() {
    test_owned_benchmark_temporary_root_requires_precise_boundary_and_owner();
    test_owned_benchmark_temporary_root_rejects_foreign_leaf_without_deletion();
    test_normal_setup_reaches_runtime_factory_and_cleans_outputs();
    test_failed_setup_skips_runtime_and_cleans_outputs();
    test_stop_during_blocking_setup_skips_runtime_and_allows_next_run();
    test_stop_after_runtime_factory_is_observed_before_start();
    test_console_handler_only_claims_interrupt_events();
    test_console_stop_before_run_is_not_reset();
    test_main_machine_defaults();
    test_network_encoded_override();
    test_performance_probe_option();
    test_benchmark_report_consumer_pair_publication();
    test_stop_after_formal_gate_prevents_report_finalize();
    test_stop_at_benchmark_report_publish_boundary_keeps_staging_pair();
    test_coverage_phase_tracker();
    test_sample_phase_tracker();
    test_formal_sample_tracker_waits_for_time_gate_after_retention_limit();
    test_bounded_report_metadata_line();
    test_invalid_options();
    test_provider_mapping();
    test_openvino_options();
    test_per_frame_geometry_validation();
    if (failures != 0) {
        std::cerr << "Benchmark 测试失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "Benchmark 测试全部通过。\n";
    return 0;
}
