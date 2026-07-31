#include "log/log.h"

#include <spdlog/async_logger.h>
#include <spdlog/details/log_msg.h>
#include <spdlog/details/thread_pool.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kAsyncQueueCapacity = 4096;
constexpr std::size_t kPriorityQueueCapacity = 256;
constexpr std::size_t kAsyncWorkerCount = 1;
constexpr std::size_t kBytesPerMiB = 1024U * 1024U;
// spdlog 会预构造 capacity + 1 个 log_msg_buffer，每项自带 250 字节内联缓冲；
// 百万级容量仅内联区就超过 238 MiB，因此把显式配置的基线预分配限制在数十 MiB。
constexpr int kMaxRingBufferCapacity = 65'536;
constexpr std::uint64_t kAccessPausedBit = std::uint64_t{1} << 63U;
constexpr std::uint64_t kAccessCountMask = ~kAccessPausedBit;

std::mutex& lifecycle_mutex() noexcept {
    static std::mutex mutex;
    return mutex;
}

std::atomic<std::uint64_t>& access_state() noexcept {
    static std::atomic<std::uint64_t> state{0};
    return state;
}

class AccessLease {
public:
    AccessLease() noexcept {
        auto current = access_state().load(std::memory_order_relaxed);
        while ((current & kAccessPausedBit) == 0U &&
               (current & kAccessCountMask) != kAccessCountMask) {
            if (access_state().compare_exchange_weak(
                    current, current + 1U,
                    std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                acquired_ = true;
                return;
            }
        }
    }

    ~AccessLease() {
        if (!acquired_) return;
        const auto previous =
            access_state().fetch_sub(1U, std::memory_order_release);
        if ((previous & kAccessCountMask) == 1U) {
            access_state().notify_all();
        }
    }

    AccessLease(const AccessLease&) = delete;
    AccessLease& operator=(const AccessLease&) = delete;

    explicit operator bool() const noexcept { return acquired_; }

private:
    bool acquired_ = false;
};

class AccessPause {
public:
    AccessPause() noexcept {
        auto current = access_state().fetch_or(
            kAccessPausedBit, std::memory_order_acq_rel);
        current |= kAccessPausedBit;
        while ((current & kAccessCountMask) != 0U) {
            access_state().wait(current, std::memory_order_acquire);
            current = access_state().load(std::memory_order_acquire);
        }
    }

    ~AccessPause() {
        access_state().store(0U, std::memory_order_release);
        access_state().notify_all();
    }

    AccessPause(const AccessPause&) = delete;
    AccessPause& operator=(const AccessPause&) = delete;
};

void report_error(const char* message) noexcept {
    std::fprintf(stderr, "[Log] %s\n", message);
}

void report_error(const char* prefix, const char* detail) noexcept {
    std::fprintf(stderr, "[Log] %s: %s\n", prefix, detail ? detail : "未知错误");
}

bool valid_level(LogLevel level) noexcept {
    return level >= LogLevel::TRACE && level <= LogLevel::OFF;
}

spdlog::level::level_enum to_spdlog_level(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::TRACE: return spdlog::level::trace;
        case LogLevel::DEBUG: return spdlog::level::debug;
        case LogLevel::INFO:  return spdlog::level::info;
        case LogLevel::WARN:  return spdlog::level::warn;
        case LogLevel::ERROR: return spdlog::level::err;
        case LogLevel::OFF:   return spdlog::level::off;
    }
    return spdlog::level::off;
}

bool valid_config(const LogConfig& config) noexcept {
    if (!valid_level(config.global_level)) return false;

    if (config.enable_ringbuf &&
        (config.ringbuf_capacity <= 0 ||
         config.ringbuf_capacity > kMaxRingBufferCapacity)) {
        return false;
    }

    if (config.enable_file) {
        if (config.log_dir.empty() || config.file_max_size_mb <= 0 ||
            config.file_max_count <= 0 ||
            static_cast<std::size_t>(config.file_max_count) >
                spdlog::sinks::rotating_file_sink_mt::MaxFiles) {
            return false;
        }

        const auto max_mib =
            std::numeric_limits<std::size_t>::max() / kBytesPerMiB;
        if (static_cast<std::size_t>(config.file_max_size_mb) > max_mib) {
            return false;
        }
    }

    return !config.enable_debug_file || !config.log_dir.empty();
}

struct TransparentStringHash {
    using is_transparent = void;

    std::size_t operator()(std::string_view value) const noexcept {
        return std::hash<std::string_view>{}(value);
    }

    std::size_t operator()(const std::string& value) const noexcept {
        return (*this)(std::string_view(value));
    }
};

} // namespace

struct Log::Impl {
    struct ModuleState {
        ModuleState(std::string module_name, LogLevel initial_level,
                    std::shared_ptr<spdlog::async_logger> normal,
                    std::shared_ptr<spdlog::async_logger> priority)
            : name(std::move(module_name)),
              level(initial_level),
              normal_logger(std::move(normal)),
              priority_logger(std::move(priority)) {}

        std::string name;
        std::atomic<LogLevel> level;
        std::shared_ptr<spdlog::async_logger> normal_logger;
        std::shared_ptr<spdlog::async_logger> priority_logger;
    };

    explicit Impl(const LogConfig& requested_config) {
        init_sinks(requested_config);
        global_level.store(requested_config.global_level,
                           std::memory_order_relaxed);
        if (!async_sinks.empty()) {
            // 普通日志覆盖最旧记录，保证热路径永不因 I/O 阻塞；WARN/ERROR
            // 使用独立的小型阻塞队列，避免异常日志被低等级洪峰挤掉。
            normal_thread_pool =
                std::make_shared<spdlog::details::thread_pool>(
                kAsyncQueueCapacity, kAsyncWorkerCount);
            priority_thread_pool =
                std::make_shared<spdlog::details::thread_pool>(
                    kPriorityQueueCapacity, kAsyncWorkerCount);
        }
        accepting.store(true, std::memory_order_release);
    }

    ~Impl() noexcept {
        accepting.store(false, std::memory_order_release);
        // 优先队列先落盘，再清空普通队列；线程池析构均会在队尾放入 terminate
        // 并等待工作线程，确保 logger 与 sink 析构前处理完已入队消息。
        priority_thread_pool.reset();
        normal_thread_pool.reset();
    }

    void init_sinks(const LogConfig& requested_config) {
        if (requested_config.enable_console) {
            auto console_sink =
                std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            console_sink->set_pattern("[%H:%M:%S.%e] [%^%l%$] [%n] %v");
            console_sink->set_level(spdlog::level::info);
            async_sinks.push_back(std::move(console_sink));
            minimum_sink_level = LogLevel::INFO;
            minimum_async_sink_level = LogLevel::INFO;
        }

        std::filesystem::path log_directory;
        if (requested_config.enable_file || requested_config.enable_debug_file) {
            log_directory = std::filesystem::u8path(requested_config.log_dir);
            std::error_code error;
            std::filesystem::create_directories(log_directory, error);
            if (error || !std::filesystem::is_directory(log_directory)) {
                throw std::runtime_error("日志目录创建失败");
            }
        }

        if (requested_config.enable_file) {
            const auto log_path = log_directory / "xen.log";
            const auto max_size =
                static_cast<std::size_t>(requested_config.file_max_size_mb) *
                kBytesPerMiB;
            auto file_sink =
                std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                    log_path.string(), max_size,
                    static_cast<std::size_t>(requested_config.file_max_count),
                    false);
            file_sink->set_pattern(
                "[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] %v");
            file_sink->set_level(spdlog::level::warn);
            async_sinks.push_back(std::move(file_sink));
            minimum_sink_level = std::min(minimum_sink_level, LogLevel::WARN);
            minimum_async_sink_level =
                std::min(minimum_async_sink_level, LogLevel::WARN);
        }

        if (requested_config.enable_debug_file) {
            const auto debug_path = log_directory / "debug.log";
            auto debug_sink =
                std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                    debug_path.string(), false);
            debug_sink->set_pattern(
                "[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] %v");
            debug_sink->set_level(spdlog::level::trace);
            async_sinks.push_back(std::move(debug_sink));
            minimum_sink_level = LogLevel::TRACE;
            minimum_async_sink_level = LogLevel::TRACE;
        }

        if (requested_config.enable_ringbuf) {
            ringbuf_sink =
                std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(
                    static_cast<std::size_t>(requested_config.ringbuf_capacity));
            ringbuf_sink->set_level(spdlog::level::trace);
            minimum_sink_level = LogLevel::TRACE;
        }
    }

    bool register_module(std::string_view name, LogLevel level) {
        if (name.empty() || !valid_level(level) ||
            !accepting.load(std::memory_order_acquire)) {
            return false;
        }

        // 查找、创建和插入位于同一个独占临界区，同名并发注册只会创建一次。
        std::unique_lock<std::shared_mutex> lock(modules_mutex);
        const auto existing = modules.find(name);
        if (existing != modules.end()) {
            existing->second->level.store(level, std::memory_order_release);
            return true;
        }

        const std::string owned_name(name);
        std::shared_ptr<spdlog::async_logger> normal_logger;
        std::shared_ptr<spdlog::async_logger> priority_logger;
        if (normal_thread_pool && priority_thread_pool) {
            normal_logger = std::make_shared<spdlog::async_logger>(
                owned_name, async_sinks.begin(), async_sinks.end(),
                normal_thread_pool,
                spdlog::async_overflow_policy::overrun_oldest);
            priority_logger = std::make_shared<spdlog::async_logger>(
                owned_name, async_sinks.begin(), async_sinks.end(),
                priority_thread_pool,
                spdlog::async_overflow_policy::block);
            normal_logger->set_level(spdlog::level::trace);
            priority_logger->set_level(spdlog::level::trace);
            priority_logger->flush_on(spdlog::level::warn);
        }

        modules.emplace(
            owned_name,
            std::make_shared<ModuleState>(
                owned_name, level, std::move(normal_logger),
                std::move(priority_logger)));
        return true;
    }

    std::shared_ptr<ModuleState> find_module(std::string_view name) const {
        std::shared_lock<std::shared_mutex> lock(modules_mutex);
        const auto iterator = modules.find(name);
        return iterator == modules.end() ? nullptr : iterator->second;
    }

    bool should_log(const std::shared_ptr<ModuleState>& module,
                    LogLevel message_level) const noexcept {
        if (!module || !valid_level(message_level) ||
            message_level == LogLevel::OFF ||
            !accepting.load(std::memory_order_acquire) ||
            message_level < minimum_sink_level) {
            return false;
        }

        const auto module_level =
            module->level.load(std::memory_order_acquire);
        const auto current_global_level =
            global_level.load(std::memory_order_acquire);
        const auto effective_level =
            std::max(static_cast<int>(module_level),
                     static_cast<int>(current_global_level));
        return static_cast<int>(message_level) >= effective_level;
    }

    void write(const std::shared_ptr<ModuleState>& module,
               LogLevel level, std::string_view message) {
        if (!should_log(module, level)) return;

        const auto spdlog_level = to_spdlog_level(level);
        const auto timestamp = spdlog::log_clock::now();
        if (ringbuf_sink && ringbuf_sink->should_log(spdlog_level)) {
            // Ring 在调用线程同步写入，因此 write()/日志宏返回时即可读取。
            const spdlog::details::log_msg ring_message(
                timestamp, spdlog::source_loc{}, module->name,
                spdlog_level, message);
            ringbuf_sink->log(ring_message);
        }

        auto& logger = level >= LogLevel::WARN
            ? module->priority_logger
            : module->normal_logger;
        if (logger && level >= minimum_async_sink_level) {
            logger->log(
                timestamp, spdlog::source_loc{}, spdlog_level, message);
        }
    }

    std::vector<std::string> ring_lines(std::size_t last_n) {
        if (!ringbuf_sink) return {};

        // last_raw() 只在 ring 锁内复制快照；逐条格式化和结果分配放到锁外，
        // 避免崩溃转储或诊断读取长时间阻塞日志生产线程。
        const auto raw_messages = ringbuf_sink->last_raw(last_n);
        std::vector<std::string> result;
        result.reserve(raw_messages.size());
        spdlog::pattern_formatter formatter;
        for (const auto& message : raw_messages) {
            spdlog::memory_buf_t formatted;
            formatter.format(message, formatted);
            result.emplace_back(formatted.data(), formatted.size());
        }
        return result;
    }

    void set_level(std::string_view name, LogLevel level) {
        if (!valid_level(level)) return;
        const auto module = find_module(name);
        if (module) {
            module->level.store(level, std::memory_order_release);
        }
    }

    std::size_t normal_overrun_count() noexcept {
        try {
            return normal_thread_pool
                ? normal_thread_pool->overrun_counter()
                : 0U;
        } catch (...) {
            return 0U;
        }
    }

    // 显式 reset 保证线程先停止；其余成员在析构函数体之后才开始销毁。
    std::shared_ptr<spdlog::details::thread_pool> normal_thread_pool;
    std::shared_ptr<spdlog::details::thread_pool> priority_thread_pool;
    std::vector<spdlog::sink_ptr> async_sinks;
    std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> ringbuf_sink;

    mutable std::shared_mutex modules_mutex;
    std::unordered_map<
        std::string, std::shared_ptr<ModuleState>,
        TransparentStringHash, std::equal_to<>> modules;
    // sink 集合初始化后不再变化，用最低接收等级在格式化前排除必然被全部 sink 丢弃的日志。
    LogLevel minimum_sink_level = LogLevel::OFF;
    // Ring 同步写入，不属于异步 sink；低于此等级的消息禁止无效入队。
    LogLevel minimum_async_sink_level = LogLevel::OFF;
    std::atomic<LogLevel> global_level{LogLevel::TRACE};
    std::atomic<bool> accepting{false};
};

std::unique_ptr<Log::Impl> Log::impl_owner_;
std::atomic<Log::Impl*> Log::impl_{nullptr};

void Log::init(const LogConfig& config) {
    try {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex());
        if (impl_.load(std::memory_order_acquire)) {
            report_error("已经初始化，请先调用 shutdown()");
            return;
        }
        if (!valid_config(config)) {
            report_error("LogConfig 参数非法");
            return;
        }

        // 完整构造成功后再一次性发布，失败时不会留下半初始化对象。
        auto candidate = std::make_unique<Impl>(config);
        auto* published = candidate.get();
        impl_owner_ = std::move(candidate);
        impl_.store(published, std::memory_order_release);
    } catch (const std::exception& exception) {
        report_error("初始化失败", exception.what());
    } catch (...) {
        report_error("初始化失败: 未知异常");
    }
}

void Log::shutdown() {
    try {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex());
        // 先阻止新调用并等待已进入调用退出，确保队列排空与线程池析构固定
        // 发生在 shutdown() 调用线程，且整个销毁期间 init() 无法发布新 sink。
        AccessPause access_pause;
        auto* previous = impl_.exchange(nullptr, std::memory_order_acq_rel);
        if (previous) {
            previous->accepting.store(false, std::memory_order_release);
            const std::size_t overrun_count =
                previous->normal_overrun_count();
            if (overrun_count > 0U) {
                std::fprintf(
                    stderr,
                    "[Log] 普通日志队列累计覆盖 %zu 条低等级日志\n",
                    overrun_count);
            }
        }
        impl_owner_.reset();
    } catch (const std::exception& exception) {
        report_error("关闭失败", exception.what());
    } catch (...) {
        report_error("关闭失败: 未知异常");
    }
}

bool Log::initialized() noexcept {
    AccessLease access;
    if (!access) return false;
    const auto* state = impl_.load(std::memory_order_acquire);
    return state && state->accepting.load(std::memory_order_acquire);
}

std::string Log::register_module(const std::string& module_name,
                                 LogLevel level) {
    std::string result;
    try {
        result = module_name;
        AccessLease access;
        if (!access) return result;
        auto* state = impl_.load(std::memory_order_acquire);
        if (state) {
            state->register_module(module_name, level);
        }
    } catch (const std::exception& exception) {
        report_error("模块注册失败", exception.what());
    } catch (...) {
        report_error("模块注册失败: 未知异常");
    }
    return result;
}

bool Log::should_log(const std::string& module, LogLevel level) noexcept {
    try {
        AccessLease access;
        if (!access) return false;
        const auto* state = impl_.load(std::memory_order_acquire);
        if (!state) return false;
        return state->should_log(state->find_module(module), level);
    } catch (...) {
        return false;
    }
}

void Log::write(const std::string& module, LogLevel level,
                const std::string& message) {
    write_view(module, level, message);
}

void Log::write_view(const std::string& module, LogLevel level,
                     std::string_view message) noexcept {
    try {
        AccessLease access;
        if (!access) return;
        auto* state = impl_.load(std::memory_order_acquire);
        if (!state) return;
        const auto module_state = state->find_module(module);
        if (!module_state) return;
        state->write(module_state, level, message);
    } catch (const std::exception& exception) {
        report_error("日志写入失败", exception.what());
    } catch (...) {
        report_error("日志写入失败: 未知异常");
    }
}

LogLevel Log::level_from_name(const std::string& name) {
    if (name == "TRACE") return LogLevel::TRACE;
    if (name == "DEBUG") return LogLevel::DEBUG;
    if (name == "INFO")  return LogLevel::INFO;
    if (name == "WARN")  return LogLevel::WARN;
    if (name == "ERROR") return LogLevel::ERROR;
    if (name == "OFF")   return LogLevel::OFF;
    return LogLevel::INFO;
}

void Log::set_level(const std::string& module, LogLevel level) {
    try {
        AccessLease access;
        if (!access) return;
        auto* state = impl_.load(std::memory_order_acquire);
        if (state) {
            state->set_level(module, level);
        }
    } catch (...) {
        // 运行时调级失败时保留原等级，不影响业务线程。
    }
}

void Log::set_global_level(LogLevel level) {
    if (!valid_level(level)) return;
    AccessLease access;
    if (!access) return;
    auto* state = impl_.load(std::memory_order_acquire);
    if (state) {
        state->global_level.store(level, std::memory_order_release);
    }
}

std::vector<std::string> Log::get_ring_buffer(int last_n) {
    if (last_n < 0) return {};
    try {
        AccessLease access;
        if (!access) return {};
        auto* state = impl_.load(std::memory_order_acquire);
        if (!state || !state->ringbuf_sink) return {};
        return state->ring_lines(
            last_n == 0 ? 0U : static_cast<std::size_t>(last_n));
    } catch (...) {
        return {};
    }
}

void Log::dump_ring_buffer(const std::string& path) {
    try {
        const auto lines = get_ring_buffer(0);
        if (lines.empty() || path.empty()) return;

        const auto dump_path = std::filesystem::u8path(path);
        const auto directory = dump_path.parent_path();
        if (!directory.empty()) {
            std::error_code error;
            std::filesystem::create_directories(directory, error);
            if (error) {
                report_error("环形缓冲区目录创建失败");
                return;
            }
        }

        std::FILE* file = std::fopen(dump_path.string().c_str(), "wb");
        if (!file) {
            report_error("环形缓冲区文件打开失败");
            return;
        }

        for (const auto& line : lines) {
            // spdlog formatter 已附带平台换行，不再追加第二个换行符。
            std::fwrite(line.data(), sizeof(char), line.size(), file);
        }
        std::fclose(file);
    } catch (const std::exception& exception) {
        report_error("环形缓冲区转储失败", exception.what());
    } catch (...) {
        report_error("环形缓冲区转储失败: 未知异常");
    }
}
