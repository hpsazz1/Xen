#ifndef AIM_PRODUCTION_RED_INTERNAL_H
#define AIM_PRODUCTION_RED_INTERNAL_H

#include "aim_production_red/aim_production_red.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <string>

namespace aim_production_red::detail {

using RenameOperation = std::function<void(
    const std::filesystem::path&, const std::filesystem::path&)>;

// 文件系统关闭边界可注入失败；生产与回归共用完整生成、哈希和发布流程。
using CloseFileOperation = std::function<void(std::ofstream&, const std::filesystem::path&)>;

bool produce_output_off_bundle(
    const ProduceOptions& options,
    ProduceResult& result,
    std::string& error,
    const CloseFileOperation& close_file) noexcept;

bool rename_directory_with_retry(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    int maximum_attempts,
    std::chrono::milliseconds initial_delay,
    const RenameOperation& rename_operation,
    std::string& error) noexcept;

} // namespace aim_production_red::detail

#endif // AIM_PRODUCTION_RED_INTERNAL_H
