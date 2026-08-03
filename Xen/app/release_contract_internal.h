#ifndef APP_RELEASE_CONTRACT_INTERNAL_H
#define APP_RELEASE_CONTRACT_INTERNAL_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "detector/detector.h"

namespace app::detail {

inline constexpr std::uint32_t kReleaseRestartExitCode = 42;

struct ReleaseEnvironment {
    bool managed = false;
    std::filesystem::path root;
    std::string runtime_id;
    std::vector<BackendType> available_backends;
};

struct ReleaseRuntimeEntry {
    std::string id;
    std::filesystem::path executable;
    std::vector<BackendType> backends;
};

struct ReleaseFileEntry {
    std::filesystem::path path;
    std::string runtime_id;
    std::uintmax_t size = 0;
    std::string sha256;
};

struct ReleaseManifest {
    int schema = 0;
    std::string git_commit;
    std::vector<ReleaseRuntimeEntry> runtimes;
    std::vector<ReleaseFileEntry> files;
};

const char* backend_config_name(BackendType backend) noexcept;
bool parse_backend_config_name(const std::string& name,
                               BackendType& backend) noexcept;
const char* runtime_for_backend(BackendType backend) noexcept;
bool backend_allowed(const std::vector<BackendType>& available,
                     BackendType backend) noexcept;

bool load_release_environment(ReleaseEnvironment& environment,
                              std::string& error) noexcept;
bool apply_release_working_directory(const ReleaseEnvironment& environment,
                                     std::string& error) noexcept;

bool load_release_manifest(const std::filesystem::path& manifest_path,
                           ReleaseManifest& manifest,
                           std::string& error) noexcept;
bool validate_release_manifest(const std::filesystem::path& release_root,
                               const ReleaseManifest& manifest,
                               std::string& error) noexcept;
const ReleaseRuntimeEntry* find_runtime_for_backend(
    const ReleaseManifest& manifest, BackendType backend) noexcept;

} // namespace app::detail

#endif // APP_RELEASE_CONTRACT_INTERNAL_H
