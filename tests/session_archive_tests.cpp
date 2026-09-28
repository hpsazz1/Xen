#include "debug/session_archive.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <stdexcept>
#include <algorithm>
#include <array>

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
int benchmark(const std::filesystem::path& root) {
    using Clock=std::chrono::steady_clock;
    SessionArchiveConfig config; config.directory=(root/"archive").string();
    config.report_config.session_id="offline-240fps-45s";
    SessionArchive archive; std::string error;
    check(archive.start(config,error),"吞吐实验启动");
    RuntimeSnapshot snapshot; snapshot.trigger_telemetry_available=true;
    snapshot.state=RuntimeState::RUNNING;
    std::vector<double> latency; latency.reserve(2700);
    std::uint64_t trigger_sequence=0;
    const auto started=Clock::now();
    for (int batch=0;batch<2700;++batch) {
        std::this_thread::sleep_until(started+std::chrono::nanoseconds(static_cast<std::int64_t>(batch)*1000000000LL/60));
        std::array<RuntimePipelineSample,4> samples;
        for (std::size_t i=0;i<samples.size();++i) {
            auto& sample=samples[i]; sample.sequence=static_cast<std::uint64_t>(batch)*4+i+1;
            sample.frame_timing.control_steady_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
            sample.profile.total_ms=2.5; sample.aim_control_center_x=320; sample.aim_control_center_y=320;
            sample.person_detection_count=2; sample.max_person_confidence=0.85f;
        }
        snapshot.trigger_execution_log.events.clear();
        if (batch%6==0) {
            TriggerExecutionEvent event; event.sequence=++trigger_sequence; event.observed_at=Clock::now();
            snapshot.trigger_execution_log.events.push_back(event);
        }
        const auto before=Clock::now();
        archive.submit(samples,snapshot);
        latency.push_back(std::chrono::duration<double,std::micro>(Clock::now()-before).count());
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
        {"kind","synthetic_numeric_archive_only"}, {"equivalent_game_seconds",45},
        {"actual_elapsed_seconds",elapsed}, {"submitted_samples",10800},
        {"written_samples",state.written_samples}, {"dropped_samples",state.dropped_samples},
        {"dropped_batches",state.dropped_batches}, {"trigger_events_dropped",state.trigger_events_dropped},
        {"written_segments",state.written_segments}, {"last_error",state.last_error},
        {"submit_p50_us",percentile(0.50)}, {"submit_p95_us",percentile(0.95)},
        {"submit_p99_us",percentile(0.99)}, {"submit_max_us",latency.back()},
        {"stop_drain_ms",stop_ms}, {"files_bytes",bytes}, {"sample_size_bytes",sizeof(RuntimePipelineSample)},
        {"physical_effect_verified",false}};
    std::ofstream output(root/"benchmark.json"); output<<result.dump(2); output.close();
    check(static_cast<bool>(output),"吞吐实验结果写入");
    std::cout<<result.dump(2)<<'\n';
    check(state.written_samples+state.dropped_samples==10800,"归档帧数守恒");
    return 0;
}
}
int main(int argc, char** argv) {
    try {
        if (argc==3 && std::string(argv[1])=="--benchmark") return benchmark(argv[2]);
        const auto root=std::filesystem::temp_directory_path()/
            ("xen-session-archive-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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
        std::cout<<"SessionArchive 测试通过\n";
        return 0;
    } catch(const std::exception& ex) { std::cerr<<ex.what()<<'\n'; return 1; }
}
