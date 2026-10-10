#include "app/report_lifecycle_internal.h"
#include "debug/debug.h"
#include "debug/session_archive.h"
#include "log/log.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <future>
#include <iostream>

namespace {
std::string utf8_path(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    return std::string(encoded.begin(), encoded.end());
}

using Json = nlohmann::json;
using namespace std::chrono_literals;
using app::detail::ReportRestartTarget;
int failures = 0;
void expect(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "[失败] " << message << '\n'; }
}
Json read_json(const std::filesystem::path& path) {
    std::ifstream file(path); return Json::parse(file);
}
struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("xen-app-report-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { std::filesystem::create_directories(root); }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    DebugReportConfig config(const std::string& name) const {
        DebugReportConfig value;
        value.session_id = name; value.json_path = utf8_path((root / (name + ".json")));
        value.csv_path = utf8_path((root / (name + ".csv"))); value.enable_lock_marker = false;
        value.recoil_config = RecoilConfig{}; value.trigger_config = TriggerConfig{};
        return value;
    }
};
struct Source {
    RuntimeSnapshot state;
    RecoilBatchArchive* archive = nullptr;
    RuntimeSnapshot snapshot() const {
        auto value = state;
        if (archive) value.recoil_archive = archive->snapshot();
        return value;
    }
    TriggerExecutionLog trigger_execution_log(std::uint64_t after = 0) const {
        auto log = state.trigger_execution_log;
        std::erase_if(log.events, [after](const auto& event) { return event.sequence <= after; });
        return log;
    }
    RecoilExecutionLog recoil_execution_log() const { return state.recoil_execution_log; }
    bool set_recoil_archive(std::optional<RecoilArchiveConfig>) { archive->stop(); return true; }
    void append(std::uint64_t trigger, std::uint64_t recoil) {
        TriggerExecutionEvent event; event.sequence = trigger;
        state.trigger_execution_log.events.push_back(event);
        state.trigger_execution_log.last_sequence = trigger;
        RecoilExecutionRecord record; record.intent.command_id = recoil;
        state.recoil_execution_log.records.push_back(record);
        state.trigger_telemetry_available = state.recoil_telemetry_available = true;
    }
};

void final_archive_state(bool fail_write) {
    Fixture fixture;
    std::promise<void> sink_entered, release_sink;
    auto entered = sink_entered.get_future(); auto release = release_sink.get_future();
    RecoilBatchArchive archive;
    RecoilArchiveConfig config; config.directory = fixture.root / std::filesystem::u8path("中文 批次目录"); config.acquisition_run_id = "test";
    auto profile = std::make_shared<RecoilProfile>(); profile->id = "synthetic"; profile->weapon_id = "test";
    profile->state = RecoilProfileState::CALIBRATED; profile->phase_tolerance_ms = 20; profile->recovery_ms = 10;
    profile->source.sha256 = std::string(64, 'a'); profile->source.source_unit = "synthetic";
    profile->calibration = {"test", "fake", "test", "synthetic:test_only", 1.0};
    profile->points = {{0, 0, 0}, {4000, 0, 4000}};
    RecoilExecutionEvent begin; begin.sequence = 1; begin.firing_id = 1; begin.profile = profile;
    begin.event_at = begin.firing_started_at = RecoilTime(100ms);
    expect(archive.start(config, [begin](auto after, auto maximum) {
        RecoilEventSlice slice; slice.latest_sequence = slice.oldest_available_sequence = 1;
        if (after < 1 && maximum) slice.events.push_back(begin);
        return slice;
    }, [&](const auto&, const auto&, std::string& error) {
        sink_entered.set_value();
        if (release.wait_for(5s) != std::future_status::ready) { error = "屏障超时"; return false; }
        if (fail_write) { error = "合成尾部写盘失败"; return false; }
        return true;
    }), "启动真实归档对象");
    Source source; source.archive = &archive; source.append(1, 1);
    archive.request_stop();
    const bool ready = entered.wait_for(5s) == std::future_status::ready;
    expect(ready, "在尾部落盘中固定关闭窗口");
    auto frozen = source.snapshot();
    expect(frozen.recoil_archive.running && frozen.recoil_archive.files_written == 0, "封尾前快照确实过时");
    source.append(2, 2); // 关闭水位后的执行事件不能随归档终态一起补入。
    DebugReport report; std::string error; const auto report_config = fixture.config("final");
    expect(report.start(report_config, error), "启动最终报告");
    auto finish = std::async(std::launch::async, [&] {
        app::detail::finish_reports_after_archive(source, frozen, [&](RuntimeSnapshot value) {
            expect(value.trigger_execution_log.events.size() == 1 && value.recoil_execution_log.records.size() == 1,
                   "归档终态回填不能扩大冻结执行水位");
            expect(report.finalize(value, error), "写出最终报告");
        });
    });
    release_sink.set_value(); finish.get();
    const auto json = read_json(std::filesystem::u8path(report_config.json_path));
    const auto& result = json["recoil"]["batch_archive"];
    expect(result["directory"] == utf8_path(config.directory), "中文压枪归档目录必须以 UTF-8 写入报告元数据");
    expect(result["running"] == false && result["last_sequence"] == "1", "最终报告使用封尾后的状态");
    if (fail_write) expect(result["available"] == false && result["error"] == "批次归档失败：合成尾部写盘失败", "最终报告保留尾部写盘失败");
    else expect(result["files_written"] == 1 && result["incomplete_batches"] == 1, "最终报告包含关闭时截断的尾批");
}

void reopen_during_reload(DetectorReloadState result) {
    Fixture fixture; Source source; source.append(5, 7);
    source.state.state = RuntimeState::RUNNING;
    source.state.detector_reload_state = DetectorReloadState::LOADING;
    source.state.active_model_path = "old.onnx"; source.state.detector_generation = 1;
    app::detail::ReportBoundary boundary;
    app::detail::ReportResume resume;
    DebugReport report; SessionArchive archive; std::string error;
    auto config = fixture.config("reopen");
    int starts = 0;
    const auto open = [&](const RuntimeSnapshot& snapshot) {
        ++starts;
        boundary.begin_segment(source);
        config.model_path = snapshot.active_model_path;
        expect(report.start(config, error), "重载后启动报告");
        SessionArchiveConfig archive_config; archive_config.directory = (fixture.root / "session").string();
        archive_config.report_config = config; archive_config.trigger_sequence_baseline = boundary.archive_after_event;
        expect(archive.start(archive_config, error), "重载后启动全程归档");
        return report.active();
    };
    resume.reload_requested(true);
    resume.diagnostics_changed(true); // 用户在 LOADING 窗口开启，而不是直接调用开段 helper。
    resume.poll(source.snapshot(), true, open);
    resume.poll(source.snapshot(), false, open);
    expect(starts == 0, "封尾忙碌或模型仍 LOADING 时不能提前打开报告");
    source.append(6, 8); // 加载窗口内继续产生的事件也必须被恢复水位排除。
    source.state.detector_reload_state = result;
    if (result == DetectorReloadState::SUCCEEDED) {
        source.state.active_model_path = "new.onnx"; source.state.detector_generation = 2;
    }
    resume.poll(source.snapshot(), true, open);
    expect(starts == 0, "重载完成但后台封尾未回收时仍不能开段");
    const auto reopened = resume.poll(source.snapshot(), false, open);
    expect(reopened.reload_finished && reopened.report_started && starts == 1, "真实恢复路由只打开一个报告");
    resume.poll(source.snapshot(), false, open);
    expect(starts == 1, "恢复已消费的开段意图不能下一帧重复执行");
    expect(config.model_path == (result == DetectorReloadState::SUCCEEDED ? "new.onnx" : "old.onnx"),
           "重载成功使用新模型，失败仍使用旧模型身份");
    source.append(7, 9);
    auto snapshot = source.snapshot(); snapshot.trigger_execution_log = source.trigger_execution_log(boundary.archive_after_event);
    expect(archive.submit({}, snapshot), "提交新段事件"); archive.stop();
    boundary.filter_final(snapshot);
    expect(report.finalize(snapshot, error), "封尾重载后的报告");
    const auto json = read_json(std::filesystem::u8path(config.json_path));
    expect(json["recoil"]["execution"]["records"].size() == 1 &&
           json["recoil"]["execution"]["records"][0]["command_id"] == 9, "重载成功或失败后均不补关闭期间压枪记录");
    const auto segment = read_json(fixture.root / "session/segment-1.json");
    expect(segment["trigger"]["execution"]["events"].size() == 1 &&
           segment["trigger"]["execution"]["events"][0]["sequence"] == "7", "全程归档不补关闭期间扳机事件");
}

void cancelled_report_resume() {
    RuntimeSnapshot snapshot; snapshot.state = RuntimeState::RUNNING;
    snapshot.detector_reload_state = DetectorReloadState::LOADING;
    for (bool stopped : {false, true}) {
        app::detail::ReportResume resume;
        int starts = 0;
        const auto open = [&](const auto&) { ++starts; return true; };
        resume.reload_requested(true); resume.diagnostics_changed(true);
        resume.poll(snapshot, false, open);
        if (stopped) {
            snapshot.state = RuntimeState::STOPPED;
            resume.poll(snapshot, false, open);
            snapshot.state = RuntimeState::RUNNING;
        } else resume.cancel();
        snapshot.detector_reload_state = DetectorReloadState::SUCCEEDED;
        resume.poll(snapshot, false, open);
        expect(starts == 0, "停止或取消清除整个恢复意图，晚到的重载结果不能重新开段");
        snapshot.detector_reload_state = DetectorReloadState::LOADING;
    }
    app::detail::ReportResume disabled;
    disabled.reload_requested(true); disabled.diagnostics_changed(true);
    disabled.diagnostics_changed(false);
    snapshot.detector_reload_state = DetectorReloadState::FAILED;
    const auto outcome = disabled.poll(snapshot, false, [](const auto&) { return false; });
    expect(outcome.reload_finished && !outcome.report_started, "加载期间再次关闭记录，重载失败后也不生成报告");
}

void failed_session_restart() {
    Fixture fixture; DebugReport report; std::string error;
    const auto old_config = fixture.config("failed"), new_config = fixture.config("restarted");
    expect(report.start(old_config, error), "启动故障前报告");
    RuntimePipelineSample sample; sample.sequence = 41; report.ingest(std::span(&sample, 1));
    RuntimeSnapshot failed; failed.state = RuntimeState::FAILED;
    app::detail::ReportRestart restart;
    std::promise<void> allow_finish; auto release = allow_finish.get_future(); std::future<void> job;
    bool active = true; int starts = 0;
    const auto start = [&] { ++starts; expect(report.start(new_config, error), "重启后创建新报告"); };
    const bool deferred = restart.defer_if_active(active, [&] {
        job = std::async(std::launch::async, [&] {
            release.wait(); expect(report.finalize(failed, error), "重启前封尾故障报告"); active = false;
        });
    });
    if (!deferred) start();
    expect(starts == 0 && restart.take_ready(job.valid()) == ReportRestartTarget::NONE, "后台封尾未结束时不重置 Runtime 或报告");
    allow_finish.set_value(); if (job.valid()) job.get();
    if (restart.take_ready(false) == ReportRestartTarget::RUNTIME) start();
    expect(starts == 1 && restart.take_ready(false) == ReportRestartTarget::NONE, "回收封尾结果后只重启一次");
    const bool exists = std::filesystem::exists(std::filesystem::u8path(old_config.json_path));
    expect(exists, "故障重启保留旧段最终报告");
    if (exists) expect(read_json(std::filesystem::u8path(old_config.json_path))["sample_count"] == 1, "旧报告保留故障前样本");
    restart.defer_if_active(true, [] {}); restart.cancel();
    expect(restart.take_ready(false) == ReportRestartTarget::NONE, "用户停止或退出撤销排队重启");
    for (const bool busy : {true, false}) {
        restart.defer_if_active(true, [] {});
        expect(restart.take_ready(busy, true) == ReportRestartTarget::NONE &&
               restart.take_ready(false) == ReportRestartTarget::NONE,
               "键盘或界面急停在封尾中或刚完成时均优先撤销重启");
        restart.request_application([] {});
        expect(restart.take_ready(busy, true) == ReportRestartTarget::NONE &&
               restart.take_ready(false) == ReportRestartTarget::NONE, "停止或急停同样撤销跨运行时重启");
    }
}

void final_report_failure_recovery(ReportRestartTarget target) {
    Fixture fixture; Source source; source.append(5, 7);
    app::detail::ReportBoundary boundary; boundary.begin_segment(source);
    app::detail::ReportFinalization finalization;
    app::detail::ReportRestart restart;
    const auto request_restart = [&] {
        if (target == ReportRestartTarget::APPLICATION) restart.request_application([] {});
        else restart.defer_if_active(true, [] {});
    };
    DebugReport report; SessionArchive archive; std::string error;
    auto config = fixture.config("retry");
    const auto blocked = fixture.root / "blocked";
    { std::ofstream file(blocked); file << "模拟报告父目录被文件占用"; }
    config.csv_path = utf8_path((blocked / "retry.csv"));
    config.json_path = utf8_path((blocked / "retry.json"));
    bool active = report.start(config, error), archive_active = false;
    expect(active, "启动等待封尾的报告");
    source.append(6, 8); source.state.state = RuntimeState::FAILED;
    source.state.last_error = "合成运行故障";
    RuntimePipelineSample sample; sample.sequence = 41;
    int freezes = 0, drains = 0;
    const auto attempt = [&] {
        auto job = std::async(std::launch::async, [&] {
            return finalization.finish(active, archive_active, boundary, report, archive,
                [&] { ++freezes; return source.snapshot(); },
                [&](const auto&) { ++drains; report.ingest(std::span(&sample, 1)); });
        });
        const bool finished = job.get();
        restart.finish_completed(finished);
        return finished;
    };
    request_restart();
    expect(restart.take_ready(true) == ReportRestartTarget::NONE, "两种重启都必须等待后台封尾结果");
    const bool finished = attempt();
    expect(!finished, "最终报告发布失败必须传回失败结果");
    expect(active && report.active() && finalization.pending(), "失败后保留旧报告与冻结快照");
    expect(finalization.failed() && !finalization.error().empty(), "发布失败保留可见错误");
    expect(restart.take_ready(false) == ReportRestartTarget::NONE, "发布失败必须取消 Runtime 或跨运行时自动重启");
    if (finished || !active) return; // 红灯时不把缺少产物误报为 JSON 解析故障。
    source.append(99, 99); source.state.last_error = "后续状态不能替换旧故障";
    expect(!attempt(), "故障未解除的再次封尾仍明确失败");
    expect(freezes == 1 && drains == 1, "失败重试不扩大事件截止或重复摄入旧样本");
    std::filesystem::remove(blocked); std::filesystem::create_directory(blocked);
    request_restart();
    expect(attempt(), "解除磁盘故障后可通过同一生产封尾入口重试");
    expect(!active && !report.active() && !finalization.pending() && !finalization.failed(), "成功发布后才释放旧段");
    expect(restart.take_ready(false) == target && restart.take_ready(false) == ReportRestartTarget::NONE,
           "显式重试成功后只允许一次对应目标的重启");
    const auto json = read_json(std::filesystem::u8path(config.json_path));
    expect(json["sample_count"] == 1, "重试保留故障前样本且不重复");
    expect(json["final_snapshot"]["last_error"] == "合成运行故障", "重试仍保留旧段故障原因");
    expect(json["recoil"]["execution"]["records"].size() == 1 &&
           json["recoil"]["execution"]["records"][0]["command_id"] == 8, "重试保留原冻结压枪事件");
    expect(json["trigger"]["execution"]["events"].size() == 1 &&
           json["trigger"]["execution"]["events"][0]["sequence"] == "6", "重试不丢原扳机事件或混入后续事件");
    expect(freezes == 1 && drains == 1, "成功重试仍使用原冻结范围");
    const auto new_config = fixture.config("after-recovery");
    expect(report.start(new_config, error), "原段保存成功后允许创建独立新段");
    sample.sequence = 73; report.ingest(std::span(&sample, 1));
    expect(report.finalize({}, error), "新段独立封尾");
    expect(read_json(std::filesystem::u8path(new_config.json_path))["sample_count"] == 1 && read_json(std::filesystem::u8path(config.json_path)) == json,
           "新段不混入旧样本，也不覆盖已恢复的旧文件");
}
}
int main() {
    LogConfig config; config.enable_console = config.enable_file = config.enable_debug_file = false; Log::init(config);
    try {
        final_archive_state(false); final_archive_state(true);
        reopen_during_reload(DetectorReloadState::SUCCEEDED); reopen_during_reload(DetectorReloadState::FAILED);
        cancelled_report_resume();
        failed_session_restart();
        final_report_failure_recovery(ReportRestartTarget::RUNTIME);
        final_report_failure_recovery(ReportRestartTarget::APPLICATION);
    } catch (const std::exception& error) { ++failures; std::cerr << error.what() << '\n'; }
    Log::shutdown();
    std::cout << "App 报告生命周期失败数: " << failures << '\n';
    return failures ? 1 : 0;
}
