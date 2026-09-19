#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "weapon/weapon_timing.h"
#include "weapon/weapon_catalog.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>

namespace {
int failures = 0;
void expect(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "[失败] " << message << '\n'; }
}
void write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc); stream << text;
}
std::string read_text(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
} // namespace
int main() {
    using namespace weapon;
    auto catalog = default_timing_catalog();
    expect(gsi_name("cz75")=="weapon_cz75a" && gsi_name("M4A1-S")=="weapon_m4a1_silencer" &&
        gsi_name("m4a4")=="weapon_m4a1" && gsi_name("weapon_m4a1")=="weapon_m4a1" &&
        gsi_name("sg553")=="weapon_sg556" && gsi_name("p2000")=="weapon_hkp2000" &&
        gsi_name("m4a1").empty() && gsi_name("unknown").empty(),"GSI导出复用明确目录映射且不猜歧义短名");
    for(const auto& row:kWeaponNames)
        expect(gsi_name(row.canonical_id)==row.gsi_name && normalize_weapon_id(gsi_name(row.canonical_id))==row.canonical_id,
            "全部目录名称往返保持现有武器对应关系");
    expect(valid_timing_catalog(catalog) && catalog.profiles.size() == 33, "完整默认资料");
    expect(find_timing(catalog,"deagle")->fire_interval_ms == 600 &&
        find_timing(catalog,"deagle")->shot_hold_ms == 60, "沙鹰保持人工测试值");
    expect(!find_timing(catalog,"revolver")->enabled &&
        find_timing(catalog,"revolver")->shot_hold_ms == 300, "R8仅留档");
    expect(find_timing(catalog,"nova") && find_timing(catalog,"mp5sd") && !find_timing(catalog,"Nowa") &&
        !find_timing(catalog,"g3sg1"), "采用协议canonical ID，不猜测未测试武器");
    expect(find_timing(catalog,"M4A1-S")->canonical_id == "m4a1_s" &&
        find_timing(catalog,"weapon_m4a1")->canonical_id == "m4a4" && !find_timing(catalog,"m4a1"),
        "共享资料接受明确别名且区分M4A4与M4A1-S");
    for (const auto& profile : catalog.profiles) if (profile.enabled)
        expect(profile.shot_hold_ms >= 1 && profile.shot_hold_ms <= 500 &&
            profile.fire_interval_ms > profile.shot_hold_ms && profile.fire_interval_ms <= 2000,
            "全部启用默认资料符合扳机参数范围");
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH,temp);
    wchar_t file[MAX_PATH]{};
    GetTempFileNameW(temp,L"wtt",0,file);
    const std::filesystem::path path(file);
    std::string error;
    expect(save_timing_catalog(path,catalog,error), "默认资料原子保存");
    const auto original = read_text(path);
    const auto saved = nlohmann::json::parse(original);
    expect(saved.at("schema_version")==2,"保存统一使用GSI命名版本2");
    expect(saved.size()==3&&!saved.contains("source")&&!saved.contains("calibration"),"schema2仅保留版本和武器条目");
    for(std::size_t i=0;i<catalog.profiles.size();++i) {
        const auto& row=saved.at("profiles")[i];
        const auto* name=static_cast<const NameProfile*>(nullptr);
        for(const auto& item:kWeaponNames)if(item.canonical_id==catalog.profiles[i].canonical_id)name=&item;
        expect(name && row.contains("weapon_id") && row.at("weapon_id").get<std::string>()==name->gsi_name && !row.contains("canonical_id"),
            "保存目录中的实际GSI原名，不拼接canonical短名");
    }
    auto legacy=saved;legacy["schema_version"]=1;legacy["source"]=timing_catalog_source();
    for(std::size_t i=0;i<catalog.profiles.size();++i) {
        legacy["profiles"][i].erase("weapon_id");
        legacy["profiles"][i]["canonical_id"]=catalog.profiles[i].canonical_id;
    }
    write_text(path,legacy.dump());
    auto legacy_loaded=default_timing_catalog();
    expect(load_timing_catalog(path,legacy_loaded,error),"旧schema1 canonical资料仍可载入");
    for(std::size_t i=0;i<catalog.profiles.size();++i)
        expect(legacy_loaded.profiles[i].canonical_id==catalog.profiles[i].canonical_id &&
            legacy_loaded.profiles[i].shot_hold_ms==catalog.profiles[i].shot_hold_ms &&
            legacy_loaded.profiles[i].fire_interval_ms==catalog.profiles[i].fire_interval_ms &&
            legacy_loaded.profiles[i].enabled==catalog.profiles[i].enabled,"旧版迁移不改变内部ID或任何时间值");
    write_text(path,original);
    auto loaded = default_timing_catalog();
    expect(load_timing_catalog(path,loaded,error), "默认资料读取");
    catalog.revision = 2;
    catalog.profiles[0].shot_hold_ms = 70;
    catalog.profiles[0].fire_interval_ms = 400;
    catalog.profiles[0].enabled = false;
    expect(save_timing_catalog(path,catalog,error) && load_timing_catalog(path,loaded,error) &&
        loaded.revision == 2 && loaded.profiles[0].shot_hold_ms == 70 &&
        loaded.profiles[0].fire_interval_ms == 400 && !loaded.profiles[0].enabled, "编辑版本与参数完整往返");
    const auto good = nlohmann::json::parse(original);
    const auto reject = [&](const nlohmann::json& value, const char* message) {
        write_text(path,value.dump());
        expect(!load_timing_catalog(path,loaded,error) && !error.empty() &&
            loaded.revision == 2 && loaded.profiles[0].shot_hold_ms == 70, message);
    };
    auto bad=good; bad["schema_version"]=3; reject(bad,"未知版本拒绝且保留已载入资料");
    bad=good; bad["revision"]=0; reject(bad,"零编辑版本拒绝");
    bad=good; bad["revision"]=-1; reject(bad,"负编辑版本拒绝");
    bad=good; bad["extra"]=1; reject(bad,"未知根字段拒绝");
    bad=good; bad["source"]=timing_catalog_source();reject(bad,"schema2拒绝混入旧来源字段");
    bad=legacy;bad.erase("source");reject(bad,"schema1仍要求旧来源字段");
    bad=legacy;bad["source"]="unknown";reject(bad,"schema1仍核验旧来源身份");
    bad=good; bad["profiles"][0]["extra"]=1; reject(bad,"未知行字段拒绝");
    bad=good; bad["profiles"][0]["weapon_id"]="unknown"; reject(bad,"未知武器拒绝");
    bad=good; bad["profiles"][0]["weapon_id"]="p250"; reject(bad,"schema2只接受GSI原名，不混用内部短名");
    bad=good; bad["profiles"][0]["canonical_id"]="p250"; reject(bad,"同条目混合新旧身份字段拒绝");
    bad=good; bad["profiles"][0].erase("weapon_id");bad["profiles"][0]["canonical_id"]="p250";
    reject(bad,"schema2不混入schema1条目");
    bad=legacy;bad["profiles"][1]["canonical_id"]="weapon_p250";reject(bad,"旧别名归一后重复武器拒绝");
    bad=good; bad["profiles"][1]=bad["profiles"][0]; reject(bad,"重复武器拒绝");
    bad=good; bad["profiles"].erase(0); reject(bad,"缺项拒绝");
    bad=good; bad["profiles"][0]["shot_hold_ms"]=true; reject(bad,"布尔值不能当时长");
    bad=good; bad["profiles"][0]["shot_hold_ms"]=60.0; reject(bad,"浮点时长拒绝");
    bad=good; bad["profiles"][0]["shot_hold_ms"]=0; reject(bad,"零时长拒绝");
    bad=good; bad["profiles"][0]["shot_hold_ms"]=501; reject(bad,"过长按住拒绝");
    bad=good; bad["profiles"][0]["fire_interval_ms"]=2001; reject(bad,"过长间隔拒绝");
    bad=good; bad["profiles"][0]["fire_interval_ms"]=60; reject(bad,"间隔必须超过按住时长");
    bad=good; bad["profiles"][6]["enabled"]=true; reject(bad,"R8不可启用");
    write_text(path,"{\"schema_version\":1,\"schema_version\":1,"+original.substr(1));
    expect(!load_timing_catalog(path,loaded,error), "重复JSON键拒绝");
    write_text(path,std::string(65537,' '));
    expect(!load_timing_catalog(path,loaded,error), "超过64KiB拒绝");
    write_text(path,original);
    auto invalid = catalog; invalid.profiles[0].fire_interval_ms=1;
    expect(!save_timing_catalog(path,invalid,error) && read_text(path)==original, "无效编辑不覆盖原文件");
    invalid=catalog; invalid.profiles[0].canonical_id=invalid.profiles[1].canonical_id;
    expect(!valid_timing_catalog(invalid), "内存编辑重复ID也拒绝");
    std::filesystem::remove(path);
    expect(!load_timing_catalog(path,loaded,error) && loaded.revision==2, "缺失文件明确失败保留旧快照");
    const auto nested = path / "new" / "profiles" / "weapon-timing.json";
    expect(save_timing_catalog(nested,catalog,error) && load_timing_catalog(nested,loaded,error),
        "首次保存创建多层父目录并可读取");
    std::filesystem::remove(nested);
    std::filesystem::remove(nested.parent_path());
    std::filesystem::remove(nested.parent_path().parent_path());
    std::filesystem::remove(path);
    std::cout << (failures ? "武器点射资料测试失败\n" : "武器点射资料测试通过\n");
    return failures ? 1 : 0;
}
