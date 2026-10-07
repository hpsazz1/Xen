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
