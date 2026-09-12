#include "recoil/recoil_calibration_io.h"
#include "recoil/recoil_worker.h"
#include "recoil/recoil_archive.h"
#include <windows.h>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {
std::atomic<bool> canceled{false};
BOOL WINAPI control(DWORD event) {
    if(event==CTRL_C_EVENT || event==CTRL_BREAK_EVENT || event==CTRL_CLOSE_EVENT){canceled.store(true);return TRUE;}
    return FALSE;
}
std::string environment_token(const char* name) {
    const auto* value=std::getenv(name);
    if(!value || !*value)throw std::runtime_error("缺少GSI或源端焦点环境凭据");
    return value;
}
struct Resources {
    std::shared_ptr<IMouseController> mouse;
    source_context::SourceContextClient source;
    weapon::GsiReceiver gsi;
    std::shared_ptr<RecoilWorker> worker;
    RecoilBatchArchive archive;
    void stop() noexcept {
        if(worker)worker->stop();
        archive.stop();gsi.stop();source.stop();if(mouse)mouse->close();
    }
    ~Resources(){stop();}
};
int run(RecoilCalibrationPrepared prepared) {
    std::string error;
    if(!consume_recoil_calibration_session(prepared,error)){std::cerr<<error<<'\n';return 1;}
    Resources resources;
    std::string termination="START_FAILED";
    RecoilCalibrationBudgetSnapshot budget;
    int code=1;
    try {
        auto source_config=prepared.config.source_context;
        auto gsi_config=prepared.config.gsi;
        source_config.token=environment_token("XEN_SOURCE_CONTEXT_TOKEN");
        gsi_config.token=environment_token("XEN_GSI_TOKEN");
        auto mouse_config=prepared.config.mouse;mouse_config.allow_send_input=true;
        auto device=MouseDeviceFactory::create(mouse_config);
        if(!device || !device->open() || !device->output_owner_exclusive())throw std::runtime_error("无法获得真实设备独占所有权");
        resources.mouse=std::shared_ptr<IMouseController>(std::move(device));
        if(!resources.source.start(source_config) || !resources.gsi.start(gsi_config))throw std::runtime_error("校准上下文服务无法启动");
        const auto armed=RecoilClock::now();
        const auto permit=authorize_recoil_calibration(prepared.manifest,prepared.profile,armed,error);
        if(!permit)throw std::runtime_error(error);
        auto arbiter=std::make_shared<AutoStopOutputArbiter>();
        auto ledger=std::make_shared<MotionLedger>();
        ledger->reset(permit->limits().rolling_window_counts,permit->limits().rolling_window_ms,armed);
        auto recoil=prepared.config.recoil;
        recoil.enabled=true;recoil.mixed_aim=false;recoil.hold_virtual_key=prepared.manifest.hold_virtual_key;
        recoil.game_build=prepared.manifest.environment.game_build;recoil.conditions=prepared.manifest.environment.conditions;
        recoil.input_path=prepared.manifest.environment.input_path;recoil.sensitivity=prepared.manifest.environment.sensitivity;
        recoil.budget_window_ms=permit->limits().rolling_window_ms;
        resources.worker=std::make_shared<RecoilWorker>(resources.mouse,arbiter,ledger,
            [&resources,permit,previous_weapon=std::string{},previous_epoch=std::uint64_t{},generation=std::uint64_t{1},
                focus_session=std::uint64_t{}]() mutable {
                RecoilInput input;
                const auto weapon=resources.gsi.snapshot();const auto focus=resources.source.snapshot();
                if(weapon.canonical_id!=previous_weapon || weapon.source_epoch!=previous_epoch) {
                    previous_weapon=weapon.canonical_id;previous_epoch=weapon.source_epoch;
                    if(generation==UINT64_MAX){canceled.store(true);return input;}++generation;
                }
                input.profile=permit->profile();input.device_epoch=1;input.weapon_generation=generation;
                input.profile_conditions_match=weapon.valid&&weapon.identity_match&&weapon.state==weapon::WeaponState::ACTIVE&&
                    weapon.canonical_id==permit->manifest().environment.weapon_id&&weapon.ammo_clip&&*weapon.ammo_clip>0&&
                    weapon.valid_until>RecoilClock::now();
                input.focused=focus.available&&focus.focused&&focus.session_id==focus_session;
                focus_session=focus.available&&focus.focused?focus.session_id:0;
                input.permission=!canceled.load();return input;
            },[]{return TriggerFiringSignal{};});
        RecoilArchiveConfig archive;
        archive.acquisition_run_id=prepared.manifest.session_id;archive.directory=prepared.directory/"batches";archive.recoil=recoil;
        const auto worker=resources.worker;
        if(!resources.archive.start(archive,[worker](std::uint64_t after,std::size_t max){return worker->read_execution_events(after,max);}))
            throw std::runtime_error("校准批次归档无法启动");
        if(!resources.worker->start_calibration(recoil,permit))throw std::runtime_error("校准调度器无法启动");
        std::cout<<"校准已等待源端前台、完整释放与人工保持键/左键新沿；一次弹序结束即退出。Ctrl+C可取消。\n";
        for(;;) {
            budget=resources.worker->calibration_snapshot();
            if(budget.terminal){termination=RecoilCalibrationEndName(budget.end);code=budget.end==RecoilCalibrationEnd::COMPLETED?0:1;break;}
            if(canceled.load()){termination="CANCELED";resources.worker->cancel();break;}
            if(!resources.archive.snapshot().available){termination="ARCHIVE_FAILED";resources.worker->cancel();break;}
            if(RecoilClock::now()>=permit->expires_at()){termination="TIME_LIMIT";resources.worker->cancel();break;}
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';}
    resources.stop();
    if(resources.worker)budget=resources.worker->calibration_snapshot();
    const auto archive=resources.archive.snapshot();
    const auto archive_status=archive.available&&archive.complete_batches==1&&archive.incomplete_batches==0?"COMPLETE":"INCOMPLETE";
    if(archive_status==std::string("INCOMPLETE"))code=1;
    if(!write_recoil_calibration_result(prepared,termination,budget,archive_status,error)){std::cerr<<error<<'\n';return 1;}
    std::cout<<"会话结束："<<termination<<"；原始报告已保存，校准结论仍为空。\n";return code;
}
}
int main(int argc,char** argv) {
    try {
        std::string error;RecoilCalibrationPrepared prepared;
        if(argc==6&&std::string(argv[1])=="prepare") {
            RecoilCalibrationPrepareRequest request;
            if(!load_recoil_calibration_request(argv[4],request,error)){std::cerr<<error<<'\n';return 1;}
            request.profile_path=argv[2];request.config_path=argv[3];request.output_directory=argv[5];
            wchar_t executable[32768]{};const auto length=GetModuleFileNameW(nullptr,executable,32768);
            if(length==0||length==32768)throw std::runtime_error("无法定位前台工具");
            request.executable_path=std::filesystem::path(executable);
            if(!prepare_recoil_calibration(request,prepared,error)){std::cerr<<error<<'\n';return 1;}
            std::cout<<prepared.launch_command<<'\n';return 0;
        }
        if(argc==3&&std::string(argv[1])=="inspect") {
            if(!load_recoil_calibration_prepared(argv[2],prepared,error)){std::cerr<<error<<'\n';return 1;}
            std::cout<<"SCHEMA_VALID；武器="<<prepared.manifest.environment.weapon_id<<"；次数=1；总counts="
                <<prepared.manifest.limits.max_sent_l1_counts<<"；校准结果=null\n"<<prepared.launch_command<<'\n';return 0;
        }
        if(argc>=2&&std::string(argv[1])=="run") {
            // 缺任一标志、额外参数或确认串不符均在设备工厂之前返回。
            if(argc!=6||std::string(argv[3])!="--allow-physical-output"||std::string(argv[4])!="--confirm")
                throw std::runtime_error("run必须提供会话目录、--allow-physical-output和--confirm绑定确认串");
            if(!verify_recoil_calibration_launch(argv[2],true,argv[5],prepared,error)){std::cerr<<error<<'\n';return 1;}
            if(!SetConsoleCtrlHandler(control,TRUE))throw std::runtime_error("无法设置前台取消处理器");
            return run(std::move(prepared));
        }
        std::cout<<"用法：prepare PROFILE CONFIG REQUEST NEW_DIR | inspect SESSION | run SESSION --allow-physical-output --confirm TOKEN\n";
        return argc==2&&std::string(argv[1])=="--help"?0:1;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
