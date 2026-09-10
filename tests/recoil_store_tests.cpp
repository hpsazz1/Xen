#include "recoil/recoil_store.h"
#include <filesystem>
#include <fstream>
#include <iostream>
namespace {
int failures=0;
void expect(bool value,const char* message){if(!value){++failures;std::cerr<<message<<'\n';}}
RecoilProfile profile(){RecoilProfile p;p.id="synthetic";p.weapon_id="test_weapon";
    p.state=RecoilProfileState::CALIBRATED;p.phase_tolerance_ms=20;p.recovery_ms=100;
    p.calibration={"test_build","kmbox_net","test_conditions","synthetic:test_only",1.0};
    p.source.sha256=std::string(64,'a');p.points={{0,0,0},{10,1,2}};return p;}
}
int main(){
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
    cfg.game_build.clear();expect(!store.resolve(cfg,p.weapon_id,error),"未知build拒绝");cfg.game_build="test_build";
    cfg.input_path="other";expect(!store.resolve(cfg,p.weapon_id,error),"输入路径失配拒绝");cfg.input_path="kmbox_net";
    expect(!store.resolve(cfg,"other_weapon",error),"不猜相似武器");
    p.points.back().y_counts=3;expect(store.save_new(p,file3,error,true),"新校准修订保存");
    expect(store.set_active(p.weapon_id,file3,error)&&store.resolve(cfg,p.weapon_id,error)->revision==3,"第二次发布");
    expect(store.rollback(p.weapon_id,error)&&store.resolve(cfg,p.weapon_id,error)->revision==2,"显式回退原版本");
    cfg.use_trial=true;cfg.trial_file=file3;expect(store.resolve(cfg,p.weapon_id,error)->revision==3,"试验引用独立选择");
    cfg.use_trial=false;expect(store.resolve(cfg,p.weapon_id,error)->revision==2,"试验不覆盖活动版本");
    cfg.use_trial=true;cfg.trial_file=file1;expect(!store.resolve(cfg,p.weapon_id,error),"试验也不可执行未校准候选");
    expect(!store.load("../escape.json",loaded,error),"拒绝目录穿越");
    std::vector<RecoilStoredProfile> profiles;expect(store.list(profiles,error)&&profiles.size()==3,"限量列举保存版本");
    {std::ofstream broken(directory/"broken.json");broken<<"{}";}
    expect(!store.list(profiles,error),"损坏候选显式失败不假成功");
    std::filesystem::remove_all(directory);
    return failures?1:0;
}
