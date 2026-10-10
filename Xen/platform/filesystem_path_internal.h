#ifndef XEN_PLATFORM_FILESYSTEM_PATH_INTERNAL_H
#define XEN_PLATFORM_FILESYSTEM_PATH_INTERNAL_H

#include <filesystem>
#include <string>

namespace xen::filesystem_detail {

// 仅用于文件 API 边界；对外路径和证据仍使用普通绝对路径。显式扩展路径
// 不依赖宿主的 longPathAware manifest 或机器的 LongPathsEnabled 设置。
// 这是路径表示转换，不承担调用方的授权、目录归属或 reparse 检查。
inline std::filesystem::path windows_io_path(const std::filesystem::path& path) {
    if (path.empty()) return path;
    const auto absolute = std::filesystem::absolute(path).lexically_normal();
    const auto& value = absolute.native();
    if (value.starts_with(L"\\\\?\\") || value.starts_with(L"\\\\.\\")) return absolute;
    if (value.starts_with(L"\\\\")) {
        return std::filesystem::path(L"\\\\?\\UNC\\" + value.substr(2));
    }
    return std::filesystem::path(L"\\\\?\\" + value);
}

inline std::string path_utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return std::string(value.begin(), value.end());
}

inline std::string generic_path_utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return std::string(value.begin(), value.end());
}

} // namespace xen::filesystem_detail

#endif
