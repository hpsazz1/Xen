#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <bcrypt.h>

#include "detector/video_visibility_internal.h"

#include <opencv2/core.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace detector::detail {
namespace {

constexpr std::size_t kMaximumAnnotationBytes = 16U * 1024U * 1024U;
constexpr std::size_t kHashBufferBytes = 64U * 1024U;
constexpr std::size_t kMaximumAnnotatedFrames = 10U * 1000U * 1000U;

void set_error(std::string& output, const std::string& value) noexcept {
    try {
        output = value;
    } catch (...) {
    }
}

std::string path_to_utf8(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

std::wstring lowercase_path_component(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t character) {
                       return static_cast<wchar_t>(std::towlower(character));
                   });
    return value;
}

bool has_visibility_suffix(const std::filesystem::path& path) {
    constexpr std::wstring_view kSuffix = L".visibility.json";
    const std::wstring name = lowercase_path_component(
        path.filename().wstring());
    return name.size() >= kSuffix.size() &&
           std::wstring_view(name).substr(name.size() - kSuffix.size()) ==
               kSuffix;
}

bool normalize_sha256(std::string_view input, std::string& output) {
    if (input.size() != 64U) return false;
    output.clear();
    output.reserve(input.size());
    for (const unsigned char character : input) {
        if (!std::isxdigit(character)) return false;
        output.push_back(static_cast<char>(std::toupper(character)));
    }
    return true;
}

bool nt_succeeded(NTSTATUS status) noexcept { return status >= 0; }

class Sha256Context {
public:
    ~Sha256Context() {
        if (hash_) BCryptDestroyHash(hash_);
        if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
    }

    Sha256Context(const Sha256Context&) = delete;
    Sha256Context& operator=(const Sha256Context&) = delete;

    Sha256Context() = default;

    bool initialize(std::string& error) {
        DWORD object_bytes = 0;
        DWORD hash_bytes = 0;
        DWORD copied = 0;
        if (!nt_succeeded(BCryptOpenAlgorithmProvider(
                &algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) ||
            !nt_succeeded(BCryptGetProperty(
                algorithm_, BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&object_bytes),
                sizeof(object_bytes), &copied, 0)) ||
            copied != sizeof(object_bytes) || object_bytes == 0 ||
            !nt_succeeded(BCryptGetProperty(
                algorithm_, BCRYPT_HASH_LENGTH,
                reinterpret_cast<PUCHAR>(&hash_bytes),
                sizeof(hash_bytes), &copied, 0)) ||
            copied != sizeof(hash_bytes) || hash_bytes != 32U) {
            set_error(error, "无法初始化 Windows CNG SHA-256");
            return false;
        }
        object_.resize(object_bytes);
        digest_.resize(hash_bytes);
        if (!nt_succeeded(BCryptCreateHash(
                algorithm_, &hash_, object_.data(),
                static_cast<ULONG>(object_.size()), nullptr, 0, 0))) {
            set_error(error, "无法创建 Windows CNG SHA-256 句柄");
            return false;
        }
        return true;
    }

    bool update(std::span<const unsigned char> bytes,
                std::string& error) noexcept {
        if (bytes.empty()) return true;
        if (bytes.size() > static_cast<std::size_t>(
                std::numeric_limits<ULONG>::max()) ||
            !nt_succeeded(BCryptHashData(
                hash_, const_cast<PUCHAR>(bytes.data()),
                static_cast<ULONG>(bytes.size()), 0))) {
            set_error(error, "Windows CNG SHA-256 读取文件数据失败");
            return false;
        }
        return true;
    }

    bool finish(std::string& output, std::string& error) {
        if (!nt_succeeded(BCryptFinishHash(
                hash_, digest_.data(),
                static_cast<ULONG>(digest_.size()), 0))) {
            set_error(error, "Windows CNG SHA-256 完成计算失败");
            return false;
        }
        constexpr char kHex[] = "0123456789ABCDEF";
        output.resize(digest_.size() * 2U);
        for (std::size_t index = 0; index < digest_.size(); ++index) {
            output[index * 2U] = kHex[digest_[index] >> 4U];
            output[index * 2U + 1U] = kHex[digest_[index] & 0x0FU];
        }
        return true;
    }

private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    std::vector<unsigned char> object_;
    std::vector<unsigned char> digest_;
};

bool validate_known_fields(
    const cv::FileNode& node,
    const std::unordered_set<std::string>& expected,
    std::string_view context,
    std::string& error) {
    if (!node.isMap()) {
        set_error(error, std::string(context) + " 必须是 JSON 对象");
        return false;
    }
    for (auto iterator = node.begin(); iterator != node.end(); ++iterator) {
        const std::string name = (*iterator).name();
        if (!expected.contains(name)) {
            set_error(error, std::string(context) + " 包含未知字段：" + name);
            return false;
        }
    }
    return true;
}

bool read_required_int(const cv::FileNode& root, const char* name,
                       int& output, std::string& error) {
    const cv::FileNode node = root[name];
    if (node.empty() || !node.isInt()) {
        set_error(error, std::string("标注字段必须是整数：") + name);
        return false;
    }
    output = static_cast<int>(node);
    return true;
}

bool read_required_string(const cv::FileNode& root, const char* name,
                          std::string& output, std::string& error) {
    const cv::FileNode node = root[name];
    if (node.empty() || !node.isString()) {
        set_error(error, std::string("标注字段必须是字符串：") + name);
        return false;
    }
    output = static_cast<std::string>(node);
    if (output.empty()) {
        set_error(error, std::string("标注字段不能为空：") + name);
        return false;
    }
    return true;
}

bool match_int_field(const cv::FileNode& root, const char* name,
                     int expected, std::string& error) {
    int actual = 0;
    if (!read_required_int(root, name, actual, error)) return false;
    if (actual != expected) {
        std::ostringstream message;
        message << "标注字段与视频评价契约不一致：" << name
                << "，expected=" << expected << "，actual=" << actual;
        set_error(error, message.str());
        return false;
    }
    return true;
}

bool parse_visibility_state(std::string_view value,
                            FrameVisibility& output) noexcept {
    if (value == "visible") {
        output = FrameVisibility::VISIBLE;
    } else if (value == "not_visible") {
        output = FrameVisibility::NOT_VISIBLE;
    } else if (value == "ignore") {
        output = FrameVisibility::IGNORED;
    } else {
        return false;
    }
    return true;
}

} // namespace

std::filesystem::path video_visibility_annotation_path(
    const std::filesystem::path& directory,
    const std::filesystem::path& video_path) {
    std::filesystem::path name = video_path.filename();
    name += L".visibility.json";
    return directory / name;
}

bool validate_video_visibility_annotation_set(
    const std::filesystem::path& directory,
    std::span<const std::filesystem::path> video_paths,
    std::string& error) noexcept {
    try {
        std::error_code filesystem_error;
        if (!std::filesystem::is_directory(directory, filesystem_error) ||
            filesystem_error) {
            set_error(error, "可见性标注目录不存在：" +
                path_to_utf8(directory));
            return false;
        }

        std::set<std::wstring> expected_names;
        for (const auto& video_path : video_paths) {
            const auto annotation_path = video_visibility_annotation_path(
                directory, video_path);
            const std::wstring normalized = lowercase_path_component(
                annotation_path.filename().wstring());
            if (!expected_names.insert(normalized).second) {
                set_error(error, "多个视频映射到同一标注文件：" +
                    path_to_utf8(annotation_path.filename()));
                return false;
            }
            if (!std::filesystem::is_regular_file(
                    annotation_path, filesystem_error) || filesystem_error) {
                set_error(error, "缺少视频可见性标注：" +
                    path_to_utf8(annotation_path));
                return false;
            }
        }

        for (std::filesystem::directory_iterator iterator(
                 directory, filesystem_error), end;
             !filesystem_error && iterator != end;
             iterator.increment(filesystem_error)) {
            if (!iterator->is_regular_file() ||
                !has_visibility_suffix(iterator->path())) {
                continue;
            }
            const std::wstring normalized = lowercase_path_component(
                iterator->path().filename().wstring());
            if (!expected_names.contains(normalized)) {
                set_error(error, "标注目录存在不属于本次视频集合的文件：" +
                    path_to_utf8(iterator->path()));
                return false;
            }
        }
        if (filesystem_error) {
            set_error(error, "遍历可见性标注目录失败：" +
                filesystem_error.message());
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        set_error(error, std::string("校验可见性标注集合异常：") +
            exception.what());
        return false;
    } catch (...) {
        set_error(error, "校验可见性标注集合发生未知异常");
        return false;
    }
}

bool compute_file_sha256(const std::filesystem::path& path,
                         std::string& sha256,
                         std::string& error) noexcept {
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            set_error(error, "无法打开 SHA-256 输入文件：" +
                path_to_utf8(path));
            return false;
        }
        Sha256Context context;
        if (!context.initialize(error)) return false;

        std::array<unsigned char, kHashBufferBytes> buffer{};
        while (input) {
            input.read(reinterpret_cast<char*>(buffer.data()),
                       static_cast<std::streamsize>(buffer.size()));
            const std::streamsize read_bytes = input.gcount();
            if (read_bytes > 0 && !context.update(
                    std::span<const unsigned char>(
                        buffer.data(), static_cast<std::size_t>(read_bytes)),
                    error)) {
                return false;
            }
        }
        if (!input.eof()) {
            set_error(error, "读取 SHA-256 输入文件失败：" +
                path_to_utf8(path));
            return false;
        }
        return context.finish(sha256, error);
    } catch (const std::exception& exception) {
        set_error(error, std::string("计算文件 SHA-256 异常：") +
            exception.what());
        return false;
    } catch (...) {
        set_error(error, "计算文件 SHA-256 发生未知异常");
        return false;
    }
}

bool parse_video_visibility_annotation(
    std::string_view json,
    const VideoVisibilityExpectation& expected,
    VideoVisibilityAnnotation& annotation,
    std::string& error) noexcept {
    try {
        if (json.empty() || json.size() > kMaximumAnnotationBytes ||
            expected.video_file.empty() || expected.frame_count == 0 ||
            expected.frame_count > kMaximumAnnotatedFrames ||
            expected.source_width <= 0 || expected.source_height <= 0 ||
            expected.roi_x < 0 || expected.roi_y < 0 ||
            expected.roi_width <= 0 || expected.roi_height <= 0 ||
            static_cast<long long>(expected.roi_x) + expected.roi_width >
                expected.source_width ||
            static_cast<long long>(expected.roi_y) + expected.roi_height >
                expected.source_height) {
            set_error(error, "视频可见性标注输入契约无效");
            return false;
        }

        if (json.size() >= 3U &&
            static_cast<unsigned char>(json[0]) == 0xEFU &&
            static_cast<unsigned char>(json[1]) == 0xBBU &&
            static_cast<unsigned char>(json[2]) == 0xBFU) {
            json.remove_prefix(3U);
        }
        cv::FileStorage storage(
            std::string(json), cv::FileStorage::READ |
                cv::FileStorage::MEMORY | cv::FileStorage::FORMAT_JSON);
        if (!storage.isOpened()) {
            set_error(error, "无法解析视频可见性 JSON");
            return false;
        }
        const cv::FileNode root = storage.root();
        static const std::unordered_set<std::string> kRootFields{
            "schema_version", "video_file", "video_sha256",
            "source_width", "source_height", "frame_count",
            "input_mode", "roi_x", "roi_y", "roi_width", "roi_height",
            "policy", "intervals"};
        if (!validate_known_fields(
                root, kRootFields, "标注根节点", error)) {
            return false;
        }

        int schema_version = 0;
        int frame_count = 0;
        std::string video_file;
        std::string annotation_sha256;
        std::string input_mode;
        std::string policy;
        if (!read_required_int(
                root, "schema_version", schema_version, error)) {
            return false;
        }
        if (schema_version != kVideoVisibilitySchemaVersion) {
            set_error(error, "不支持的视频可见性标注 schema_version");
            return false;
        }
        if (!read_required_string(root, "video_file", video_file, error) ||
            !read_required_string(
                root, "video_sha256", annotation_sha256, error) ||
            !read_required_int(root, "frame_count", frame_count, error) ||
            !read_required_string(root, "input_mode", input_mode, error) ||
            !read_required_string(root, "policy", policy, error)) {
            return false;
        }
        if (video_file != expected.video_file ||
            frame_count != static_cast<int>(expected.frame_count) ||
            input_mode != expected.input_mode ||
            policy != kVideoVisibilityPolicy) {
            set_error(error, "标注的视频名、帧数、输入模式或策略与评价契约不一致");
            return false;
        }

        std::string normalized_expected_sha256;
        std::string normalized_annotation_sha256;
        if (!normalize_sha256(
                expected.video_sha256, normalized_expected_sha256) ||
            !normalize_sha256(
                annotation_sha256, normalized_annotation_sha256) ||
            normalized_expected_sha256 != normalized_annotation_sha256) {
            set_error(error, "标注绑定的视频 SHA-256 不一致或格式无效");
            return false;
        }
        if (!match_int_field(root, "source_width", expected.source_width,
                             error) ||
            !match_int_field(root, "source_height", expected.source_height,
                             error) ||
            !match_int_field(root, "roi_x", expected.roi_x, error) ||
            !match_int_field(root, "roi_y", expected.roi_y, error) ||
            !match_int_field(root, "roi_width", expected.roi_width, error) ||
            !match_int_field(root, "roi_height", expected.roi_height,
                             error)) {
            return false;
        }

        const cv::FileNode intervals = root["intervals"];
        if (intervals.empty() || !intervals.isSeq() || intervals.size() == 0) {
            set_error(error, "标注 intervals 必须是非空数组");
            return false;
        }
        static const std::unordered_set<std::string> kIntervalFields{
            "start_frame", "end_frame", "state"};
        VideoVisibilityAnnotation candidate;
        candidate.policy = policy;
        candidate.frames.resize(expected.frame_count);
        std::size_t next_frame = 0;
        for (auto iterator = intervals.begin(); iterator != intervals.end();
             ++iterator) {
            const cv::FileNode interval = *iterator;
            if (!validate_known_fields(
                    interval, kIntervalFields, "标注区间", error)) {
                return false;
            }
            int start_frame = 0;
            int end_frame = 0;
            std::string state_text;
            FrameVisibility state = FrameVisibility::IGNORED;
            if (!read_required_int(
                    interval, "start_frame", start_frame, error) ||
                !read_required_int(
                    interval, "end_frame", end_frame, error) ||
                !read_required_string(interval, "state", state_text, error) ||
                !parse_visibility_state(state_text, state)) {
                if (!state_text.empty()) {
                    set_error(error, "未知标注状态：" + state_text);
                }
                return false;
            }
            if (start_frame < 0 || end_frame < start_frame ||
                static_cast<std::size_t>(start_frame) != next_frame ||
                static_cast<std::size_t>(end_frame) >=
                    expected.frame_count) {
                set_error(error,
                    "标注区间必须按帧号升序、首尾相接且位于视频范围内");
                return false;
            }
            std::fill(
                candidate.frames.begin() + start_frame,
                candidate.frames.begin() + end_frame + 1, state);
            next_frame = static_cast<std::size_t>(end_frame) + 1U;
        }
        if (next_frame != expected.frame_count) {
            set_error(error, "标注区间没有完整覆盖视频全部帧");
            return false;
        }
        annotation = std::move(candidate);
        return true;
    } catch (const cv::Exception& exception) {
        set_error(error, std::string("解析视频可见性 JSON 失败：") +
            exception.what());
        return false;
    } catch (const std::exception& exception) {
        set_error(error, std::string("解析视频可见性标注异常：") +
            exception.what());
        return false;
    } catch (...) {
        set_error(error, "解析视频可见性标注发生未知异常");
        return false;
    }
}

bool load_video_visibility_annotation(
    const std::filesystem::path& path,
    const VideoVisibilityExpectation& expected,
    VideoVisibilityAnnotation& annotation,
    std::string& error) noexcept {
    try {
        std::error_code filesystem_error;
        const std::uintmax_t file_bytes = std::filesystem::file_size(
            path, filesystem_error);
        if (filesystem_error || file_bytes == 0 ||
            file_bytes > kMaximumAnnotationBytes) {
            set_error(error, "可见性标注文件为空、过大或不可读取：" +
                path_to_utf8(path));
            return false;
        }
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            set_error(error, "无法打开可见性标注文件：" +
                path_to_utf8(path));
            return false;
        }
        std::string json(static_cast<std::size_t>(file_bytes), '\0');
        input.read(json.data(), static_cast<std::streamsize>(json.size()));
        if (input.gcount() != static_cast<std::streamsize>(json.size()) ||
            !input) {
            set_error(error, "无法完整读取可见性标注文件：" +
                path_to_utf8(path));
            return false;
        }
        return parse_video_visibility_annotation(
            json, expected, annotation, error);
    } catch (const std::exception& exception) {
        set_error(error, std::string("加载视频可见性标注异常：") +
            exception.what());
        return false;
    } catch (...) {
        set_error(error, "加载视频可见性标注发生未知异常");
        return false;
    }
}

void record_video_visibility(FrameVisibility visibility,
                             bool detected,
                             VideoVisibilityMetrics& metrics) noexcept {
    metrics.annotations_present = true;
    ++metrics.annotated_frames;
    if (visibility == FrameVisibility::VISIBLE) {
        ++metrics.visible_frames;
        if (detected) {
            ++metrics.visible_detected_frames;
            metrics.current_visible_miss_sequence = 0;
        } else {
            ++metrics.visible_missed_frames;
            ++metrics.current_visible_miss_sequence;
            metrics.longest_visible_miss_sequence = std::max(
                metrics.longest_visible_miss_sequence,
                metrics.current_visible_miss_sequence);
        }
        return;
    }

    metrics.current_visible_miss_sequence = 0;
    if (visibility == FrameVisibility::NOT_VISIBLE) {
        ++metrics.not_visible_frames;
        if (detected) ++metrics.not_visible_detected_frames;
    } else {
        ++metrics.ignored_frames;
        if (detected) ++metrics.ignored_detected_frames;
    }
}

bool video_visibility_recall_available(
    const VideoVisibilityMetrics& metrics) noexcept {
    return metrics.annotations_present && metrics.visible_frames > 0;
}

double video_visibility_recall(
    const VideoVisibilityMetrics& metrics) noexcept {
    if (!video_visibility_recall_available(metrics)) return 0.0;
    return static_cast<double>(metrics.visible_detected_frames) /
           static_cast<double>(metrics.visible_frames);
}

double video_visibility_evaluable_rate(
    const VideoVisibilityMetrics& metrics) noexcept {
    if (!metrics.annotations_present || metrics.annotated_frames == 0) {
        return 0.0;
    }
    return static_cast<double>(
               metrics.visible_frames + metrics.not_visible_frames) /
           static_cast<double>(metrics.annotated_frames);
}

} // namespace detector::detail
