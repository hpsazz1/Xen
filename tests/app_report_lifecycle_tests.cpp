#include "app/report_lifecycle_internal.h"
#include "debug/debug.h"
#include "debug/session_archive.h"
#include "log/log.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <future>
#include <iostream>

namespace {
using Json = nlohmann::json;
using namespace std::chrono_literals;
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
        value.session_id = name; value.json_path = (root / (name + ".json")).string();
        value.csv_path = (root / (name + ".csv")).string(); value.enable_lock_marker = false;
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
    RecoilArchiveConfig config; config.directory = fixture.root / "batches"; config.acquisition_run_id = "test";
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
    const auto json = read_json(report_config.json_path);
    const auto& result = json["recoil"]["batch_archive"];
    expect(result["running"] == false && result["last_sequence"] == "1", "最终报告使用封尾后的状态");
    if (fail_write) expect(result["available"] == false && result["error"] == "批次归档失败：合成尾部写盘失败", "最终报告保留尾部写盘失败");
    else expect(result["files_written"] == 1 && result["incomplete_batches"] == 1, "最终报告包含关闭时截断的尾批");
}

void reopen_during_reload(DetectorReloadState result) {
    Fixture fixture; Source source; source.append(5, 7);
    source.state.state = RuntimeState::RUNNING; source.state.detector_reload_state = result;
    app::detail::ReportBoundary boundary;
    // 加载期间打开记录，直接从重载完成入口进入共用的分段起点。
    boundary.begin_segment(source);
    DebugReport report; SessionArchive archive; std::string error;
    const auto config = fixture.config("reopen");
    expect(report.start(config, error), "重载后启动报告");
    SessionArchiveConfig archive_config; archive_config.directory = (fixture.root / "session").string();
    archive_config.report_config = config; archive_config.trigger_sequence_baseline = boundary.archive_after_event;
    expect(archive.start(archive_config, error), "重载后启动全程归档");
    source.append(6, 8);
    auto snapshot = source.snapshot(); snapshot.trigger_execution_log = source.trigger_execution_log(boundary.archive_after_event);
    expect(archive.submit({}, snapshot), "提交新段事件"); archive.stop();
    boundary.filter_final(snapshot);
    expect(report.finalize(snapshot, error), "封尾重载后的报告");
    const auto json = read_json(config.json_path);
    expect(json["recoil"]["execution"]["records"].size() == 1 &&
           json["recoil"]["execution"]["records"][0]["command_id"] == 8, "重载成功或失败后均不补关闭期间压枪记录");
    const auto segment = read_json(fixture.root / "session/segment-1.json");
    expect(segment["trigger"]["execution"]["events"].size() == 1 &&
           segment["trigger"]["execution"]["events"][0]["sequence"] == "6", "全程归档不补关闭期间扳机事件");
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
    expect(starts == 0 && !restart.take_ready(job.valid()), "后台封尾未结束时不重置 Runtime 或报告");
    allow_finish.set_value(); if (job.valid()) job.get();
    if (restart.take_ready(false)) start();
    expect(starts == 1 && !restart.take_ready(false), "回收封尾结果后只重启一次");
    const bool exists = std::filesystem::exists(old_config.json_path);
    expect(exists, "故障重启保留旧段最终报告");
    if (exists) expect(read_json(old_config.json_path)["sample_count"] == 1, "旧报告保留故障前样本");
    restart.defer_if_active(true, [] {}); restart.cancel();
    expect(!restart.take_ready(false), "用户停止或退出撤销排队重启");
    for (const bool busy : {true, false}) {
        restart.defer_if_active(true, [] {});
        expect(!restart.take_ready(busy, true) && !restart.take_ready(false),
               "键盘或界面急停在封尾中或刚完成时均优先撤销重启");
    }
}
}
int main() {
    LogConfig config; config.enable_console = config.enable_file = config.enable_debug_file = false; Log::init(config);
    try {
        final_archive_state(false); final_archive_state(true);
        reopen_during_reload(DetectorReloadState::SUCCEEDED); reopen_during_reload(DetectorReloadState::FAILED);
        failed_session_restart();
    } catch (const std::exception& error) { ++failures; std::cerr << error.what() << '\n'; }
    Log::shutdown();
    std::cout << "App 报告生命周期失败数: " << failures << '\n';
    return failures ? 1 : 0;
}
