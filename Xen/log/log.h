#ifndef LOG_H
#define LOG_H

#include <string>
#include <memory>
#include <vector>
#include <utility>
#include <spdlog/spdlog.h>

// ── 配置 ──
struct LogConfig {
    bool enable_console    = true;   // 控制台输出
    bool enable_file       = true;   // 轮转文件输出（warn/err）
    bool enable_debug_file = false;  // 调试文件（全级别）
    bool enable_ringbuf    = true;   // 环形缓冲区（内存）
    int  ringbuf_capacity  = 1024;   // 环形缓冲区容量

    std::string log_dir          = "logs";
    int         file_max_size_mb = 10;
    int         file_max_count   = 3;
};

// ── 日志级别（与 spdlog 对应，但解耦） ──
enum class LogLevel {
    TRACE = 0,
    DEBUG = 1,
    INFO  = 2,
    WARN  = 3,
    ERROR = 4,
    OFF   = 5,
};

// ── 日志核心类（全局单例） ──
class Log {
public:
    // ── 生命周期 ──
    static void init(const LogConfig& cfg);
    static void shutdown();
    static bool initialized() noexcept;

    // ── 模块注册 ──
    // 每个模块在 init 时注册自己的 logger
    // 返回 logger 名称，后续通过 LOG_INFO(logger_name, ...) 使用
    static std::string register_module(
        const std::string& module_name,
        LogLevel level = LogLevel::INFO);

    // ── 日志写入（模块应使用宏，而非直接调用这些） ──
    static void write(const std::string& module,
                      LogLevel level, const std::string& msg);
    // fmt 格式化版本 — 内联模板，用 spdlog::fmt_lib::format 格式化后走 write()
    template<typename... Args>
    static void writef(const std::string& module,
                       LogLevel level,
                       spdlog::format_string_t<Args...> fmt_str,
                       Args&&... args) {
        if (!impl_) return;
        auto formatted = spdlog::fmt_lib::format(
            fmt_str, std::forward<Args>(args)...);
        write(module, level, formatted);
    }

    // ── 运行时控制 ──
    static void set_level(const std::string& module, LogLevel level);
    static void set_global_level(LogLevel level);

    // ── 环形缓冲区（崩溃 dump） ──
    static std::vector<std::string> get_ring_buffer(
        int last_n = 0);                    // 0=全部
    static void dump_ring_buffer(
        const std::string& path = "logs/crash_dump.txt");

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

    struct Impl;
    static std::unique_ptr<Impl> impl_;
};

// ── 宏 ──
// 始终可用，release 不编译参数（零开销）
//
// 使用示例：
//   LOG_INFO("detector", "Model loaded: {}", path);
//   LOG_WARN("detector", "Empty result");

#if defined(LOG_ENABLE_DEBUG) || !defined(NDEBUG)
    #define LOG_TRACE(module, ...) \
        Log::writef(module, LogLevel::TRACE, __VA_ARGS__)
    #define LOG_DEBUG(module, ...) \
        Log::writef(module, LogLevel::DEBUG, __VA_ARGS__)
#else
    #define LOG_TRACE(module, ...) ((void)0)
    #define LOG_DEBUG(module, ...) ((void)0)
#endif

#define LOG_INFO(module, ...) \
    Log::writef(module, LogLevel::INFO, __VA_ARGS__)
#define LOG_WARN(module, ...) \
    Log::writef(module, LogLevel::WARN, __VA_ARGS__)
#define LOG_ERROR(module, ...) \
    Log::writef(module, LogLevel::ERROR, __VA_ARGS__)

#endif // LOG_H
