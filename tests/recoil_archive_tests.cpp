#include "recoil/recoil_archive.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>

namespace {
using Json=nlohmann::json;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
RecoilTime time(int ms){return RecoilTime(std::chrono::milliseconds(ms));}
std::shared_ptr<RecoilProfile> profile() {
    auto p=std::make_shared<RecoilProfile>();p->id="synthetic";p->weapon_id="test";
    p->state=RecoilProfileState::CALIBRATED;p->phase_tolerance_ms=20;p->recovery_ms=10;
    p->source.sha256=std::string(64,'a');p->source.source_unit="synthetic";
    p->calibration={"test","fake","test","synthetic:test_only",1.0};
    p->points={{0,0,0},{4000,0,4000}};return p;
}
std::vector<RecoilExecutionEvent> batch(std::uint64_t firing,std::uint64_t first,std::size_t count,
    RecoilBatchEndReason reason=RecoilBatchEndReason::EXHAUSTED) {
    RecoilExecutionEvent begin;begin.sequence=first;begin.firing_id=firing;begin.profile=profile();
    begin.event_at=begin.firing_started_at=time(100);begin.firing_source=RecoilFiringSource::INPUT_ESTIMATED;
    begin.weapon_generation=begin.device_epoch=1;
    std::vector<RecoilExecutionEvent> result{begin};
    for(std::size_t i=0;i<count;++i) {
        auto event=begin;event.kind=RecoilExecutionEventKind::COMMAND;event.sequence=first+i+1;
        event.event_at=time(101+static_cast<int>(i));
        auto& record=event.command;record.profile=begin.profile;record.backend_called=true;
        record.firing_started_at=begin.firing_started_at;record.firing_source=begin.firing_source;
        record.intent={i+1,firing,begin.profile->revision,1,1,event.event_at,event.event_at+std::chrono::milliseconds(20),0,1};
        record.receipt={i+1,RecoilReceiptStatus::ACKNOWLEDGED,event.event_at};result.push_back(event);
    }
    auto end=begin;end.kind=RecoilExecutionEventKind::END;end.sequence=first+count+1;
    end.event_at=time(5000);end.end_reason=reason;result.push_back(end);return result;
}
struct Fixture {
    std::filesystem::path root;
    std::vector<Json> written;
    std::mutex mutex;
    RecoilBatchArchive archive;
    Fixture(){root=std::filesystem::temp_directory_path()/("xen-recoil-archive-test-"+
        std::to_string(RecoilClock::now().time_since_epoch().count()));}
    ~Fixture(){archive.stop();std::error_code error;std::filesystem::remove_all(root,error);}
    RecoilArchiveConfig config(){RecoilArchiveConfig c;c.directory=root/"archive";c.acquisition_run_id="one-real-acquisition";
        c.recoil.enabled=true;return c;}
    bool start(const std::vector<RecoilExecutionEvent>& events,RecoilArchiveConfig config,bool fail=false) {
        return archive.start(config,[events](std::uint64_t after,std::size_t maximum){
            RecoilEventSlice slice;slice.oldest_available_sequence=events.empty()?0:events.front().sequence;
            slice.latest_sequence=events.empty()?0:events.back().sequence;
            slice.cursor_gap=slice.oldest_available_sequence&&after<slice.oldest_available_sequence-1;
            for(const auto& event:events)if(event.sequence>after&&slice.events.size()<maximum)slice.events.push_back(event);
            return slice;
        },[this,fail](const auto&,const std::string& value,std::string& error){
            if(fail){error="synthetic disk failure";return false;}
            std::lock_guard lock(mutex);written.push_back(Json::parse(value));return true;
        });
    }
};
}
int main(int argc,char** argv) {
    try {
        if(argc!=1) {
            check(argc==3&&std::string(argv[1])=="--export-test-batch","只支持--export-test-batch NEW_DIR");
            RecoilArchiveConfig config;config.directory=std::filesystem::path(argv[2]);
            config.acquisition_run_id="synthetic-cpp-archive-test-only";config.recoil.enabled=true;
            config.recoil.game_build="test";config.recoil.input_path="fake";
            config.recoil.conditions="test";config.recoil.sensitivity=1.0;
            const auto events=batch(1,1,4000);
            RecoilBatchArchive archive;
            check(archive.start(config,[events](std::uint64_t after,std::size_t maximum){
                RecoilEventSlice slice;slice.oldest_available_sequence=1;slice.latest_sequence=events.back().sequence;
                for(const auto& event:events)if(event.sequence>after&&slice.events.size()<maximum)slice.events.push_back(event);
                return slice;
            }),"生产disk_sink导出需唯一新目录");
            archive.stop();const auto status=archive.snapshot();
            check(status.available&&status.files_written==1&&status.complete_batches==1&&status.last_sequence==4002,
                "生产disk_sink必须读尽事件并写完整文件");
            std::cout<<"synthetic batch exported; no device used\n";return 0;
        }
        {
            Fixture f;RecoilBatchArchive archive;const auto events=batch(1,1,2);
            check(archive.start(f.config(),[events](std::uint64_t after,std::size_t){
                RecoilEventSlice slice;slice.oldest_available_sequence=1;slice.latest_sequence=events.back().sequence;
                for(const auto& event:events)if(event.sequence>after)slice.events.push_back(event);return slice;
            }),"默认生产disk_sink启动");
            archive.stop();
            const auto file=f.root/"archive"/"batch-1-1.json";
            std::ifstream stream(file,std::ios::binary);Json value;stream>>value;
            check(stream.good()||stream.eof(),"已发布批次可读取");
            check(value["recoil"]["execution"]["batch"]["coverage_complete"]==true&&
                !std::filesystem::exists(std::filesystem::path(file.string()+".tmp")),
                "生产sink原子发布完成JSON且不留下临时文件");
        }
        {
            Fixture f;check(f.start(batch(1,1,3000),f.config()),"归档启动");f.archive.stop();
            check(f.written.size()==1&&f.archive.snapshot().complete_batches==1,"超过2048命令的单批完整归档");
            const auto& root=f.written[0];const auto& execution=root["recoil"]["execution"];
            check(root["session_id"]==root["acquisition_run_id"]&&execution["records"].size()==3000&&
                execution["batch"]["end_sequence"]==3002&&execution["batch"]["coverage_complete"]==true,
                "归档保持真实Run身份与连续事件覆盖");
        }
        {
            Fixture f;auto events=batch(1,1,3);events.erase(events.begin()+2);
            const auto next=batch(2,6,1);events.insert(events.end(),next.begin(),next.end());
            check(f.start(events,f.config()),"缺口测试启动");f.archive.stop();
            check(f.written.size()==2&&f.written[0]["recoil"]["execution"]["batch"]["coverage_complete"]==false&&
                f.written[1]["recoil"]["execution"]["batch"]["coverage_complete"]==true,
                "缺口只污染交叠批次，新BEGIN可恢复完整归档");
            check(f.written[0]["session_id"]==f.written[1]["session_id"],"分批不伪造独立Run");
        }
        {
            Fixture f;auto c=f.config();c.max_records=2;
            check(f.start(batch(1,1,3),c),"资源上限测试启动");f.archive.stop();
            check(f.archive.snapshot().incomplete_batches==1&&f.written[0]["recoil"]["execution"]["records"].size()==2,
                "单批超过上限不能无限保留或标完整");
        }
        {
            Fixture f;auto events=batch(1,1,1);events.pop_back();
            check(f.start(events,f.config()),"缺END测试启动");f.archive.stop();
            check(f.written[0]["recoil"]["execution"]["batch"]["end_reason"]=="MISSING_END"&&
                f.archive.snapshot().incomplete_batches==1,"关闭不能给缺END证据补自然耗尽");
        }
        {
            Fixture f;check(f.start(batch(1,1,0,RecoilBatchEndReason::CANCELED),f.config()),"零命令测试启动");f.archive.stop();
            check(f.archive.snapshot().complete_batches==1&&f.written[0]["recoil"]["execution"]["records"].empty()&&
                f.written[0]["recoil"]["execution"]["batch"]["end_reason"]=="CANCELED",
                "零命令可有完整生命周期但不伪造ACK或自然耗尽");
        }
        {
            Fixture f;check(f.start(batch(1,1,1),f.config(),true),"写失败测试启动");f.archive.stop();
            check(!f.archive.snapshot().available&&f.archive.snapshot().files_written==0&&!f.archive.snapshot().error.empty(),
                "磁盘错误必须公开且不计成功");
        }
        {
            Fixture f;auto c=f.config();c.max_files=1;auto events=batch(1,1,1);
            const auto next=batch(2,4,1);events.insert(events.end(),next.begin(),next.end());
            check(f.start(events,c),"总上限测试启动");f.archive.stop();
            check(!f.archive.snapshot().available&&f.archive.snapshot().files_written==1,"总文件上限不删除旧证据");
        }
        std::cout<<"recoil_archive_tests passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
