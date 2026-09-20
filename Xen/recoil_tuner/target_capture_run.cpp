#include "recoil_tuner/target_capture_run.h"
#include "recoil_tuner/target_anchor.h"
#include "recoil_tuner/wall_capture_analysis.h"
#include "recoil/recoil_calibration.h"
#include <opencv2/imgcodecs.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <thread>
#include <limits>

namespace recoil_tuner {
namespace {
using Json=nlohmann::json;
using Clock=std::chrono::steady_clock;
std::int64_t stamp(Clock::time_point t){return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();}
double ms(Clock::time_point a,Clock::time_point b){return std::chrono::duration<double,std::milli>(a-b).count();}
void save(const std::filesystem::path& path,const Json& j){std::ofstream f(path,std::ios::binary);if(!(f<<j.dump(2)))throw std::runtime_error("固定目标归档失败");}
void line(std::ofstream& f,const Json& j){if(!(f<<j.dump()<<'\n'))throw std::runtime_error("固定目标逐帧记录失败");}
std::string path_text(const std::filesystem::path& p){auto s=std::filesystem::absolute(p).u8string();return {reinterpret_cast<const char*>(s.data()),s.size()};}
std::string image_hash(const cv::Mat& image){const auto a=image.isContinuous()?image:image.clone();return recoil_calibration_sha256(std::string_view(reinterpret_cast<const char*>(a.data),a.total()*a.elemSize()));}
std::string save_image(const std::filesystem::path& path,const cv::Mat& image){std::vector<unsigned char> data;if(!cv::imencode(".png",image,data))throw std::runtime_error("原始图像编码失败");std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char*>(data.data()),static_cast<std::streamsize>(data.size()));if(!f)throw std::runtime_error("原始图像保存失败");return recoil_calibration_sha256(std::string_view(reinterpret_cast<const char*>(data.data()),data.size()));}
cv::Mat load_image(const std::filesystem::path& path){std::error_code error;const auto size=std::filesystem::file_size(path,error);if(error||size>64*1024*1024)return {};
    std::ifstream f(path,std::ios::binary);std::vector<unsigned char> b(static_cast<std::size_t>(size));f.read(reinterpret_cast<char*>(b.data()),static_cast<std::streamsize>(b.size()));if(!f)return {};return cv::imdecode(b,cv::IMREAD_COLOR);}
cv::Rect rectangle(const Json& j){auto a=j.get<std::array<int,4>>();return {a[0],a[1],a[2],a[3]};}
Json geometry(const CapturedFrame& f){return {{"processing","target_native_bgr_v1"},{"size",{f.bgr.cols,f.bgr.rows}},{"source_size",{f.source_width,f.source_height}},{"roi_origin",{f.roi_x,f.roi_y}},{"source_pixels_per_pixel",{f.source_pixels_per_pixel_x,f.source_pixels_per_pixel_y}}};}
Json quality(const TargetAnchorQuality& q){return {{"response",q.response},{"texture_stddev",q.texture_stddev},{"residual",q.residual},{"raw_residual",std::isfinite(q.raw_residual)?Json(q.raw_residual):Json(nullptr)},
    {"template_score",q.template_score},{"peak_separation",q.peak_separation},{"failure",q.failure}};}
struct WindowEnded {};
struct SavedTargetFrame {cv::Mat image;Json row;};
}

bool TargetCommandLedger::prepare(std::array<double,2> ff,std::array<double,2> fb,int limit,int total,Json& record,std::string& reason){
    if(terminal_||pending_){reason="输出账本已终止或仍有在途命令";return false;}
    std::array<int,2> a{},b{},sum{};auto ra=ff_remainder_,rb=fb_remainder_;
    std::int64_t cost=0;
    for(int i=0;i<2;++i){
        if(!std::isfinite(ff[i])||!std::isfinite(fb[i])||std::abs(ff[i])+std::abs(fb[i])>65534){reason="命令分量无效";terminal_=true;return false;}
        a[i]=static_cast<int>(std::round(ff[i]+ra[i]));b[i]=static_cast<int>(std::round(fb[i]+rb[i]));
        sum[i]=a[i]+b[i];ra[i]+=ff[i]-a[i];rb[i]+=fb[i]-b[i];cost+=std::abs(a[i])+std::abs(b[i]);
        // 对消分量仍计入库存，避免前馈与反馈互相抵消后绕过预算。
        if(limit<1||std::abs(a[i])+std::abs(b[i])>limit){reason="分量饱和，整轮拒绝";terminal_=true;return false;}
    }
    if(total<1||cost>total-spent_){reason="累计输出预算耗尽，整轮拒绝";terminal_=true;return false;}
    record={{"ff_requested",ff},{"fb_requested",fb},{"ff_completed",a},{"fb_completed",b},{"total",sum},
        {"remainder",{ra[0]+rb[0],ra[1]+rb[1]}},{"ff_remainder",ra},{"fb_remainder",rb},{"queue_inventory",0}};
    ff_remainder_=ra;fb_remainder_=rb;spent_+=static_cast<int>(cost);pending_=true;return true;
}
void TargetCommandLedger::complete(bool known) noexcept {if(!pending_||!known)terminal_=true;pending_=false;}

TargetRunResult run_target_capture(const TargetRunRequest& request,const std::shared_ptr<IMouseController>& device,
        const std::atomic<bool>& canceled,const std::function<void(const std::string&)>& progress) noexcept {
    TargetRunResult result;result.report={{"mode","recoil_target"},{"physical_validation_passed",false},{"training_eligible",false}};
    std::unique_ptr<ICapture> capture;bool left=false,owned=false;std::ofstream frames,commands,events;
    std::string termination="failed";Json manifest,calibration;std::uint64_t frame_count=0,command_count=0;
    std::vector<SavedTargetFrame> saved_frames;std::vector<Json> command_rows,event_rows;
    auto event=[&](Json e){e["at_ns"]=stamp(Clock::now());e["event_loss_count"]=0;if(event_rows.size()>=30000)throw std::runtime_error("事件库存已满");event_rows.push_back(std::move(e));};
    auto release=[&]{if(!left||!device)return;const auto called=Clock::now();const auto r=device->set_left_button(false);left=false;
        result.cleanup_unknown=result.cleanup_unknown||r.disposition!=ButtonDisposition::ACKNOWLEDGED||r.cleanup_required||device->left_button_cleanup_required();
        event({{"type","UP"},{"call_started_ns",stamp(called)},{"backend_completed_ns",stamp(r.backend_completed_at)},
            {"disposition",static_cast<int>(r.disposition)},{"cleanup_known",!result.cleanup_unknown}});};
    try{
        const auto& p=request.plan;const auto mode=p.at("target_mode").get<std::string>();
        const bool calibrating=mode=="calibrate",observing=mode=="observe",feedback=mode=="control",fire=p.value("fire",false);
        const int duration=p.at("duration_ms"),hold=p.at("hold_key"),cancel=p.at("cancel_key"),shots=p.value("target_shots",5);
        result.report["target_mode"]=mode;result.report["fire"]=fire;result.report["weapon_id"]=p.value("weapon_id",std::string{});
        if(!device||!device->output_owner_exclusive()||!request.context_valid||duration<100||duration>3000||hold<=0||hold>=256||cancel<=0||cancel>=256||hold==cancel)
            throw std::runtime_error("固定目标参数或独占输出无效");
        if(!observing&&!calibrating&&!p.contains("calibration"))throw std::runtime_error("缺少固定锚点标定");
        if(fire&&(!p.value("ammo_limit_confirmed",false)||shots<1||shots>5||!request.ammo||!device->supports_left_button()||device->left_button_faulted()||device->left_button_cleanup_required()))
            throw std::runtime_error("需确认本轮最多五发可用弹量、不自动补弹且不换弹");
        if(canceled.load())throw std::runtime_error("任务已取消");
        if(std::filesystem::exists(request.output_directory))throw std::runtime_error("Run已存在，拒绝覆盖");
        std::filesystem::create_directories(request.output_directory);owned=true;
        std::filesystem::create_directory(request.output_directory/"frames");
        frames.open(request.output_directory/"frames.jsonl",std::ios::binary);commands.open(request.output_directory/"commands.jsonl",std::ios::binary);events.open(request.output_directory/"events.jsonl",std::ios::binary);
        if(!frames||!commands||!events)throw std::runtime_error("Run记录器不可用");
        manifest=p;manifest["schema"]="recoil_target_iteration_v1";
        manifest["run_id"]=request.output_directory.parent_path().filename().string()+"-"+request.output_directory.filename().string()+"-"+std::to_string(stamp(Clock::now()));manifest["attempt_id"]=manifest["run_id"];
        manifest["time_basis"]="receive_steady_not_exposure";manifest["x_strength"]=1.0;manifest["y_strength"]=1.0;
        manifest["source_hash"]=nullptr;manifest["binary_hash"]=nullptr;manifest["model_hash"]=nullptr;manifest["physical_effect_observed"]=false;
        manifest["source_hash_reason"]="not_embedded";manifest["binary_hash_reason"]="not_measured";manifest["model_hash_reason"]="no_detector_used";
        save(request.output_directory/"manifest.json",manifest);
        std::optional<Clock::time_point> live_deadline;bool tail_capture=false;
        auto safe=[&]{
            if(canceled.load())throw std::runtime_error("任务已取消");
            if(live_deadline&&Clock::now()>=*live_deadline)throw WindowEnded{};
            if(!request.context_valid())throw std::runtime_error("焦点、GSI或身份上下文失效");
            InputSnapshot input;
            if(!device->poll_input(input)||!input.state_valid||input.status!=InputMonitorStatus::READY)throw std::runtime_error("物理输入状态未知");
            if(input.virtual_keys[cancel]||(!tail_capture&&!input.virtual_keys[hold]))throw std::runtime_error("测试键释放或紧急停止");
            for(const int k:{1,65,68,83,87})if(input.virtual_keys[k])throw std::runtime_error("物理左键或WASD介入，停止本轮");
            if(request.context_facts)event({{"type","context"},{"facts",request.context_facts()}});
        };
        safe();
        auto cfg=request.capture;cfg.enable_d3d11_cuda_interop=false;cfg.enable_d3d11_directml_interop=false;
        // 单次采集等待有界，UP优先于下一帧处理；PNG编码全部留到释放之后。
        cfg.acquire_timeout_ms=std::min(cfg.acquire_timeout_ms,10);cfg.ndi_receive_timeout_ms=std::min(cfg.ndi_receive_timeout_ms,10);cfg.udp_read_timeout_ms=std::min(cfg.udp_read_timeout_ms,10);
        capture=request.capture_factory?request.capture_factory(cfg):create_capture(cfg);
        if(!capture||!capture->open())throw std::runtime_error("固定目标画面源不可用");
        CapturedFrame frame;auto last_received=Clock::now();std::uint64_t last_sequence=0,last_source_drops=0,last_transport_drops=0;Json frozen_geometry;bool preparing=true;
        TargetAnchor anchor;bool anchored=false;double noise=0,max_error=0,max_gap=0;std::uint64_t bytes_saved=0;
        TargetAnchorConfig anchor_cfg;
        const bool has_roi=p.contains("roi")&&p.contains("background_roi")&&p.at("roi").size()==4&&p.at("roi")[2].get<int>()>0;
        if(has_roi){anchor_cfg.target_roi=rectangle(p.at("roi"));anchor_cfg.background_roi=rectangle(p.at("background_roi"));anchor_cfg.max_relative_shift_normalized=p.value("relative_tolerance",0.05);}
        if(!observing&&!has_roi)throw std::runtime_error("请先在短录像中选择目标与独立背景ROI");
        auto grab=[&]() -> TargetAnchorObservation {
            const auto deadline=Clock::now()+std::chrono::milliseconds(1000);
            for(;;){safe();const auto s=capture->grab(frame);const auto received=Clock::now();
                if(s==CaptureStatus::NO_FRAME||s==CaptureStatus::READY){if(received>=deadline)throw std::runtime_error("画面超时");std::this_thread::sleep_for(std::chrono::milliseconds(1));continue;}
                safe();if(s!=CaptureStatus::FRAME||frame.bgr.empty()||frame.bgr.type()!=CV_8UC3||frame.bgr.total()*frame.bgr.elemSize()>64*1024*1024)throw std::runtime_error("原始BGR画面失效");
                if(frame.timing.sequence==0||(last_sequence&&frame.timing.sequence<=last_sequence))throw std::runtime_error("缺失、重复或倒序帧，整轮拒绝");
                if(frame_count&&!preparing&&(frame.timing.source_dropped_frames!=last_source_drops||frame.timing.transport_dropped_frames!=last_transport_drops))throw std::runtime_error("源端丢帧或计数重置，整轮拒绝");
                last_source_drops=frame.timing.source_dropped_frames;last_transport_drops=frame.timing.transport_dropped_frames;last_sequence=frame.timing.sequence;
                if(frame_count)max_gap=std::max(max_gap,ms(received,last_received));last_received=received;
                const auto g=geometry(frame);if(frame_count&&g!=frozen_geometry)throw std::runtime_error("画面几何变化");
                if(!frame_count){frozen_geometry=g;manifest["geometry"]=g;
                    cv::Mat reference=frame.bgr;
                    if(p.contains("calibration")){
                        calibration=p.at("calibration");if(calibration.at("geometry")!=g)throw std::runtime_error("标定几何不匹配");
                        reference=load_image(std::filesystem::u8path(calibration.at("template_path").get<std::string>()));
                        if(reference.empty()||image_hash(reference)!=calibration.at("template_sha256").get<std::string>())throw std::runtime_error("固定模板缺失或哈希不一致");
                    }
                    manifest["template_sha256"]=image_hash(reference);
                    save_image(request.output_directory/"template.png",reference);
                    if(has_roi){std::string error;if(!anchor.initialize(reference,anchor_cfg,error))throw std::runtime_error(error);anchored=true;}
                    save(request.output_directory/"manifest.json",manifest);
                }
                TargetAnchorObservation observation;if(anchored)observation=anchor.observe(frame.bgr);
                bytes_saved+=frame.bgr.total()*frame.bgr.elemSize();if(bytes_saved>256ULL*1024*1024)throw std::runtime_error("原始图像超过256MiB有界库存");
                const auto raw="frames/"+std::to_string(++frame_count)+".png";
                if(frame_count>1500)throw std::runtime_error("逐帧记录超过有界数量");
                Json row{{"frame_id",frame_count},{"sequence",frame.timing.sequence},{"received_ns",stamp(received)},
                    {"processed_ns",stamp(Clock::now())},{"source_time_ns",frame.timing.source_time_timing_valid?Json(stamp(frame.timing.source_time_at)):Json(nullptr)},
                    {"source_time_basis",SourceTimeBasisName(frame.timing.source_time_basis)},
                    {"source_uncertainty_ms",frame.timing.source_time_timing_valid?Json(frame.timing.source_clock_uncertainty_ms):Json(nullptr)},
                    {"source_clock_session_id",frame.timing.source_clock_session_id},{"source_clock_sample_count",frame.timing.source_clock_sample_count},
                    {"phase",preparing?"prepare":tail_capture?"tail":"active"},{"source_dropped_frames",frame.timing.source_dropped_frames},{"transport_dropped_frames",frame.timing.transport_dropped_frames},
                    {"raw_path",raw},{"raw_sha256",nullptr},{"event_loss_count",0},{"anchor_valid",anchored&&observation.valid},{"background_consistent",anchored?Json(observation.valid):Json(nullptr)},
                    {"error_px",{observation.shift.x,observation.shift.y}},
                    {"quality",quality(observation.target_quality)},{"background_quality",quality(observation.background_quality)},
                    {"relative_shift_normalized",observation.relative_shift_normalized},{"identity_sha256",manifest["template_sha256"]},
                    {"rejection_reason",anchored?observation.failure:"anchor_not_selected"}};saved_frames.push_back({frame.bgr.clone(),std::move(row)});
                if(anchored&&!observation.valid)throw std::runtime_error("锚点失效："+observation.failure);
                return observation;
            }
        };
        auto current=grab();safe();
        RecoilProfile profile;std::string profile_error;bool has_profile=p.contains("profile");
        if(has_profile&&!load_recoil_profile(p.at("profile").dump(),profile,profile_error))throw std::runtime_error(profile_error);
        if(has_profile){const auto text=p.value("baseline_source_text",serialize_recoil_profile(profile));std::ofstream base(request.output_directory/"baseline.json",std::ios::binary);
            if(!(base<<text))throw std::runtime_error("父基线冻结失败");result.report["baseline_path"]=path_text(request.output_directory/"baseline.json");}
        // 预先为协议最多1500帧保留容量；过大的Capture输出需先缩小其配置ROI，不能开火后静默抽帧。
        const auto frame_bytes=frame.bgr.total()*frame.bgr.elemSize();
        const auto planned_frames=static_cast<std::uint64_t>(std::ceil(duration*0.25))+3;
        if(!observing&&frame_bytes*planned_frames>256ULL*1024*1024)throw std::runtime_error("当前Capture ROI与时域超过原始帧库存，请缩小Capture ROI或缩短时域");
        // 模板编码和初始化属于输出前准备。重新取得新鲜起点后才冻结活动期丢帧基准。
        current=grab();preparing=false;max_gap=0;safe();
        std::optional<int> initial_ammo,last_ammo;
        if(fire){initial_ammo=request.ammo();if(!initial_ammo||*initial_ammo<1||*initial_ammo>shots)throw std::runtime_error("当前可发弹量未限制在本轮1至5发以内");last_ammo=initial_ammo;}
        TargetCommandLedger ledger;Clock::time_point start=Clock::now(),last_command{};int total_limit=p.value("total_counts_limit",4096),command_limit=p.value("command_counts_limit",64);
        auto dispatch=[&](std::array<double,2> ff,std::array<double,2> fb,const std::string& phase){safe();
            if(fire){const auto ammo=request.ammo();if(!ammo||*ammo<0||*ammo>*last_ammo)throw std::runtime_error("派发前弹量未知或增加");last_ammo=ammo;if(*ammo==0){termination="ammo_empty";throw WindowEnded{};}}
            Json row;std::string why;
            if(!ledger.prepare(ff,fb,command_limit,total_limit,row,why))throw std::runtime_error(why);
            row["command_id"]=++command_count;row["firing_id"]=fire?1:0;row["phase"]=phase;row["saturated"]=false;row["device_epoch"]=p.value("device_epoch",std::string("exclusive_debug_session"));row["event_loss_count"]=0;
            row["planned_ns"]=row["calculated_ns"]=stamp(Clock::now());row["physical_effect"]=nullptr;
            const auto sum=row.at("total").get<std::array<int,2>>();const auto called=Clock::now();row["call_started_ns"]=stamp(called);
            // 零命令仍记录量化余数，但不伪造后端回执。
            const bool no_op=sum[0]==0&&sum[1]==0;MouseMoveReceipt r;if(!no_op)r=device->move({sum[0],sum[1]});
            const auto returned=Clock::now();const bool known=no_op||(r.succeeded&&r.backend_completed_at>=called&&r.backend_completed_at<=returned);
            ledger.complete(known);row["returned_ns"]=stamp(returned);row["status"]=no_op?"no_op":known?"completed":"unknown";
            row["backend_completed_ns"]=known&&!no_op?Json(stamp(r.backend_completed_at)):Json(nullptr);row["ack_ns"]=r.protocol_ack_received?Json(stamp(r.protocol_ack_received_at)):Json(nullptr);
            row["zero_command"]=sum[0]==0&&sum[1]==0;
            if(!known){row["ff_completed"]=nullptr;row["fb_completed"]=nullptr;result.cleanup_unknown=true;}
            command_rows.push_back(std::move(row));if(!known)throw std::runtime_error("移动结果未知，不重发、不继续本轮");
            if(sum[0]||sum[1])last_command=r.backend_completed_at;
        };
        if(progress)progress(fire?"固定目标单轮即将开始；保持测试键，松键或End立即停止":"固定目标无射击实验运行中；保持测试键");
        if(fire){safe();const auto ammo=request.ammo();if(!ammo||ammo!=initial_ammo)throw std::runtime_error("DOWN前弹量改变，重新准备本轮");
            left=true;const auto called=Clock::now();live_deadline=called+std::chrono::milliseconds(duration);manifest["software_fire_start_ns"]=stamp(called);const auto r=device->set_left_button(true);
            event({{"type","DOWN"},{"call_started_ns",stamp(called)},{"backend_completed_ns",stamp(r.backend_completed_at)},{"disposition",static_cast<int>(r.disposition)},
                {"initial_ammo",*initial_ammo},{"ammo_received_ns",stamp(Clock::now())}});
            if(r.disposition!=ButtonDisposition::ACKNOWLEDGED||r.backend_completed_at<called)throw std::runtime_error("开火回执未知，停止并清理");
            manifest["alignment_uncertainty_us"]=std::chrono::duration_cast<std::chrono::microseconds>(r.backend_completed_at-called).count();
            manifest["alignment_semantics"]="software_DOWN_call_to_backend_completion_not_first_shot";
            if(manifest.at("alignment_uncertainty_us").get<std::int64_t>()>p.value("max_alignment_uncertainty_us",std::int64_t(100000)))throw std::runtime_error("DOWN软件参考不确定度超过冻结预算");
            start=called;
        }
        if(!fire)live_deadline=start+std::chrono::milliseconds(duration);
        if(!fire)manifest["software_fire_start_ns"]=nullptr;
        if(!fire)manifest["alignment_uncertainty_us"]=nullptr;
        manifest["actual_first_shot_uncertainty_us"]=nullptr;manifest["start_ns"]=stamp(start);event({{"type","start"},{"fire",fire}});
        RecoilPoint sampled{};std::vector<WallCalibrationSample> samples;int stage=0;const int pulse=p.value("calibration_counts",4);
        cv::Point2d stage_origin=current.shift,last_stage_shift=current.shift;double stage_first_visible=-1,max_response=0,max_first_visible=0;Clock::time_point pulse_at{};
        const double slot=duration/5.0;std::vector<cv::Point2d> noise_points;std::array<double,4> response{};
        std::vector<std::pair<double,cv::Point2d>> stage_points;
        auto finish_response=[&]{
            const double numerical_floor=0.001*std::min(anchor_cfg.target_roi.width,anchor_cfg.target_roi.height);
            std::size_t settled=0;for(const auto& q:stage_points)if(q.first>=slot*0.75){++settled;if(cv::norm(q.second-last_stage_shift)>std::max(noise*3.0,numerical_floor))throw std::runtime_error("标定响应尾部仍变化，不能冻结反馈等待时域");}
            if(settled<2)throw std::runtime_error("标定尾部稳定观察不足");
            max_first_visible=std::max(max_first_visible,stage_first_visible);max_response=std::max(max_response,ms(last_received,pulse_at)+max_gap);
        };
        if(feedback)response=calibration.at("pixel_response").get<std::array<double,4>>();
        const double gain=0.25;const double delay=feedback?calibration.at("response_upper_ms").get<double>():0;
        const auto baseline_start=current.shift;Clock::time_point next_feedback{};max_error=0;
        while(ms(Clock::now(),start)<duration){
            try{current=grab();safe();}catch(const WindowEnded&){break;}
            const double elapsed=ms(last_received,start);if(anchored)max_error=std::max(max_error,cv::norm(current.shift-baseline_start));
            if(fire){const auto ammo=request.ammo();event({{"type","ammo"},{"ammo",ammo?Json(*ammo):Json(nullptr)}});
                if(!ammo||*ammo<0||*ammo>*last_ammo)throw std::runtime_error("弹量未知、换弹或自动补弹，整轮拒绝");
                last_ammo=ammo;if(*ammo==0){termination="ammo_empty";break;}}
            if(calibrating){
                if(stage==0)noise_points.push_back(current.shift);
                else {stage_points.push_back({ms(last_received,pulse_at),current.shift});last_stage_shift=current.shift;
                    if(stage_first_visible<0&&cv::norm(current.shift-stage_origin)>std::max(noise*3.0,0.001*std::min(anchor_cfg.target_roi.width,anchor_cfg.target_roi.height)))stage_first_visible=ms(last_received,pulse_at);}
                if(elapsed>=slot*(stage+1)&&stage<4){
                    if(stage==0){for(auto q:noise_points)noise=std::max(noise,cv::norm(q-baseline_start));}
                    else {
                        if(stage_first_visible<0||stage_first_visible>slot*0.75)throw std::runtime_error("标定响应未在有界窗口内可靠出现");
                        finish_response();
                        const auto d=current.shift-stage_origin;const std::array<double,2> c=stage==1?std::array<double,2>{double(pulse),0}:stage==2?std::array<double,2>{-double(pulse),0}:std::array<double,2>{0,double(pulse)};
                        samples.push_back({c,{d.x,d.y},true,"command-"+std::to_string(command_count)});
                    }
                    stage_origin=current.shift;stage_first_visible=-1;stage_points.clear();++stage;
                    const std::array<double,2> c=stage==1?std::array<double,2>{double(pulse),0}:stage==2?std::array<double,2>{-double(pulse),0}:stage==3?std::array<double,2>{0,double(pulse)}:std::array<double,2>{0,-double(pulse)};
                    dispatch(c,{0,0},"calibrate");pulse_at=last_command;
                }
                last_stage_shift=current.shift;continue;
            }
            if(observing)continue;
            std::array<double,2> ff{},fb{};
            if(has_profile&&fire){const auto next=sample_recoil_profile(profile,std::min<double>(elapsed,duration));ff={next.x_counts-sampled.x_counts,next.y_counts-sampled.y_counts};sampled=next;}
            if(feedback){
                if(max_gap>calibration.at("frame_gap_limit_ms").get<double>())throw std::runtime_error("反馈帧间隔超过标定证据，整轮拒绝");
                if(Clock::now()<last_received||ms(Clock::now(),last_received)>calibration.at("frame_gap_limit_ms").get<double>())throw std::runtime_error("反馈处理延迟超出标定证据");
                if(last_received>=next_feedback){
                    const double det=response[0]*response[3]-response[1]*response[2];
                    if(!std::isfinite(det)||std::abs(det)<1e-9)throw std::runtime_error("标定响应不可逆");
                    const auto e=current.shift-baseline_start;const auto noise_bound=calibration.at("noise_pixels").get<double>();
                    if(cv::norm(e)>noise_bound*3.0){fb={-gain*(response[3]*e.x-response[1]*e.y)/det,-gain*(-response[2]*e.x+response[0]*e.y)/det};}
                    if(std::abs(fb[0])+std::abs(fb[1])>pulse)throw std::runtime_error("反馈超过已标定位移库存，整轮拒绝");
                }
            }
            try{dispatch(ff,fb,fire?"fire":"control");}catch(const WindowEnded&){break;}
            if(fb[0]!=0||fb[1]!=0)next_feedback=Clock::now()+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double,std::milli>(delay));
        }
        manifest["window_end_ns"]=stamp(Clock::now());manifest["observed_shots"]=initial_ammo&&last_ammo?Json(*initial_ammo-*last_ammo):Json(nullptr);
        release();
        // UP后只补一帧覆盖真实停发边界；不延长射击窗口，不派发回中。
        live_deadline.reset();tail_capture=true;grab();
        if(calibrating){
            if(stage!=4||stage_first_visible<0||stage_first_visible>slot*0.75)throw std::runtime_error("两轴正负标定未完成");
            const auto d=last_stage_shift-stage_origin;samples.push_back({{0,-double(pulse)},{d.x,d.y},true,"command-4"});finish_response();
            std::string why;if(!fit_wall_calibration(samples,20,response,why))throw std::runtime_error(why);
            Json raw=Json::array();for(auto& s:samples)raw.push_back({{"counts",s.counts},{"pixel_delta",s.pixel_delta},{"acknowledged",s.acknowledged},{"evidence_id",s.evidence_id}});
            Json c{{"schema","recoil_target_calibration_v1"},{"environment_fingerprint",p.at("environment_fingerprint")},{"geometry",frozen_geometry},
                {"roi",p.at("roi")},{"background_roi",p.at("background_roi")},{"relative_tolerance",anchor_cfg.max_relative_shift_normalized},
                {"template_sha256",manifest.at("template_sha256")},{"template_path",path_text(request.output_directory/"template.png")},
                {"pixel_response",response},{"samples",raw},{"noise_pixels",noise},{"response_upper_ms",max_response},
                {"first_visible_upper_ms",max_first_visible},{"response_bound_semantics","observed_settled_receive_window_not_physical_latency"},
                {"frame_gap_limit_ms",max_gap*2},{"feedback_gain",gain},{"calibration_counts",pulse},
                {"time_basis","receive_steady_not_exposure"},{"learning_phase_qualified",false},{"physical_validation_passed",false}};
            save(request.output_directory/"calibration.json",c);result.report["calibration_path"]=path_text(request.output_directory/"calibration.json");
        }
        termination=termination=="ammo_empty"?termination:"duration_complete";result.completed=!result.cleanup_unknown;
        result.message="有界实验记录完成；尚未物理验收或获得弹道学习相位资格";
        result.report["max_error_pixels"]=max_error;result.report["noise_pixels"]=noise;result.report["frame_gap_max_ms"]=max_gap;
        if(feedback&&!fire){const auto size=std::min(anchor_cfg.target_roi.width,anchor_cfg.target_roi.height);
            result.report["control_stable"]=max_error/std::max(1,size)<=anchor_cfg.max_relative_shift_normalized;
            result.report["control_observed_stable_confirmed"]=false;}
    }catch(const std::exception& e){result.message=e.what();result.completed=false;}
    catch(const WindowEnded&){result.message="设备调用超过本轮时域，已停止";result.completed=false;}
    catch(...){result.message="固定目标运行异常";result.completed=false;}
    try{release();}catch(...){result.cleanup_unknown=true;result.completed=false;}
    if(capture)capture->close();
    result.report["success"]=result.completed&&!result.cleanup_unknown;result.report["cleanup_known"]=!result.cleanup_unknown;
    result.report["message"]=result.message;result.report["termination"]=termination;result.report["frame_count"]=frame_count;result.report["command_count"]=command_count;
    result.report["capture_path"]=path_text(request.output_directory);result.report["preview_path"]=path_text(request.output_directory/"preview.png");
    for(const auto* key:{"calibration_sha256","baseline_sha256","environment_fingerprint","template_sha256"})if(manifest.contains(key))result.report[key]=manifest[key];
    if(owned)try{
        event({{"type","end"},{"reason",result.completed?"completed":termination},{"message",result.message},{"cleanup_known",!result.cleanup_unknown},{"cleanup_complete",!result.cleanup_unknown}});
        for(auto& saved:saved_frames){saved.row["raw_sha256"]=save_image(request.output_directory/std::filesystem::u8path(saved.row.at("raw_path").get<std::string>()),saved.image);line(frames,saved.row);}
        if(!saved_frames.empty())save_image(request.output_directory/"preview.png",saved_frames.back().image);
        for(const auto& row:command_rows)line(commands,row);for(const auto& row:event_rows)line(events,row);
        frames.flush();commands.flush();events.flush();
        if(!frames||!commands||!events)throw std::runtime_error("归档尾部写入失败");
        save(request.output_directory/"review.json",{{"data_complete",result.completed},{"training_eligible",false},{"physical_validation_passed",false},{"termination",termination},{"human_observation",nullptr},
            {"split",manifest.value("split",std::string("fit"))},{"identity_pass",false},{"integrity_pass",false}});
        Json files=Json::object();for(const auto* name:{"frames.jsonl","commands.jsonl","events.jsonl","review.json"}){std::ifstream f(request.output_directory/name,std::ios::binary);std::string data((std::istreambuf_iterator<char>(f)),{});if(!f)throw std::runtime_error("归档摘要读取失败");files[name]=recoil_calibration_sha256(data);}manifest["files"]=files;
        save(request.output_directory/"manifest.json",manifest);
        save(request.output_directory/"result.json",result.report);
    }catch(...){result.completed=false;result.report["success"]=false;result.message="归档封存失败";result.report["message"]=result.message;}
    return result;
}
}
