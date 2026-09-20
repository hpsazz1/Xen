#include "recoil_tuner/target_capture_run.h"
#include "recoil/recoil_calibration.h"
#include <opencv2/imgproc.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
namespace {
using Json=nlohmann::json;using namespace recoil_tuner;int failed=0;
void check(bool ok,const char* text){if(!ok){++failed;std::cerr<<text<<'\n';}}
class FakeMouse final:public IMouseController {
public:
    bool open() noexcept override{return true;}bool output_owner_exclusive() const noexcept override{return true;}
    bool supports_left_button() const noexcept override{return true;}
    bool left_button_cleanup_required() const noexcept override{return down;}
    MouseMoveReceipt move(const MouseMoveCommand& c) noexcept override{++moves;x+=c.dx_counts;y+=c.dy_counts;MouseMoveReceipt r;r.succeeded=!unknown;r.backend_completed_at=RecoilClock::now();return r;}
    bool poll_input(InputSnapshot& s) noexcept override{s.state_valid=true;s.status=InputMonitorStatus::READY;s.sequence=++sequence;s.virtual_keys[5]=held;return true;}
    ButtonReceipt set_left_button(bool value) noexcept override{down=value;if(value)++downs;else ++ups;ButtonReceipt r;r.disposition=unknown_down&&value?ButtonDisposition::APPLICATION_UNKNOWN:ButtonDisposition::ACKNOWLEDGED;r.backend_completed_at=RecoilClock::now();r.cleanup_required=value;return r;}
    void close() noexcept override{}MouseStatus status() const noexcept override{return MouseStatus::READY;}std::string last_error() const override{return {};}
    bool down=false,held=true,unknown=false,unknown_down=false;int moves=0,downs=0,ups=0,x=0,y=0;std::uint64_t sequence=0;
};
class FakeCapture final:public ICapture {
public:
    FakeCapture(std::shared_ptr<FakeMouse> m,bool lose=false):mouse(std::move(m)),lose_(lose){image=cv::Mat(192,192,CV_8UC3);cv::RNG random(4321);random.fill(image,cv::RNG::UNIFORM,20,220);}
    bool open() noexcept override{return true;}
    CaptureStatus grab(CapturedFrame& f) noexcept override{std::this_thread::sleep_for(std::chrono::milliseconds(5));
        cv::Mat transform=(cv::Mat_<double>(2,3)<<1,0,mouse->x,0,1,mouse->y);cv::warpAffine(image,f.bgr,transform,image.size(),cv::INTER_LINEAR,cv::BORDER_REFLECT);
        if(lose_&&sequence>2)f.bgr.setTo(cv::Scalar(100,100,100));f.width=f.height=192;f.timing.sequence=++sequence;f.timing.captured_at=RecoilClock::now();return CaptureStatus::FRAME;}
    void close() noexcept override{}CaptureStatus status() const noexcept override{return CaptureStatus::READY;}std::string last_error() const override{return {};}
    std::shared_ptr<FakeMouse> mouse;cv::Mat image;bool lose_;std::uint64_t sequence=0;
};
TargetRunRequest request(const std::filesystem::path& path,const std::shared_ptr<FakeMouse>& m){TargetRunRequest r;
    r.plan={{"kind","recoil_target"},{"target_mode","observe"},{"mode","observe"},{"duration_ms",100},{"hold_key",5},{"cancel_key",35},{"weapon_id","weapon_ak47"},
        {"fire",false},{"split","fit"},{"environment_fingerprint","fake"},{"session_id","fake-session"},{"baseline_sha256","zero"},{"sensitivity",1.4},
        {"roi",{20,20,48,48}},{"background_roi",{110,100,48,48}},{"relative_tolerance",0.05},{"device_epoch","fake"}};
    r.output_directory=path;r.context_valid=[] {return true;};r.ammo=[] {return std::optional<int>(5);};r.capture_factory=[m](const CaptureConfig&){return std::make_unique<FakeCapture>(m);};return r;}
}
int main(int argc,char** argv){
    const bool fixture=argc==3&&std::string(argv[1])=="--write-fixture";
    const auto root=fixture?std::filesystem::absolute(std::filesystem::u8path(argv[2])):std::filesystem::temp_directory_path()/("xen-target-run-"+std::to_string(RecoilClock::now().time_since_epoch().count()));
    std::atomic<bool> canceled=false;auto device=std::make_shared<FakeMouse>();auto r=request(root/"observe",device);
    auto out=run_target_capture(r,device,canceled);check(out.completed&&device->moves==0&&device->downs==0,"无输出录像必须不发命令");
    check(std::filesystem::exists(r.output_directory/"manifest.json"),"必须封存manifest");
    r.output_directory=root/"cancel";canceled=true;out=run_target_capture(r,device,canceled);check(!out.completed&&device->downs==0,"取消不得输出");canceled=false;
    r.output_directory=root/"lost";r.capture_factory=[device](const CaptureConfig&){return std::make_unique<FakeCapture>(device,true);};
    out=run_target_capture(r,device,canceled);check(!out.completed&&device->downs==0,"丢靶必须拒绝");
    TargetCommandLedger ledger;Json record;std::string why;
    check(ledger.prepare({1.2,2.8},{0.8,-0.8},10,100,record,why),"有效分量应准备成功");
    const auto ff=record.at("ff_completed").get<std::array<int,2>>(),fb=record.at("fb_completed").get<std::array<int,2>>(),sum=record.at("total").get<std::array<int,2>>();
    check(ff[0]+fb[0]==sum[0]&&ff[1]+fb[1]==sum[1],"分量守恒");
    ledger.complete(false);check(!ledger.prepare({1,0},{0,0},10,100,record,why),"未知回执后不得再次派发");
    TargetCommandLedger saturated;check(!saturated.prepare({10,0},{-10,0},4,100,record,why),"对消分量不得绕过预算");
    device=std::make_shared<FakeMouse>();r=request(root/"calibrate",device);r.plan["target_mode"]="calibrate";r.plan["duration_ms"]=1000;
    out=run_target_capture(r,device,canceled);check(out.completed&&device->moves==4&&device->downs==0,"两轴正负标定应只有四次移动无射击");
    if(out.completed){
        std::ifstream f(r.output_directory/"calibration.json");Json calibration;f>>calibration;
        if(fixture){
            RecoilProfile parent;parent.id="target-fixture-parent";parent.weapon_id="weapon_ak47";parent.points={{0,0,0},{1000,0,10}};
            const auto text=serialize_recoil_profile(parent);std::ofstream base(root/"baseline.json",std::ios::binary);base<<text;base.close();
            const auto hash=recoil_calibration_sha256(text);
            for(int i=0;i<5;++i){
                auto m=std::make_shared<FakeMouse>();auto run=request(root/("fit-"+std::to_string(i)),m);
                run.plan["target_mode"]="test";run.plan["mode"]="test";run.plan["fire"]=true;run.plan["duration_ms"]=300;
                run.plan["target_shots"]=5;run.plan["ammo_limit_confirmed"]=true;run.plan["profile"]=Json::parse(text);
                run.plan["baseline_sha256"]=hash;run.plan["calibration"]=calibration;run.plan["generation"]=0;
                run.plan["calibration_sha256"]=recoil_calibration_sha256(calibration.dump());run.plan["max_alignment_uncertainty_us"]=100000;
                run.plan["ff_schedule_tolerance_us"]=50000;run.ammo=[m]{return std::optional<int>(m->moves?4:5);};
                const auto saved=run_target_capture(run,m,canceled);check(saved.completed,"跨语言fixture必须成功封存");
            }
        }
        device=std::make_shared<FakeMouse>();r=request(root/"ammo-rejected",device);r.plan["target_mode"]="test";r.plan["fire"]=true;r.plan["ammo_limit_confirmed"]=true;r.plan["target_shots"]=5;r.plan["calibration"]=calibration;r.ammo=[] {return std::optional<int>(30);};
        out=run_target_capture(r,device,canceled);check(!out.completed&&device->downs==0,"超过五发弹量应在DOWN前拒绝");
        r.output_directory=root/"unknown-down";r.ammo=[] {return std::optional<int>(5);};device->unknown_down=true;
        out=run_target_capture(r,device,canceled);check(!out.completed&&device->downs==1&&device->ups==1&&!device->down,"未知DOWN必须释放且整轮拒绝");
        device=std::make_shared<FakeMouse>();r=request(root/"empty-ammo",device);r.plan["target_mode"]="test";r.plan["fire"]=true;r.plan["ammo_limit_confirmed"]=true;r.plan["calibration"]=calibration;
        r.ammo=[device]{return std::optional<int>(device->down?0:5);};
        out=run_target_capture(r,device,canceled);check(out.completed&&device->downs==1&&device->ups==1&&device->moves==0,"自然打空必须先停发并完成UP");
        device=std::make_shared<FakeMouse>();r=request(root/"cancel-firing",device);r.plan["target_mode"]="test";r.plan["fire"]=true;r.plan["ammo_limit_confirmed"]=true;r.plan["calibration"]=calibration;
        r.context_valid=[&]{if(device->down)canceled=true;return true;};
        out=run_target_capture(r,device,canceled);check(!out.completed&&device->downs==1&&device->ups==1&&device->moves==0,"DOWN后取消必须优先UP并禁止后续移动");canceled=false;
        device=std::make_shared<FakeMouse>();device->unknown=true;r=request(root/"unknown-fire-move",device);r.plan["target_mode"]="test";r.plan["fire"]=true;r.plan["ammo_limit_confirmed"]=true;r.plan["calibration"]=calibration;r.plan["duration_ms"]=250;
        RecoilProfile moving;moving.id="moving";moving.weapon_id="weapon_ak47";moving.points={{0,0,0},{1000,0,40}};r.plan["profile"]=Json::parse(serialize_recoil_profile(moving));
        out=run_target_capture(r,device,canceled);check(!out.completed&&out.cleanup_unknown&&device->moves==1&&device->ups==1,"UP成功不能抹掉未知移动");
        device=std::make_shared<FakeMouse>();r=request(root/"control-no-fire",device);r.plan["target_mode"]="control";r.plan["calibration"]=calibration;r.plan["duration_ms"]=400;r.plan["profile"]=Json::parse(serialize_recoil_profile(moving));
        out=run_target_capture(r,device,canceled);bool zero_ff=true;std::ifstream nofire(r.output_directory/"commands.jsonl");std::string json_line;
        while(std::getline(nofire,json_line)){const auto row=Json::parse(json_line);zero_ff=zero_ff&&row.at("ff_requested")==Json::array({0.0,0.0});}
        check(out.completed&&zero_ff&&device->moves==0&&device->downs==0,"无射击对照必须只测反馈，不执行绑定的父前馈");
        device=std::make_shared<FakeMouse>();r=request(root/"ff-with-feedback",device);r.plan["target_mode"]="control";r.plan["calibration"]=calibration;r.plan["fire"]=true;r.plan["ammo_limit_confirmed"]=true;r.plan["duration_ms"]=800;
        moving.points={{0,0,0},{1000,0,8}};r.plan["profile"]=Json::parse(serialize_recoil_profile(moving));
        out=run_target_capture(r,device,canceled);bool used_feedback=false;std::ifstream combined(r.output_directory/"commands.jsonl");
        while(std::getline(combined,json_line)){const auto row=Json::parse(json_line);if(row.at("fb_completed").is_array())used_feedback=used_feedback||row.at("fb_completed")!=Json::array({0,0});}
        check(out.completed&&used_feedback,"持续前馈不能令反馈饥饿");
        device=std::make_shared<FakeMouse>();device->unknown=true;r=request(root/"unknown-move",device);r.plan["target_mode"]="calibrate";r.plan["duration_ms"]=1000;
        out=run_target_capture(r,device,canceled);check(!out.completed&&out.cleanup_unknown&&device->moves==1,"未知移动不得重发或恢复");
    }
    return failed?1:0;
}
