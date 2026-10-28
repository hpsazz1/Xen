#ifndef DEBUG_SESSION_MOVEMENT_LIBRARY_INTERNAL_H
#define DEBUG_SESSION_MOVEMENT_LIBRARY_INTERNAL_H

#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

namespace debug_session::detail {
struct SavedMovementPlan { std::string path, name, map, voice_command, raw_name; };
// 只扫描显式保存的方案库；保留旧参数兼容调用，不枚举调试快照。路径使用 UTF-8。
std::vector<SavedMovementPlan> list_movement_plans(
    const std::string& library_root = "cache/movement/plans",
    const std::string& legacy_root = "cache/debug");
// 仅更新库内文件；旧调试报告与外部文件始终另存。异常交由 Session 后台边界报告。
std::string save_movement_plan(const nlohmann::json& plan,
    const std::string& selected_path, bool save_as,
    const std::string& library_root = "cache/movement/plans");
// 将库内方案与同stem独占资源移入同域备份，返回归档批次目录；失败尝试原位回滚。
std::string archive_movement_plan(const std::string& selected_path,
    const std::string& library_root = "cache/movement/plans");
}
#endif
