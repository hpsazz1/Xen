#include "recoil/recoil_store.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
namespace {
int failures=0;
void expect(bool value,const char* message){if(!value){++failures;std::cerr<<message<<'\n';}}
RecoilProfile profile(){RecoilProfile p;p.id="synthetic";p.weapon_id="test_weapon";
    p.state=RecoilProfileState::CALIBRATED;p.phase_tolerance_ms=20;p.recovery_ms=100;
    p.calibration={"test_build","kmbox_net","test_conditions","synthetic:test_only",1.0};
    p.source.sha256=std::string(64,'a');p.points={{0,0,0},{10,1,2}};return p;}
}
int main(int argc, char** argv){
    // 实际本地导入产物的只读验收；不设置活动版本、不启动设备。
    if (argc == 3 && std::string_view(argv[1]) == "--validate-imported") {
        RecoilStore imported(std::filesystem::u8path(argv[2]));
        std::vector<RecoilStoredProfile> profiles; std::string error;
        if (!imported.list(profiles, error) || profiles.empty()) {
            std::cerr << "导入目录不可读取或为空：" << error << '\n'; return 1;
        }
        for (const auto& entry : profiles) {
            const auto& value = *entry.profile;
            if (value.state != RecoilProfileState::IMPORTED || value.phase_tolerance_ms ||
                value.recovery_ms || !value.calibration.evidence.empty()) {
                std::cerr << "候选状态不符：" << entry.file << '\n'; return 1;
            }
            std::cout << entry.file << " | " << value.weapon_id << " | " << value.points.size() << '\n';
        }
        std::cout << "生产解析器通过 " << profiles.size() << " 份导入候选；未激活。\n";
        return 0;
    }
    if (argc != 1) return 2;
    auto directory=std::filesystem::temp_directory_path()/("xen-recoil-store-test-"+std::to_string(RecoilClock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    RecoilStore store(directory);std::string error,file1,file2,file3;
    auto p=profile();
    expect(store.save_new(p,file1,error),"默认候选保存");
    RecoilProfile loaded;expect(store.load(file1,loaded,error)&&loaded.state==RecoilProfileState::SCHEMA_VALID,"默认保存降级候选");
    expect(!store.set_active(p.weapon_id,file1,error),"未校准候选不能活动发布");
    expect(store.save_new(p,file2,error,true)&&file1!=file2,"显式校准保存新revision不覆盖");
    RecoilConfig cfg;cfg.game_build="test_build";cfg.input_path="kmbox_net";cfg.conditions="test_conditions";cfg.sensitivity=1;
    expect(!store.resolve(cfg,p.weapon_id,error),"目录有校准版但无活动索引不能任取");
    expect(store.set_active(p.weapon_id,file2,error),"显式发布已校准版本");
    auto resolved=store.resolve(cfg,p.weapon_id,error);expect(resolved&&resolved->revision==2,"活动引用精确解析");
    cfg.sensitivity=2;expect(!store.resolve(cfg,p.weapon_id,error),"灵敏度失配拒绝");cfg.sensitivity=1;
    cfg.game_build.clear();cfg.conditions.clear();expect(bool(store.resolve(cfg,p.weapon_id,error)),"版本与条件可空且不阻断已校准活动曲线");
    cfg.game_build="other_build";cfg.conditions="other_conditions";
    expect(bool(store.resolve(cfg,p.weapon_id,error)),"旧版本与条件仅作元数据不参与运行匹配");
    cfg.game_build="test_build";cfg.conditions="test_conditions";
    cfg.input_path="other";expect(!store.resolve(cfg,p.weapon_id,error),"输入路径失配拒绝");cfg.input_path="kmbox_net";
    expect(!store.resolve(cfg,"other_weapon",error),"不猜相似武器");
    p.points.back().y_counts=3;expect(store.save_new(p,file3,error,true),"新校准修订保存");
    expect(store.set_active(p.weapon_id,file3,error)&&store.resolve(cfg,p.weapon_id,error)->revision==3,"第二次发布");
    expect(store.rollback(p.weapon_id,error)&&store.resolve(cfg,p.weapon_id,error)->revision==2,"显式回退原版本");
    expect(store.load(file3,loaded,error)&&loaded.revision==3,"可读取其它保存版本用于调试");
    expect(store.resolve(cfg,p.weapon_id,error)->revision==2,"读取其它版本不改变活动曲线选择");
    expect(!store.set_active(p.weapon_id,file1,error),"未校准候选不能替换已有活动版本");
    expect(store.resolve(cfg,p.weapon_id,error)->revision==2,"拒绝未校准发布后保留原活动版本");
    expect(!store.load("../escape.json",loaded,error),"拒绝目录穿越");
    std::vector<RecoilStoredProfile> profiles;expect(store.list(profiles,error)&&profiles.size()==3,"限量列举保存版本");
    auto manual=p;manual.id="manual";
    expect(confirm_recoil_profile(manual,error),"人工确认不需要用户填来源或校准声明");
    std::string manual_file;
    expect(store.save_new(manual,manual_file,error,true),"人工确认保存新版本");
    expect(store.load(manual_file,loaded,error)&&loaded.state==RecoilProfileState::USER_CONFIRMED&&
        loaded.source.sha256.size()==64&&!loaded.phase_tolerance_ms&&!loaded.recovery_ms,"精简存储恢复身份及内容摘要，不伪造实测");
    {std::ifstream stream(directory/manual_file);std::string content((std::istreambuf_iterator<char>(stream)),{});
        expect(content.find("source")==std::string::npos&&content.find("calibration")==std::string::npos&&
            content.find("phase")==std::string::npos,"用户JSON移除来源校准及软件预算节点");}
    expect(store.set_active(p.weapon_id,manual_file,error)&&store.resolve(cfg,p.weapon_id,error)->state==RecoilProfileState::USER_CONFIRMED,
        "用户确认曲线可作为本武器活动版本");
    cfg.sensitivity=2;expect(!store.resolve(cfg,p.weapon_id,error),"人工确认灵敏度失配仍拒绝");cfg.sensitivity=1;
    cfg.input_path="other";expect(!store.resolve(cfg,p.weapon_id,error),"人工确认输入路径失配仍拒绝");cfg.input_path="kmbox_net";
    std::string draft_file;
    expect(store.save_new(manual,draft_file,error)&&store.load(draft_file,loaded,error)&&
        loaded.state==RecoilProfileState::SCHEMA_VALID&&!loaded.execution_phase_budget_ms,
        "普通保存清除用户确认与软件预算");
    expect(!store.set_active(p.weapon_id,draft_file,error),"未确认精简候选不能启用");
    expect(store.rollback(p.weapon_id,error)&&store.resolve(cfg,p.weapon_id,error)->state==RecoilProfileState::CALIBRATED,
        "人工确认版本可回退既有实测版本");
    auto named=p;named.weapon_id="ak47";named.id="different_curve";named.revision=1;
    expect(confirm_recoil_profile(named,error),"目录武器人工确认");
    std::string named_file,named_next;
    expect(store.save_new(named,named_file,error,true)&&named_file=="weapon_ak47-r1.json","文件名使用GSI规范名");
    expect(store.set_active("weapon_ak47",named_file,error)&&bool(store.resolve(cfg,"ak47",error)),"GSI活动键与内部短名兼容");
    named.id="another_curve";
    expect(store.save_new(named,named_next,error,true)&&named_next=="weapon_ak47-r2.json","同武器不同曲线ID共享递增文件版本避免冲突");
    {std::ofstream index(directory/"active.json");index<<"{\"schema_version\":1,\"active\":{\"ak47\":{\"file\":\""<<named_file<<"\",\"previous\":\"\"}}}";}
    expect(bool(store.resolve(cfg,"weapon_ak47",error)),"旧短名活动索引兼容读取");
    expect(store.set_active("ak47",named_next,error)&&store.rollback("weapon_ak47",error),"旧索引切换后使用GSI键且可回退");
    {std::ifstream index(directory/"active.json");std::string content((std::istreambuf_iterator<char>(index)),{});
        expect(content.find("weapon_ak47")!=std::string::npos&&content.find("\"ak47\"")==std::string::npos,"写活动索引只保留GSI名称");}
    RecoilProfile discrete;
    expect(load_recoil_profile(R"({"schema_version":3,"id":"discrete_store","revision":1,"weapon_id":"ak47",
        "sensitivity":1,"verified":true,"sample_semantics":"discrete_delta","events":[[5,0.5,-0.5],[10,2.5,-2.5]]})",
        discrete,error),"离散曲线可由生产存储入口加载");
    std::string discrete_file;
    expect(store.save_new(discrete,discrete_file,error,true)&&store.load(discrete_file,loaded,error)&&
        loaded.schema_version==3&&loaded.events.size()==2&&!loaded.execution_phase_budget_ms,
        "新版本存储保留离散事件及无相位预算语义");
    expect(store.set_active("ak47",discrete_file,error)&&store.resolve(cfg,"ak47",error)->schema_version==3,
        "活动索引解析离散版本");
    expect(store.rollback("ak47",error)&&store.resolve(cfg,"ak47",error)->schema_version==1,
        "离散迁移可回退原累计版本");
    {std::ofstream broken(directory/"broken.json");broken<<"{}";}
    expect(!store.list(profiles,error),"损坏候选显式失败不假成功");
    std::filesystem::remove_all(directory);
    return failures?1:0;
}
