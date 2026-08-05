#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "app/release_contract_internal.h"

#include <Windows.h>
#include <bcrypt.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <set>
#include <system_error>

namespace app::detail {
namespace {

constexpr std::array<BackendType, 5> kAllBackends{
    BackendType::CUDA,
    BackendType::TENSORRT,
    BackendType::DIRECTML,
    BackendType::OPENVINO,
    BackendType::CPU};

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    return value;
}

bool read_environment(const wchar_t* name, std::wstring& value) {
    value.clear();
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) return false;
    std::vector<wchar_t> buffer(required, L'\0');
    const DWORD length = GetEnvironmentVariableW(
        name, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return false;
    value.assign(buffer.data(), length);
    return true;
}

bool is_safe_relative_path(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_path()) {
        return false;
    }
    for (const auto& component : path) {
        if (component == L".." || component == L".") return false;
    }
    return true;
}

bool is_mutable_release_file(const std::filesystem::path& path) {
    if (!is_safe_relative_path(path)) return false;
    auto component = path.begin();
    if (component == path.end()) return false;
    const std::wstring root = component->native();
    const bool mutable_root =
        CompareStringOrdinal(root.c_str(), -1, L"cache", -1, TRUE) ==
            CSTR_EQUAL ||
        CompareStringOrdinal(root.c_str(), -1, L"logs", -1, TRUE) ==
            CSTR_EQUAL;
    if (!mutable_root) return false;
    return ++component != path.end();
}

std::string bytes_to_hex(const std::vector<unsigned char>& bytes) {
    constexpr char kDigits[] = "0123456789abcdef";
    std::string result(bytes.size() * 2, '0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        result[index * 2] = kDigits[bytes[index] >> 4];
        result[index * 2 + 1] = kDigits[bytes[index] & 0x0F];
    }
    return result;
}

bool sha256_file(const std::filesystem::path& path,
                 std::string& sha256,
                 std::string& error) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<unsigned char> object;
    std::vector<unsigned char> digest;
    bool success = false;
    do {
        if (BCryptOpenAlgorithmProvider(
                &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
            error = "无法初始化发布包 SHA-256 校验";
            break;
        }
        DWORD object_size = 0;
        DWORD digest_size = 0;
        DWORD result_size = 0;
        if (BCryptGetProperty(
                algorithm, BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size),
                &result_size, 0) < 0 ||
            BCryptGetProperty(
                algorithm, BCRYPT_HASH_LENGTH,
                reinterpret_cast<PUCHAR>(&digest_size), sizeof(digest_size),
                &result_size, 0) < 0) {
            error = "无法读取发布包 SHA-256 参数";
            break;
        }
        object.resize(object_size);
        digest.resize(digest_size);
        if (BCryptCreateHash(
                algorithm, &hash, object.data(), object_size,
                nullptr, 0, 0) < 0) {
            error = "无法创建发布包 SHA-256 上下文";
            break;
        }
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            error = "无法读取发布包文件";
            break;
        }
        // 发布运行库可能很大，读缓冲必须放在堆上，避免耗尽 Windows 默认线程栈。
        std::vector<char> buffer(1024 * 1024);
        while (input) {
            input.read(buffer.data(), buffer.size());
            const std::streamsize count = input.gcount();
            if (count > 0 && BCryptHashData(
                    hash, reinterpret_cast<PUCHAR>(buffer.data()),
                    static_cast<ULONG>(count), 0) < 0) {
                error = "计算发布包 SHA-256 失败";
                break;
            }
        }
        if (!input.eof()) break;
        if (BCryptFinishHash(
                hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
            error = "结束发布包 SHA-256 计算失败";
            break;
        }
        sha256 = bytes_to_hex(digest);
        success = true;
    } while (false);
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return success;
}

} // namespace

const char* backend_config_name(BackendType backend) noexcept {
    switch (backend) {
        case BackendType::CUDA: return "cuda";
        case BackendType::TENSORRT: return "tensorrt";
        case BackendType::DIRECTML: return "directml";
        case BackendType::OPENVINO: return "openvino";
        case BackendType::CPU: return "cpu";
    }
    return "";
}

bool parse_backend_config_name(const std::string& name,
                               BackendType& backend) noexcept {
    const std::string normalized = lowercase_ascii(name);
    for (const BackendType candidate : kAllBackends) {
        if (normalized == backend_config_name(candidate)) {
            backend = candidate;
            return true;
        }
    }
    return false;
}

const char* runtime_for_backend(BackendType backend) noexcept {
    switch (backend) {
        case BackendType::DIRECTML: return "directml";
        case BackendType::OPENVINO: return "openvino";
        case BackendType::CUDA:
        case BackendType::TENSORRT:
        case BackendType::CPU:
            return "nvidia";
    }
    return "";
}

bool backend_allowed(const std::vector<BackendType>& available,
                     BackendType backend) noexcept {
    return std::find(available.begin(), available.end(), backend) !=
           available.end();
}

bool load_release_environment(ReleaseEnvironment& environment,
                              std::string& error) noexcept {
    try {
        environment = {};
        environment.available_backends.assign(
            kAllBackends.begin(), kAllBackends.end());
        std::wstring root;
        std::wstring runtime;
        std::wstring backends;
        const bool has_root = read_environment(L"XEN_RELEASE_ROOT", root);
        const bool has_runtime = read_environment(L"XEN_RUNTIME_ID", runtime);
        const bool has_backends = read_environment(
            L"XEN_RELEASE_BACKENDS", backends);
        if (!has_root && !has_runtime && !has_backends) {
            error.clear();
            return true;
        }
        if (!has_root || !has_runtime || !has_backends) {
            error = "发布环境变量不完整";
            return false;
        }

        std::error_code filesystem_error;
        environment.root = std::filesystem::canonical(
            std::filesystem::path(root), filesystem_error);
        if (filesystem_error || !std::filesystem::is_directory(
                environment.root, filesystem_error)) {
            error = "发布根目录不存在或不可访问";
            return false;
        }
        environment.runtime_id = std::filesystem::path(runtime).string();
        if (environment.runtime_id != "nvidia" &&
            environment.runtime_id != "directml" &&
            environment.runtime_id != "openvino") {
            error = "发布运行时 ID 非法";
            return false;
        }

        environment.available_backends.clear();
        std::string list = std::filesystem::path(backends).string();
        std::size_t begin = 0;
        while (begin <= list.size()) {
            const std::size_t end = list.find(',', begin);
            const std::string item = list.substr(
                begin, end == std::string::npos ? std::string::npos
                                                 : end - begin);
            BackendType backend{};
            if (item.empty() || !parse_backend_config_name(item, backend) ||
                backend_allowed(environment.available_backends, backend)) {
                error = "发布后端能力列表非法";
                return false;
            }
            environment.available_backends.push_back(backend);
            if (end == std::string::npos) break;
            begin = end + 1;
        }
        if (environment.available_backends.empty()) {
            error = "发布运行时没有授权后端";
            return false;
        }
        environment.managed = true;
        error.clear();
        return true;
    } catch (...) {
        environment = {};
        error = "解析发布环境时发生未知异常";
        return false;
    }
}

bool apply_release_working_directory(const ReleaseEnvironment& environment,
                                     std::string& error) noexcept {
    if (!environment.managed) return true;
    if (!SetCurrentDirectoryW(environment.root.c_str())) {
        error = "无法切换到发布数据根目录";
        return false;
    }
    error.clear();
    return true;
}

bool load_release_manifest(const std::filesystem::path& manifest_path,
                           ReleaseManifest& manifest,
                           std::string& error) noexcept {
    try {
        manifest = {};
        std::ifstream input(manifest_path, std::ios::binary);
        if (!input) {
            error = "发布清单不存在";
            return false;
        }
        const nlohmann::json document = nlohmann::json::parse(input);
        if (!document.is_object() || document.size() != 5 ||
            !document.contains("schema") ||
            !document.contains("product") ||
            !document.contains("git_commit") ||
            !document.contains("runtimes") ||
            !document.contains("files") ||
            document.at("product") != "Xen") {
            error = "发布清单顶层契约非法";
            return false;
        }
        manifest.schema = document.at("schema").get<int>();
        manifest.git_commit = document.at("git_commit").get<std::string>();
        for (const auto& source : document.at("runtimes")) {
            if (!source.is_object() || source.size() != 3) {
                error = "发布运行时条目契约非法";
                return false;
            }
            ReleaseRuntimeEntry entry;
            entry.id = source.at("id").get<std::string>();
            entry.executable = std::filesystem::u8path(
                source.at("executable").get<std::string>());
            for (const auto& name : source.at("backends")) {
                BackendType backend{};
                if (!name.is_string() ||
                    !parse_backend_config_name(
                        name.get<std::string>(), backend)) {
                    error = "发布运行时包含未知后端";
                    return false;
                }
                entry.backends.push_back(backend);
            }
            manifest.runtimes.push_back(std::move(entry));
        }
        for (const auto& source : document.at("files")) {
            if (!source.is_object() ||
                (!source.contains("path") || !source.contains("runtime") ||
                 !source.contains("size") || !source.contains("sha256"))) {
                error = "发布文件条目契约非法";
                return false;
            }
            ReleaseFileEntry entry;
            entry.path = std::filesystem::u8path(
                source.at("path").get<std::string>());
            entry.runtime_id = source.at("runtime").get<std::string>();
            entry.size = source.at("size").get<std::uintmax_t>();
            entry.sha256 = lowercase_ascii(
                source.at("sha256").get<std::string>());
            manifest.files.push_back(std::move(entry));
        }
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = std::string("发布清单解析失败: ") + exception.what();
        manifest = {};
        return false;
    } catch (...) {
        error = "发布清单解析时发生未知异常";
        manifest = {};
        return false;
    }
}

bool validate_release_manifest(const std::filesystem::path& release_root,
                               const ReleaseManifest& manifest,
                               std::string& error) noexcept {
    try {
        if (manifest.schema != 1 || manifest.git_commit.size() != 40 ||
            manifest.runtimes.empty() || manifest.files.empty()) {
            error = "发布清单版本、提交或内容为空";
            return false;
        }
        std::set<std::string> runtime_ids;
        std::set<int> backend_ids;
        std::set<std::filesystem::path> file_paths;
        for (const auto& runtime : manifest.runtimes) {
            if ((runtime.id != "nvidia" && runtime.id != "directml" &&
                 runtime.id != "openvino") ||
                runtime.backends.empty() ||
                !is_safe_relative_path(runtime.executable) ||
                !runtime_ids.insert(runtime.id).second) {
                error = "发布运行时目录或 ID 非法";
                return false;
            }
            for (const BackendType backend : runtime.backends) {
                if (runtime_for_backend(backend) != runtime.id ||
                    !backend_ids.insert(static_cast<int>(backend)).second) {
                    error = "发布后端归属重复或越权";
                    return false;
                }
            }
        }
        for (const auto& file : manifest.files) {
            if (!is_safe_relative_path(file.path) ||
                file.sha256.size() != 64 ||
                !file_paths.insert(file.path).second ||
                (!file.runtime_id.empty() &&
                 !runtime_ids.contains(file.runtime_id))) {
                error = "发布文件路径、哈希或运行时归属非法";
                return false;
            }
            const auto absolute = release_root / file.path;
            std::error_code filesystem_error;
            if (!std::filesystem::is_regular_file(
                    std::filesystem::symlink_status(
                        absolute, filesystem_error)) ||
                filesystem_error ||
                std::filesystem::file_size(absolute, filesystem_error) !=
                    file.size || filesystem_error) {
                error = "发布文件缺失或长度不一致";
                return false;
            }
            std::string actual_sha256;
            if (!sha256_file(absolute, actual_sha256, error) ||
                actual_sha256 != file.sha256) {
                if (error.empty()) error = "发布文件 SHA-256 不一致";
                return false;
            }
        }
        for (const auto& runtime : manifest.runtimes) {
            const auto iterator = std::find_if(
                manifest.files.begin(), manifest.files.end(),
                [&](const ReleaseFileEntry& file) {
                    return file.path == runtime.executable &&
                           file.runtime_id == runtime.id;
                });
            if (iterator == manifest.files.end()) {
                error = "发布运行时入口未进入文件清单";
                return false;
            }
        }
        for (std::filesystem::recursive_directory_iterator iterator(
                 release_root), end;
             iterator != end; ++iterator) {
            std::error_code filesystem_error;
            const auto status = iterator->symlink_status(filesystem_error);
            if (filesystem_error || std::filesystem::is_symlink(status)) {
                error = "发布包包含不可校验的链接或目录项";
                return false;
            }
            if (!std::filesystem::is_regular_file(status)) continue;
            const auto relative = std::filesystem::relative(
                iterator->path(), release_root, filesystem_error);
            if (filesystem_error) {
                error = "无法计算发布文件相对路径";
                return false;
            }
            if (relative == L"manifest.json") continue;
            // cache 与 logs 由发布脚本预创建，运行时会写入报告、Provider
            // 缓存和日志。它们不属于静态供应链清单，但链接仍在上方拒绝。
            if (!file_paths.contains(relative) &&
                !is_mutable_release_file(relative)) {
                error = "发布包包含清单外文件";
                return false;
            }
        }
        error.clear();
        return true;
    } catch (...) {
        error = "校验发布清单时发生未知异常";
        return false;
    }
}

const ReleaseRuntimeEntry* find_runtime_for_backend(
        const ReleaseManifest& manifest, BackendType backend) noexcept {
    for (const auto& runtime : manifest.runtimes) {
        if (backend_allowed(runtime.backends, backend)) return &runtime;
    }
    return nullptr;
}

} // namespace app::detail
