#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "app/model_catalog_internal.h"

#include <Windows.h>

#ifdef ERROR
#undef ERROR
#endif

#include <algorithm>
#include <system_error>

namespace app::detail {
namespace {

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) {
            return static_cast<char>(
                ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch);
        });
    return value;
}

bool is_onnx_name(const std::filesystem::path& path) {
    return lowercase_ascii(path_to_utf8(path.extension())) == ".onnx";
}

bool is_direct_regular_file(const std::filesystem::path& path,
                            std::error_code& error) {
    const auto status = std::filesystem::symlink_status(path, error);
    return !error && !std::filesystem::is_symlink(status) &&
           std::filesystem::is_regular_file(status);
}

} // namespace

std::string path_to_utf8(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    return std::string(
        reinterpret_cast<const char*>(encoded.data()), encoded.size());
}

bool prepare_model_directory(
        const std::filesystem::path& executable_path,
        std::filesystem::path& model_directory,
        std::string& error) noexcept {
    try {
        if (executable_path.empty() || !executable_path.has_parent_path()) {
            error = "无法确定程序目录";
            return false;
        }

        const std::filesystem::path candidate =
            executable_path.parent_path() / L"models";
        std::error_code filesystem_error;
        const auto status =
            std::filesystem::symlink_status(candidate, filesystem_error);
        if (filesystem_error &&
            filesystem_error != std::errc::no_such_file_or_directory) {
            error = "无法检查模型目录: " + path_to_utf8(candidate);
            return false;
        }
        if (!filesystem_error && std::filesystem::exists(status)) {
            if (std::filesystem::is_symlink(status) ||
                !std::filesystem::is_directory(status)) {
                error = "程序目录下的 models 不是普通目录: " +
                        path_to_utf8(candidate);
                return false;
            }
        } else {
            filesystem_error.clear();
            if (!std::filesystem::create_directories(
                    candidate, filesystem_error) && filesystem_error) {
                error = "无法创建模型目录: " + path_to_utf8(candidate);
                return false;
            }
        }

        model_directory = std::filesystem::weakly_canonical(
            candidate, filesystem_error);
        if (filesystem_error) {
            error = "无法规范化模型目录: " + path_to_utf8(candidate);
            model_directory.clear();
            return false;
        }
        error.clear();
        return true;
    } catch (...) {
        model_directory.clear();
        error = "准备模型目录时发生未知异常";
        return false;
    }
}

bool prepare_program_model_directory(
        std::filesystem::path& model_directory,
        std::string& error) noexcept {
    try {
        std::vector<wchar_t> executable(512, L'\0');
        for (;;) {
            const DWORD length = GetModuleFileNameW(
                nullptr, executable.data(),
                static_cast<DWORD>(executable.size()));
            if (length == 0) {
                error = "无法读取程序路径";
                return false;
            }
            if (length < executable.size() - 1) {
                return prepare_model_directory(
                    std::filesystem::path(
                        std::wstring(executable.data(), length)),
                    model_directory, error);
            }
            if (executable.size() >= 32'768) {
                error = "程序路径超过 Windows 支持的长度";
                return false;
            }
            executable.resize(
                std::min<std::size_t>(executable.size() * 2, 32'768),
                L'\0');
        }
    } catch (...) {
        model_directory.clear();
        error = "读取程序路径时发生未知异常";
        return false;
    }
}

bool list_models(const std::filesystem::path& model_directory,
                 std::vector<std::string>& model_names,
                 std::string& error) noexcept {
    try {
        std::vector<std::string> candidate_names;
        std::error_code filesystem_error;
        std::filesystem::directory_iterator iterator(
            model_directory,
            std::filesystem::directory_options::skip_permission_denied,
            filesystem_error);
        if (filesystem_error) {
            error = "无法读取模型目录: " + path_to_utf8(model_directory);
            return false;
        }

        const std::filesystem::directory_iterator end;
        for (; iterator != end; iterator.increment(filesystem_error)) {
            if (filesystem_error) {
                error = "枚举模型目录失败: " +
                        path_to_utf8(model_directory);
                return false;
            }
            std::error_code status_error;
            if (!is_direct_regular_file(iterator->path(), status_error) ||
                status_error || !is_onnx_name(iterator->path())) {
                continue;
            }
            candidate_names.push_back(
                path_to_utf8(iterator->path().filename()));
        }
        if (filesystem_error) {
            error = "枚举模型目录失败: " + path_to_utf8(model_directory);
            return false;
        }

        std::sort(candidate_names.begin(), candidate_names.end(),
            [](const std::string& left, const std::string& right) {
                const std::string normalized_left = lowercase_ascii(left);
                const std::string normalized_right = lowercase_ascii(right);
                return normalized_left == normalized_right
                    ? left < right
                    : normalized_left < normalized_right;
            });
        model_names.swap(candidate_names);
        error.clear();
        return true;
    } catch (...) {
        error = "枚举模型目录时发生未知异常";
        return false;
    }
}

std::string normalize_model_selection(
        const std::string& configured_path) noexcept {
    if (configured_path.empty()) return {};
    try {
        const std::filesystem::path path =
            std::filesystem::u8path(configured_path);
        if (!is_onnx_name(path)) return {};
        return path_to_utf8(path.filename());
    } catch (...) {
        return {};
    }
}

bool resolve_model_selection(
        const std::filesystem::path& model_directory,
        const std::string& selected_name,
        std::string& resolved_path,
        std::string& error) noexcept {
    try {
        resolved_path.clear();
        const std::string normalized =
            normalize_model_selection(selected_name);
        if (normalized.empty() || normalized != selected_name) {
            error = "请选择 models 目录中的 ONNX 模型";
            return false;
        }

        const std::filesystem::path selected_path =
            std::filesystem::u8path(selected_name);
        const std::filesystem::path candidate =
            model_directory / selected_path;
        std::error_code filesystem_error;
        if (!is_direct_regular_file(candidate, filesystem_error) ||
            filesystem_error) {
            error = "模型文件不存在或不是普通文件: " + selected_name;
            return false;
        }

        const std::filesystem::path canonical_directory =
            std::filesystem::weakly_canonical(
                model_directory, filesystem_error);
        if (filesystem_error) {
            error = "无法规范化模型目录";
            return false;
        }
        const std::filesystem::path canonical_candidate =
            std::filesystem::canonical(candidate, filesystem_error);
        if (filesystem_error) {
            error = "无法规范化模型文件: " + selected_name;
            return false;
        }
        if (!std::filesystem::equivalent(
                canonical_candidate.parent_path(), canonical_directory,
                filesystem_error) || filesystem_error) {
            error = "模型文件必须直接位于程序 models 目录中";
            return false;
        }

        resolved_path = path_to_utf8(canonical_candidate);
        error.clear();
        return true;
    } catch (...) {
        resolved_path.clear();
        error = "解析模型路径时发生未知异常";
        return false;
    }
}

} // namespace app::detail
