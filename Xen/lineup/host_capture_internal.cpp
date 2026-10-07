#include "lineup/host_capture_internal.h"
#include "lineup/action_internal.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace lineup {
namespace {
using Json = nlohmann::json;
constexpr int kRoiSize = 320, kMaximumWidth = 7680, kMaximumHeight = 4320;
constexpr std::uintmax_t kMaximumJsonBytes = 64 * 1024;
constexpr std::uintmax_t kMaximumPngBytes = 128 * 1024 * 1024;
constexpr std::array<const char *, 4> kBundleFiles{"full.png", "roi.png", "overview.png", "capture.json"};

void require(bool value, const char *reason) {
    if (!value) throw std::runtime_error(reason);
}
bool valid_id(const std::string &id) {
    if (id.empty() || id.size() > 96 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '-' || c == '_';
    })) return false;
    std::string lower = id;
    for (auto &c : lower) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (lower == "con" || lower == "prn" || lower == "aux" || lower == "nul") return false;
    return !(lower.size() == 4 && (lower.starts_with("com") || lower.starts_with("lpt")) &&
             lower[3] >= '1' && lower[3] <= '9');
}
void validate_timestamp(const std::string &value) {
    require(value.size() == 24 && value[4] == '-' && value[7] == '-' && value[10] == 'T' &&
            value[13] == ':' && value[16] == ':' && value[19] == '.' && value[23] == 'Z',
            "采集时间必须为带毫秒的 UTC 时间");
    for (std::size_t i = 0; i < value.size(); ++i)
        if (i != 4 && i != 7 && i != 10 && i != 13 && i != 16 && i != 19 && i != 23)
            require(value[i] >= '0' && value[i] <= '9', "采集时间含非法字符");
    const auto number = [&](std::size_t start, std::size_t count) {
        int result = 0;
        for (std::size_t i = start; i < start + count; ++i) result = result * 10 + value[i] - '0';
        return result;
    };
    const std::chrono::year_month_day date{std::chrono::year(number(0, 4)),
        std::chrono::month(number(5, 2)), std::chrono::day(number(8, 2))};
    require(date.ok() && number(11, 2) < 24 && number(14, 2) < 60 && number(17, 2) < 60,
            "采集时间超出合法日期范围");
}
void validate_dimensions(int width, int height) {
    require(width >= kRoiSize && height >= kRoiSize && width <= kMaximumWidth && height <= kMaximumHeight,
            "全屏尺寸必须在 320 至 7680×4320 范围内");
}
int integer_field(const Json &value, const char *name) {
    require(value.at(name).is_number_integer(), "几何字段必须为整数");
    const auto number = value.at(name).get<std::int64_t>();
    require(number >= 0 && number <= kMaximumWidth, "几何字段超出范围");
    return static_cast<int>(number);
}
void validate_profile(const Json &profile) {
    require(profile.is_object() && profile.at("schema").is_number_integer() && profile.at("schema") == 1,
            "采集设置版本无效");
    require(profile.dump().size() <= kMaximumJsonBytes, "采集设置过大");
    for (auto key : {"source_id", "map", "team"})
        require(profile.at(key).is_string() && !profile.at(key).get_ref<const std::string &>().empty() &&
                profile.at(key).get_ref<const std::string &>().size() <= 512, "采集设置缺少来源、地图或阵营");
    require(profile.at("team") == "T" || profile.at("team") == "CT", "采集阵营只能为 T 或 CT");
    validate_dimensions(integer_field(profile, "source_width"), integer_field(profile, "source_height"));
    for (auto key : {"name", "target", "grenade", "region", "stance", "instructions", "standpoint_id",
                     "throw_instructions", "notes", "conditions"})
        if (profile.contains(key)) require(profile.at(key).is_string() &&
            profile.at(key).get_ref<const std::string &>().size() <= 8192, "采集文字字段无效或过长");
    if (profile.contains("throw_action"))
        require(detail::inspect_action(profile.at("throw_action")).valid, "采集投掷动作格式无效");
}
cv::Rect geometry(const Json &metadata, cv::Size &full_size) {
    require(metadata.is_object() && metadata.at("schema").is_number_integer() && metadata.at("schema") == 1 &&
            metadata.at("origin") == "desktop_duplication", "截图包版本或来源无效");
    require(valid_id(metadata.at("id").get<std::string>()), "截图编号包含不安全路径或保留名称");
    validate_timestamp(metadata.at("capture_timestamp").get<std::string>());
    const auto &full = metadata.at("full");
    full_size = {integer_field(full, "width"), integer_field(full, "height")};
    validate_dimensions(full_size.width, full_size.height);
    const auto &roi = metadata.at("roi");
    const cv::Rect rect(integer_field(roi, "x"), integer_field(roi, "y"),
                        integer_field(roi, "width"), integer_field(roi, "height"));
    require(rect == cv::Rect((full_size.width - kRoiSize) / 2, (full_size.height - kRoiSize) / 2,
                            kRoiSize, kRoiSize), "ROI 不是同帧中心 320 原始裁剪");
    validate_profile(metadata.at("profile"));
    require(metadata["profile"]["source_width"] == full_size.width &&
            metadata["profile"]["source_height"] == full_size.height, "采集设置与实际全屏几何不符");
    require(metadata.dump().size() <= kMaximumJsonBytes, "截图元数据过大");
    return rect;
}
cv::Mat make_overview(const cv::Mat &full, const cv::Rect &roi) {
    auto result = full.clone();
    cv::rectangle(result, roi, cv::Scalar(0, 200, 255), 2, cv::LINE_8);
    return result;
}
void validate_pixels(const HostCaptureBundle &bundle) {
    cv::Size size;
    const auto rect = geometry(bundle.metadata, size);
    require(bundle.full.type() == CV_8UC3 && bundle.full.size() == size &&
            bundle.roi.type() == CV_8UC3 && bundle.roi.size() == rect.size() &&
            bundle.overview.type() == CV_8UC3 && bundle.overview.size() == size,
            "截图像素类型或尺寸不符");
    require(cv::norm(bundle.full(rect), bundle.roi, cv::NORM_INF) == 0, "ROI 像素与全屏原图不一致");
    require(cv::norm(make_overview(bundle.full, rect), bundle.overview, cv::NORM_INF) == 0,
            "标框展示图与全屏原图不一致");
}
void reject_reparse(const std::filesystem::path &path) {
    const auto attributes = GetFileAttributesW(path.c_str());
    require(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT),
            "截图包文件缺失或包含路径重定向");
}
std::vector<unsigned char> read_bytes(const std::filesystem::path &path, std::uintmax_t limit) {
    reject_reparse(path);
    require(std::filesystem::is_regular_file(path), "截图包文件类型无效");
    const auto size = std::filesystem::file_size(path);
    require(size > 0 && size <= limit, "截图包文件为空或过大");
    std::ifstream input(path, std::ios::binary);
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(input.good() && input.peek() == std::char_traits<char>::eof(), "截图包读取不完整或读取期间变化");
    return bytes;
}
Json read_json(const std::filesystem::path &path) {
    const auto bytes = read_bytes(path, kMaximumJsonBytes);
    return Json::parse(bytes.begin(), bytes.end());
}
cv::Mat read_png(const std::filesystem::path &path, cv::Size expected) {
    const auto bytes = read_bytes(path, expected == cv::Size(kRoiSize, kRoiSize) ? 1024 * 1024 : kMaximumPngBytes);
    constexpr std::array<unsigned char, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
    require(bytes.size() >= 33 && std::equal(signature.begin(), signature.end(), bytes.begin()) &&
            bytes[8] == 0 && bytes[9] == 0 && bytes[10] == 0 && bytes[11] == 13 &&
            bytes[12] == 'I' && bytes[13] == 'H' && bytes[14] == 'D' && bytes[15] == 'R', "截图必须为合法 PNG");
    const auto dimension = [&](std::size_t offset) {
        return (std::uint32_t(bytes[offset]) << 24) | (std::uint32_t(bytes[offset + 1]) << 16) |
               (std::uint32_t(bytes[offset + 2]) << 8) | bytes[offset + 3];
    };
    // 解码前先查 IHDR，避免不可信压缩数据要求分配超大图像。
    require(dimension(16) == static_cast<std::uint32_t>(expected.width) &&
            dimension(20) == static_cast<std::uint32_t>(expected.height) && bytes[24] == 8 && bytes[25] == 2,
            "PNG 头部尺寸或颜色格式与元数据不符");
    auto result = cv::imdecode(bytes, cv::IMREAD_UNCHANGED);
    require(!result.empty() && result.type() == CV_8UC3 && result.size() == expected, "PNG 解码结果无效");
    return result;
}
HostCaptureBundle read_bundle(const std::filesystem::path &directory, bool published) {
    reject_reparse(directory);
    require(std::filesystem::is_directory(directory), "截图包目录不存在");
    std::size_t count = 0;
    for (const auto &entry : std::filesystem::directory_iterator(directory)) {
        const auto name = entry.path().filename().string();
        require(std::find(kBundleFiles.begin(), kBundleFiles.end(), name) != kBundleFiles.end(), "截图包含未知文件");
        ++count;
    }
    require(count == kBundleFiles.size(), "截图包尚未完整发布");
    HostCaptureBundle result;
    result.metadata = read_json(directory / "capture.json");
    cv::Size size;
    const auto rect = geometry(result.metadata, size);
    if (published) require(directory.filename() == std::filesystem::u8path(result.metadata.at("id").get<std::string>()),
                           "截图目录与包编号不符，临时包不可导入");
    result.full = read_png(directory / "full.png", size);
    result.roi = read_png(directory / "roi.png", rect.size());
    result.overview = read_png(directory / "overview.png", size);
    validate_pixels(result);
    return result;
}
void write_bytes(const std::filesystem::path &path, const unsigned char *bytes, std::size_t size) {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char *>(bytes), static_cast<std::streamsize>(size));
    output.flush();
    require(output.good(), "截图文件写入失败");
    output.close();
    require(!output.fail(), "截图文件关闭失败");
}
void write_png(const std::filesystem::path &path, const cv::Mat &pixels) {
    std::vector<unsigned char> bytes;
    require(cv::imencode(".png", pixels, bytes) && bytes.size() <= kMaximumPngBytes, "PNG 编码失败或过大");
    write_bytes(path, bytes.data(), bytes.size());
}
} // namespace

bool detail::resolve_host_capture_paths(const std::filesystem::path &local_library,
    const std::filesystem::path &inbox, const std::filesystem::path &output,
    HostCapturePaths &paths, std::string &error) noexcept {
    paths = {}; error.clear();
    try {
        HostCapturePaths result;
        if (!local_library.empty()) {
            require(inbox.empty() && output.empty(), "--local-library 不能与 --inbox 或 --output 同用");
            const auto library = std::filesystem::absolute(local_library).lexically_normal();
            const auto drive = library.root_name().native();
            require(drive.size() == 2 && drive[1] == L':', "离线采集库必须使用本机盘路径，不接受 UNC 或设备路径");
            const auto drive_type = GetDriveTypeW(library.root_path().c_str());
            require(drive_type == DRIVE_FIXED || drive_type == DRIVE_REMOVABLE || drive_type == DRIVE_RAMDISK,
                    "离线采集库必须位于可用本机盘，不能使用映射网络盘");
            // 从根逐级检查，不能先访问重定向目录下面的文件再判定它是否本地。
            auto directory = library.root_path();
            for (const auto &component : library.relative_path()) {
                directory /= component;
                const auto attributes = GetFileAttributesW(directory.c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES) {
                    const auto reason = GetLastError();
                    require(reason == ERROR_FILE_NOT_FOUND || reason == ERROR_PATH_NOT_FOUND,
                            "无法确认离线采集库路径");
                    break;
                }
                require(!(attributes & FILE_ATTRIBUTE_REPARSE_POINT) && (attributes & FILE_ATTRIBUTE_DIRECTORY),
                        "离线采集库路径不能包含文件或重定向目录");
            }
            result.local_only = true;
            result.profile_file = library / "profile.json";
            result.captures = library / "captures";
        } else {
            require(!inbox.empty() && !output.empty(), "指定 --local-library，或同时指定 --inbox 和 --output");
            const auto source = std::filesystem::absolute(inbox).lexically_normal();
            result.profile_file = source / "profile.json";
            result.captures = std::filesystem::absolute(output).lexically_normal();
            result.delivery = source / "captures";
        }
        paths = std::move(result);
        return true;
    } catch (const std::exception &e) { error = e.what(); }
      catch (...) { error = "解析采集存储路径失败"; }
    return false;
}

bool make_host_capture_bundle(const cv::Mat &full, const Json &profile,
    const std::string &id, const std::string &utc_timestamp,
    HostCaptureBundle &bundle, std::string &error) noexcept {
    bundle = {}; error.clear();
    try {
        require(!full.empty() && full.type() == CV_8UC3, "采集输入必须为 BGR 原图");
        HostCaptureBundle result;
        const cv::Rect rect((full.cols - kRoiSize) / 2, (full.rows - kRoiSize) / 2, kRoiSize, kRoiSize);
        result.metadata = {{"schema", 1}, {"id", id}, {"origin", "desktop_duplication"},
            {"capture_timestamp", utc_timestamp}, {"full", {{"width", full.cols}, {"height", full.rows}}},
            {"roi", {{"x", rect.x}, {"y", rect.y}, {"width", rect.width}, {"height", rect.height}}}, {"profile", profile}};
        cv::Size checked;
        geometry(result.metadata, checked);
        result.full = full.clone();
        result.roi = result.full(rect).clone();
        result.overview = make_overview(result.full, rect);
        bundle = std::move(result);
        return true;
    } catch (const std::exception &e) { error = e.what(); }
      catch (...) { error = "创建截图包失败"; }
    return false;
}

bool write_host_capture_bundle(const std::filesystem::path &root, const HostCaptureBundle &bundle,
    std::filesystem::path &published, std::string &error) noexcept {
    published.clear(); error.clear();
    std::filesystem::path base, temporary;
    try {
        validate_pixels(bundle);
        require(!root.empty(), "截图输出目录为空");
        base = std::filesystem::absolute(root).lexically_normal();
        std::filesystem::create_directories(base);
        reject_reparse(base);
        const auto id = bundle.metadata.at("id").get<std::string>();
        const auto destination = base / id;
        require(!std::filesystem::exists(destination), "截图编号已存在，拒绝覆盖");
        static std::atomic<std::uint64_t> sequence{0};
        for (int attempt = 0; attempt < 8; ++attempt) {
            const auto suffix = std::to_string(GetCurrentProcessId()) + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++sequence);
            const auto candidate = base / (".pending-" + id + "-" + suffix);
            if (std::filesystem::create_directory(candidate)) { temporary = candidate; break; }
        }
        require(!temporary.empty(), "无法创建独立临时截图目录");
        write_png(temporary / "full.png", bundle.full);
        write_png(temporary / "roi.png", bundle.roi);
        write_png(temporary / "overview.png", bundle.overview);
        const auto text = bundle.metadata.dump(2);
        require(text.size() <= kMaximumJsonBytes, "截图元数据过大");
        write_bytes(temporary / "capture.json", reinterpret_cast<const unsigned char *>(text.data()), text.size());
        const auto verified = read_bundle(temporary, false);
        require(verified.metadata == bundle.metadata, "截图包回读元数据不一致");
        require(MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH) != 0,
                "截图包原子发布失败");
        temporary.clear();
        published = destination;
        return true;
    } catch (const std::exception &e) { error = e.what(); }
      catch (...) { error = "写入截图包失败"; }
    // 仅清理本次成功创建的固定四文件；不递归删除任何用户目录。
    if (!temporary.empty() && temporary.parent_path() == base) {
        std::error_code ignored;
        for (auto name : kBundleFiles) std::filesystem::remove(temporary / name, ignored);
        std::filesystem::remove(temporary, ignored);
    }
    return false;
}

bool read_host_capture_bundle(const std::filesystem::path &directory, HostCaptureBundle &bundle,
    std::string &error) noexcept {
    bundle = {}; error.clear();
    try { bundle = read_bundle(std::filesystem::absolute(directory).lexically_normal(), true); return true; }
    catch (const std::exception &e) { error = e.what(); }
    catch (...) { error = "读取截图包失败"; }
    return false;
}

bool read_host_capture_profile(const std::filesystem::path &path, Json &profile, std::string &error) noexcept {
    profile = {}; error.clear();
    try {
        auto value = read_json(path);
        validate_profile(value);
        profile = std::move(value);
        return true;
    } catch (const std::exception &e) { error = e.what(); }
      catch (...) { error = "读取采集设置失败"; }
    return false;
}
} // namespace lineup
