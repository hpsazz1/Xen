#ifndef APP_MODEL_CATALOG_INTERNAL_H
#define APP_MODEL_CATALOG_INTERNAL_H

#include <filesystem>
#include <string>
#include <vector>

namespace app::detail {

// 返回 UTF-8 路径，供配置、ImGui 和日志使用；Windows 文件访问仍保留宽路径。
std::string path_to_utf8(const std::filesystem::path& path);

// models 必须与可执行文件同目录。目录不存在时创建；同名普通文件或链接会显式失败。
bool prepare_model_directory(
    const std::filesystem::path& executable_path,
    std::filesystem::path& model_directory,
    std::string& error) noexcept;
bool prepare_model_directory_at_root(
    const std::filesystem::path& data_root,
    std::filesystem::path& model_directory,
    std::string& error) noexcept;
bool prepare_program_model_directory(
    std::filesystem::path& model_directory,
    std::string& error) noexcept;

// 只枚举 models 根目录中的普通 .onnx 文件，不递归、不跟随符号链接。
bool list_models(const std::filesystem::path& model_directory,
                 std::vector<std::string>& model_names,
                 std::string& error) noexcept;

// 兼容旧配置中的相对或绝对路径，但只保留文件名；实际访问始终重新锚定到 models。
std::string normalize_model_selection(
    const std::string& configured_path) noexcept;

// 将 UI 选择解析为 models 中已存在的普通文件，并拒绝目录穿越、链接和非 ONNX 文件。
bool resolve_model_selection(
    const std::filesystem::path& model_directory,
    const std::string& selected_name,
    std::string& resolved_path,
    std::string& error) noexcept;

} // namespace app::detail

#endif // APP_MODEL_CATALOG_INTERNAL_H
