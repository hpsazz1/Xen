#ifndef LOG_H
#define LOG_H

#include <array>
#include <atomic>
#include <cstddef>
#include <format>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// ── 日志级别（与 spdlog 对应，但解耦） ──
enum class LogLevel {
    TRACE = 0,
    DEBUG = 1,
    INFO  = 2,
    WARN  = 3,
    ERROR = 4,
    OFF   = 5,
};

// ── 配置 ──
struct LogConfig {
    LogLevel global_level  = LogLevel::TRACE; // 全局最低日志级别
    bool enable_console    = true;   // 控制台输出
    bool enable_file       = true;   // 轮转文件输出（warn/err）
    bool enable_debug_file = false;  // 调试文件（全级别）
    bool enable_ringbuf    = true;   // 环形缓冲区（内存）
    int  ringbuf_capacity  = 1024;   // 环形缓冲区容量

    std::string log_dir          = "logs";
    int         file_max_size_mb = 10;
    int         file_max_count   = 3;
    // 键为模块注册名；仅覆盖模块注册时传入的默认等级。
    std::unordered_map<std::string, LogLevel> module_levels;
};

// ── 日志核心类（全局单例） ──
class Log {
private:
    class InlineFormatBuffer {
    public:
        using value_type = char;

        void push_back(char value) {
            if (overflow_.empty() && size_ < inline_.size()) {
                inline_[size_++] = value;
                return;
            }
            if (overflow_.empty()) {
                overflow_.reserve(inline_.size() * 2U);
                overflow_.assign(inline_.data(), size_);
            }
            overflow_.push_back(value);
            ++size_;
        }

        const char* data() const noexcept {
            return overflow_.empty() ? inline_.data() : overflow_.data();
        }

        std::size_t size() const noexcept { return size_; }

    private:
        // 512 字节覆盖项目常见短日志；超长结果才切换到动态 string。
        std::array<char, 512> inline_{};
        std::string overflow_;
        std::size_t size_ = 0;
    };

public:
    using EmergencyWriteCallback =
        bool (*)(void* context, const char* data,
                 std::size_t size) noexcept;

    // ── 生命周期 ──
    // 初始化失败时保持未初始化状态，错误仅写入 stderr，不向业务层抛异常。
    static void init(const LogConfig& cfg);
    static void shutdown();
    static bool initialized() noexcept;

    // ── 模块注册 ──
    // 每个模块在启动阶段注册一次；同名重复注册只更新模块等级。
    // 保留原接口：失败时仍返回输入名称，详细错误写入 stderr。
    static std::string register_module(
        const std::string& module_name,
        LogLevel level = LogLevel::INFO);

    // ── 日志写入（模块应使用宏，而非直接调用这些） ──
    static void write(const std::string& module,
                      LogLevel level, const std::string& msg);
    // 宏在求值格式化参数前调用本接口，避免被等级过滤的热路径日志仍执行格式化。
    static bool should_log(const std::string& module,
                           LogLevel level) noexcept;
    // 使用 C++20 标准格式串保持编译期校验，不向业务头文件暴露 spdlog/fmt。
    // 格式化异常不得越过日志模块边界。
    template<typename... Args>
    static void writef(const std::string& module,
                       LogLevel level,
                       std::format_string<Args...> fmt_str,
                       Args&&... args) {
        try {
            InlineFormatBuffer formatted;
            std::format_to(
                std::back_inserter(formatted), fmt_str,
                std::forward<Args>(args)...);
            write_view(
                module, level,
                std::string_view(formatted.data(), formatted.size()));
        } catch (...) {
            // 日志不得让格式化或内存分配异常越过模块边界。
        }
    }

    // ── 运行时控制 ──
    static void set_level(const std::string& module, LogLevel level);
    static void set_global_level(LogLevel level);

    // ── 环形缓冲区（崩溃 dump） ──
    static std::vector<std::string> get_ring_buffer(
        int last_n = 0);                    // 0=全部
    static void dump_ring_buffer(
        const std::string& path = "logs/crash_dump.txt");
    // 真正崩溃路径只读取固定原子槽，不访问 Impl、spdlog、堆或生命周期锁。
    // 回调必须完成同步写入，返回 false 会立即停止后续输出。
    static bool dump_emergency_tail(
        void* context, EmergencyWriteCallback writer) noexcept;

    // ── 工具 ──
    static constexpr const char* level_name(LogLevel lv) {
        switch (lv) {
            case LogLevel::TRACE: return "TRACE";
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO:  return "INFO";
            case LogLevel::WARN:  return "WARN";
            case LogLevel::ERROR: return "ERROR";
            case LogLevel::OFF:   return "OFF";
        }
        return "???";
    }
    static LogLevel level_from_name(const std::string& name);

private:
    Log() = delete;  // 静态类

    static void write_view(const std::string& module,
                           LogLevel level,
                           std::string_view msg) noexcept;

    struct Impl;
    // impl_owner_ 仅在生命周期锁下修改；调用路径先取得内部访问租约，再读取
    // impl_ 的观察指针。shutdown() 会等所有租约退出后才销毁实例。
    static std::unique_ptr<Impl> impl_owner_;
    static std::atomic<Impl*> impl_;
};

// ── 宏 ──
// 始终可用，release 不编译参数（零开销）
//
// 使用示例：
//   LOG_INFO("detector", "Model loaded: {}", path);
//   LOG_WARN("detector", "Empty result");

#if defined(LOG_ENABLE_DEBUG) || !defined(NDEBUG)
    #define LOG_TRACE(module, ...) \
        do { \
            try { \
                const std::string xen_log_module_{std::string_view((module))}; \
                if (Log::should_log(xen_log_module_, LogLevel::TRACE)) \
                    Log::writef(xen_log_module_, LogLevel::TRACE, __VA_ARGS__); \
            } catch (...) {} \
        } while (false)
    #define LOG_DEBUG(module, ...) \
        do { \
            try { \
                const std::string xen_log_module_{std::string_view((module))}; \
                if (Log::should_log(xen_log_module_, LogLevel::DEBUG)) \
                    Log::writef(xen_log_module_, LogLevel::DEBUG, __VA_ARGS__); \
            } catch (...) {} \
        } while (false)
#else
    #define LOG_TRACE(module, ...) ((void)0)
    #define LOG_DEBUG(module, ...) ((void)0)
#endif

#define LOG_INFO(module, ...) \
    do { \
        try { \
            const std::string xen_log_module_{std::string_view((module))}; \
            if (Log::should_log(xen_log_module_, LogLevel::INFO)) \
                Log::writef(xen_log_module_, LogLevel::INFO, __VA_ARGS__); \
        } catch (...) {} \
    } while (false)
#define LOG_WARN(module, ...) \
    do { \
        try { \
            const std::string xen_log_module_{std::string_view((module))}; \
            if (Log::should_log(xen_log_module_, LogLevel::WARN)) \
                Log::writef(xen_log_module_, LogLevel::WARN, __VA_ARGS__); \
        } catch (...) {} \
    } while (false)
#define LOG_ERROR(module, ...) \
    do { \
        try { \
            const std::string xen_log_module_{std::string_view((module))}; \
            if (Log::should_log(xen_log_module_, LogLevel::ERROR)) \
                Log::writef(xen_log_module_, LogLevel::ERROR, __VA_ARGS__); \
        } catch (...) {} \
    } while (false)

#endif // LOG_H
