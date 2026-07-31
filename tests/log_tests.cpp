#include "log/log.h"

#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "[FAIL] " << message << '\n';
        ++failures;
    }
}

LogConfig ring_only_config(int capacity = 128) {
    LogConfig config;
    config.enable_console = false;
    config.enable_file = false;
    config.enable_debug_file = false;
    config.enable_ringbuf = true;
    config.ringbuf_capacity = capacity;
    return config;
}

bool contains_text(const std::vector<std::string>& lines,
                   const std::string& text) {
    for (const auto& line : lines) {
        if (line.find(text) != std::string::npos) return true;
    }
    return false;
}

std::size_t count_text(const std::string& content,
                       const std::string& text) {
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = content.find(text, offset)) != std::string::npos) {
        ++count;
        offset += text.size();
    }
    return count;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream),
                       std::istreambuf_iterator<char>());
}

bool wait_for_file_text(const std::filesystem::path& path,
                        const std::string& text) {
    using namespace std::chrono_literals;
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (read_file(path).find(text) != std::string::npos) return true;
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

struct TemporaryModuleView {
    char* text;
    std::size_t size;

    operator std::string_view() const noexcept {
        return std::string_view(text, size);
    }

    ~TemporaryModuleView() {
        // 析构时改变外部缓冲区，使宏若未延长临时对象生命周期就会稳定查找失败。
        text[0] = 'x';
    }
};

struct TemporaryCStringOwner {
    char* text;

    const char* c_str() const noexcept { return text; }

    ~TemporaryCStringOwner() {
        // 模拟 make_string().c_str()：拥有者析构后指针指向的内容立即改变。
        text[0] = 'x';
    }
};

class TempDirectory {
public:
    TempDirectory() {
        const auto nonce = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("xen_log_tests_" + std::to_string(nonce));
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

void test_invalid_config_can_retry() {
    Log::shutdown();

    auto config = ring_only_config(0);
    Log::init(config);
    expect(!Log::initialized(), "零容量 ring 配置必须初始化失败");

    config = ring_only_config(65'537);
    Log::init(config);
    expect(!Log::initialized(), "超大 ring 配置必须在预分配前被拒绝");

    config = ring_only_config(32);
    config.module_levels.emplace("bad module", LogLevel::INFO);
    Log::init(config);
    expect(!Log::initialized(), "非法模块名必须在初始化前被拒绝");

    config = ring_only_config(32);
    for (int index = 0; index < 65; ++index) {
        config.module_levels.emplace(
            "module-" + std::to_string(index), LogLevel::INFO);
    }
    Log::init(config);
    expect(!Log::initialized(), "模块等级覆盖数量超过上限时必须拒绝");

    config = ring_only_config(32);
    Log::init(config);
    expect(Log::initialized(), "非法配置失败后必须允许合法配置重试");
    Log::shutdown();

    config = ring_only_config(32);
    config.enable_ringbuf = false;
    config.enable_file = true;
    config.log_dir = ".";
    config.file_max_size_mb = 0;
    Log::init(config);
    expect(!Log::initialized(), "零文件大小配置必须初始化失败");
    Log::shutdown();

    TempDirectory directory;
    const auto occupied_path = directory.path() / "not-a-directory";
    {
        std::ofstream occupied_file(occupied_path, std::ios::binary);
        occupied_file << "occupied";
    }
    config.file_max_size_mb = 1;
    config.log_dir = occupied_path.string();
    Log::init(config);
    expect(!Log::initialized(), "sink 构造失败后不得发布半初始化实例");
    Log::init(ring_only_config(32));
    expect(Log::initialized(), "sink 构造失败后必须允许重新初始化");
    Log::shutdown();
}

void test_unregistered_is_dropped_and_ring_is_immediate() {
    Log::shutdown();
    Log::register_module("before-init", LogLevel::INFO);
    expect(!Log::should_log("before-init", LogLevel::INFO),
           "未初始化时模块不得变为可写状态");
    Log::init(ring_only_config());
    expect(Log::initialized(), "ring-only 配置初始化失败");

    int argument_evaluations = 0;
    LOG_INFO("missing", "未注册日志 {}", ++argument_evaluations);
    expect(argument_evaluations == 0,
           "未注册模块必须在求值格式化参数前被宏过滤");
    expect(Log::get_ring_buffer().empty(), "未注册模块日志不得进入 ring");

    Log::register_module("registered", LogLevel::INFO);
    expect(Log::should_log("registered", LogLevel::INFO),
           "合法模块注册后必须可写");
    LOG_INFO("registered", "即时可见标记={}", 17);
    const auto lines = Log::get_ring_buffer();
    expect(contains_text(lines, "即时可见标记=17"),
           "日志宏返回后 ring 必须立即可读");

    Log::set_level("registered", LogLevel::WARN);
    LOG_INFO("registered", "不应求值 {}", ++argument_evaluations);
    expect(argument_evaluations == 0,
           "低于模块等级的日志不得求值格式化参数");
    Log::shutdown();
}

void test_macro_extends_temporary_module_lifetime() {
    Log::shutdown();
    Log::init(ring_only_config());
    expect(Log::initialized(), "临时模块名测试初始化失败");
    Log::register_module("temporary", LogLevel::INFO);

    char module_name[] = "temporary";
    LOG_INFO((TemporaryModuleView{module_name, sizeof(module_name) - 1}),
             "临时模块名可用");
    expect(module_name[0] == 'x', "临时模块名对象必须在宏结束后析构");
    expect(contains_text(Log::get_ring_buffer(), "临时模块名可用"),
           "日志宏必须延长临时模块表达式的生命周期");

    char nested_temporary_name[] = "temporary-cstr";
    Log::register_module(nested_temporary_name, LogLevel::INFO);
    LOG_INFO(TemporaryCStringOwner{nested_temporary_name}.c_str(),
             "嵌套临时模块名可用");
    expect(nested_temporary_name[0] == 'x',
           "嵌套临时模块名拥有者必须在模块名复制后析构");
    expect(contains_text(Log::get_ring_buffer(), "嵌套临时模块名可用"),
           "日志宏必须在 c_str() 临时拥有者析构前复制模块名");

    bool argument_exception_escaped = false;
    try {
        LOG_INFO("temporary", "异常参数 {}", []() -> int {
            throw std::runtime_error("格式化参数求值失败");
        }());
    } catch (...) {
        argument_exception_escaped = true;
    }
    expect(!argument_exception_escaped,
           "格式化参数求值异常不得越过日志宏边界");
    Log::shutdown();
}

void test_standard_format_and_inline_overflow() {
    Log::shutdown();
    Log::init(ring_only_config(8));
    expect(Log::initialized(), "标准格式测试初始化失败");
    Log::register_module("format", LogLevel::INFO);

    LOG_INFO("format", "decimal={:03} hex={:08X} float={:.3f} bool={}",
             7, 0x2A, 1.25, true);
    const std::string long_payload(2'048, 'L');
    LOG_INFO("format", "long={}", long_payload);

    const auto lines = Log::get_ring_buffer();
    expect(contains_text(
               lines,
               "decimal=007 hex=0000002A float=1.250 bool=true"),
           "C++20 标准格式串必须保持宽度、进制、精度和布尔语义");
    expect(contains_text(lines, "long=" + long_payload),
           "超过 512 字节的格式化结果必须完整切换到动态缓冲");
    Log::shutdown();
}

void test_sink_threshold_filters_before_formatting() {
    Log::shutdown();
    TempDirectory directory;
    LogConfig config;
    config.enable_console = false;
    config.enable_file = true;
    config.enable_debug_file = false;
    config.enable_ringbuf = false;
    config.log_dir = directory.path().string();
    config.file_max_size_mb = 1;
    config.file_max_count = 1;

    Log::init(config);
    expect(Log::initialized(), "sink 等级过滤测试初始化失败");
    Log::register_module("file-only", LogLevel::TRACE);
    int argument_evaluations = 0;
    LOG_INFO("file-only", "不应求值 {}", ++argument_evaluations);
    expect(argument_evaluations == 0,
           "低于所有 sink 等级的日志不得求值格式化参数");
    LOG_WARN("file-only", "应求值 {}", ++argument_evaluations);
    expect(argument_evaluations == 1,
           "达到 sink 等级的日志必须正常求值格式化参数");
    expect(wait_for_file_text(directory.path() / "xen.log", "应求值 1"),
           "达到 sink 等级的 WARN 必须落盘");
    Log::shutdown();
}

void test_concurrent_same_name_registration() {
    Log::shutdown();
    Log::init(ring_only_config());
    expect(Log::initialized(), "并发注册测试初始化失败");

    constexpr int kThreadCount = 16;
    std::barrier start_line(kThreadCount);
    std::vector<std::thread> workers;
    workers.reserve(kThreadCount);
    for (int index = 0; index < kThreadCount; ++index) {
        workers.emplace_back([&start_line] {
            start_line.arrive_and_wait();
            Log::register_module("concurrent", LogLevel::INFO);
        });
    }
    for (auto& worker : workers) worker.join();

    LOG_INFO("concurrent", "并发注册完成");
    const auto lines = Log::get_ring_buffer();
    expect(lines.size() == 1 && contains_text(lines, "并发注册完成"),
           "同名并发注册必须只生成一个可用模块 logger");
    Log::shutdown();
}

void test_write_shutdown_race() {
    using namespace std::chrono_literals;
    constexpr int kRounds = 20;
    constexpr int kWriterCount = 4;
    TempDirectory directory;

    for (int round = 0; round < kRounds; ++round) {
        auto config = ring_only_config(256);
        if ((round % 2) != 0) {
            config.enable_ringbuf = false;
            config.enable_debug_file = true;
            config.log_dir = directory.path().string();
        }
        Log::init(config);
        expect(Log::initialized(), "竞态测试初始化失败");
        Log::register_module("race", LogLevel::INFO);

        std::atomic<bool> running{true};
        std::vector<std::thread> writers;
        writers.reserve(kWriterCount);
        for (int index = 0; index < kWriterCount; ++index) {
            writers.emplace_back([&running, index] {
                int sequence = 0;
                while (running.load(std::memory_order_relaxed)) {
                    LOG_INFO("race", "writer={} sequence={}", index, sequence++);
                }
            });
        }

        std::this_thread::sleep_for(2ms);
        Log::shutdown();
        running.store(false, std::memory_order_relaxed);
        for (auto& writer : writers) writer.join();
        expect(!Log::initialized(), "并发 shutdown 后状态必须为未初始化");
    }
}

void test_concurrent_init_shutdown_serialization() {
    constexpr int kRounds = 200;
    constexpr int kApiThreadCount = 2;
    Log::shutdown();

    LogConfig empty_config;
    empty_config.enable_console = false;
    empty_config.enable_file = false;
    empty_config.enable_debug_file = false;
    empty_config.enable_ringbuf = false;

    std::barrier start_line(3);
    std::barrier finish_line(3);
    std::thread initializer([&] {
        for (int round = 0; round < kRounds; ++round) {
            start_line.arrive_and_wait();
            Log::init(empty_config);
            finish_line.arrive_and_wait();
        }
    });
    std::thread stopper([&] {
        for (int round = 0; round < kRounds; ++round) {
            start_line.arrive_and_wait();
            Log::shutdown();
            finish_line.arrive_and_wait();
        }
    });

    std::atomic<bool> api_running{true};
    std::vector<std::thread> api_workers;
    api_workers.reserve(kApiThreadCount);
    for (int index = 0; index < kApiThreadCount; ++index) {
        api_workers.emplace_back([&api_running, index] {
            while (api_running.load(std::memory_order_relaxed)) {
                Log::register_module("lifecycle", LogLevel::TRACE);
                Log::set_level("lifecycle", LogLevel::INFO);
                Log::write("lifecycle", LogLevel::INFO,
                           std::to_string(index));
                (void)Log::get_ring_buffer(1);
            }
        });
    }

    for (int round = 0; round < kRounds; ++round) {
        // 每轮从无实例开始，使 init/shutdown 的先后次序均可能出现。
        Log::shutdown();
        start_line.arrive_and_wait();
        finish_line.arrive_and_wait();
        Log::shutdown();
    }
    initializer.join();
    stopper.join();
    api_running.store(false, std::memory_order_relaxed);
    for (auto& worker : api_workers) worker.join();

    Log::init(ring_only_config());
    expect(Log::initialized(), "并发生命周期压力后必须允许重新初始化");
    Log::register_module("lifecycle", LogLevel::INFO);
    LOG_INFO("lifecycle", "并发生命周期恢复成功");
    expect(contains_text(Log::get_ring_buffer(), "并发生命周期恢复成功"),
           "并发生命周期压力后 ring 必须恢复正常写入");
    Log::shutdown();
}

void test_shutdown_waits_for_inflight_ring_read() {
    using namespace std::chrono_literals;
    constexpr int kRingCapacity = 8'192;

    Log::shutdown();
    Log::init(ring_only_config(kRingCapacity));
    expect(Log::initialized(), "关闭屏障测试初始化失败");
    Log::register_module("barrier", LogLevel::INFO);
    const std::string payload(4'096, 'b');
    for (int index = 0; index < kRingCapacity; ++index) {
        Log::write("barrier", LogLevel::INFO, payload);
    }

    std::barrier start_line(2);
    std::atomic<bool> read_completed{false};
    std::atomic<std::size_t> line_count{0};
    std::thread reader([&] {
        start_line.arrive_and_wait();
        const auto lines = Log::get_ring_buffer();
        line_count.store(lines.size(), std::memory_order_release);
        read_completed.store(true, std::memory_order_release);
    });

    start_line.arrive_and_wait();
    std::this_thread::sleep_for(1ms);
    const bool observed_inflight =
        !read_completed.load(std::memory_order_acquire);
    Log::shutdown();
    const bool completed_when_shutdown_returned =
        read_completed.load(std::memory_order_acquire);
    reader.join();

    expect(observed_inflight,
           "关闭屏障测试必须实际覆盖进行中的 ring 读取");
    expect(completed_when_shutdown_returned,
           "shutdown 返回前必须等待已进入的日志 API 完成");
    expect(line_count.load(std::memory_order_acquire) == kRingCapacity,
           "关闭屏障不得截断已开始的 ring 快照");
}

void test_global_level_applies_to_later_modules() {
    Log::shutdown();
    Log::init(ring_only_config());
    expect(Log::initialized(), "全局等级测试初始化失败");
    Log::set_global_level(LogLevel::WARN);
    Log::register_module("late", LogLevel::TRACE);

    expect(!Log::should_log("late", LogLevel::INFO),
           "后注册模块必须继承当前全局等级下限");
    expect(Log::should_log("late", LogLevel::WARN),
           "达到全局等级的后注册模块日志必须保留");
    Log::write("late", LogLevel::INFO, "应被全局等级过滤");
    Log::write("late", LogLevel::WARN, "全局等级保留");
    auto lines = Log::get_ring_buffer();
    expect(!contains_text(lines, "应被全局等级过滤") &&
           contains_text(lines, "全局等级保留"),
           "全局等级过滤结果错误");

    Log::set_global_level(LogLevel::TRACE);
    Log::write("late", LogLevel::DEBUG, "放宽全局等级后可见");
    lines = Log::get_ring_buffer();
    expect(contains_text(lines, "放宽全局等级后可见"),
           "放宽全局等级后模块原始等级必须恢复生效");
    Log::shutdown();
}

void test_configured_global_level_is_applied() {
    Log::shutdown();
    auto config = ring_only_config();
    config.global_level = LogLevel::WARN;
    Log::init(config);
    expect(Log::initialized(), "配置全局等级测试初始化失败");
    Log::register_module("configured-level", LogLevel::TRACE);
    expect(!Log::should_log("configured-level", LogLevel::INFO) &&
               Log::should_log("configured-level", LogLevel::WARN),
           "Log 初始化必须采用 LogConfig 中的全局等级");
    Log::shutdown();
}

void test_configured_module_levels_are_applied() {
    Log::shutdown();
    auto config = ring_only_config();
    config.module_levels.emplace("configured-module", LogLevel::WARN);
    Log::init(config);
    expect(Log::initialized(), "模块等级配置测试初始化失败");

    Log::register_module("configured-module", LogLevel::TRACE);
    Log::register_module("unconfigured-module", LogLevel::DEBUG);
    expect(!Log::should_log("configured-module", LogLevel::INFO) &&
               Log::should_log("configured-module", LogLevel::WARN),
           "模块配置必须覆盖注册时的默认等级");
    expect(Log::should_log("unconfigured-module", LogLevel::DEBUG),
           "未配置模块必须保留注册时的默认等级");

    Log::set_level("configured-module", LogLevel::DEBUG);
    expect(Log::should_log("configured-module", LogLevel::DEBUG),
           "运行时 set_level 必须能够临时覆盖模块配置");
    Log::register_module("configured-module", LogLevel::TRACE);
    expect(!Log::should_log("configured-module", LogLevel::INFO),
           "模块重新注册时必须重新应用静态配置等级");
    Log::shutdown();
}

void test_spdlog_global_registry_is_untouched() {
    const std::string external_name = "xen_log_tests_external";
    spdlog::drop(external_name);
    auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    auto external = std::make_shared<spdlog::logger>(external_name, sink);
    spdlog::register_logger(external);

    auto config = ring_only_config();
    config.enable_console = true;
    Log::init(config);
    expect(Log::initialized(), "spdlog 隔离测试初始化失败");
    Log::register_module("isolated", LogLevel::INFO);
    Log::shutdown();
    expect(spdlog::get(external_name) == external,
           "Log shutdown 不得清理外部 spdlog registry");
    spdlog::drop(external_name);
}

void test_file_flush_and_restart_preservation() {
    Log::shutdown();
    TempDirectory directory;
    const auto log_path = directory.path() / "xen.log";
    const auto rotated_path = directory.path() / "xen.1.log";
    const auto debug_path = directory.path() / "debug.log";

    LogConfig config;
    config.enable_console = false;
    config.enable_file = true;
    config.enable_debug_file = true;
    config.enable_ringbuf = false;
    config.log_dir = directory.path().string();
    config.file_max_size_mb = 1;
    config.file_max_count = 3;

    Log::init(config);
    expect(Log::initialized(), "首次文件日志初始化失败");
    Log::register_module("file", LogLevel::TRACE);
    const std::string burst_payload(256, 'x');
    std::vector<std::thread> burst_writers;
    for (int writer = 0; writer < 4; ++writer) {
        (void)writer;
        burst_writers.emplace_back([&burst_payload] {
            for (int index = 0; index < 10'000; ++index) {
                Log::write("file", LogLevel::INFO, burst_payload);
            }
        });
    }
    for (auto& writer : burst_writers) writer.join();
    LOG_INFO("file", "normal-final-marker");
    LOG_WARN("file", "first-run-marker");
    expect(wait_for_file_text(log_path, "first-run-marker"),
           "WARN 必须由后台线程及时 flush 到轮转文件");
    expect(wait_for_file_text(debug_path, "first-run-marker"),
           "WARN 必须同时 flush 到 debug 文件");
    Log::shutdown();

    Log::init(config);
    expect(Log::initialized(), "重启文件日志初始化失败");
    Log::register_module("file", LogLevel::TRACE);
    LOG_WARN("file", "second-run-marker");
    expect(wait_for_file_text(log_path, "second-run-marker"),
           "重启后的 WARN 未落盘");
    expect(wait_for_file_text(debug_path, "second-run-marker"),
           "重启后的 debug 文件未继续写入");
    Log::shutdown();

    const auto normal_log = read_file(log_path);
    const auto debug_log = read_file(debug_path);
    expect(normal_log.find("first-run-marker") != std::string::npos &&
           normal_log.find("second-run-marker") != std::string::npos,
           "轮转文件重启时不得轮转未满的旧日志");
    expect(debug_log.find("first-run-marker") != std::string::npos &&
           debug_log.find("second-run-marker") != std::string::npos,
           "debug 文件重启时不得截断旧日志");
    expect(debug_log.find("normal-final-marker") != std::string::npos,
           "shutdown 返回前必须排空普通日志队列中的尾部消息");
    expect(!std::filesystem::exists(rotated_path),
           "未达到大小阈值时重启不得生成 xen.1.log");
}

void test_priority_queue_preserves_warn_burst() {
    constexpr int kWriterCount = 4;
    constexpr int kMessagesPerWriter = 512;
    constexpr std::size_t kExpectedMessages =
        kWriterCount * kMessagesPerWriter;

    Log::shutdown();
    TempDirectory directory;
    LogConfig config;
    config.enable_console = false;
    config.enable_file = false;
    config.enable_debug_file = true;
    config.enable_ringbuf = false;
    config.log_dir = directory.path().string();

    Log::init(config);
    expect(Log::initialized(), "WARN 洪峰测试初始化失败");
    Log::register_module("priority", LogLevel::WARN);
    std::vector<std::thread> writers;
    writers.reserve(kWriterCount);
    for (int writer = 0; writer < kWriterCount; ++writer) {
        writers.emplace_back([writer] {
            for (int index = 0; index < kMessagesPerWriter; ++index) {
                LOG_WARN("priority", "priority-marker writer={} index={}",
                         writer, index);
            }
        });
    }
    for (auto& writer : writers) writer.join();
    Log::shutdown();

    const auto debug_log = read_file(directory.path() / "debug.log");
    expect(count_text(debug_log, "priority-marker") == kExpectedMessages,
           "阻塞优先队列不得覆盖或丢失 WARN 洪峰记录");
}

} // namespace

int main() {
    try {
        test_invalid_config_can_retry();
        test_unregistered_is_dropped_and_ring_is_immediate();
        test_macro_extends_temporary_module_lifetime();
        test_standard_format_and_inline_overflow();
        test_sink_threshold_filters_before_formatting();
        test_concurrent_same_name_registration();
        test_write_shutdown_race();
        test_concurrent_init_shutdown_serialization();
        test_shutdown_waits_for_inflight_ring_read();
        test_global_level_applies_to_later_modules();
        test_configured_global_level_is_applied();
        test_configured_module_levels_are_applied();
        test_spdlog_global_registry_is_untouched();
        test_file_flush_and_restart_preservation();
        test_priority_queue_preserves_warn_burst();
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] 测试出现未捕获异常: " << exception.what() << '\n';
        ++failures;
    } catch (...) {
        std::cerr << "[FAIL] 测试出现未知未捕获异常\n";
        ++failures;
    }

    Log::shutdown();
    if (failures != 0) {
        std::cerr << failures << " 个 Log 测试失败\n";
        return 1;
    }
    std::cout << "Log 测试全部通过\n";
    return 0;
}
