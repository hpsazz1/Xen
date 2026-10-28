#include "grenade_alignment/grenade_alignment.h"
#include "movement/timeline.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <opencv2/imgcodecs.hpp>

namespace grenade_alignment {
namespace {
using Json = nlohmann::json;
std::vector<unsigned char> read_bytes(const std::filesystem::path& path,
                                      std::uintmax_t maximum) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > maximum)
        throw std::runtime_error("参考资源缺失、为空或超过大小限制");
    std::ifstream input(path, std::ios::binary);
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    if (!input.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
        throw std::runtime_error("参考资源读取失败");
    return bytes;
}
std::string sha256(const std::vector<unsigned char>& bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("参考资源摘要初始化失败");
    struct Close { BCRYPT_ALG_HANDLE value; ~Close() { BCryptCloseAlgorithmProvider(value, 0); } } close{algorithm};
    std::array<unsigned char, 32> hash{};
    if (BCryptHash(algorithm, nullptr, 0, const_cast<PUCHAR>(bytes.data()),
                   static_cast<ULONG>(bytes.size()), hash.data(), static_cast<ULONG>(hash.size())) < 0)
        throw std::runtime_error("参考资源摘要计算失败");
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto byte : hash) out << std::setw(2) << static_cast<unsigned>(byte);
    return out.str();
}
Json canonical_plan(const Json& value) {
    movement::timeline::Plan plan;
    std::string error;
    if (!movement::timeline::parse_plan(value, plan, error))
        throw std::runtime_error("参考绑定方案不合法：" + error);
    return movement::timeline::plan_json(plan);
}
std::filesystem::path image_path(const std::filesystem::path& directory,
                                const Json& manifest, const char* field,
                                const char* expected) {
    // 首版只有这两张原图，不接受引用任意本机路径或目录外的软链接。
    if (manifest.at(field).get<std::string>() != expected)
        throw std::runtime_error("参考图片名称必须为 standing.png 和 aim.png");
    const auto path = std::filesystem::canonical(directory / expected);
    if (path.parent_path() != std::filesystem::canonical(directory))
        throw std::runtime_error("参考图片不能引用资源目录之外的文件");
    return path;
}
cv::Size png_dimensions(const std::vector<unsigned char>& bytes) {
    const std::array<unsigned char, 8> signature{137,80,78,71,13,10,26,10};
    if (bytes.size() < 33 || !std::equal(signature.begin(),signature.end(),bytes.begin()) ||
        bytes[8] != 0 || bytes[9] != 0 || bytes[10] != 0 || bytes[11] != 13 ||
        bytes[12] != 'I' || bytes[13] != 'H' || bytes[14] != 'D' || bytes[15] != 'R')
        throw std::runtime_error("参考图片必须为有效PNG");
    const auto read = [&](std::size_t offset) {
        return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
            (static_cast<std::uint32_t>(bytes[offset+1]) << 16) |
            (static_cast<std::uint32_t>(bytes[offset+2]) << 8) | bytes[offset+3];
    };
    const auto width = read(16), height = read(20);
    if (width == 0 || height == 0 || width > 8192 || height > 8192)
        throw std::runtime_error("参考PNG尺寸超出范围");
    return {static_cast<int>(width),static_cast<int>(height)};
}
void validate_standing_marker(const Json& marker) {
    if (!marker.is_object()) throw std::runtime_error("站位标记格式无效");
    for (const auto* field : {"x", "y"}) {
        if (!marker.contains(field) || !marker[field].is_number())
            throw std::runtime_error("站位标记坐标无效");
        const auto value = marker[field].get<double>();
        if (!std::isfinite(value) || value < 0 || value > 1)
            throw std::runtime_error("站位标记必须位于原图范围内");
    }
    if (!marker.contains("label") || !marker["label"].is_string())
        throw std::runtime_error("站位标记名称无效");
    const auto label = marker["label"].get<std::string>();
    if (label.empty() || label.size() > 64 || std::any_of(label.begin(),label.end(),[](unsigned char value) {
        return value < 32 || value == 127;
    })) throw std::runtime_error("站位标记名称无效");
}
}

Reference load_reference(const std::filesystem::path& plan_path,
                         const Json& expected_plan, bool allow_partial) {
    try {
        if (plan_path.extension() != ".json" || plan_path.stem().empty() ||
            plan_path.stem()=="." || plan_path.stem()=="..")
            throw std::runtime_error("参考资源需要已保存的道具方案文件");
        const auto expected = canonical_plan(expected_plan);
        const auto stored = read_bytes(plan_path, 1024U * 1024U);
        if (canonical_plan(Json::parse(stored.begin(), stored.end())) != expected)
            throw std::runtime_error("磁盘道具方案已变化，请重新选择");
        Reference reference;
        reference.directory = std::filesystem::weakly_canonical(
            plan_path.parent_path().parent_path() / "references" / plan_path.stem());
        const auto manifest_bytes = read_bytes(reference.directory / "reference.json", 1024U * 1024U);
        const auto manifest = Json::parse(manifest_bytes.begin(), manifest_bytes.end());
        const auto filename=plan_path.filename().u8string(), stem=plan_path.stem().u8string();
        if ((manifest.contains("plan_id") && manifest.at("plan_id").get<std::string>()!=
                std::string(filename.begin(),filename.end())) ||
            (manifest.contains("reference_id") && manifest.at("reference_id").get<std::string>()!=
                std::string(stem.begin(),stem.end())))
            throw std::runtime_error("参考资源标识与选中方案文件不一致");
        const bool has_aim = manifest.contains("aim_image"), has_standing = manifest.contains("standing_image");
        if (!has_aim && !allow_partial)
            throw std::runtime_error("瞄点图待采集，请恢复正确瞄角后按F10");
        if ((!has_aim && !has_standing) || manifest.value("capture_complete", true) != (has_aim && has_standing))
            throw std::runtime_error("参考资源的图片声明不完整");
        if (manifest.at("schema_version").get<int>() != 1 ||
            !manifest.at("manual_throw_verified").is_boolean() ||
            (has_aim && !manifest.at("manual_throw_verified").get<bool>()))
            throw std::runtime_error("参考资源尚未绑定人工投掷成功记录");
        if (manifest.contains("standing_marker")) validate_standing_marker(manifest.at("standing_marker"));
        reference.plan = canonical_plan(manifest.at("plan"));
        if (reference.plan != expected)
            throw std::runtime_error("参考资源与当前道具方案版本不一致");
        // F9更新站位时保留旧瞄点的方案快照；只有F10能重新绑定瞄点。
        if (has_aim && !allow_partial &&
            canonical_plan(manifest.value("aim_plan", manifest.at("plan"))) != expected)
            throw std::runtime_error("瞄点图与当前道具方案版本不一致，请恢复正确瞄角后按F10更新");
        auto& g = reference.geometry;
        if (has_aim) {
            const auto& geometry = manifest.at("geometry");
            g.source_width = geometry.at("source_width").get<int>();
            g.source_height = geometry.at("source_height").get<int>();
            g.roi_width = geometry.at("roi_width").get<int>();
            g.roi_height = geometry.at("roi_height").get<int>();
            g.roi_x = geometry.at("roi_x").get<double>();
            g.roi_y = geometry.at("roi_y").get<double>();
            g.scale_x = geometry.at("scale_x").get<double>();
            g.scale_y = geometry.at("scale_y").get<double>();
            if (g.source_width <= 0 || g.source_height <= 0 ||
                g.source_width > 8192 || g.source_height > 8192 ||
                g.roi_width != 320 || g.roi_height != 320 ||
                !std::isfinite(g.roi_x) || !std::isfinite(g.roi_y) ||
                !std::isfinite(g.scale_x) || !std::isfinite(g.scale_y) ||
                g.roi_x < 0 || g.roi_y < 0 || g.scale_x <= 0 || g.scale_y <= 0 ||
                g.roi_x + g.roi_width * g.scale_x > g.source_width ||
                g.roi_y + g.roi_height * g.scale_y > g.source_height)
                throw std::runtime_error("参考图必须声明有效的全屏到320×320 ROI映射");
            reference.target.x = manifest.value("target_x", 160.0);
            reference.target.y = manifest.value("target_y", 160.0);
            if (!std::isfinite(reference.target.x) || !std::isfinite(reference.target.y) ||
                reference.target.x < 0 || reference.target.y < 0 ||
                reference.target.x >= g.roi_width || reference.target.y >= g.roi_height)
                throw std::runtime_error("参考瞄点必须位于320×320 ROI内");
        }
        // F9/F10独立保存；只解码manifest声明的图片，不借残留旧文件补造另一角色。
        const auto load_image = [&](const char* field, const char* filename, cv::Mat& output) {
            if (!manifest.contains(field)) return std::string{};
            const auto bytes = read_bytes(image_path(reference.directory, manifest, field, filename), 64U * 1024U * 1024U);
            const auto hash = sha256(bytes);
            if (manifest.at("image_sha256").at(field).get<std::string>() != hash)
                throw std::runtime_error("参考图片摘要与采集记录不一致，请重新采集对应图片");
            const auto size = png_dimensions(bytes);
            output = cv::imdecode(bytes, cv::IMREAD_COLOR);
            if (output.empty() || output.size() != size)
                throw std::runtime_error("参考PNG解码尺寸与图片头不一致");
            return hash;
        };
        const auto standing_hash = load_image("standing_image", "standing.png", reference.standing_image);
        const auto aim_hash = load_image("aim_image", "aim.png", reference.aim_image);
        if (has_aim && reference.aim_image.size() != cv::Size(g.source_width,g.source_height))
            throw std::runtime_error("瞄点图不是声明分辨率的原始全屏图");
        const auto digest = sha256(manifest_bytes) + standing_hash + aim_hash;
        reference.revision = sha256(std::vector<unsigned char>(digest.begin(), digest.end()));
        return reference;
    } catch (const nlohmann::json::exception&) {
        throw std::runtime_error("reference.json字段缺失或类型错误");
    } catch (const cv::Exception&) {
        throw std::runtime_error("参考PNG图像解码失败");
    } catch (const std::filesystem::filesystem_error&) {
        throw std::runtime_error("参考资源目录或图片不存在、不可读");
    }
}
}
