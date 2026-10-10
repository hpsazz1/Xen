#include "recoil/recoil_archive.h"
#include "recoil_tuner/recoil_tuner.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <thread>

namespace {
using Json = nlohmann::json;
Json stamp(RecoilTime value) {
    return value == RecoilTime{} ? Json(nullptr) : Json(std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count());
}
Json command_json(const RecoilExecutionEvent& event) {
    const auto& r=event.command; const auto& i=r.intent;
    return {{"event_sequence",event.sequence},{"command_id",i.command_id},{"firing_id",i.session_id},
        {"profile",r.profile ? Json(r.profile->id+":"+std::to_string(r.profile->revision)) : Json(nullptr)},
        {"weapon_generation",i.weapon_generation},{"device_epoch",i.device_epoch},
        {"planned_at_steady_ns",stamp(i.planned_at)},{"firing_started_at_steady_ns",stamp(r.firing_started_at)},
        {"firing_source",RecoilFiringSourceName(r.firing_source)},
        {"source_firing_id",r.source_firing_id ? Json(std::to_string(r.source_firing_id)) : Json(nullptr)},
        {"firing_command_interval_ns",r.firing_uncertainty_ns ? Json(std::to_string(*r.firing_uncertainty_ns)) : Json(nullptr)},
        {"expires_at_steady_ns",stamp(i.expires_at)},{"requested_counts",{i.dx_counts,i.dy_counts}},
        {"backend_called",r.backend_called},{"dispatch_rejection",RecoilDispatchRejectionName(r.dispatch_rejection)},
        {"sampled_at_steady_ns",stamp(r.sampled_at)},{"arbitration_at_steady_ns",stamp(r.arbitration_at)},
        {"context_checked_at_steady_ns",stamp(r.context_checked_at)},{"backend_called_at_steady_ns",stamp(r.backend_called_at)},
        {"backend_returned_at_steady_ns",stamp(r.backend_returned_at)},
        {"receipt",r.receipt.status==RecoilReceiptStatus::ACKNOWLEDGED ? "ACKNOWLEDGED" :
            r.receipt.status==RecoilReceiptStatus::NOT_SENT ? "NOT_SENT" : "UNKNOWN"},
        {"completed_at_steady_ns",stamp(r.receipt.completed_at)},{"physical_effect_observed",nullptr}};
}
bool disk_sink(const std::filesystem::path& path,const std::string& data,std::string& error) {
    try {
        auto temporary=path;temporary+=".tmp";
        if(std::filesystem::exists(path)||std::filesystem::exists(temporary))throw std::runtime_error("批次文件已存在");
        std::ofstream stream(temporary,std::ios::binary|std::ios::out);
        stream.write(data.data(),static_cast<std::streamsize>(data.size()));stream.flush();
        if(!stream)throw std::runtime_error("批次文件写入失败");
        stream.close();if(!stream)throw std::runtime_error("批次文件关闭失败");
        std::filesystem::rename(temporary,path);return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
}

class RecoilBatchArchive::Impl {
public:
    RecoilArchiveConfig config;
    Reader reader;Sink sink;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::thread thread;
    std::atomic<bool> stopping{false};
    std::uint64_t stop_after = 0; // mutex 保护关闭边界的读取与发布
    RecoilArchiveStatus state;
    std::optional<RecoilExecutionEvent> begin;
    Json records=Json::array();
    std::size_t record_bytes=0;
    bool complete=true;
    std::string incomplete_reason;
    Json missing_ranges=Json::array();
    std::uint64_t cursor=0;
    void fail(const std::string& message) {
        std::lock_guard lock(mutex);state.available=false;state.error=message;
    }
    void damage(const char* reason) {
        complete=false;if(incomplete_reason.empty())incomplete_reason=reason;
    }
    void finish(const RecoilExecutionEvent* end,const char* missing_reason=nullptr) {
        if(!begin)return;
        if(missing_reason)damage(missing_reason);
        if(!end||end->sequence<=begin->sequence||end->sequence-begin->sequence-1!=records.size())damage("EVENT_GAP");
        Json profiles=Json::array();
        if(begin->profile) {
            auto profile=Json::parse(serialize_recoil_profile(*begin->profile));
            profile["environment_fingerprint"]=recoil_tuner::environment_fingerprint(*begin->profile);
            profiles.push_back(std::move(profile));
        } else damage("PROFILE_MISSING");
        const auto profile_key=begin->profile ? Json(begin->profile->id+":"+std::to_string(begin->profile->revision)) : Json(nullptr);
        Json batch={{"firing_id",begin->firing_id},{"begin_sequence",begin->sequence},
            {"end_sequence",end ? Json(end->sequence) : Json(nullptr)},{"coverage_complete",complete},
            {"end_reason",end ? RecoilBatchEndReasonName(end->end_reason) : "MISSING_END"},
            {"begin_at_steady_ns",stamp(begin->event_at)},{"end_at_steady_ns",end ? stamp(end->event_at) : Json(nullptr)},
            {"firing_started_at_steady_ns",stamp(begin->firing_started_at)},
            {"firing_source",RecoilFiringSourceName(begin->firing_source)},
            {"profile",profile_key},{"weapon_generation",begin->weapon_generation},{"device_epoch",begin->device_epoch},
            {"records_count",records.size()},{"incomplete_reason",incomplete_reason},{"missing_sequence_ranges",missing_ranges}};
        Json root={{"session_id",config.acquisition_run_id},{"acquisition_run_id",config.acquisition_run_id},
            {"recoil",{{"schema",2},{"physical_acceptance",nullptr},
            {"config",{{"enabled",config.recoil.enabled},{"mixed_aim",config.recoil.mixed_aim},
                {"hold_virtual_key",config.recoil.hold_virtual_key},{"game_build",config.recoil.game_build},
                {"conditions",config.recoil.conditions},{"input_path",config.recoil.input_path},
                {"sensitivity",config.recoil.sensitivity},{"fire_mode",config.recoil.fire_mode},
                {"budget_window_ms",config.recoil.budget_window_ms},{"max_observation_age_ms",config.recoil.max_observation_age_ms}}},
            {"execution",{{"clock_domain","local_steady"},{"batch",std::move(batch)},
                {"records",std::move(records)},{"profiles",std::move(profiles)}}}}}};
        auto data=root.dump();
        if(data.size()>config.max_batch_bytes) {
            damage("SIZE_LIMIT_INCOMPLETE");
            auto& execution=root["recoil"]["execution"];
            execution["records"]=Json::array();execution["profiles"]=Json::array();
            execution["batch"]["coverage_complete"]=false;
            execution["batch"]["incomplete_reason"]="SIZE_LIMIT_INCOMPLETE";
            execution["batch"]["records_count"]=0;
            data=root.dump();
        }
        RecoilArchiveStatus current;
        {std::lock_guard lock(mutex);current=state;}
        if(current.available) {
            if(data.size()>config.max_batch_bytes||current.files_written>=config.max_files||
                data.size()>config.max_total_bytes-current.total_bytes)fail("归档资源上限已到达");
            else {
                std::string error;
                const auto path=config.directory/("batch-"+std::to_string(begin->firing_id)+"-"+std::to_string(begin->sequence)+".json");
                if(!sink(path,data,error))fail("批次归档失败："+error);
                else {std::lock_guard lock(mutex);++state.files_written;state.total_bytes+=data.size();
                    if(complete)++state.complete_batches;else ++state.incomplete_batches;}
            }
        }
        begin.reset();records=Json::array();record_bytes=0;complete=true;incomplete_reason.clear();missing_ranges=Json::array();
    }
    void consume(const RecoilExecutionEvent& event) {
        if(event.sequence<=cursor)return;
        if(event.sequence!=cursor+1&&begin) {
            damage("EVENT_GAP");
            if(missing_ranges.size()<128)missing_ranges.push_back({cursor+1,event.sequence-1});
        }
        cursor=event.sequence;
        if(event.kind==RecoilExecutionEventKind::BEGIN) {
            if(begin)finish(nullptr,"MISSING_END");
            begin=event;complete=true;incomplete_reason.clear();
            if(!event.firing_id||!event.sequence||event.event_at==RecoilTime{}||
                event.firing_started_at==RecoilTime{}||!event.profile)damage("INVALID_BEGIN");
            return;
        }
        if(!begin) {
            begin=event;begin->sequence=0;damage("MISSING_BEGIN");
        }
        if(event.firing_id!=begin->firing_id||event.profile!=begin->profile||
            event.weapon_generation!=begin->weapon_generation||event.device_epoch!=begin->device_epoch||
            event.firing_started_at!=begin->firing_started_at||event.firing_source!=begin->firing_source||
            event.event_at<begin->event_at)damage("IDENTITY_MISMATCH");
        if(event.kind==RecoilExecutionEventKind::COMMAND) {
            const auto& command=event.command;
            if(command.intent.session_id!=begin->firing_id||command.profile!=begin->profile||
                command.intent.weapon_generation!=begin->weapon_generation||command.intent.device_epoch!=begin->device_epoch||
                command.firing_started_at!=begin->firing_started_at||command.firing_source!=begin->firing_source)
                damage("COMMAND_IDENTITY_MISMATCH");
            auto value=command_json(event);const auto bytes=value.dump().size();
            if(records.size()>=config.max_records||bytes>config.max_batch_bytes-record_bytes)damage("SIZE_LIMIT_INCOMPLETE");
            else {record_bytes+=bytes;records.push_back(std::move(value));}
        } else finish(&event);
    }
    void run() noexcept {
        try {
            std::optional<std::uint64_t> final_sequence;
            for(;;) {
                const bool boundary_known_before_read = final_sequence.has_value();
                const auto slice=reader(cursor,256);
                {
                    // request_stop 在同一锁内取水位并发布停止；不能越过两者之间的窗口。
                    std::lock_guard lock(mutex);
                    if (stopping.load(std::memory_order_acquire) && !final_sequence) final_sequence = stop_after;
                }
                if(slice.sequence_exhausted)fail("事件序号已耗尽，完整性不可继续承诺");
                if((slice.cursor_gap||slice.sequence_exhausted)&&begin)damage("EVENT_GAP");
                const auto before=cursor;
                for(const auto& event:slice.events)
                    if (!final_sequence || event.sequence <= *final_sequence) consume(event);
                {std::lock_guard lock(mutex);state.last_sequence=cursor;}
                if(final_sequence) {
                    if(cursor>=*final_sequence)break;
                    // 首次看见停止水位时，slice 可能早于关闭前的最后事件。
                    // 保留已读事件并重新读取；确认水位后的无推进才是真实缺口。
                    if(cursor==before && boundary_known_before_read) {
                        fail("停止归档时事件尾部无法读取");
                        break;
                    }
                    continue;
                }
                if(cursor<slice.latest_sequence&&cursor!=before)continue;
                std::unique_lock lock(mutex);wake.wait_for(lock,std::chrono::milliseconds(20),[this]{return stopping.load();});
            }
            if(begin)finish(nullptr,"RECORDING_STOPPED_WITHOUT_END");
        }catch(const std::exception& e){fail(e.what());}catch(...){fail("归档线程异常");}
        std::lock_guard lock(mutex);state.running=false;
    }
};

RecoilBatchArchive::RecoilBatchArchive():impl_(std::make_unique<Impl>()){}
RecoilBatchArchive::~RecoilBatchArchive(){stop();}
bool RecoilBatchArchive::start(const RecoilArchiveConfig& config,Reader reader,Sink sink) noexcept {
    try {
        if(impl_->thread.joinable()||!reader||config.acquisition_run_id.empty()||config.acquisition_run_id.size()>256||
            config.directory.empty()||config.max_records==0||config.max_records>32768||
            config.max_batch_bytes<4096||config.max_batch_bytes>16*1024*1024||
            config.max_total_bytes<config.max_batch_bytes||config.max_files==0)return false;
        std::filesystem::create_directories(config.directory.parent_path());
        if(!std::filesystem::create_directory(config.directory))throw std::runtime_error("归档需要唯一新目录");
        impl_->config=config;impl_->reader=std::move(reader);impl_->sink=sink ? std::move(sink) : disk_sink;
        impl_->stopping=false;impl_->cursor=config.after_sequence;
        // 状态进入报告 JSON 和界面文本，不能把 Windows 原生路径降为 ACP 字节。
        const auto encoded_directory = config.directory.u8string();
        const std::string directory(encoded_directory.begin(), encoded_directory.end());
        {std::lock_guard lock(impl_->mutex);impl_->state={true,true,config.acquisition_run_id,directory,{}};}
        impl_->thread=std::thread([this]{impl_->run();});return true;
    }catch(const std::exception& e){impl_->fail(e.what());return false;}catch(...){return false;}
}
void RecoilBatchArchive::request_stop() noexcept {
    try {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->stopping.load() && impl_->reader) {
            impl_->stop_after = impl_->reader(0, 0).latest_sequence;
            impl_->stopping.store(true, std::memory_order_release);
        }
        impl_->wake.notify_one();
    } catch (...) {
        // 读取边界失败时只保留已接受事件，明确报告完整性未知。
        std::lock_guard lock(impl_->mutex);
        impl_->stop_after = impl_->state.last_sequence;
        impl_->state.available = false;
        impl_->state.error = "停止归档时无法取得事件边界";
        impl_->stopping.store(true, std::memory_order_release);
        impl_->wake.notify_one();
    }
}
void RecoilBatchArchive::stop() noexcept {
    request_stop();if(impl_->thread.joinable())impl_->thread.join();
}
RecoilArchiveStatus RecoilBatchArchive::snapshot() const {std::lock_guard lock(impl_->mutex);return impl_->state;}
