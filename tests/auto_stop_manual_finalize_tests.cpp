#include "auto_stop_probe/manual_recording_internal.h"
#include <iostream>

namespace {
using namespace auto_stop_probe_detail;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
input_training::Event event(std::uint64_t seq, std::int64_t ms, bool left=false) {
    input_training::Event value;
    value.epoch=1; value.sequence=seq; value.received_at_ns=ms*1000000;
    value.state_valid=true; value.left_down=left; return value;
}
void write_archive(const std::filesystem::path& path, const std::vector<input_training::Event>& events,
    input_training::Limits limits={}) {
    auto items=std::make_shared<std::vector<input_training::Event>>(events);
    input_training::Session session;
    require(session.start(path,limits,[items] {
        input_training::ReadBatch batch; batch.events.swap(*items); return batch;
    }),"归档会话启动失败");
    session.stop();
    const auto state=session.snapshot()->status;
    require(state==input_training::Status::STOPPED || state==input_training::Status::LIMIT,"软件档案未完成");
}
void test(const std::filesystem::path& root) {
    const auto short_path=root/"short";
    write_archive(short_path,{event(1,0),event(2,100,true),event(3,105)});
    auto stopped=finalize_manual_archive(short_path,{},110000000);
    require(stopped["time_ns"]==110000000 && stopped["total_sample_count"]==0,
        "停止早于首样本到期，排盘耗时不能让样本凭空出现");
    auto later=finalize_manual_archive(short_path,{},120000000);
    require(later["total_sample_count"]==1,"测试水位必须能区分错误使用关闭后时间");
    bool rejected=false;
    try { (void)finalize_manual_archive(short_path,{},104000000); } catch(const std::exception&) { rejected=true; }
    require(rejected,"水位早于原始尾事件必须拒绝");
    const std::vector<input_training::Event> full{event(1,0),event(2,10,true),event(3,20),event(4,30,true),event(5,40)};
    auto limits=input_training::Limits{}; limits.max_run_events=3;
    write_archive(root/"limited",full,limits);
    ManualSamplingAccumulator premature;
    for(const auto& value:full) premature.consume(value);
    require(premature.snapshot(50000000,false)["shot_count"]==2,"提前分析确实包含未落盘第二hold");
    auto final=finalize_manual_archive(root/"limited",{},50000000);
    require(final["received_events"]==3 && final["shot_count"]==1 && final["shots"].size()==1 &&
        final["archive_status"]=="达到预算" && final["finalized_from_persisted_raw"]==true,
        "最终报告只能含清单内原始事件，不能夹带Reader超预算尾部");
    rejected=false;
    try { (void)finalize_manual_archive(root/"missing",{},50000000); } catch(const std::exception&) { rejected=true; }
    require(rejected,"缺少完整manifest不得生成最终报告");
}
}
int main() {
    const auto root=std::filesystem::temp_directory_path()/
        ("xen-manual-finalize-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    bool owned=false;
    try {
        owned=std::filesystem::create_directory(root);
        require(owned,"测试目录必须新建");
        test(root);
        std::filesystem::remove_all(root);
        std::cout<<"人工最终报告原始水位专项通过\n"; return 0;
    } catch(const std::exception& e) {
        std::cerr<<e.what()<<'\n';
        if(owned) { std::error_code ignored; std::filesystem::remove_all(root,ignored); }
        return 1;
    }
}
