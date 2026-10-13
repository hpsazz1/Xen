#ifndef DEBUG_SESSION_MOVEMENT_LIBRARY_INTERNAL_H
#define DEBUG_SESSION_MOVEMENT_LIBRARY_INTERNAL_H

#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

namespace debug_session::detail {
struct SavedMovementPlan { std::string path, name; };
// 只扫描方案库与旧调试目录的一层；坏文件不影响其他方案。路径统一使用 UTF-8。
std::vector<SavedMovementPlan> list_movement_plans(
    const std::string& library_root = "cache/movement/plans",
    const std::string& legacy_root = "cache/debug");
// 仅更新库内文件；旧调试报告与外部文件始终另存。异常交由 Session 后台边界报告。
std::string save_movement_plan(const nlohmann::json& plan,
    const std::string& selected_path, bool save_as,
    const std::string& library_root = "cache/movement/plans");
}
#endif
