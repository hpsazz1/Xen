#include "lineup/host_capture_internal.h"
#include <opencv2/imgcodecs.hpp>
#include <fstream>
#include <iostream>
#include <chrono>
#include <stdexcept>
#include <vector>

namespace {
using Json = nlohmann::json;
void check(bool condition, const char *reason) { if (!condition) throw std::runtime_error(reason); }
void write_bytes(const std::filesystem::path &path, const std::vector<unsigned char> &bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    check(file.good(), "测试文件写入失败");
}
void write_json(const std::filesystem::path &path, const Json &value) {
    const auto text = value.dump();
    write_bytes(path, {text.begin(), text.end()});
}
void write_png(const std::filesystem::path &path, const cv::Mat &image) {
    std::vector<unsigned char> bytes;
    check(cv::imencode(".png", image, bytes), "测试 PNG 编码失败");
    write_bytes(path, bytes);
}
} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("xen-host-capture-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        const Json action = {{"schema", 1}, {"type", "phases"}, {"phases", Json::array({
            {{"buttons", Json::array({"left", "right"})}, {"movement", Json::array()},
             {"jump", true}, {"duration_ms", nullptr}},
            {{"buttons", Json::array()}, {"movement", Json::array()},
             {"jump", false}, {"duration_ms", nullptr}}})}};
        const Json profile = {{"schema", 1}, {"source_id", "合成主机来源"},
            {"source_width", 641}, {"source_height", 481}, {"map", "de_dust2"}, {"team", "CT"},
            {"name", "合成参考"}, {"target", "当前瞄点"}, {"grenade", "烟雾弹"},
            {"throw_action", action}, {"throw_instructions", "原地双键跳投；时序待填写"}};
        cv::Mat original(481, 641, CV_8UC3);
        cv::RNG random(20261007); random.fill(original, cv::RNG::UNIFORM, 0, 256);
        const auto source = original.clone();
        lineup::HostCaptureBundle bundle, loaded;
        std::string error;
        check(lineup::make_host_capture_bundle(original, profile, "capture-valid", "2026-10-07T04:00:00.123Z", bundle, error),
              "同帧截图包创建失败");
        check(bundle.metadata["roi"] == Json{{"x", 160}, {"y", 80}, {"width", 320}, {"height", 320}},
              "奇数全屏尺寸必须按整数中心规则裁剪");
        check(cv::norm(bundle.roi, source(cv::Rect(160, 80, 320, 320)), cv::NORM_INF) == 0 &&
              cv::norm(bundle.full, source, cv::NORM_INF) == 0 && cv::norm(bundle.overview, source, cv::NORM_INF) > 0,
              "全屏与 ROI 保持干净像素，标框仅存在独立展示图");
        original.setTo(cv::Scalar(0, 0, 0));
        check(cv::norm(bundle.full, source, cv::NORM_INF) == 0, "截图包必须拥有像素而非借用输入缓冲");
        std::filesystem::path published;
        check(lineup::write_host_capture_bundle(root, bundle, published, error), "截图包原子发布失败");
        check(published == root / "capture-valid" && lineup::read_host_capture_bundle(published, loaded, error),
              "已发布截图包无法回读");
        check(loaded.metadata == bundle.metadata && loaded.metadata["profile"] == profile &&
              cv::norm(loaded.full, source, cv::NORM_INF) == 0 && cv::norm(loaded.roi, bundle.roi, cv::NORM_INF) == 0,
              "回读像素、来源、分类和空时序动作须完整保留");
        check(!lineup::write_host_capture_bundle(root, bundle, published, error) && published.empty(),
              "相同编号不得覆盖已有证据");

        auto malformed = profile; malformed["source_width"] = 640;
        check(!lineup::make_host_capture_bundle(source, malformed, "capture-wrong-size", "2026-10-07T04:00:00.123Z", loaded, error),
              "辅机设置几何不匹配必须拒绝");
        check(!lineup::make_host_capture_bundle(source, profile, "../escape", "2026-10-07T04:00:00.123Z", loaded, error),
              "编号路径穿越必须拒绝");
        check(!lineup::make_host_capture_bundle(source, profile, "CON", "2026-10-07T04:00:00.123Z", loaded, error),
              "Windows 保留名称必须拒绝");
        check(!lineup::make_host_capture_bundle(source, profile, "capture-invalid-time", "2026-02-30T04:00:00.123Z", loaded, error),
              "无效 UTC 日期必须拒绝");
        check(!lineup::make_host_capture_bundle(cv::Mat(481, 641, CV_8UC4), profile, "capture-bgra", "2026-10-07T04:00:00.123Z", loaded, error),
              "非 BGR 输入必须拒绝");

        const auto publish_case = [&](const char *id) {
            auto value = bundle; value.metadata["id"] = id;
            std::filesystem::path directory;
            check(lineup::write_host_capture_bundle(root, value, directory, error), "损坏用例原始包发布失败");
            return directory;
        };
        auto directory = publish_case("capture-roi-tampered");
        auto changed = bundle.roi.clone(); changed.at<cv::Vec3b>(30, 30)[0] ^= 1;
        write_png(directory / "roi.png", changed);
        check(!lineup::read_host_capture_bundle(directory, loaded, error) && loaded.full.empty(),
              "ROI 单像素篡改必须拒绝且不能泄露部分成功结果");
        directory = publish_case("capture-overview-tampered");
        write_png(directory / "overview.png", bundle.full);
        check(!lineup::read_host_capture_bundle(directory, loaded, error), "展示图标框不一致必须拒绝");
        directory = publish_case("capture-incomplete");
        std::filesystem::remove(directory / "overview.png");
        check(!lineup::read_host_capture_bundle(directory, loaded, error), "不完整包不能导入");
        directory = publish_case("capture-oversized-png");
        std::vector<unsigned char> encoded;
        cv::imencode(".png", bundle.roi, encoded); encoded[16] = 0x7f;
        write_bytes(directory / "roi.png", encoded);
        check(!lineup::read_host_capture_bundle(directory, loaded, error), "PNG 头部巨大尺寸须在解码前拒绝");
        directory = publish_case("capture-path-mismatch");
        auto metadata = bundle.metadata; metadata["id"] = "capture-another";
        write_json(directory / "capture.json", metadata);
        check(!lineup::read_host_capture_bundle(directory, loaded, error), "目录与编号不符不能导入");

        const auto profile_path = root / "profile.json";
        write_json(profile_path, profile);
        Json recovered;
        check(lineup::read_host_capture_profile(profile_path, recovered, error) && recovered == profile,
              "采集设置必须保留全部已配置字段");
        malformed = profile; malformed["throw_action"]["schema"] = 2;
        write_json(profile_path, malformed);
        check(!lineup::read_host_capture_profile(profile_path, recovered, error), "设置中的无效动作必须拒绝");
        write_bytes(profile_path, std::vector<unsigned char>(65537, ' '));
        check(!lineup::read_host_capture_profile(profile_path, recovered, error), "超大设置文件必须拒绝");
        std::cout << "主机截图包合成接口测试通过；未采集桌面、未注册热键、未操作设备\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "；合成证据目录=" << root << '\n';
        return 1;
    }
}
