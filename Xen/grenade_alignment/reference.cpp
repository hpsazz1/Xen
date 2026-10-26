#include "grenade_alignment/grenade_alignment.h"
#include "movement/timeline.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>
#include <array>
#include <cmath>
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
}

Reference load_reference(const std::filesystem::path& plan_path,
                         const Json& expected_plan) {
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
        if (manifest.at("schema_version").get<int>() != 1 ||
            !manifest.at("manual_throw_verified").is_boolean() ||
            !manifest.at("manual_throw_verified").get<bool>())
            throw std::runtime_error("参考资源尚未绑定人工投掷成功记录");
        reference.plan = canonical_plan(manifest.at("plan"));
        if (reference.plan != expected)
            throw std::runtime_error("参考资源与当前道具方案版本不一致");
        const auto& geometry = manifest.at("geometry");
        auto& g = reference.geometry;
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
        const auto standing = read_bytes(image_path(reference.directory, manifest,
            "standing_image", "standing.png"), 64U * 1024U * 1024U);
        const auto aim = read_bytes(image_path(reference.directory, manifest,
            "aim_image", "aim.png"), 64U * 1024U * 1024U);
        const auto standing_hash=sha256(standing), aim_hash=sha256(aim);
        if (manifest.at("image_sha256").at("standing_image").get<std::string>()!=standing_hash ||
            manifest.at("image_sha256").at("aim_image").get<std::string>()!=aim_hash)
            throw std::runtime_error("参考图片摘要与人工确认时不一致，请重新登记参考资源");
        reference.standing_image = cv::imdecode(standing, cv::IMREAD_COLOR);
        reference.aim_image = cv::imdecode(aim, cv::IMREAD_COLOR);
        for (const auto* image : {&reference.standing_image, &reference.aim_image})
            if (image->empty() || image->cols != g.source_width || image->rows != g.source_height)
                throw std::runtime_error("参考图片不是声明分辨率的原始全屏图");
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
