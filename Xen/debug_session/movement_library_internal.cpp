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
            plan.map, plan.voice_command});
    } catch (const std::exception&) { /* 单个坏文件不阻断方案下拉列表。 */ }
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
    if (!plan.voice_command.empty()) {
        for (const auto& entry : list_movement_plans(utf8(root))) {
            std::error_code code;
            if (!destination.empty() && fs::equivalent(fs::u8path(entry.path), destination, code)) continue;
            if (entry.map == plan.map && entry.voice_command == plan.voice_command)
                throw std::runtime_error("同地图语音口令与方案冲突：" + entry.name);
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
}
