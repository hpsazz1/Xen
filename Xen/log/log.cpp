#include "log/log.h"

#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/ringbuffer_sink.h>

#include <cstdio>
#include <unordered_map>
#include <mutex>
#include <filesystem>

// ============================================================
// 工具函数
// ============================================================

static spdlog::level::level_enum to_spdlog_level(LogLevel lv) {
    switch (lv) {
        case LogLevel::TRACE: return spdlog::level::trace;
        case LogLevel::DEBUG: return spdlog::level::debug;
        case LogLevel::INFO:  return spdlog::level::info;
        case LogLevel::WARN:  return spdlog::level::warn;
        case LogLevel::ERROR: return spdlog::level::err;
        case LogLevel::OFF:   return spdlog::level::off;
    }
    return spdlog::level::info;
}

// ============================================================
// Impl
// ============================================================
struct Log::Impl {
    LogConfig config;

    std::shared_ptr<spdlog::details::thread_pool> thread_pool;
    std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> ringbuf_sink;
    std::vector<spdlog::sink_ptr> common_sinks; // console + file sinks shared by all loggers

    std::unordered_map<std::string, std::shared_ptr<spdlog::logger>> loggers;
    std::mutex mtx;

    bool inited = false;

    void init_sinks(const LogConfig& cfg) {
        common_sinks.clear();

        if (cfg.enable_console) {
            auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            console_sink->set_pattern("[%H:%M:%S.%e] [%^%l%$] [%n] %v");
            console_sink->set_level(spdlog::level::info); // console only shows INFO+
            common_sinks.push_back(std::move(console_sink));
        }

        if (cfg.enable_file) {
            auto log_path = std::filesystem::path(cfg.log_dir) / "detector.log";
            std::filesystem::create_directories(cfg.log_dir);
            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                log_path.string(),
                cfg.file_max_size_mb * 1024 * 1024,
                cfg.file_max_count,
                true // flush immediately for warn/err
            );
            file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] %v");
            file_sink->set_level(spdlog::level::warn); // file only records WARN+
            common_sinks.push_back(std::move(file_sink));
        }

        if (cfg.enable_debug_file) {
            auto debug_path = std::filesystem::path(cfg.log_dir) / "debug.log";
            std::filesystem::create_directories(cfg.log_dir);
            auto debug_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                debug_path.string(), true);
            debug_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] %v");
            debug_sink->set_level(spdlog::level::trace); // always full level
            common_sinks.push_back(std::move(debug_sink));
        }

        if (cfg.enable_ringbuf) {
            ringbuf_sink = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(
                (size_t)cfg.ringbuf_capacity);
            ringbuf_sink->set_level(spdlog::level::trace);
        }
    }

    std::shared_ptr<spdlog::logger> create_logger(const std::string& name) {
        // build sink list: common sinks + ringbuffer (if enabled)
        std::vector<spdlog::sink_ptr> sinks = common_sinks;
        if (ringbuf_sink)
            sinks.push_back(ringbuf_sink);

        // create async logger with thread pool
        auto logger = std::make_shared<spdlog::async_logger>(
            name,
            sinks.begin(), sinks.end(),
            thread_pool,
            spdlog::async_overflow_policy::block  // block on full queue for warn/err safety
        );
        logger->set_level(spdlog::level::trace); // logger accepts all levels; sinks filter individually
        spdlog::register_logger(logger);
        return logger;
    }

    std::shared_ptr<spdlog::logger> get_or_create(const std::string& name) {
        {
            std::lock_guard<std::mutex> lk(mtx);
            auto it = loggers.find(name);
            if (it != loggers.end()) return it->second;
        }

        auto logger = create_logger(name);
        {
            std::lock_guard<std::mutex> lk(mtx);
            loggers[name] = logger;
        }
        return logger;
    }
};

// ============================================================
// 全局单例
// ============================================================
std::unique_ptr<Log::Impl> Log::impl_;

// ============================================================
// 生命周期
// ============================================================

void Log::init(const LogConfig& cfg) {
    if (impl_) {
        std::fprintf(stderr, "[Log] Already initialized — call shutdown() first\n");
        return;
    }

    impl_ = std::make_unique<Impl>();
    impl_->config = cfg;

    // 1. 创建全局线程池
    //    queue_size: 4096 条 ≈ 够约 1 秒的爆发日志量（按每帧 5 条 × 200fps）
    //    thread_count: 1，I/O 瓶颈在磁盘不在线程数
    spdlog::init_thread_pool(4096, 1);
    // async_logger 构造函数需要持有同一个线程池。只初始化 spdlog 全局池但
    // 不保存指针，会把空 shared_ptr 传入 logger，首次写日志即失败。
    impl_->thread_pool = spdlog::thread_pool();

    // 2. 创建各 sink
    impl_->init_sinks(cfg);

    impl_->inited = true;
}

void Log::shutdown() {
    if (!impl_) return;

    // flush all loggers before tearing down
    for (auto& [name, logger] : impl_->loggers)
        logger->flush();
    spdlog::shutdown();
    impl_.reset();
}

bool Log::initialized() noexcept {
    return impl_ && impl_->inited;
}

// ============================================================
// 模块注册
// ============================================================

std::string Log::register_module(const std::string& module_name, LogLevel level) {
    if (!impl_) return module_name;

    auto logger = impl_->get_or_create(module_name);
    logger->set_level(to_spdlog_level(level));
    return module_name;
}

// ============================================================
// 日志写入
// ============================================================

void Log::write(const std::string& module, LogLevel level, const std::string& msg) {
    if (!impl_) return;

    auto logger = impl_->get_or_create(module);
    logger->log(to_spdlog_level(level), msg);
}

// ============================================================
// 工具函数
// ============================================================

LogLevel Log::level_from_name(const std::string& name) {
    if (name == "TRACE")      return LogLevel::TRACE;
    else if (name == "DEBUG") return LogLevel::DEBUG;
    else if (name == "INFO")  return LogLevel::INFO;
    else if (name == "WARN")  return LogLevel::WARN;
    else if (name == "ERROR") return LogLevel::ERROR;
    else if (name == "OFF")   return LogLevel::OFF;
    return LogLevel::INFO; // 默认
}

// ============================================================
// 运行时控制
// ============================================================

void Log::set_level(const std::string& module, LogLevel level) {
    if (!impl_) return;

    auto logger = impl_->get_or_create(module);
    logger->set_level(to_spdlog_level(level));
}

void Log::set_global_level(LogLevel level) {
    if (!impl_) return;

    spdlog::set_level(to_spdlog_level(level));
}

// ============================================================
// 环形缓冲区
// ============================================================

std::vector<std::string> Log::get_ring_buffer(int last_n) {
    if (!impl_ || !impl_->ringbuf_sink)
        return {};

    auto raw = impl_->ringbuf_sink->last_formatted(last_n == 0
        ? (size_t)impl_->config.ringbuf_capacity
        : (size_t)last_n);

    std::vector<std::string> result;
    result.reserve(raw.size());
    for (auto& line : raw)
        result.push_back(std::string(line.begin(), line.end()));
    return result;
}

void Log::dump_ring_buffer(const std::string& path) {
    auto lines = get_ring_buffer(0);
    if (lines.empty()) return;

    auto dir = std::filesystem::path(path).parent_path();
    if (!dir.empty())
        std::filesystem::create_directories(dir);

    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        std::fprintf(stderr, "[Log] Failed to dump ring buffer to: %s\n", path.c_str());
        return;
    }

    for (auto& line : lines)
        std::fprintf(f, "%s\n", line.c_str());
    std::fclose(f);

    std::fprintf(stderr, "[Log] Ring buffer dumped to: %s (%zu lines)\n",
                 path.c_str(), lines.size());
}
