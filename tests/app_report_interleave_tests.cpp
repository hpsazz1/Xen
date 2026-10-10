#include "app/report_lifecycle_internal.h"
#include "debug/debug.h"
#include "debug/session_archive.h"
#include "log/log.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
std::string utf8_path(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    return std::string(encoded.begin(), encoded.end());
}

using Json = nlohmann::json;
using namespace std::chrono_literals;
using app::detail::ReportJobKind;
using app::detail::ReportRestartTarget;
using app::detail::ReportStopRequest;
int failures = 0;

void expect(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "[失败] " << message << '\n'; }
}
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
Json read_json(const std::filesystem::path& path) {
    std::ifstream file(path);
    return Json::parse(file);
}

// 主线程确认工作已进入，再明确放行；工作线程只记录原子状态，不写断言计数。
struct Gate {
    std::promise<void> entered, release;
    std::future<void> arrival = entered.get_future();
    std::shared_future<void> allowed = release.get_future().share();
    std::atomic<bool> timed_out = false;
    bool opened = false;
    bool wait() {
        entered.set_value();
        const bool ready = allowed.wait_for(5s) == std::future_status::ready;
        timed_out = !ready;
        return ready;
    }
    void await_entry() { require(arrival.wait_for(5s) == std::future_status::ready, "后台任务未进入受控窗口"); }
    void open() {
        if (!std::exchange(opened, true)) release.set_value();
    }
};

enum class Event { FINISH_RETURNED, FINISH_COLLECTED, STOP_ENTERED, STOP_RETURNED, STOP_COLLECTED };
struct Trace {
    std::mutex mutex;
    std::vector<Event> values;
    void add(Event value) { std::lock_guard lock(mutex); values.push_back(value); }
    bool before(Event first, Event second) {
        std::lock_guard lock(mutex);
        const auto left = std::find(values.begin(), values.end(), first);
        const auto right = std::find(values.begin(), values.end(), second);
        return left != values.end() && right != values.end() && left < right;
    }
};

// Runtime 使用无设备适配；报告、封尾、future 所有权和调度决策使用生产对象。
struct RuntimeAdapter {
    explicit RuntimeAdapter(Trace& value) : trace(value) { state.state = RuntimeState::RUNNING; }
    Trace& trace;
    std::mutex mutex;
    RuntimeSnapshot state;
    std::shared_ptr<Gate> stop_gate = std::make_shared<Gate>();
    std::atomic<int> stops = 0, disarms = 0;
    RuntimeSnapshot snapshot() { std::lock_guard lock(mutex); return state; }
    TriggerExecutionLog trigger_execution_log() { return snapshot().trigger_execution_log; }
    RecoilExecutionLog recoil_execution_log() { return snapshot().recoil_execution_log; }
    bool post_intent(const RuntimeIntent& intent) {
        if (intent.type == RuntimeIntentType::DISARM_OUTPUT) ++disarms;
        return true;
    }
    void stop() {
        ++stops;
        { std::lock_guard lock(mutex); state.state = RuntimeState::STOPPING; }
        trace.add(Event::STOP_ENTERED);
        stop_gate->wait();
        { std::lock_guard lock(mutex); state.state = RuntimeState::STOPPED; }
        trace.add(Event::STOP_RETURNED);
    }
    void append(std::uint64_t trigger, std::uint64_t recoil, const std::string& error) {
        std::lock_guard lock(mutex);
        TriggerExecutionEvent event; event.sequence = trigger;
        state.trigger_execution_log.events.push_back(event);
        state.trigger_execution_log.last_sequence = trigger;
        RecoilExecutionRecord record; record.intent.command_id = recoil;
        state.recoil_execution_log.records.push_back(record);
        state.trigger_telemetry_available = state.recoil_telemetry_available = true;
        state.last_error = error;
    }
};

struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("xen-app-interleave-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { std::filesystem::create_directories(root); }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
};

struct Session {
    Fixture fixture;
    Trace trace;
    RuntimeAdapter runtime{trace};
    DebugReport report;
    SessionArchive archive;
    app::detail::ReportBoundary boundary;
    app::detail::ReportFinalization finalization;
    bool report_active = false, archive_active = false;
    DebugReportConfig config;
    std::shared_ptr<Gate> finish_gate = std::make_shared<Gate>();
    std::atomic<int> finish_calls = 0, freezes = 0, drains = 0;
    std::vector<std::pair<ReportJobKind, bool>> collected;
    std::vector<bool> stop_retries;
    app::detail::ReportLifecycle lifecycle{report_active, archive_active, finalization};

    explicit Session(bool blocked = false) {
        const auto directory = fixture.root / "reports";
        if (blocked) { std::ofstream file(directory); file << "模拟报告目录被普通文件占用"; }
        config.session_id = "interleave";
        config.json_path = utf8_path((directory / "report.json"));
        config.csv_path = utf8_path((directory / "report.csv"));
        config.enable_lock_marker = false;
        config.recoil_config = RecoilConfig{}; config.trigger_config = TriggerConfig{};
        std::string error;
        report_active = report.start(config, error);
        require(report_active, "初始化真实报告失败");
        runtime.append(5, 7, "分段前状态");
        boundary.begin_segment(runtime);
        runtime.append(6, 8, "原始故障");
    }
    ~Session() {
        // 断言抛出也先放行工作，随后 lifecycle 成员析构回收 future，最后才销毁引用对象。
        finish_gate->open();
        runtime.stop_gate->open();
    }
    bool finish() {
        ++finish_calls;
        return finalization.finish(report_active, archive_active, boundary, report, archive,
            [&] { ++freezes; return runtime.snapshot(); },
            [&](const auto&) {
                ++drains;
                RuntimePipelineSample sample; sample.sequence = 41;
                report.ingest(std::span(&sample, 1));
            });
    }
    void begin_finish() {
        lifecycle.finish_async([this] {
            if (!finish_gate->wait()) return false;
            const bool result = finish();
            trace.add(Event::FINISH_RETURNED);
            return result;
        });
        finish_gate->await_entry();
    }
    ReportStopRequest stop(bool retry = true) {
        stop_retries.push_back(retry);
        return lifecycle.stop(runtime, [this] { return finish(); }, retry);
    }
    void request(ReportRestartTarget target) {
        const auto request_stop = [this] { stop(); };
        if (target == ReportRestartTarget::APPLICATION) lifecycle.request_application_restart(request_stop);
        else require(lifecycle.defer_runtime_restart(request_stop), "活动报告的 Runtime 重启没有延期");
    }
    void poll(bool stop_blocked = false) {
        lifecycle.poll(stop_blocked, [this](bool retry) { stop(retry); },
            [this](bool succeeded, ReportJobKind kind) {
                collected.emplace_back(kind, succeeded);
                trace.add(kind == ReportJobKind::FINISH ? Event::FINISH_COLLECTED : Event::STOP_COLLECTED);
            });
    }
    template<class Predicate>
    void poll_until(Predicate&& ready, bool stop_blocked = false) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        do {
            poll(stop_blocked);
            if (ready()) return;
            std::this_thread::yield();
        } while (std::chrono::steady_clock::now() < deadline);
        throw std::runtime_error("生产调度器回收任务超时");
    }
    void expect_old_report() {
        const auto json = read_json(std::filesystem::u8path(config.json_path));
        expect(json["sample_count"] == 1, "只发布一次原始样本");
        expect(json["final_snapshot"]["last_error"] == "原始故障", "保存原冻结故障原因");
        expect(json["trigger"]["execution"]["events"].size() == 1 &&
               json["trigger"]["execution"]["events"][0]["sequence"] == "6", "保留原扳机事件截止");
        expect(json["recoil"]["execution"]["records"].size() == 1 &&
               json["recoil"]["execution"]["records"][0]["command_id"] == 8, "保留原压枪事件截止");
        expect(freezes == 1 && drains == 1, "封尾和恢复始终只冻结排空一次");
    }
};

void complete_chain(ReportRestartTarget target) {
    Session session;
    session.begin_finish();
    session.request(target);
    expect(session.lifecycle.finishing() && session.lifecycle.stop_pending() && !session.lifecycle.stopping(),
           "封尾未回收时停止必须挂起");
    expect(session.runtime.disarms > 0 && session.runtime.stops == 0, "挂起停止立即解除输出但尚未停止设备");
    expect(session.lifecycle.take_restart(false) == ReportRestartTarget::NONE, "封尾运行中不能重启");
    session.finish_gate->open();
    session.poll_until([&] { return !session.lifecycle.finishing(); }, true);
    expect(session.lifecycle.stop_pending() && session.runtime.stops == 0, "外部任务仍忙时保留挂起停止");
    expect(session.lifecycle.take_restart(false) == ReportRestartTarget::NONE, "空 future 不能越过挂起停止");
    session.poll();
    session.runtime.stop_gate->await_entry();
    expect(session.lifecycle.stopping() && !session.lifecycle.stop_pending(), "回收后把挂起停止升级为真实停止任务");
    expect(session.trace.before(Event::FINISH_COLLECTED, Event::STOP_ENTERED), "必须先 get 封尾结果再开始停止");
    expect(session.lifecycle.take_restart(false) == ReportRestartTarget::NONE, "停止运行中不能重启");
    session.runtime.stop_gate->open();
    session.poll_until([&] { return !session.lifecycle.busy(); });
    expect(session.collected.size() == 2 && session.collected[0] == std::pair{ReportJobKind::FINISH, true} &&
           session.collected[1] == std::pair{ReportJobKind::STOP, true}, "依次回收两种任务的成功结果");
    expect(session.lifecycle.take_restart(true) == ReportRestartTarget::NONE, "外部任务屏障仍阻止重启");
    expect(session.lifecycle.take_restart(false) == target &&
           session.lifecycle.take_restart(false) == ReportRestartTarget::NONE, "所有屏障解除后只消费一次对应目标");
    expect(session.runtime.stops == 1 && !session.finish_gate->timed_out && !session.runtime.stop_gate->timed_out,
           "成功链只停止一次且屏障没有超时");
    session.expect_old_report();
}

void failure_recovery(ReportRestartTarget target, bool fail_during_finish) {
    Session session(true);
    if (fail_during_finish) session.begin_finish();
    session.request(target);
    if (fail_during_finish) {
        session.finish_gate->open();
        session.poll_until([&] { return !session.lifecycle.finishing(); }, true);
        require(session.collected.size() == 1 && !session.collected[0].second, "真实封尾发布故障没有传回失败");
        expect(session.lifecycle.stop_pending() && session.runtime.stops == 0, "报告失败也须保留尚未完成的设备停止");
        session.poll();
    }
    session.runtime.stop_gate->await_entry();
    expect(session.lifecycle.take_restart(false) == ReportRestartTarget::NONE, "失败链停止尚未回收时不得重启");
    session.runtime.stop_gate->open();
    session.poll_until([&] { return !session.lifecycle.busy(); });
    require(session.finalization.failed() && session.finalization.pending() && session.report_active,
            "真实发布失败后没有保留旧报告与冻结快照");
    expect(session.finish_calls == 1 && session.freezes == 1 && session.drains == 1,
           "封尾失败后的自动停止不再次发布、冻结或排空");
    expect(session.collected.size() == (fail_during_finish ? 2u : 1u) &&
           session.collected.back() == std::pair{ReportJobKind::STOP, false}, "停止任务必须传回发布失败");
    expect(session.lifecycle.take_restart(false) == ReportRestartTarget::NONE, "失败回收取消对应重启请求");
    if (fail_during_finish)
        expect(session.stop_retries.size() == 2 && !session.stop_retries.back(), "挂起停止以不重复发布模式升级");
    expect(session.runtime.stops == 1, "初次失败仍真实执行一次停止");

    // 关闭路径可反复轮询，但磁盘故障期间不能按帧率创建任务或重试发布。
    for (int frame = 0; frame != 4; ++frame) {
        session.poll();
        expect(session.stop(false) == ReportStopRequest::IDLE, "已停止的失败报告在自动关闭路径保持空闲");
        expect(!session.lifecycle.busy() && session.lifecycle.take_restart(false) == ReportRestartTarget::NONE,
               "自动关闭不能复活任务或重启请求");
    }
    expect(session.finish_calls == 1 && session.runtime.stops == 1, "连续关闭轮询没有重复工作");
    require(!session.runtime.stop_gate->timed_out && !session.finish_gate->timed_out, "初次失败链屏障超时");

    session.runtime.stop_gate = std::make_shared<Gate>();
    session.request(target);
    session.runtime.stop_gate->await_entry();
    expect(session.stop_retries.back() && session.lifecycle.take_restart(false) == ReportRestartTarget::NONE,
           "磁盘仍失败的显式重试也等待真实停止屏障");
    session.runtime.stop_gate->open();
    session.poll_until([&] { return !session.lifecycle.busy(); });
    expect(session.collected.back() == std::pair{ReportJobKind::STOP, false} &&
           session.lifecycle.take_restart(false) == ReportRestartTarget::NONE, "连续发布失败再次撤销重启请求");
    expect(session.finalization.pending() && session.finalization.failed() && session.report_active,
           "连续失败仍保留原段和可见错误");
    expect(session.finish_calls == 2 && session.runtime.stops == 2 && session.freezes == 1 && session.drains == 1,
           "每次显式失败重试只追加一次停止和发布，不改变原冻结范围");
    require(!session.runtime.stop_gate->timed_out, "连续失败重试屏障超时");

    const auto directory = session.fixture.root / "reports";
    require(std::filesystem::remove(directory), "解除合成文件故障失败");
    std::filesystem::create_directory(directory);
    session.runtime.append(99, 99, "后续状态不能替换原冻结状态");
    session.runtime.stop_gate = std::make_shared<Gate>();
    session.request(target);
    session.runtime.stop_gate->await_entry();
    expect(session.stop_retries.back() && session.lifecycle.take_restart(false) == ReportRestartTarget::NONE,
           "显式请求重新执行保存且仍等待停止回收");
    session.runtime.stop_gate->open();
    session.poll_until([&] { return !session.lifecycle.busy(); });
    expect(session.collected.back() == std::pair{ReportJobKind::STOP, true}, "恢复后的停止任务回传发布成功");
    expect(!session.report_active && !session.finalization.pending() && !session.finalization.failed(),
           "发布成功后才释放旧段和错误");
    expect(session.lifecycle.take_restart(false) == target &&
           session.lifecycle.take_restart(false) == ReportRestartTarget::NONE, "显式重试成功仅允许一次对应重启");
    expect(session.finish_calls == 3 && session.runtime.stops == 3 && !session.runtime.stop_gate->timed_out,
           "恢复只追加一次有界停止和一次发布尝试");
    session.expect_old_report();
}

enum class CancelWindow { FINISH_RUNNING, STOP_RUNNING, STOP_COLLECTED };
void cancellation(ReportRestartTarget target, bool stop_requested, CancelWindow window) {
    Session session;
    session.begin_finish();
    session.request(target);
    const auto cancel = [&] {
        if (stop_requested) {
            session.lifecycle.cancel_restart();
            const auto result = session.stop();
            const auto expected = window == CancelWindow::FINISH_RUNNING ? ReportStopRequest::DEFERRED :
                window == CancelWindow::STOP_RUNNING ? ReportStopRequest::BUSY : ReportStopRequest::IDLE;
            expect(result == expected, "显式停止在对应窗口正确挂起、复用或保持空闲");
        } else session.runtime.post_intent({RuntimeIntentType::EMERGENCY_STOP, true});
        // 分别约束显式撤销和末帧取消，避免前者成功掩盖后者失效；主循环两层均保留。
        expect(session.lifecycle.take_restart(false, !stop_requested) == ReportRestartTarget::NONE,
               "停止或急停同帧先于重启消费");
    };
    if (window == CancelWindow::FINISH_RUNNING) cancel();
    session.finish_gate->open();
    session.poll_until([&] { return session.lifecycle.stopping(); });
    session.runtime.stop_gate->await_entry();
    if (window == CancelWindow::STOP_RUNNING) cancel();
    expect(session.lifecycle.take_restart(false) == ReportRestartTarget::NONE, "取消交错期间仍遵守停止屏障");
    session.runtime.stop_gate->open();
    session.poll_until([&] { return !session.lifecycle.busy(); });
    if (window == CancelWindow::STOP_COLLECTED) cancel();
    expect(session.collected.size() == 2 && session.runtime.stops == 1, "取消不丢失必要回收也不创建第二个停止任务");
    expect(session.lifecycle.take_restart(false) == ReportRestartTarget::NONE, "全部完成后已取消的重启不复活");
    session.poll();
    expect(session.lifecycle.take_restart(false) == ReportRestartTarget::NONE, "下一空帧仍无残留重启请求");
    expect(session.trace.before(Event::FINISH_COLLECTED, Event::STOP_ENTERED) &&
           session.trace.before(Event::STOP_RETURNED, Event::STOP_COLLECTED), "取消仍保留完整任务回收次序");
    expect(!session.finish_gate->timed_out && !session.runtime.stop_gate->timed_out, "取消窗口屏障没有超时");
    session.expect_old_report();
}
}

int main() {
    LogConfig config; config.enable_console = config.enable_file = config.enable_debug_file = false;
    Log::init(config);
    int cases = 0;
    const auto run = [&](const std::string& name, auto&& test) {
        ++cases;
        const int before = failures;
        try { test(); }
        catch (const std::exception& error) { ++failures; std::cerr << "[异常] " << name << ": " << error.what() << '\n'; }
        std::cout << (failures == before ? "[通过] " : "[失败] ") << name << '\n';
    };
    for (const auto target : {ReportRestartTarget::RUNTIME, ReportRestartTarget::APPLICATION}) {
        const std::string prefix = target == ReportRestartTarget::RUNTIME ? "Runtime 重启 / " : "跨运行时重启 / ";
        run(prefix + "完整成功链", [&] { complete_chain(target); });
        for (const bool finish : {true, false})
            run(prefix + (finish ? "封尾失败后恢复" : "停止保存失败后恢复"), [&] { failure_recovery(target, finish); });
        for (const bool stop : {true, false}) {
            for (const auto window : {CancelWindow::FINISH_RUNNING, CancelWindow::STOP_RUNNING, CancelWindow::STOP_COLLECTED}) {
                const auto stage = window == CancelWindow::FINISH_RUNNING ? "封尾中" :
                    window == CancelWindow::STOP_RUNNING ? "停止中" : "停止刚回收";
                run(prefix + (stop ? "停止取消 / " : "急停取消 / ") + stage, [&] { cancellation(target, stop, window); });
            }
        }
    }
    Log::shutdown();
    std::cout << "App 报告启停交错场景数: " << cases << "，失败数: " << failures << '\n';
    return failures ? 1 : 0;
}
