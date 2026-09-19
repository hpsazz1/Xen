#include "recoil/recoil_debug_run.h"
#include "recoil/recoil_calibration.h"
#include "recoil/recoil_worker.h"
#include "recoil/recoil_archive.h"
#include "recoil_tuner/wall_capture_run.h"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <thread>
#include <mutex>
#include <cstdlib>
#include <limits>

namespace {
using Json = nlohmann::json;
std::string utf8(const std::filesystem::path& path) {
    const auto text=std::filesystem::absolute(path).u8string();
    return {reinterpret_cast<const char*>(text.data()),text.size()};
}
std::string wall_fingerprint(const std::string& weapon_id,double sensitivity,const CaptureConfig& capture) {
    return recoil_calibration_sha256(Json{{"weapon_id",weapon_id},{"sensitivity",sensitivity},
        {"processing","wall_processing_v1_max960x540_area"},
        {"backend",static_cast<int>(capture.backend)},{"adapter",capture.adapter_index},{"output",capture.output_index},
        {"ndi_source",capture.ndi_source_name},{"udp_source",capture.udp_url},
        {"ndi_layout",static_cast<int>(capture.ndi_frame_layout)},{"udp_layout",static_cast<int>(capture.udp_frame_layout)},
        {"ndi_width",capture.ndi_source_width},{"ndi_height",capture.ndi_source_height},
        {"udp_width",capture.udp_source_width},{"udp_height",capture.udp_source_height},
        {"roi",{capture.roi_x,capture.roi_y,capture.roi_width,capture.roi_height,capture.center_roi?1:0}}}.dump());
}
RecoilCalibrationManifest manifest(const Json& plan, const RecoilProfile& profile, const std::string& id) {
    RecoilCalibrationManifest m;
    m.session_id = id;
    const auto text = serialize_recoil_profile(profile);
    m.profile_file_sha256 = m.profile_semantic_sha256 = recoil_calibration_sha256(text);
    m.config_binding_sha256 = recoil_calibration_sha256(plan.dump());
    m.environment = {profile.weapon_id,"","kmbox_net","",plan.at("sensitivity").get<double>()};
    m.environment_fingerprint = recoil_calibration_environment_fingerprint(m.environment);
    m.hold_virtual_key = plan.at("hold_key"); m.cancel_virtual_key = plan.at("cancel_key");
    const auto& l = plan.at("limits");
    m.limits = {1,l.at("session_ms"),l.at("firing_ms"),l.at("total_counts"),
        l.at("command_counts"),16,65534,20};
    return m;
}

void write(const std::filesystem::path& path, const Json& json) {
    std::ofstream file(path,std::ios::binary);
    if (!file || !(file << json.dump(2))) throw std::runtime_error("弹道测试归档写入失败");
}
}


Json prepare_wall_debug_plan(bool calibrate,const std::string& weapon_id,int duration_ms,
        const std::filesystem::path& calibration_path,const AppConfig& config) {
    if (config.keyboard.debug_test_virtual_keys.size()!=1 || config.keyboard.emergency_virtual_keys.empty() ||
        !valid_keyboard_config(config.keyboard)) throw std::runtime_error("请绑定一个独立测试键及紧急停止键");
    if (weapon_id.empty()||weapon_id.size()>128||!std::isfinite(config.recoil.sensitivity)||config.recoil.sensitivity<=0||
        duration_ms<100||duration_ms>10000) throw std::runtime_error("请填写武器、有效灵敏度和100–10000ms采集时长");
    RecoilCalibrationEnvironment environment{weapon_id,"","kmbox_net","",config.recoil.sensitivity};
    Json plan{{"schema_version",1},{"kind",calibrate?"recoil_calibrate":"recoil_capture"},{"weapon_id",weapon_id},
        {"duration_ms",duration_ms},{"sensitivity",config.recoil.sensitivity},
        {"environment_fingerprint",wall_fingerprint(weapon_id,config.recoil.sensitivity,config.capture)},
        {"hold_key",config.keyboard.debug_test_virtual_keys.front()},{"cancel_key",config.keyboard.emergency_virtual_keys.front()}};
    if (!calibrate) {
        std::ifstream file(calibration_path,std::ios::binary);
        if (!file) throw std::runtime_error("请先运行一次画面标定，并带入标定结果");
        std::string text(1024*1024+1,'\0');file.read(text.data(),static_cast<std::streamsize>(text.size()));
        text.resize(static_cast<std::size_t>(file.gcount()));
        if(text.size()>1024*1024)throw std::runtime_error("标定文件超过大小限制");
        const auto calibration=Json::parse(text);
        if(calibration.value("schema_version",0)!=1 || calibration.value("environment_fingerprint",std::string{})!=plan.at("environment_fingerprint").get<std::string>() ||
            !calibration.contains("samples")||calibration.at("samples").size()<2||!calibration.contains("geometry")||
            !calibration.at("geometry").is_object()||calibration.at("geometry").empty())
            throw std::runtime_error("标定环境与本次武器或灵敏度不一致");
        std::vector<recoil_tuner::WallCalibrationSample> samples;
        if(calibration.at("samples").size()>1000)throw std::runtime_error("标定样本超过限制");
        for(const auto& sample:calibration.at("samples"))samples.push_back({
            sample.at("counts").get<std::array<double,2>>(),sample.at("pixel_delta").get<std::array<double,2>>(),
            sample.at("acknowledged").get<bool>(),sample.at("evidence_id").get<std::string>()});
        std::array<double,4> response{};std::string fit_error;
        if(!recoil_tuner::fit_wall_calibration(samples,20,response,fit_error))throw std::runtime_error(fit_error);
        plan["calibration"]=calibration;
    }
    return plan;
}

bool recoil_debug_geometry_matches(const Json& plan,const Json& actual) noexcept {
    try {
        if(!actual.is_object()||actual.empty())return false;
        if(plan.value("kind",std::string{})=="recoil_calibrate")return true;
        if(plan.value("kind",std::string{})=="recoil_test"&&!plan.value("measurement_enabled",true)&&
            !plan.contains("calibration"))return true;
        return plan.contains("calibration")&&plan.at("calibration").contains("geometry")&&
            plan.at("calibration").at("geometry")==actual;
    }catch(...){return false;}
}

Json run_recoil_debug(const Json& plan,const AppConfig& config,const std::shared_ptr<IMouseController>& device,
        const std::filesystem::path& output,const std::atomic<bool>& canceled,RecoilDebugCaptureHook capture) {
    if(!device||!device->output_owner_exclusive())throw std::runtime_error("弹道测试缺少独占设备");
    const auto directory=std::filesystem::absolute(output);
    std::filesystem::create_directories(directory);write(directory/"plan.json",plan);
    const bool testing=plan.at("kind")=="recoil_test", calibrating=plan.at("kind")=="recoil_calibrate";
    auto profile=std::make_shared<RecoilProfile>();std::string error;
    if(testing&&!load_recoil_profile(plan.at("profile").dump(),*profile,error))throw std::runtime_error(error);
    const std::string weapon_id=testing?profile->weapon_id:plan.at("weapon_id").get<std::string>();
    RecoilCalibrationEnvironment environment{weapon_id,"","kmbox_net","",plan.at("sensitivity")};
    const auto fingerprint=wall_fingerprint(weapon_id,environment.sensitivity,config.capture);
    source_context::SourceContextClient source;weapon::GsiReceiver gsi;
    std::shared_ptr<RecoilWorker> worker;RecoilBatchArchive archive;
    std::mutex signal_mutex;TriggerFiringSignal signal;
    bool firing_finished = false;
    struct Cleanup {
        std::shared_ptr<RecoilWorker>& worker;RecoilBatchArchive& archive;
        weapon::GsiReceiver& gsi;source_context::SourceContextClient& source;
        void stop(){if(worker)worker->stop();archive.stop();gsi.stop();source.stop();}
        ~Cleanup(){stop();}
    } cleanup{worker,archive,gsi,source};
    if(canceled.load())return {{"success",false},{"cleanup_known",true},{"message","任务已取消"}};
    auto source_config=config.source_context;
    char* token=nullptr;std::size_t token_size=0;
    if(_dupenv_s(&token,&token_size,"XEN_SOURCE_CONTEXT_TOKEN")==0&&token){source_config.token=token;std::free(token);}
    if(!source.start(source_config)||!gsi.start(config.gsi))
        return {{"success",false},{"cleanup_known",true},{"message","源焦点或GSI接收器未就绪，未开始输出"}};
    std::atomic<std::uint64_t> source_session{0},weapon_epoch{0};
    const auto valid_context=[&]{
        const auto focus=source.snapshot();const auto weapon=gsi.snapshot();
        const bool valid=!canceled.load()&&focus.available&&focus.focused&&weapon.valid&&weapon.identity_match&&
            weapon.state==weapon::WeaponState::ACTIVE&&weapon.canonical_id==weapon_id&&weapon.ammo_clip&&
            *weapon.ammo_clip>=0&&weapon.valid_until>RecoilClock::now();
        if(!valid)return false;
        std::uint64_t unset=0;source_session.compare_exchange_strong(unset,focus.session_id);
        unset=0;weapon_epoch.compare_exchange_strong(unset,weapon.source_epoch);
        return source_session.load()==focus.session_id&&weapon_epoch.load()==weapon.source_epoch;
    };
    recoil_tuner::WallRunRequest request;
    request.mode=testing?recoil_tuner::WallRunMode::TEST:calibrating?recoil_tuner::WallRunMode::CALIBRATE:recoil_tuner::WallRunMode::CAPTURE;
    request.capture=config.capture;request.output_directory=directory/"capture";request.weapon_id=weapon_id;
    request.environment_fingerprint=fingerprint;request.sensitivity=environment.sensitivity;
    request.duration_ms=plan.at("duration_ms").get<int>();
    request.target_shots=calibrating?0:plan.value("target_shots",5);
    std::optional<int> initial_ammo;
    request.observed_ammo_delta=[&]() -> std::optional<int> {
        const auto weapon=gsi.snapshot();
        if(!initial_ammo||!weapon.valid||!weapon.ammo_clip||weapon.canonical_id!=weapon_id||
            weapon.valid_until<=RecoilClock::now()||*weapon.ammo_clip>*initial_ammo)return {};
        return *initial_ammo-*weapon.ammo_clip;
    };
    request.trigger_virtual_key=plan.at("hold_key");request.cancel_virtual_key=plan.at("cancel_key");
    request.context_valid=[&]{
        if(!valid_context())return false;
        if(!worker || firing_finished)return true;
        const auto state=worker->snapshot();const auto budget=worker->calibration_snapshot();
        return !state.faulted&&archive.snapshot().available&&
            (!budget.terminal||budget.end==RecoilCalibrationEnd::COMPLETED);
    };
    request.geometry_valid=[&](const Json& actual){return recoil_debug_geometry_matches(plan,actual);};
    if(testing) {
        write(directory/"candidate.json",plan.at("profile"));
        request.on_ready=[&]{
            initial_ammo=gsi.snapshot().ammo_clip;
            if(!initial_ammo||*initial_ammo<=0)return false;
            const auto permit=authorize_recoil_calibration(manifest(plan,*profile,directory.parent_path().filename().string()),profile,RecoilClock::now(),error);
            if(!permit)return false;
            auto recoil=config.recoil;recoil.enabled=true;recoil.mixed_aim=false;recoil.hold_virtual_key=request.trigger_virtual_key;
            recoil.game_build.clear();recoil.conditions.clear();recoil.input_path="kmbox_net";recoil.sensitivity=environment.sensitivity;
            recoil.budget_window_ms=permit->limits().rolling_window_ms;
            worker=std::make_shared<RecoilWorker>(device,std::make_shared<AutoStopOutputArbiter>(),std::make_shared<MotionLedger>(),
                [&,previous_epoch=std::uint64_t{},generation=std::uint64_t{1}]() mutable {
                    RecoilInput input;const auto weapon=gsi.snapshot();
                    if(weapon.source_epoch!=previous_epoch){previous_epoch=weapon.source_epoch;++generation;}
                    input.profile=profile;input.device_epoch=1;input.weapon_generation=generation;
                    input.profile_conditions_match=valid_context();input.focused=input.profile_conditions_match;
                    input.permission=!canceled.load();return input;
                },[&]{std::lock_guard lock(signal_mutex);return signal;});
            RecoilArchiveConfig cfg;cfg.acquisition_run_id=permit->session_id();cfg.directory=directory/"batches";cfg.recoil=recoil;
            if(!archive.start(cfg,[worker](std::uint64_t after,std::size_t max){return worker->read_execution_events(after,max);})||
                !worker->start_calibration(recoil,permit,true))return false;
            // DOWN前等待Worker实际观察到已释放状态，不能用固定sleep冒充就绪。
            const auto deadline=RecoilClock::now()+std::chrono::seconds(1);
            while(!canceled.load()&&valid_context()&&RecoilClock::now()<deadline){
                if(worker->snapshot().phase==RecoilPhase::READY)return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return false;
        };
        request.on_firing_started=[&](RecoilTime start,const ButtonReceipt& receipt){
            if(receipt.disposition!=ButtonDisposition::ACKNOWLEDGED||receipt.backend_completed_at<start)return false;
            std::lock_guard lock(signal_mutex);signal.confirmed_down=true;signal.id=1;
            signal.started_at=signal.backend_completed_at=receipt.backend_completed_at;
            signal.call_started_at=start;signal.protocol_ack_received_at=receipt.protocol_ack_received_at;
            signal.uncertainty=receipt.backend_completed_at-start;return true;
        };
        request.on_firing_stopped=[&]{
            firing_finished = true;
            {std::lock_guard lock(signal_mutex);signal.confirmed_down=false;}
            // 先撤销补偿许可，不在UP之前join在途move；左键释放后统一join并排空归档。
            if(worker)worker->cancel();
        };
    }
    if(!testing)request.on_ready=[&]{initial_ammo=gsi.snapshot().ammo_clip;return initial_ammo&&*initial_ammo>0;};
    const auto run=recoil_tuner::run_wall_capture(request,device,canceled,[&](const std::string&){if(capture)capture(directory,RecoilClock::now());});
    cleanup.stop();
    Json result=run.report;result["success"]=run.completed;result["cleanup_known"]=!run.cleanup_unknown;
    result["message"]=run.message;result["physical_validation_passed"]=false;
    result["archive_directory"]=utf8(directory/"batches");
    if(testing){
        result["base_profile_path"]=utf8(directory/"candidate.json");
        const auto state=worker?worker->snapshot():RecoilSnapshot{};
        if(state.faulted||state.pending)result["cleanup_known"]=false;
        const auto archived=archive.snapshot();
        result["archive_complete"]=archived.available&&archived.complete_batches==1&&archived.incomplete_batches==0;
        if(!result["archive_complete"].get<bool>())result["success"]=false;
    }
    if(calibrating&&run.completed){
        std::array<double,4> response{};std::string fit_error;
        if(recoil_tuner::fit_wall_calibration(run.calibration,20,response,fit_error)){
            Json samples=Json::array();
            for(const auto& sample:run.calibration)samples.push_back({{"counts",sample.counts},{"pixel_delta",sample.pixel_delta},
                {"acknowledged",sample.acknowledged},{"evidence_id",sample.evidence_id}});
            write(directory/"calibration.json",{{"schema_version",1},{"environment_fingerprint",fingerprint},
                {"weapon_id",weapon_id},{"sensitivity",environment.sensitivity},{"samples",samples},{"geometry",run.report.at("geometry")}});
            result["calibration_path"]=utf8(directory/"calibration.json");
        }else{result["success"]=false;result["message"]=fit_error;}
    }
    if(!run.frames.empty()){
        auto preview=run.frames.back().image.clone();
        if(!preview.empty()){
            cv::drawMarker(preview,{preview.cols/2,preview.rows/2},{0,255,0},cv::MARKER_CROSS,20,1);
            std::vector<unsigned char> bytes;
            if(cv::imencode(".png",preview,bytes)){std::ofstream out(directory/"preview.png",std::ios::binary);
                out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
                if(out)result["preview_path"]=utf8(directory/"preview.png");}
        }
    }
    if(!calibrating&&run.completed&&plan.contains("calibration")&&run.frames.size()>1){
        recoil_tuner::WallCaptureRequest analysis;analysis.mode=recoil_tuner::WallCaptureRequest::Mode::CAMERA_MOTION;
        analysis.weapon_id=weapon_id;analysis.profile_id=directory.parent_path().filename().string();
        analysis.environment_fingerprint=fingerprint;analysis.sensitivity=environment.sensitivity;
        std::string frame_hashes;
        for(const auto& captured:run.frames){
            const auto contiguous=captured.image.isContinuous()?captured.image:captured.image.clone();
            frame_hashes+=std::to_string(captured.time_ms)+recoil_calibration_sha256(std::string_view(
                reinterpret_cast<const char*>(contiguous.data),contiguous.total()*contiguous.elemSize()));
        }
        analysis.source_hash=run.report.value("source_hash",recoil_calibration_sha256(frame_hashes));
        const auto& frame=run.frames.front().image;
        analysis.image.registration_roi={frame.cols/8,frame.rows/8,frame.cols/4,frame.rows/4};
        analysis.image.measurement_roi={frame.cols/2,frame.rows/2,std::max(1,frame.cols/4),std::max(1,frame.rows/4)};
        analysis.image.reference={frame.cols/2.0,frame.rows/2.0};
        analysis.image.reference_confirmed=true; // 程序定义中心参考；弹孔/墙面身份仍须用户复核。
        analysis.image.max_translation_pixels=std::min(analysis.image.registration_roi.width,analysis.image.registration_roi.height)*0.3;
        for(const auto& sample:plan.at("calibration").at("samples"))analysis.calibration.push_back({
            sample.at("counts").get<std::array<double,2>>(),sample.at("pixel_delta").get<std::array<double,2>>(),
            sample.at("acknowledged").get<bool>(),sample.at("evidence_id").get<std::string>()});
        // 只以命令完成与接收时刻对齐；没有曝光/逐发时间资格，不写入校准相位。
        std::vector<recoil_tuner::WallFrame> firing_frames;
        if(run.report.contains("firing")&&run.report.contains("frames")){
            const auto start=run.report.at("firing").value("completed_ns",std::int64_t{});
            const auto finish=run.report.at("firing").value("release_call_started_ns",std::numeric_limits<std::int64_t>::max());
            std::optional<std::size_t> baseline;
            const auto& frames=run.report.at("frames");
            bool mapped=!frames.empty();
            double uncertainty_ms=0;
            for(const auto& metadata:frames){
                mapped=mapped&&metadata.contains("source_time_ns")&&!metadata.at("source_time_ns").is_null()&&
                    metadata.contains("source_uncertainty_ms")&&!metadata.at("source_uncertainty_ms").is_null();
                if(metadata.contains("source_uncertainty_ms")&&metadata.at("source_uncertainty_ms").is_number())
                    uncertainty_ms=std::max(uncertainty_ms,metadata.at("source_uncertainty_ms").get<double>());
            }
            const auto frame_time=[&](std::size_t i){return frames[i].at(mapped?"source_time_ns":"received_ns").get<std::int64_t>();};
            result["measurement_time_basis"]=mapped?"source_mapped_vs_command_ack":"receive_vs_command_ack";
            result["measurement_source_uncertainty_ms"]=mapped?Json(uncertainty_ms):Json(nullptr);
            // 接收域与映射源时域不得作为同一组训练样本混用；绑定实际几何与时间基准。
            analysis.environment_fingerprint=recoil_calibration_sha256(Json{{"calibration_environment",fingerprint},
                {"measurement_time_basis",result["measurement_time_basis"]},{"geometry",run.report.at("geometry")}}.dump());
            for(std::size_t i=0;i<run.frames.size()&&i<frames.size();++i)
                if(frame_time(i)<=start)baseline=i;
            if(baseline){
                firing_frames.push_back({0,run.frames[*baseline].image});
                for(std::size_t i=*baseline+1;i<run.frames.size()&&i<frames.size();++i){
                    const auto received=frame_time(i);
                    if(received>start&&received<=finish)
                        firing_frames.push_back({static_cast<double>(received-start)/1000000.0,run.frames[i].image});
                }
            }
        }
        const auto measured=recoil_tuner::analyze_wall_capture(firing_frames,analysis);
        Json observations=Json::array();
        for(const auto& observation:measured.observations)observations.push_back({{"center",{observation.center.x,observation.center.y}},
            {"earliest_ms",observation.earliest_ms},{"first_visible_ms",observation.first_visible_ms},{"cumulative_counts",observation.cumulative_counts}});
        Json report{{"schema_version",1},{"mode","CAMERA_MOTION"},{"valid",measured.valid},{"message",measured.message},
            {"environment_fingerprint",measured.environment_fingerprint},{"requires_manual_confirmation",true},
            {"shot_timing_available",false},{"pixel_response",measured.pixel_response},
            {"reference",{measured.reference.x,measured.reference.y}},{"calibration_evidence",measured.calibration_evidence},{"observations",observations}};
        if(!recoil_tuner::save_wall_report(directory/"measurement.json",measured,error))
            throw std::runtime_error("画面测量归档失败");
        result["measurement_path"]=utf8(directory/"measurement.json");
        if(!testing&&measured.valid&&measured.candidate){
            write(directory/"candidate.json",Json::parse(serialize_recoil_profile(*measured.candidate)));
            result["candidate_path"]=utf8(directory/"candidate.json");
        }
        if(!measured.valid){result["success"]=false;result["message"]=measured.message;}
    }
    result["success"]=result.value("success",false)&&result.value("cleanup_known",false)&&
        (!testing||result.value("archive_complete",false));
    result["measurement_enabled"]=plan.contains("calibration");
    if(!result["success"].get<bool>()||!plan.contains("calibration"))result["training_eligible"]=false;
    if(testing&&!plan.contains("calibration")&&result["success"].get<bool>())
        result["message"]="已有曲线验证完成，请人工观察效果；未提供画面标定，本组不参与优化训练";
    write(directory/"result.json",result);return result;
}
Json prepare_recoil_debug_plan(const std::filesystem::path& path, const AppConfig& config,
        double x_strength, double y_strength) {
    if (config.keyboard.debug_test_virtual_keys.size()!=1 || config.keyboard.emergency_virtual_keys.empty())
        throw std::runtime_error("请绑定一个测试键及独立紧急停止键后重新准备");
    if (!valid_keyboard_config(config.keyboard)) throw std::runtime_error("测试键与其他功能冲突");
    std::ifstream file(path,std::ios::binary);
    if (!file) throw std::runtime_error("候选曲线文件无法读取");
    std::string text(16*1024*1024+1,'\0'); file.read(text.data(),static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(file.gcount()));
    RecoilProfile base, profile; std::string error;
    if (!load_recoil_profile(text,base,error) || !compile_recoil_profile(base,{x_strength,y_strength,0,1},profile,error))
        throw std::runtime_error(error);
    // load与compile已完成真实结构校验；只更新本次测试副本，IMPORTED源文件及生产准入不变。
    profile.state=RecoilProfileState::SCHEMA_VALID; profile.calibration.evidence.clear();
    profile.phase_tolerance_ms.reset(); profile.recovery_ms.reset();
    double variation = 0;
    for (std::size_t i=1;i<profile.points.size();++i)
        variation += std::abs(profile.points[i].x_counts-profile.points[i-1].x_counts)+
            std::abs(profile.points[i].y_counts-profile.points[i-1].y_counts);
    // 累计总变差与舍入余量形成资源预算，绝不是实测稳定性或武器速度阈值。
    const double count_limit = std::ceil(variation)+2*static_cast<double>(profile.points.size());
    if (!std::isfinite(count_limit)||count_limit<1||count_limit>1000000000)
        throw std::runtime_error("候选曲线超过单组测试资源预算");
    const auto total = static_cast<std::uint64_t>(count_limit);
    const int firing_ms=std::min(60000,static_cast<int>(std::ceil(profile.points.back().time_ms))+100);
    Json plan{{"schema_version",1},{"kind","recoil_test"},{"profile",Json::parse(serialize_recoil_profile(profile))},
        {"source_profile_state",Json::parse(text).at("state")},{"measurement_enabled",false},
        {"source_sha256",recoil_calibration_sha256(text)},{"sensitivity",config.recoil.sensitivity},
        {"hold_key",config.keyboard.debug_test_virtual_keys.front()},{"cancel_key",config.keyboard.emergency_virtual_keys.front()},
        {"limits",{{"session_ms",std::min(600000,firing_ms+30000)},{"firing_ms",firing_ms},
            {"total_counts",total},{"command_counts",std::min<std::uint64_t>(65534,total)}}},
        {"budget_origin","SOFTWARE_RESOURCE_BOUND_NOT_CALIBRATION"}};
    if (!validate_recoil_calibration_manifest(manifest(plan,profile,"prepared"),profile,error)) throw std::runtime_error(error);
    return plan;
}
