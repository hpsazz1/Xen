#ifndef XEN_LINEUP_HOST_CAPTURE_INTERNAL_H
#define XEN_LINEUP_HOST_CAPTURE_INTERNAL_H

#include <filesystem>
#include <string>
#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>

namespace lineup {
struct HostCaptureBundle {
    nlohmann::json metadata;
    cv::Mat full, roi, overview;
};

namespace detail {
struct HostCapturePaths {
    bool local_only = false;
    std::filesystem::path profile_file, captures, delivery;
};
// 离线库只使用本机盘；解析不创建目录，也不连接或探测辅机收件目录。
bool resolve_host_capture_paths(const std::filesystem::path &local_library,
    const std::filesystem::path &inbox, const std::filesystem::path &output,
    HostCapturePaths &paths, std::string &error) noexcept;
}

// 输入必须是一张完整 BGR 原图；原图、中心模板和标框展示图分别拥有像素。
bool make_host_capture_bundle(const cv::Mat &full, const nlohmann::json &profile,
    const std::string &id, const std::string &utc_timestamp,
    HostCaptureBundle &bundle, std::string &error) noexcept;
// 仅发布 root/id；同名拒绝覆盖，失败不暴露半包，已发布目录保留。
bool write_host_capture_bundle(const std::filesystem::path &root,
    const HostCaptureBundle &bundle, std::filesystem::path &published,
    std::string &error) noexcept;
bool read_host_capture_bundle(const std::filesystem::path &directory,
    HostCaptureBundle &bundle, std::string &error) noexcept;
bool read_host_capture_profile(const std::filesystem::path &path,
    nlohmann::json &profile, std::string &error) noexcept;
} // namespace lineup
#endif
