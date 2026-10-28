#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "debug_session/movement_library_internal.h"
#include "movement/timeline.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using namespace debug_session::detail;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
nlohmann::json read(const std::string& path) {
    std::ifstream stream(fs::u8path(path));
    return nlohmann::json::parse(stream);
}
std::string bytes(const fs::path& path) {
    std::ifstream file(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
}
void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary); file << text;
    require(static_cast<bool>(file), "测试文件创建失败");
}
void archive_contract(const fs::path& root) {
    const auto domain = root / "grenade", plans = domain / "plans";
    movement::timeline::Plan plan; plan.name = "T垃圾桶左 拱门烟"; plan.map = "de_mirage";
    plan.team = "T"; plan.actions.push_back({});
    const auto document = movement::timeline::plan_json(plan);
    const auto selected = plans / "sample.json", other = plans / "other.json";
    const auto reference = domain / "references" / "sample" / "aim.png";
    const auto capture = domain / "captures" / "sample.json";
    const auto standing = domain / "standing" / "sample.png";
    const auto shared = domain / "standing" / "shared" / "de_mirage-T.png";
    const auto historical = domain / "packages" / "de_mirage" / "historical.zip";
    write(selected, document.dump()); write(other, document.dump());
    write(reference,"original aim"); write(capture,"original capture"); write(standing,"original standing");
    write(shared,"shared standing"); write(historical,"historical package");
    const auto backup = fs::u8path(archive_movement_plan(utf8(selected),utf8(plans)));
    require(backup.parent_path() == domain / "backups" / "deleted-plans" &&
        !fs::exists(selected) && !fs::exists(reference.parent_path()) && !fs::exists(capture) && !fs::exists(standing),
        "删除必须移出本方案及独占活动资源到同域归档");
    require(bytes(backup / "plans" / "sample.json") == document.dump() &&
        bytes(backup / "references" / "sample" / "aim.png") == "original aim" &&
        bytes(backup / "captures" / "sample.json") == "original capture" &&
        bytes(backup / "standing" / "sample.png") == "original standing",
        "归档必须保留原方案及图片记录字节");
    require(fs::exists(other) && fs::exists(shared) && fs::exists(historical) &&
        list_movement_plans(utf8(plans)).size() == 1,
        "删除不得改动其他方案、共享站位或历史包，归档不得继续列入现役库");
    const auto reject = [&](const fs::path& path) {
        bool failed = false;
        try { archive_movement_plan(utf8(path),utf8(plans)); } catch (const std::exception&) { failed = true; }
        require(failed && fs::exists(other) && read(utf8(other)) == document,"非法删除目标必须拒绝且不伤及现役方案");
    };
    write(domain / "external.json",document.dump()); write(plans / "nested" / "inside.json",document.dump());
    fs::create_directory(plans / "directory.json");
    reject(selected); reject(domain / "external.json"); reject(plans / "nested" / "inside.json");
    reject(plans / "directory.json"); reject(plans / "missing.json");

    // 最后一步方案移动失败时，之前已移出的图片和采集记录必须恢复。
    write(reference,"original aim"); write(capture,"original capture"); write(standing,"original standing");
    write(selected,document.dump());
    const HANDLE locked = CreateFileW(selected.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,
        OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(locked != INVALID_HANDLE_VALUE,"删除回滚夹具占用失败");
    bool rolled_back = false;
    try { archive_movement_plan(utf8(selected),utf8(plans)); }
    catch (const std::exception& error) { rolled_back = std::string(error.what()).find("已恢复") != std::string::npos; }
    CloseHandle(locked);
    require(rolled_back && bytes(selected) == document.dump() && bytes(reference) == "original aim" &&
        bytes(capture) == "original capture" && bytes(standing) == "original standing",
        "归档方案被占用时必须恢复先前移出的独占资源");
}
void run(const fs::path& root) {
    const auto library = utf8(root / fs::u8path("身法方案"));
    const auto legacy = utf8(root / "legacy");
    require(list_movement_plans(library, legacy).empty(), "缺失目录应返回空列表");
    movement::timeline::Plan plan;
    plan.name = "中文方案"; plan.map = "训练地图"; plan.location = "出生点";
    plan.actions = {{movement::timeline::ActionKind::KEY, movement::timeline::Key::SPACE, 0, 100, 25}};
    auto document = movement::timeline::plan_json(plan);
    const auto first = save_movement_plan(document, "", false, library);
    require(read(first) == document, "保存载入往返不一致");
    auto listed = list_movement_plans(library, legacy);
    require(listed.size() == 1 && listed[0].path == first && listed[0].name.find(plan.name) != std::string::npos &&
        listed[0].name.find(plan.map) != std::string::npos && listed[0].name.find(plan.location) != std::string::npos,
        "方案名称地图点位或中文路径未保留");
    document["actions"][0]["duration_ms"] = 250;
    require(save_movement_plan(document, first, false, library) == first && read(first) == document,
        "修改已选方案未更新原文件");
    const auto copy = save_movement_plan(document, first, true, library);
    require(copy != first && read(copy) == document, "另存未保留原文件");
    listed = list_movement_plans(library, legacy);
    require(listed.size() == 2 && listed[0].name != listed[1].name, "重名方案标签不能区分");

    const auto old_path = fs::u8path(legacy) / "debug-old" / "plan.json";
    write(old_path, document.dump());
    write(fs::u8path(legacy) / "debug-other" / "plan.json", "{\"schema_version\":2}");
    write(fs::u8path(legacy) / "debug-broken" / "plan.json", "{broken");
    write(fs::u8path(legacy) / "unrelated" / "plan.json", document.dump());
    write(fs::u8path(legacy) / "debug-deep" / "nested" / "plan.json", document.dump());
    write(fs::u8path(library) / "bad.json", "bad json");
    auto invalid = document; invalid["actions"][0]["key"] = "ESC";
    write(fs::u8path(library) / "invalid.json", invalid.dump());
    require(list_movement_plans(library, legacy).size() == 2, "调试历史被错误列为已保存方案");
    auto edited = document; edited["name"] = "已修改";
    const auto imported = save_movement_plan(edited, utf8(old_path), false, library);
    require(imported != utf8(old_path) && read(utf8(old_path)) == document && read(imported) == edited,
        "编辑历史方案覆写历史报告");
    const auto external = root / "external.json";
    write(external, document.dump());
    const auto adopted = save_movement_plan(edited, utf8(external), false, library);
    require(read(utf8(external)) == document && adopted != utf8(external), "保存覆写外部文件");
    bool rejected = false;
    try { save_movement_plan(invalid, first, false, library); } catch (const std::exception&) { rejected = true; }
    require(rejected && read(first) == document, "无效方案破坏有效文件");

    // 文件被占用时验证原子替换失败不会截断原文件，并清理临时文件。
    const HANDLE locked = CreateFileW(fs::u8path(first).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(locked != INVALID_HANDLE_VALUE, "测试文件占用失败");
    rejected = false;
    try { save_movement_plan(edited, first, false, library); } catch (const std::exception&) { rejected = true; }
    CloseHandle(locked);
    require(rejected && read(first) == document, "替换失败破坏原方案");
    for (const auto& entry : fs::directory_iterator(fs::u8path(library)))
        require(entry.path().extension() != ".tmp", "失败后残留临时文件");

    auto voiced = document;
    voiced["map"] = "de_dust2"; voiced["voice_command"] = "跳箱";
    const auto bound = save_movement_plan(voiced, "", false, library);
    require(save_movement_plan(voiced, bound, false, library) == bound, "更新自身误报口令冲突");
    const auto before = list_movement_plans(library, legacy);
    bool structured = false;
    for (const auto& entry : before)
        if (entry.path == bound) structured = entry.map == "de_dust2" && entry.voice_command == "跳箱";
    require(structured, "目录未提供结构化地图及口令");
    for (const bool save_as : {false, true}) {
        const auto saved = save_movement_plan(voiced, save_as ? bound : first, save_as, library);
        require(read(saved) == voiced && read(bound) == voiced,
            "历史语音字段应保留，但不得继续阻止同地图另存或保存");
    }
    auto other_map = voiced; other_map["map"] = "de_mirage";
    require(read(save_movement_plan(other_map, bound, true, library)) == other_map, "跨地图同口令被拒绝");
    auto manual = voiced; manual["voice_command"] = "";
    require(read(save_movement_plan(manual, bound, true, library)) == manual && read(bound) == voiced,
        "清空口令另存失败或改写原方案");
    auto legacy_plan = document; legacy_plan["schema_version"] = 2; legacy_plan.erase("voice_command");
    const auto migrated = save_movement_plan(legacy_plan, "", false, library);
    require(read(migrated) == document, "旧格式保存未保留动作及空口令");
    archive_contract(root);
}
}
int main(int argc, char** argv) {
    const auto base = argc > 1 ? fs::u8path(argv[1]) : fs::path("temp/XEN-MOVEMENT-EDITOR-20261014-001");
    const auto root = fs::absolute(base /
        ("library-tests-" + std::to_string(GetCurrentProcessId()) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())));
    try {
        run(root);
        fs::remove_all(root);
        std::cout << "身法方案库专项通过\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::error_code ignored; fs::remove_all(root, ignored);
        return 1;
    }
}
