#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "weapon/weapon_timing.h"
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
    auto bad=good; bad["schema_version"]=2; reject(bad,"未知版本拒绝且保留已载入资料");
    bad=good; bad["revision"]=0; reject(bad,"零编辑版本拒绝");
    bad=good; bad["revision"]=-1; reject(bad,"负编辑版本拒绝");
    bad=good; bad["extra"]=1; reject(bad,"未知根字段拒绝");
    bad=good; bad["profiles"][0]["extra"]=1; reject(bad,"未知行字段拒绝");
    bad=good; bad["profiles"][0]["canonical_id"]="unknown"; reject(bad,"未知武器拒绝");
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
