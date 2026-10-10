#include "debug/session_archive.h"
#include "debug/session_archive_internal.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <stdexcept>
#include <algorithm>
#include <array>
#include <future>
#include <atomic>
#include <type_traits>

static_assert(std::is_nothrow_copy_constructible_v<RuntimePipelineSample>);
static_assert(std::is_nothrow_copy_assignable_v<RuntimePipelineSample>);
static_assert(std::is_nothrow_move_assignable_v<RuntimeSnapshot>);

namespace {
using Json=nlohmann::json;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
Json read(const std::filesystem::path& path) { std::ifstream input(path); Json value; input>>value; return value; }
void send(SessionArchive& archive, std::span<const RuntimePipelineSample> samples, const RuntimeSnapshot& snapshot) {
    for (int i=0;i<100;++i) {
        if (archive.submit(samples,snapshot)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    throw std::runtime_error("提交归档超时");
}
int benchmark(const std::filesystem::path& root, bool representative = false) {
    using Clock=std::chrono::steady_clock;
    SessionArchiveConfig config; config.directory=(root/"archive").string();
    config.report_config.session_id="offline-240fps-45s";
    const int submit_hz=representative?144:60;
    const int seconds=representative?15:45;
    const int batches=seconds*submit_hz;
    if (representative) {
        config.report_config.session_id="offline-240fps-144hz-137events-15s";
        config.report_config.trigger_config=TriggerConfig{};
        config.report_config.trigger_config->enabled=true;
    }
    SessionArchive archive; std::string error;
    check(archive.start(config,error),"吞吐实验启动");
    RuntimeSnapshot snapshot; snapshot.trigger_telemetry_available=true;
    snapshot.state=RuntimeState::RUNNING;
    std::vector<double> latency; latency.reserve(batches);
    std::uint64_t trigger_sequence=0;
    std::uint64_t sample_sequence=0;
    std::size_t queue_peak=0;
    const auto started=Clock::now();
    for (int batch=0;batch<batches;++batch) {
        std::this_thread::sleep_until(started+std::chrono::nanoseconds(static_cast<std::int64_t>(batch)*1000000000LL/submit_hz));
        std::vector<RuntimePipelineSample> samples((batch+1)*240/submit_hz-batch*240/submit_hz);
        for (std::size_t i=0;i<samples.size();++i) {
            auto& sample=samples[i]; sample.sequence=++sample_sequence;
            sample.frame_timing.control_steady_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
            sample.profile.total_ms=2.5; sample.aim_control_center_x=320; sample.aim_control_center_y=320;
            sample.person_detection_count=2; sample.max_person_confidence=0.85f;
        }
        snapshot.trigger_execution_log.events.clear();
        if (representative && batch%14==0) {
            const auto target_sequence=static_cast<std::uint64_t>((batch+1)*137/submit_hz);
            while (trigger_sequence<target_sequence) {
                TriggerExecutionEvent event; event.sequence=++trigger_sequence; event.observed_at=Clock::now();
                event.snapshot.context.timing_weapon_id="ak47";
                snapshot.trigger_execution_log.events.push_back(event);
            }
        } else if (!representative && batch%6==0) {
            TriggerExecutionEvent event; event.sequence=++trigger_sequence; event.observed_at=Clock::now();
            snapshot.trigger_execution_log.events.push_back(event);
        }
        const auto before=Clock::now();
        archive.submit(samples,snapshot);
        latency.push_back(std::chrono::duration<double,std::micro>(Clock::now()-before).count());
        queue_peak=std::max(queue_peak,archive.status().queued_batches);
    }
    const auto stop_started=Clock::now(); archive.stop();
    const double stop_ms=std::chrono::duration<double,std::milli>(Clock::now()-stop_started).count();
    const double elapsed=std::chrono::duration<double>(Clock::now()-started).count();
    auto state=archive.status(); std::sort(latency.begin(),latency.end());
    auto percentile=[&](double q){ return latency[static_cast<std::size_t>(q*(latency.size()-1))]; };
    std::uint64_t bytes=0;
    for (const auto& file:std::filesystem::directory_iterator(config.directory))
        if (file.is_regular_file()) bytes+=file.file_size();
    Json result={
        {"kind","synthetic_numeric_archive_only"}, {"equivalent_game_seconds",seconds},
        {"submit_hz",submit_hz}, {"trigger_events_generated",trigger_sequence}, {"queue_peak_batches",queue_peak},
        {"actual_elapsed_seconds",elapsed}, {"submitted_samples",sample_sequence},
        {"written_samples",state.written_samples}, {"dropped_samples",state.dropped_samples},
        {"dropped_batches",state.dropped_batches}, {"trigger_events_dropped",state.trigger_events_dropped},
        {"coalesced_batches",state.coalesced_batches},{"queue_capacity_rejections",state.queue_capacity_rejections},
        {"sample_capacity_rejections",state.sample_capacity_rejections},
        {"written_segments",state.written_segments}, {"last_error",state.last_error},
        {"submit_p50_us",percentile(0.50)}, {"submit_p95_us",percentile(0.95)},
        {"submit_p99_us",percentile(0.99)}, {"submit_max_us",latency.back()},
        {"stop_drain_ms",stop_ms}, {"files_bytes",bytes}, {"sample_size_bytes",sizeof(RuntimePipelineSample)},
        {"physical_effect_verified",false}};
    std::ofstream output(root/"benchmark.json"); output<<result.dump(2); output.close();
    check(static_cast<bool>(output),"吞吐实验结果写入");
    std::cout<<result.dump(2)<<'\n';
    check(state.written_samples+state.dropped_samples==sample_sequence,"归档帧数守恒");
    return 0;
}
int queue_pressure(const std::filesystem::path& root) {
    SessionArchiveConfig config; config.directory=(root/"archive").string();
    config.segment_samples=1200; config.report_config.trigger_config=TriggerConfig{};
    SessionArchive archive; std::string error;
    check(archive.start(config,error),"开始可控写入压力测试");
    std::promise<void> entered, release;
    auto entered_future=entered.get_future(); auto release_future=release.get_future().share();
    std::atomic<bool> first{true};
    xen::debug::detail::SessionArchiveTestAccess::before_flush(archive,[&]{
        if (first.exchange(false)) { entered.set_value(); release_future.wait(); }
    });
    RuntimeSnapshot snapshot; snapshot.trigger_telemetry_available=true;
    std::vector<RuntimePipelineSample> initial(1200);
    for (std::size_t i=0;i<initial.size();++i) initial[i].sequence=i+1;
    send(archive,initial,snapshot);
    const bool paused=entered_future.wait_for(std::chrono::seconds(5))==std::future_status::ready;
    if (!paused) { release.set_value(); archive.stop(); check(false,"后台未到达可控暂停点"); }
    const auto started=std::chrono::steady_clock::now();
    std::uint64_t sequence=1200; std::size_t peak=0;
    for (int tick=0;tick<50;++tick) {
        std::this_thread::sleep_until(started+std::chrono::nanoseconds(static_cast<std::int64_t>(tick)*1000000000LL/144));
        std::vector<RuntimePipelineSample> samples((tick+1)*240/144-tick*240/144);
        for (auto& sample:samples) sample.sequence=++sequence;
        snapshot.trigger_execution_log.events.clear();
        TriggerExecutionEvent event; event.sequence=tick+1; snapshot.trigger_execution_log.events.push_back(event);
        archive.submit(samples,snapshot); peak=std::max(peak,archive.status().queued_batches);
    }
    release.set_value(); archive.stop(); const auto state=archive.status();
    Json result={{"written_samples",state.written_samples},{"submitted_samples",sequence},
        {"dropped_samples",state.dropped_samples},{"dropped_batches",state.dropped_batches},
        {"queue_peak_batches",peak},{"queued_sample_budget",12000},{"samples_during_pause",sequence-1200},
        {"coalesced_batches",state.coalesced_batches},{"queue_capacity_rejections",state.queue_capacity_rejections},
        {"pause_ms",1000.0*49/144},{"trigger_events_dropped",state.trigger_events_dropped}};
    std::ofstream output(root/"pressure.json"); output<<result.dump(2); output.close();
    std::cout<<result.dump(2)<<'\n';
    check(state.written_samples==sequence && !state.dropped_samples && !state.trigger_events_dropped,
        "短暂停写且样本远低预算时，不应按UI批数丢弃");
    std::uint64_t csv_sequence=0,event_sequence=0;
    for(std::uint64_t segment=1;segment<=state.written_segments;++segment) {
        const auto base=root/"archive"/("segment-"+std::to_string(segment));
        std::ifstream csv(base.string()+".csv"); std::string line;
        while(std::getline(csv,line)) {
            if(line.empty() || line.front()=='#' || line.starts_with("sequence,")) continue;
            const auto actual=std::stoull(line.substr(0,line.find(',')));
            check(actual==++csv_sequence,"合并后CSV序号必须跨段连续且无重复");
        }
        const auto report=read(base.string()+".json");
        for(const auto& event:report["trigger"]["execution"]["events"])
            check(std::stoull(event["sequence"].get<std::string>())==++event_sequence,
                "合并后Trigger事件序号必须连续且无重复");
    }
    check(csv_sequence==1283 && event_sequence==50,"压力用例保留全部1283帧及50个有序事件");
    return 0;
}
void queue_contracts(const std::filesystem::path& root, bool capacity) {
    SessionArchiveConfig config; config.directory=(root/"archive").string();
    config.report_config.trigger_config=TriggerConfig{};
    SessionArchive archive; std::string error; check(archive.start(config,error),"开始合并边界测试");
    std::promise<void> entered,release; auto ready=entered.get_future(); auto gate=release.get_future().share();
    std::atomic<bool> first{true};
    xen::debug::detail::SessionArchiveTestAccess::before_flush(archive,[&]{
        if(first.exchange(false)){entered.set_value();gate.wait();}
    });
    RuntimeSnapshot snapshot; snapshot.trigger_telemetry_available=true;
    std::vector<RuntimePipelineSample> samples(1200); send(archive,samples,snapshot);
    const bool paused=ready.wait_for(std::chrono::seconds(5))==std::future_status::ready;
    if(!paused){release.set_value();archive.stop();check(false,"合并边界暂停失败");}
    bool accepted=true, rejected=false, marked=false; std::size_t queued=0;
    if(capacity) {
        for(int i=0;i<10;++i) accepted=archive.submit(samples,snapshot)&&accepted;
        rejected=!archive.submit(std::span(samples).first(1),snapshot);
    } else {
        TriggerExecutionEvent event; event.sequence=1; snapshot.trigger_execution_log.events={event};
        accepted=archive.submit({},snapshot);
        marked=archive.mark("顺序屏障");
        event.sequence=2; snapshot.trigger_execution_log.events={event};
        accepted=archive.submit({},snapshot)&&accepted;
        snapshot.trigger_execution_log.events.clear(); snapshot.last_sequence=9876;
        accepted=archive.submit({},snapshot)&&accepted;
        queued=archive.status().queued_batches;
    }
    release.set_value(); archive.stop(); auto state=archive.status();
    check(accepted,"边界内提交保持成功");
    if(capacity) {
        check(rejected && state.sample_capacity_rejections==1 && state.dropped_samples==1 && state.written_samples==13200,
            "真正超过12000队列帧预算才明确拒绝");
    } else {
        check(marked && queued==3 && state.marker_count==1,"marker阻止跨标记合并");
        auto report=read(root/"archive"/"segment-2.json");
        const auto& events=report["trigger"]["execution"]["events"];
        check(events.size()==2 && events[0]["sequence"]=="1" && events[1]["sequence"]=="2","纯事件合并保持事件顺序");
        check(report["final_snapshot"]["last_sequence"]==9876,"空帧最终提交保留最新快照");
    }
}
void native_chinese_report_paths(const std::filesystem::path& root) {
    const auto directory = root / std::filesystem::u8path("中文 归档目录");
    SessionArchiveConfig config;
    // SessionArchive 保留 native narrow 输入契约；进入 DebugReport 时才转 UTF-8。
    config.directory = directory.string();
    config.segment_samples = 2;
    config.segment_interval = std::chrono::seconds(30);
    config.report_config.session_id = "native-chinese-archive";
    config.report_config.enable_lock_marker = true;
    SessionArchive archive;
    std::string error;
    check(archive.start(config, error), "中文含空格 native 路径启动归档");
    std::array<RuntimePipelineSample, 5> samples{};
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i].sequence = i + 1;
        samples[i].aim_lock_active = true;
        samples[i].detection_status = DetectionStatus::SUCCESS;
        samples[i].aim_status = AimStatus::SUCCESS;
        samples[i].mouse_status = MouseStatus::READY;
    }
    RuntimeSnapshot snapshot;
    check(archive.submit(samples, snapshot), "中文路径提交全部样本");
    bool marked = false;
    for (int attempt = 0; attempt < 100 && !marked; ++attempt) {
        marked = archive.mark("中文 路径标记");
        if (!marked) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    check(marked, "中文路径接受归档标记");
    archive.stop();
    const auto state = archive.status();
    check(!state.active && state.accepted_samples == samples.size() &&
              state.written_samples == samples.size() && state.written_segments == 3 &&
              state.dropped_samples == 0 && state.dropped_batches == 0 &&
              state.trigger_events_dropped == 0 && state.last_error.empty(),
          "中文路径多段与停止尾段必须完整且不丢样本");
    const auto manifest = read(directory / "manifest.json");
    check(manifest["closed"] == true && manifest["complete"] == true &&
              manifest["accepted_samples"] == samples.size() &&
              manifest["written_samples"] == samples.size() &&
              manifest["written_segments"] == 3 && manifest["dropped_samples"] == 0 &&
              manifest["dropped_batches"] == 0 && manifest["last_error"] == "",
          "中文路径最终 manifest 必须如实完整且无丢失");
    std::vector<std::filesystem::path> expected{
        directory / "manifest.json", directory / "marker-1.json"};
    std::uint64_t sequence = 0;
    for (int segment = 1; segment <= 3; ++segment) {
        const std::string leaf = "segment-" + std::to_string(segment);
        const auto csv_path = directory / (leaf + ".csv");
        const auto json_path = directory / (leaf + ".json");
        const auto meta_path = directory / (leaf + ".meta.json");
        expected.insert(expected.end(), {csv_path, json_path, meta_path});
        const auto meta = read(meta_path);
        const std::size_t expected_count = segment == 3 ? 1 : 2;
        check(meta["published"] == true && meta["sample_count"] == expected_count &&
                  meta["csv"] == leaf + ".csv" && meta["json"] == leaf + ".json",
              "中文路径各段元数据必须对应实际文件与尾段样本数");
        const auto report = read(json_path);
        check(report["session_id"] == config.report_config.session_id &&
                  report["samples_omitted"] == true && report["samples"].empty() &&
                  report["samples_csv"] == leaf + ".csv",
              "中文路径分段 JSON 必须可解析且引用对应 CSV");
        std::ifstream csv(csv_path, std::ios::binary);
        check(static_cast<bool>(csv), "中文路径分段 CSV 必须可打开");
        std::string line;
        std::size_t segment_count = 0;
        while (std::getline(csv, line)) {
            if (line.empty() || line.front() == '#' || line.starts_with("sequence,")) continue;
            check(std::stoull(line.substr(0, line.find(','))) == ++sequence,
                  "中文路径 CSV 样本跨段必须连续且不重复");
            ++segment_count;
        }
        check(csv.eof() && segment_count == expected_count,
              "中文路径 CSV 必须完整读到各段预期样本数");
    }
    check(sequence == samples.size(), "中文路径 CSV 必须保留全部样本");
    const auto marker = read(directory / "marker-1.json");
    check(marker["label"] == "中文 路径标记" && marker["closed"] == true,
          "中文路径归档 marker 必须可读且完成封尾");
    std::size_t file_count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        check(entry.is_regular_file() &&
                  std::find(expected.begin(), expected.end(), entry.path()) != expected.end(),
              "中文归档仅可保留正式文件，不得遗留 tmp、rollback 或锁定 marker");
        ++file_count;
    }
    check(file_count == expected.size(), "中文归档正式文件集合必须完整");
    std::size_t directory_count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        check(entry.path() == directory && entry.is_directory(),
              "native 到 UTF-8 桥接不得创建乱码兄弟目录或旁路报告");
        ++directory_count;
    }
    check(directory_count == 1, "中文归档父目录只能包含预期目录");
}
}
void failed_snapshot_only_test(const std::filesystem::path& root) {
    for (const bool with_sample : {false, true}) {
        SessionArchiveConfig config;
        config.directory = (root / (with_sample ? "failed-after-flush" : "failed-before-frame")).string();
        config.segment_samples = 1;
        config.segment_interval = std::chrono::milliseconds(20);
        config.report_config.session_id = with_sample ? "failed-after-flush" : "failed-before-frame";
        SessionArchive archive;
        std::string error;
        check(archive.start(config, error), "启动空终态归档回归");
        RuntimeSnapshot snapshot;
        snapshot.state = RuntimeState::RUNNING;
        if (with_sample) {
            RuntimePipelineSample sample;
            sample.sequence = 1;
            send(archive, std::span(&sample, 1), snapshot);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (archive.status().written_segments != 1 && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            check(archive.status().written_segments == 1, "故障前已真实发布完整分段");
        }
        snapshot.state = RuntimeState::FAILED;
        snapshot.last_error = "确定性采集故障";
        send(archive, {}, snapshot);
        const auto terminal_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (archive.status().written_segments != (with_sample ? 2 : 1) &&
                   std::chrono::steady_clock::now() < terminal_deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        check(archive.status().written_segments == (with_sample ? 2 : 1), "故障终态已真实发布");
        send(archive, {}, snapshot);
        archive.stop();
        const auto state = archive.status();
        check(state.written_segments == (with_sample ? 2 : 1), "无帧无事件的FAILED快照必须持久化");
        const auto base = std::filesystem::path(config.directory) / ("segment-" + std::to_string(state.written_segments));
        const auto report = read(base.string() + ".json");
        check(report["final_snapshot"]["runtime_state"] == "FAILED" &&
                  report["final_snapshot"]["last_error"] == snapshot.last_error,
              "封尾报告必须保留实际故障终态和错误");
        check(read(base.string() + ".meta.json")["sample_count"] == 0 &&
                  state.written_samples == (with_sample ? 1 : 0) && state.dropped_samples == 0,
              "终态证据不得伪造帧或重复旧样本");
    }
}
void changed_failed_snapshot_test(const std::filesystem::path& root) {
    SessionArchiveConfig config;
    config.directory = (root / "failed-error-change").string();
    config.segment_interval = std::chrono::milliseconds(20);
    SessionArchive archive;
    std::string error;
    check(archive.start(config, error), "启动故障终态变更回归");
    RuntimeSnapshot snapshot;
    snapshot.state = RuntimeState::FAILED;
    snapshot.last_error = "首次故障";
    send(archive, {}, snapshot);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (archive.status().written_segments != 1 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    check(archive.status().written_segments == 1, "首次故障终态已发布");
    snapshot.last_error = "后续故障原因";
    send(archive, {}, snapshot);
    archive.stop();
    check(archive.status().written_segments == 2 &&
              read(std::filesystem::path(config.directory) / "segment-2.json")["final_snapshot"]["last_error"] == snapshot.last_error,
          "故障终态相关字段变化仍须发布新证据");
}
int main(int argc, char** argv) {
    try {
        if (argc==3 && std::string(argv[1])=="--benchmark") return benchmark(argv[2]);
        if (argc==3 && std::string(argv[1])=="--queue-repro") return benchmark(argv[2],true);
        if (argc==3 && std::string(argv[1])=="--queue-pressure") return queue_pressure(argv[2]);
        const auto root=std::filesystem::temp_directory_path()/
            ("xen-session-archive-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        failed_snapshot_only_test(root);
        changed_failed_snapshot_test(root);
        SessionArchiveConfig config; config.directory=(root/"first").string();
        config.segment_samples=2; config.report_config.session_id="test-first";
        config.segment_interval=std::chrono::milliseconds(20);
        SessionArchive archive; std::string error;
        check(archive.start(config,error),"开始首会话");
        RuntimeSnapshot snapshot; snapshot.trigger_telemetry_available=true;
        TriggerExecutionEvent event; event.sequence=1; snapshot.trigger_execution_log.events.push_back(event);
        std::vector<RuntimePipelineSample> samples(5);
        for (std::size_t i=0;i<samples.size();++i) {
            samples[i].sequence=i+1; samples[i].frame_timing.control_steady_ns=
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }
        send(archive,samples,snapshot);
        send(archive,{},snapshot);
        bool marked=false;
        for (int i=0;i<100&&!marked;++i) { marked=archive.mark("贴脸异常"); if(!marked) std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        check(marked,"接受异常标记"); archive.stop();
        auto state=archive.status();
        check(!state.active && state.written_samples==5 && state.written_segments==3,"多段及停止尾段完整");
        auto manifest=read(root/"first"/"manifest.json");
        check(manifest["closed"]==true && manifest["written_samples"]==5,"封口清单");
        std::size_t frame_count=0,event_count=0;
        for (int i=1;i<=3;++i) {
            auto base=root/"first"/("segment-"+std::to_string(i));
            auto meta=read(base.string()+".meta.json"); frame_count+=meta["sample_count"].get<std::size_t>();
            event_count+=meta["trigger_event_count"].get<std::size_t>();
            auto report=read(base.string()+".json");
            check(report["samples_omitted"]==true && report["samples"].empty(),"JSON不重复帧，CSV保留完整帧");
            check(std::filesystem::file_size(base.string()+".csv")>0,"CSV已发布");
            check(!std::filesystem::exists(base.string()+".json.aim-lock-active"),"后台归档不污染锁定marker");
        }
        check(frame_count==5 && event_count==1,"跨段样本与Trigger事件不重复");
        auto marker=read(root/"first"/"marker-1.json");
        check(marker["pre_truncated"]==true && marker["post_truncated"]==true && marker["closed"]==true,"短会话窗口明确截短");
        const auto mark_ns=std::stoll(marker["steady_ns"].get<std::string>());
        check(std::stoll(marker["window_end_steady_ns"].get<std::string>())-mark_ns==30000000000LL &&
              mark_ns-std::stoll(marker["window_start_steady_ns"].get<std::string>())==30000000000LL &&
              std::stoll(marker["system_unix_ns"].get<std::string>())>0,"双时钟与窗口单位");
        check(!archive.start(config,error),"禁止覆盖旧会话");
        config.directory=(root/"second").string(); config.report_config.session_id="test-second";
        config.trigger_sequence_baseline=100; snapshot.trigger_execution_log.events.front().sequence=101;
        check(archive.start(config,error),"重启独立会话"); send(archive,std::span(samples).first(1),snapshot); archive.stop();
        check(archive.status().written_samples==1 && read(root/"first"/"manifest.json")["written_samples"]==5,"重启无串会话");
        check(archive.status().trigger_events_dropped==0,"模型分段游标不误报历史缺口");
        archive.stop();
        config.directory=(root/"failure").string();
        check(archive.start(config,error),"开始磁盘错误测试");
        std::filesystem::create_directory(root/"failure"/"segment-1.csv");
        send(archive,std::span(samples).first(1),snapshot); archive.stop();
        check(!archive.status().last_error.empty() && archive.status().written_samples==0 && archive.status().dropped_samples>=1,"落盘错误可见且不冒充保存成功");
        check(read(root/"failure"/"manifest.json")["complete"]==false,"错误清单不完整");
        config.directory=(root/"upstream-gap").string();
        check(archive.start(config,error),"开始上游缺口测试");
        archive.note_gap("上游样本读取失败"); archive.stop();
        check(read(root/"upstream-gap"/"manifest.json")["complete"]==false &&
              !archive.status().last_error.empty(),"上游缺口持久化而非仅标记");
        config.directory=(root/"bounded").string(); config.queue_capacity=1; config.segment_samples=1000;
        check(archive.start(config,error),"开始有界队列测试");
        std::vector<RuntimePipelineSample> large(1000);
        for(int i=0;i<200;++i) archive.submit(large,snapshot);
        archive.stop();
        check(archive.status().dropped_batches>0 && archive.status().dropped_samples>0,"队列饱和明确计数");
        check(read(root/"bounded"/"manifest.json")["complete"]==false,"队列丢弃不冒充完整");
        queue_pressure(root/"pressure");
        queue_contracts(root/"marker-contract",false);
        queue_contracts(root/"capacity-contract",true);
        native_chinese_report_paths(root/"native-encoding-contract");
        std::cout<<"SessionArchive 测试通过\n";
        return 0;
    } catch(const std::exception& ex) { std::cerr<<ex.what()<<'\n'; return 1; }
}
