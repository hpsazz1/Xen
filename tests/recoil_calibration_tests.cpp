#include "recoil/recoil_calibration_io.h"
#include "recoil/recoil_store.h"
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <climits>

namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
RecoilTime time(int ms){return RecoilTime{}+std::chrono::milliseconds(1000+ms);}
std::shared_ptr<const RecoilProfile> profile() {
    RecoilProfile p;p.id="synthetic";p.weapon_id="ak47";p.state=RecoilProfileState::SCHEMA_VALID;
    p.points={{0,0,0},{10,2,2},{20,4,4}};return std::make_shared<const RecoilProfile>(p);
}
RecoilCalibrationManifest manifest(const RecoilProfile& p) {
    RecoilCalibrationManifest m;m.session_id="test-calibration";
    m.profile_file_sha256=m.profile_semantic_sha256=recoil_calibration_sha256(serialize_recoil_profile(p));
    m.config_binding_sha256=std::string(64,'a');m.environment={"ak47","synthetic","kmbox_net","test-only",1};
    m.environment_fingerprint=recoil_calibration_environment_fingerprint(m.environment);
    m.limits={1,1000,100,10,8,16,14,20};m.hold_virtual_key=0x12;m.cancel_virtual_key=0x23;return m;
}
auto permit(std::shared_ptr<const RecoilProfile> p){std::string error;auto result=authorize_recoil_calibration(manifest(*p),p,time(0),error);check(bool(result),error.c_str());return result;}
void contracts() {
    auto p=profile();auto m=manifest(*p);std::string error;
    check(recoil_calibration_sha256("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA256标准向量");
    check(validate_recoil_calibration_manifest(m,*p,error),error.c_str());
    auto changed=*p;changed.points[1].x_counts+=1;
    check(!validate_recoil_calibration_manifest(m,changed,error),"点变化拒绝");
    changed=*p;changed.state=RecoilProfileState::IMPORTED;m=manifest(changed);
    check(!validate_recoil_calibration_manifest(m,changed,error),"IMPORTED不得借校准旁路执行");
    m=manifest(*p);m.environment.conditions="different";
    check(!validate_recoil_calibration_manifest(m,*p,error),"环境哈希变化拒绝");
    m=manifest(*p);m.limits.max_firing_sessions=2;
    check(!validate_recoil_calibration_manifest(m,*p,error),"多弹序拒绝");
    m=manifest(*p);m.limits.command_phase_budget_ms=std::numeric_limits<double>::quiet_NaN();
    check(!validate_recoil_calibration_manifest(m,*p,error),"NaN拒绝");
    m=manifest(*p);m.hold_virtual_key=1;
    check(!validate_recoil_calibration_manifest(m,*p,error),"保持键不能是左键");
    m=manifest(*p);check(!authorize_recoil_calibration(m,p,RecoilTime{},error),"空授权时间拒绝");
    check(!authorize_recoil_calibration(m,p,RecoilTime::max(),error),"到期溢出拒绝");
}
void budgets() {
    auto auth=permit(profile());RecoilCalibrationBudget b(auth);
    check(b.begin_firing(time(1)),"一次起弹");check(b.reserve(4,-4,time(2)),"L1记8而非抵消");
    check(b.reserve(-1,1,time(3)),"恰好额度");check(!b.reserve(1,0,time(4)),"差一count拒绝");
    check(b.snapshot().sent_l1_counts==10&&b.snapshot().terminal,"拒绝不截断命令");
    RecoilCalibrationBudget unknown(auth);check(unknown.begin_firing(time(1))&&unknown.reserve(2,2,time(2)),"未知前真实调用预留");
    unknown.finish(RecoilCalibrationEnd::UNKNOWN_RECEIPT);check(unknown.snapshot().sent_l1_counts==4&&!unknown.reserve(1,1,time(3)),"UNKNOWN不退款不复活");
    RecoilCalibrationBudget no_start(auth);check(!no_start.reserve(1,1,time(0)),"未起弹不能预留");
    RecoilCalibrationBudget twice(auth);check(twice.begin_firing(time(1))&&!twice.begin_firing(time(2)),"零平台也消费一次");
    RecoilCalibrationBudget late(auth);check(!late.check_time(time(1000)),"arm超时无续期");
    RecoilCalibrationBudget fire_late(auth);check(fire_late.begin_firing(time(1))&&!fire_late.check_time(time(101)),"弹序到期停止");
    RecoilCalibrationBudget backwards(auth);check(backwards.check_time(time(2))&&!backwards.check_time(time(1)),"时间倒退停止");
    RecoilCalibrationBudget extreme(auth);check(extreme.begin_firing(time(1))&&!extreme.reserve(INT_MIN,INT_MIN,time(2)),"INT_MIN绝对值不能溢出绕过预算");
}
void controllers() {
    auto p=profile();RecoilInput input;input.profile=p;input.enabled=input.healthy=input.focused=input.permission=input.profile_conditions_match=true;
    input.device_epoch=input.weapon_generation=1;
    RecoilController ordinary;check(ordinary.advance(input,time(0)).snapshot.reason==RecoilReason::UNCALIBRATED,"普通执行拒绝候选");
    auto auth=permit(p);RecoilController c(auth), reuse(auth);
    check(reuse.advance(input,time(0)).snapshot.reason==RecoilReason::UNCALIBRATED,"新Controller不能重用已消费permit");
    input.held=true;check(!c.advance(input,time(0)).has_intent,"已按下启动不发送");
    input.held=false;c.advance(input,time(1));input.held=true;input.firing_started_at=time(2);c.advance(input,time(2));
    auto intent=c.advance(input,time(12));check(intent.has_intent&&intent.intent.dx_counts==2,"校准候选正常求值");
    c.acknowledge({intent.intent.command_id,RecoilReceiptStatus::ACKNOWLEDGED,time(12)},time(12));
    input.held=false;c.advance(input,time(13));input.held=true;input.recovery_qualified=true;
    check(!c.advance(input,time(14)).has_intent&&c.snapshot().session_id==1,"释放与伪恢复不能重复试验");
    check(p->state==RecoilProfileState::SCHEMA_VALID&&!p->phase_tolerance_ms&&p->calibration.evidence.empty(),"校准不改profile状态或证据");
    RecoilController mismatch(permit(p));input.profile=std::make_shared<const RecoilProfile>(*p);
    check(mismatch.advance(input,time(0)).snapshot.reason==RecoilReason::UNCALIBRATED,"未绑定对象不能顶替许可曲线");
}
void files(const std::filesystem::path& root) {
    auto p=profile();const auto source=root/"source.json";{std::ofstream output(source);output<<serialize_recoil_profile(*p);}
    AppConfig config;config.gsi.enabled=true;config.gsi.expected_player_id="76561198000000000";
    config.source_context.enabled=true;config.source_context.host="127.0.0.1";config.source_context.port=5014;
    config.source_context.process_name="synthetic.exe";
    const auto cfg=root/"config.ini";std::string error;check(save_app_config(cfg.string(),config,error),error.c_str());
    RecoilCalibrationPrepareRequest request;request.profile_path=source;request.config_path=cfg;
    request.output_directory=root/"session";request.executable_path=root/"tool.exe";
    {std::ofstream executable(request.executable_path);executable<<"test-only-placeholder";}
    const auto m=manifest(*p);request.environment=m.environment;request.limits=m.limits;request.hold_virtual_key=m.hold_virtual_key;request.cancel_virtual_key=m.cancel_virtual_key;
    RecoilCalibrationPrepared prepared;check(prepare_recoil_calibration(request,prepared,error),error.c_str());
    check(std::filesystem::exists(prepared.directory/"TASK.md")&&!std::filesystem::exists(prepared.directory/"CONSUMED"),"prepare只准备");
    RecoilCalibrationPrepared loaded;
    check(!verify_recoil_calibration_launch(prepared.directory,false,prepared.confirmation,loaded,error),"无物理旗标拒绝");
    check(!verify_recoil_calibration_launch(prepared.directory,true,"WRONG",loaded,error),"错误绑定串拒绝");
    check(verify_recoil_calibration_launch(prepared.directory,true,prepared.confirmation,loaded,error),error.c_str());
    check(consume_recoil_calibration_session(loaded,error)&&!consume_recoil_calibration_session(loaded,error),"创建式单次消费");
    check(!verify_recoil_calibration_launch(prepared.directory,true,prepared.confirmation,loaded,error),"重放拒绝");
    check(write_recoil_calibration_result(prepared,"TEST_ONLY",{},"INCOMPLETE",error),error.c_str());
    check(!write_recoil_calibration_result(prepared,"OVERWRITE",{},"COMPLETE",error),"报告不可覆盖");
    RecoilStore store(root/"profiles");std::string file;check(store.save_new(*p,file,error),error.c_str());
    RecoilConfig rc;rc.use_trial=true;rc.trial_file=file;rc.game_build="synthetic";rc.conditions="test-only";rc.sensitivity=1;
    check(!store.resolve(rc,"ak47",error),"普通use_trial保持拒绝SCHEMA_VALID");
    check(!store.set_active("ak47",file,error),"不得发布未校准版本");
    {std::ofstream append(cfg,std::ios::app);append<<"\n;changed\n";}
    check(!load_recoil_calibration_prepared(prepared.directory,loaded,error),"配置变化需要重新准备");
}
}
int main() {
    const auto root=std::filesystem::temp_directory_path()/("xen-calibration-test-"+std::to_string(RecoilClock::now().time_since_epoch().count()));
    try {std::filesystem::create_directory(root);contracts();budgets();controllers();files(root);std::filesystem::remove_all(root);
        std::cout<<"recoil_calibration_tests passed\n";return 0;}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
