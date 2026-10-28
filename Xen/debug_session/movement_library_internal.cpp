#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "debug_session/movement_library_internal.h"
#include "movement/timeline.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <utility>

namespace debug_session::detail {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
std::string utf8(const fs::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
movement::timeline::Plan read_plan(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("无法打开身法方案");
    std::string text(16 * 1024 * 1024 + 1, '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(file.gcount()));
    if (file.bad() || text.size() > 16 * 1024 * 1024) throw std::runtime_error("身法方案读取失败或超过16MiB");
    std::vector<std::set<std::string>> keys;
    auto document = Json::parse(text, [&](int depth, Json::parse_event_t event, Json& value) {
        if (depth > 32) throw std::runtime_error("方案嵌套过深");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
            throw std::runtime_error("方案包含重复字段");
        if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    }, true, true);
    movement::timeline::Plan plan;
    std::string error;
    if (!movement::timeline::parse_plan(document, plan, error)) throw std::runtime_error(error);
    return plan;
}
void append_plan(std::vector<SavedMovementPlan>& result, const fs::path& path, const std::string& suffix) {
    try {
        if (fs::is_symlink(path) || !fs::is_regular_file(path)) return;
        const auto plan = read_plan(path);
        std::string name = plan.name;
        if (!plan.map.empty()) name += " / " + plan.map;
        if (!plan.location.empty()) name += " / " + plan.location;
        result.push_back({utf8(fs::absolute(path).lexically_normal()), name + " [" + suffix + "]",
            plan.map, plan.voice_command, plan.name});
    } catch (const std::exception&) { /* 单个坏文件不阻断方案下拉列表。 */ }
}
DWORD plain_attributes(const fs::path& path, bool allow_missing = false) {
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto code = GetLastError();
        if (allow_missing && (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)) return attributes;
        throw std::runtime_error("归档路径不存在或属性无法确认");
    }
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) throw std::runtime_error("归档路径不能包含重解析点");
    return attributes;
}
void plain_chain(const fs::path& path, bool allow_missing = false) {
    // 从叶到根逐层检查，不能只检查文件本身而漏掉父目录联接。
    for (auto current = path; !current.empty() && current != current.root_path(); current = current.parent_path())
        plain_attributes(current,allow_missing);
}
void plain_tree(const fs::path& path) {
    plain_chain(path);
    if (fs::is_directory(path))
        for (const auto& item : fs::recursive_directory_iterator(path)) {
            plain_attributes(item.path());
            if (!item.is_directory() && !item.is_regular_file())
                throw std::runtime_error("归档资源包含非常规文件");
        }
}
}
std::vector<SavedMovementPlan> list_movement_plans(const std::string& library_root, const std::string&) {
    std::vector<SavedMovementPlan> result;
    const auto library = fs::u8path(library_root);
    if (fs::exists(library)) {
        for (const auto& entry : fs::directory_iterator(library)) {
            if (entry.path().extension() == ".json")
                append_plan(result, entry.path(), "方案库/" + utf8(entry.path().filename()));
        }
    }
    // 测试/录制目录只作证据，不能自动变成用户显式保存的方案。
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return result;
}
std::string save_movement_plan(const Json& document, const std::string& selected_path, bool save_as,
    const std::string& library_root) {
    movement::timeline::Plan plan;
    std::string error;
    if (!movement::timeline::parse_plan(document, plan, error)) throw std::runtime_error(error);
    const auto content = movement::timeline::plan_json(plan).dump(2) + "\n";
    const auto root = fs::absolute(fs::u8path(library_root)).lexically_normal();
    fs::create_directories(root);
    fs::path destination;
    if (!save_as && !selected_path.empty()) {
        const auto selected = fs::absolute(fs::u8path(selected_path)).lexically_normal();
        std::error_code code;
        // 只覆盖用户已载入的有效库文件，不把历史 Run 或任意路径变成保存目标。
        if (selected.extension() == ".json" && !fs::is_symlink(selected, code) &&
            fs::is_regular_file(selected, code) && fs::equivalent(selected.parent_path(), root, code)) {
            read_plan(selected);
            destination = selected;
        }
    }
    wchar_t temporary_name[MAX_PATH]{};
    if (!GetTempFileNameW(root.c_str(), L"xmp", 0, temporary_name))
        throw std::runtime_error("无法创建身法方案临时文件");
    const fs::path temporary(temporary_name);
    const bool replace = !destination.empty();
    if (!replace) {
        // 临时文件由系统独占创建；新文件绝不使用覆盖标记。
        static std::atomic_uint64_t sequence{0};
        destination = root / ("plan-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count())
            + "-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++sequence) + ".json");
    }
    try {
        const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("无法写入身法方案临时文件");
        DWORD written = 0;
        const bool success = WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr)
            && written == content.size() && FlushFileBuffers(file);
        CloseHandle(file);
        if (!success || !MoveFileExW(temporary.c_str(), destination.c_str(),
                MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0)))
            throw std::runtime_error("身法方案保存失败，原文件保持不变");
    } catch (...) {
        DeleteFileW(temporary.c_str());
        throw;
    }
    return utf8(destination);
}
std::string archive_movement_plan(const std::string& selected_path, const std::string& library_root) {
    if (selected_path.empty()) throw std::runtime_error("请先选择已保存的方案");
    const auto root = fs::absolute(fs::u8path(library_root)).lexically_normal();
    const auto selected = fs::absolute(fs::u8path(selected_path)).lexically_normal();
    plain_chain(root); plain_chain(selected);
    if (!fs::is_directory(root) || selected.extension() != ".json" || !fs::is_regular_file(selected) ||
        selected.filename().wstring().find_first_of(L"/\\:") != std::wstring::npos ||
        !fs::equivalent(selected.parent_path(),root))
        throw std::runtime_error("只能归档正式方案库本层的JSON方案");
    const auto domain = root.parent_path();
    const auto stem = selected.stem();
    // 只移动由该文件stem独占的活动资源；出生点共图和历史短名包不属于单一方案。
    std::vector<fs::path> sources;
    for (const auto& relative : {fs::path("references") / stem,
            fs::path("captures") / (stem.wstring() + L".json"),
            fs::path("standing") / (stem.wstring() + L".png")}) {
        const auto source = domain / relative;
        plain_chain(source,true);
        const auto attributes = plain_attributes(source,true);
        if (attributes == INVALID_FILE_ATTRIBUTES) continue;
        const bool directory = relative.parent_path() == fs::path("references");
        if (directory != static_cast<bool>(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (!directory && !fs::is_regular_file(source)))
            throw std::runtime_error("方案关联资源类型不符合归档契约");
        plain_tree(source); sources.push_back(source);
    }
    sources.push_back(selected);
    const auto base = domain / "backups" / "deleted-plans";
    plain_chain(base,true); fs::create_directories(base); plain_chain(base);
    static std::atomic_uint64_t sequence{0};
    fs::path backup;
    for (int attempt = 0; attempt < 16; ++attempt) {
        auto candidate = base / (std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) +
            "-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++sequence));
        if (fs::create_directory(candidate)) { backup = std::move(candidate); break; }
    }
    if (backup.empty()) throw std::runtime_error("无法创建独占方案归档目录");
    std::vector<std::pair<fs::path,fs::path>> moved;
    try {
        for (const auto& source : sources) {
            const auto relative = source == selected ? fs::path("plans") / selected.filename() : source.lexically_relative(domain);
            const auto destination = backup / relative;
            fs::create_directories(destination.parent_path());
            plain_chain(source); plain_chain(destination,true);
            // 不跨卷复制、不覆盖文件；方案最后移出，使前序资源失败时库项仍存在。
            if (!MoveFileExW(source.c_str(),destination.c_str(),MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("方案或关联资源正在使用，归档移动失败");
            moved.emplace_back(source,destination);
        }
    } catch (...) {
        bool restored = true;
        for (auto item = moved.rbegin(); item != moved.rend(); ++item) {
            try {
                plain_chain(item->first,true); plain_chain(item->second);
                if (!MoveFileExW(item->second.c_str(),item->first.c_str(),MOVEFILE_WRITE_THROUGH)) restored = false;
            } catch (...) { restored = false; }
        }
        if (!restored) throw std::runtime_error("归档失败，部分资源恢复未确认；请保留归档目录检查：" + utf8(backup));
        throw std::runtime_error("归档未完成，已恢复原方案与资源");
    }
    return utf8(backup);
}
}
