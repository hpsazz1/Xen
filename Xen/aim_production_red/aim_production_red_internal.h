#ifndef AIM_PRODUCTION_RED_INTERNAL_H
#define AIM_PRODUCTION_RED_INTERNAL_H

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>

namespace aim_production_red::detail {

using RenameOperation = std::function<void(
    const std::filesystem::path&, const std::filesystem::path&)>;

bool rename_directory_with_retry(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    int maximum_attempts,
    std::chrono::milliseconds initial_delay,
    const RenameOperation& rename_operation,
    std::string& error) noexcept;

} // namespace aim_production_red::detail

#endif // AIM_PRODUCTION_RED_INTERNAL_H
