#include <spdlog/sinks/rotating_file_sink.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace fixture {
using namespace std::chrono_literals;
struct SinkGate {
    std::mutex mutex;
    std::condition_variable changed;
    bool blocked = true, timed_out = false;
    unsigned arrivals = 0;
    void enter() {
        std::unique_lock lock(mutex);
        ++arrivals; changed.notify_all();
        if (!changed.wait_for(lock, 5s, [&] { return !blocked; })) timed_out = true;
    }
    bool await(unsigned count) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 2s, [&] { return arrivals >= count; });
    }
    void release() {
        { std::lock_guard lock(mutex); blocked = false; }
        changed.notify_all();
    }
} gate;
}

namespace spdlog::sinks {
// 阻塞边界包围真实rotating sink；文件格式化、写入、flush与队列仍用当前依赖。
class ControlledRotatingSink final : public sink {
public:
    static constexpr auto MaxFiles = rotating_file_sink_mt::MaxFiles;
    template<class... Args>
    explicit ControlledRotatingSink(Args&&... args)
        : actual_(std::make_shared<rotating_file_sink_mt>(std::forward<Args>(args)...)) {}
    void log(const details::log_msg& message) override { fixture::gate.enter(); actual_->log(message); }
    void flush() override { actual_->flush(); }
    void set_pattern(const std::string& value) override { actual_->set_pattern(value); }
    void set_formatter(std::unique_ptr<spdlog::formatter> value) override { actual_->set_formatter(std::move(value)); }
private:
    std::shared_ptr<rotating_file_sink_mt> actual_;
};
}

// 不修改生产接口、线程池、队列或AccessLease；仅在此独立EXE中包围文件sink。
#define rotating_file_sink_mt ControlledRotatingSink
#include "../Xen/log/log.cpp"
#undef rotating_file_sink_mt

struct ShutdownFormatter {};
template<>
struct std::formatter<ShutdownFormatter> : std::formatter<std::string_view> {
    auto format(ShutdownFormatter, std::format_context& context) const {
        Log::write("reentry", LogLevel::WARN, "INNER-FORMATTER");
        Log::shutdown();
        return std::formatter<std::string_view>::format("formatted", context);
    }
};

namespace {
using namespace std::chrono_literals;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Predicate>
void await(Predicate predicate, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    require(predicate(), message);
}
struct Background {
    std::thread producer, stopper;
    ~Background() {
        fixture::gate.release();
        if (producer.joinable()) producer.join();
        if (stopper.joinable()) stopper.join();
    }
};
struct Fixture {
    std::filesystem::path directory = std::filesystem::temp_directory_path() /
        ("xen-log-saturation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~Fixture() {
        fixture::gate.release(); Log::shutdown();
        std::error_code error; std::filesystem::remove_all(directory, error);
    }
    LogConfig config() const {
        LogConfig value;
        value.enable_console = value.enable_debug_file = value.enable_ringbuf = false;
        const auto bytes = directory.u8string(); value.log_dir.assign(bytes.begin(), bytes.end());
        return value;
    }
    std::string read() const {
        std::ifstream file(directory / "xen.log", std::ios::binary);
        require(file.good(), "真实日志文件存在");
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }
};
std::size_t occurrences(const std::string& text, std::string_view needle) {
    std::size_t count = 0, position = 0;
    while ((position = text.find(needle, position)) != std::string::npos) { ++count; position += needle.size(); }
    return count;
}

void saturation_and_shutdown() {
    Fixture fixture;
    Log::init(fixture.config()); require(Log::initialized(), "初始化真实双队列日志");
    Log::register_module("saturation", LogLevel::TRACE);
    Log::write("saturation", LogLevel::INFO, "BLOCKING-FIRST");
    require(fixture::gate.await(1), "普通worker已进入受控慢sink");
    for (unsigned i = 0; i < 5000; ++i) Log::write("saturation", LogLevel::INFO, "NORMAL#" + std::to_string(i) + ";");
    std::atomic<unsigned> priority_completed = 0;
    std::atomic<bool> shutdown_completed = false;
    Background jobs;
    jobs.producer = std::thread([&] {
        for (unsigned i = 0; i < 258; ++i) {
            Log::write("saturation", LogLevel::WARN, "PRIORITY#" + std::to_string(i) + ";");
            ++priority_completed;
        }
    });
    require(fixture::gate.await(2), "优先worker也已进入慢sink");
    await([&] { return priority_completed.load() == 257; }, "256槽优先队列填满且一条正在sink中");
    require(priority_completed == 257, "第258条优先消息正在等待队列空间");
    jobs.stopper = std::thread([&] { Log::shutdown(); shutdown_completed = true; });
    await([] { return !Log::initialized(); }, "shutdown已暂停新访问并等待在途调用");
    require(!shutdown_completed && priority_completed == 257, "慢sink未放行前不能提前销毁活动logger");
    Log::write("saturation", LogLevel::WARN, "AFTER-PAUSE-MUST-NOT-ENTER");
    require(priority_completed == 257 && !shutdown_completed, "暂停后新写入立即拒绝，不参与阻塞队列");
    fixture::gate.release();
    jobs.producer.join(); jobs.stopper.join();
    require(!fixture::gate.timed_out && shutdown_completed && priority_completed == 258, "主动放行后在途生产者与shutdown均正常结束");
    const auto text = fixture.read();
    require(occurrences(text, "BLOCKING-FIRST") == 1 && occurrences(text, "NORMAL#") == 4096,
            "普通队列覆盖904条旧消息，已消费首条与最后4096条排空");
    require(text.find("NORMAL#0;") == std::string::npos && text.find("NORMAL#4999;") != std::string::npos,
            "普通队列保留最新尾部");
    require(occurrences(text, "PRIORITY#") == 258, "优先队列全部258条保留且不重复");
    for (unsigned i = 0; i < 258; ++i)
        require(occurrences(text, "PRIORITY#" + std::to_string(i) + ";") == 1, "逐条核对每个WARN唯一落盘");
    require(text.find("AFTER-PAUSE-MUST-NOT-ENTER") == std::string::npos, "关闭暂停之后的新消息没有进入旧文件");
    Log::init(fixture.config()); require(Log::initialized(), "同目录关闭后重新初始化");
    Log::register_module("saturation", LogLevel::TRACE);
    Log::write("saturation", LogLevel::WARN, "HEALTHY-RESTART"); Log::shutdown();
    const auto restored = fixture.read();
    require(restored.starts_with(text) && occurrences(restored, "HEALTHY-RESTART") == 1, "重启追加新记录且完整保留旧文件");
    std::cout << "普通5000条保留4096/覆盖904，优先258条逐条落盘；双队列饱和停机与同目录恢复通过\n";
}

void formatter_reentry() {
    Fixture fixture;
    Log::init(fixture.config()); require(Log::initialized(), "格式化重入对照初始化");
    Log::register_module("reentry", LogLevel::TRACE);
    // 用户类型formatter发生在write_view取得AccessLease之前；在formatter内关闭不能自等。
    LOG_INFO("reentry", "OUTER-MUST-NOT-PUBLISH-{}", ShutdownFormatter{});
    require(!Log::initialized(), "格式化回调中关闭已完成");
    const auto before = fixture.read();
    require(occurrences(before, "INNER-FORMATTER") == 1 && before.find("OUTER-MUST-NOT-PUBLISH") == std::string::npos,
            "嵌套记录已排空，关闭后外层记录不会访问旧logger");
    Log::init(fixture.config()); Log::register_module("reentry", LogLevel::TRACE);
    LOG_INFO("reentry", "AFTER-FORMATTER-RESTART"); Log::shutdown();
    const auto after = fixture.read();
    require(after.starts_with(before) && occurrences(after, "AFTER-FORMATTER-RESTART") == 1,
            "格式化重入关闭后同目录正常恢复");
    std::cout << "真实自定义formatter重入写入/关闭及重新初始化通过\n";
}
}

int main() {
    try { saturation_and_shutdown(); formatter_reentry(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
