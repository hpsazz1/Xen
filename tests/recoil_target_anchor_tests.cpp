#include "recoil_tuner/target_anchor.h"
#include <opencv2/imgproc.hpp>
#include <cmath>
#include <iostream>
#include <limits>

namespace {
int failures = 0;
void expect(bool ok, const char* message) {
    if (!ok) { ++failures; std::cerr << message << '\n'; }
}
cv::Mat texture() {
    cv::Mat result(300, 420, CV_8UC1);
    cv::RNG random(314159);
    random.fill(result, cv::RNG::UNIFORM, 25, 230);
    cv::GaussianBlur(result, result, {3, 3}, 0.6);
    return result;
}
cv::Mat translate(const cv::Mat& source, double x, double y) {
    cv::Mat result;
    const cv::Mat transform = (cv::Mat_<double>(2, 3) << 1, 0, x, 0, 1, y);
    cv::warpAffine(source, result, transform, source.size(), cv::INTER_LINEAR, cv::BORDER_REFLECT_101);
    return result;
}
bool near(cv::Point2d value, cv::Point2d expected, double tolerance = 0.2) {
    return std::hypot(value.x - expected.x, value.y - expected.y) < tolerance;
}
}

int main() {
    using namespace recoil_tuner;
    const TargetAnchorConfig config{{60, 100, 64, 64}, {280, 100, 64, 64}, 0.01, {24, 24}};
    const auto original = texture();
    auto input = original.clone();
    TargetAnchor anchor;
    std::string error;
    expect(!anchor.observe(original).valid, "未初始化不能产生观测");
    expect(anchor.initialize(input, config, error) && error.empty(), "固定纹理初始化成功");
    auto observed = anchor.observe(original);
    expect(observed.valid && near(observed.shift, {0, 0}), "静态画面零位移");
    expect(near(observed.position, {91.5, 131.5}), "锚点位置使用冻结模板像素中心");
    expect(observed.target_quality.texture_stddev > 5 && observed.target_quality.template_score > 0.99,
        "观测保留真实纹理与配准质量");

    input.setTo(0);
    expect(anchor.observe(original).valid, "调用者复用采集缓冲不能改写冻结模板");
    for (const auto delta : {cv::Point2d(7, -5), cv::Point2d(-6, 4), cv::Point2d(2.5, -1.25)}) {
        observed = anchor.observe(translate(original, delta.x, delta.y));
        expect(observed.valid && near(observed.shift, delta), "共同平移保持相机位移测量");
        expect(near(observed.background_shift, delta), "独立背景核对共同平移");
    }
    expect(near(anchor.observe(original).shift, {0, 0}), "多帧后仍对原参考测量，不累加相邻帧漂移");

    auto local_motion = original.clone();
    const auto moved = translate(original, 6, 0);
    const cv::Rect target_neighborhood(40, 80, 104, 104);
    moved(target_neighborhood).copyTo(local_motion(target_neighborhood));
    observed = anchor.observe(local_motion);
    expect(!observed.valid && observed.failure == "target_background_motion_inconsistent",
        "仅目标运动必须经独立背景差异拒绝");
    expect(observed.target_quality.failure.empty() && observed.background_quality.failure.empty(),
        "局部运动负例单独配准成功，拒绝来自目标背景一致性");

    auto occluded = original.clone();
    occluded(target_neighborhood).setTo(0);
    expect(!anchor.observe(occluded).valid, "目标遮挡拒绝，不能沿用旧位置");
    auto changed_background = original.clone();
    changed_background(config.background_roi).setTo(0);
    expect(!anchor.observe(changed_background).valid, "背景核对丢失拒绝");
    expect(!anchor.observe(cv::Mat(301, 420, CV_8UC1)).valid, "分辨率变化拒绝");
    cv::Mat color;
    cv::cvtColor(original, color, cv::COLOR_GRAY2BGR);
    expect(!anchor.observe(color).valid, "帧格式变化拒绝");
    cv::Mat scaled;
    const auto transform = cv::getRotationMatrix2D({91.5f, 131.5f}, 0, 1.3);
    cv::warpAffine(original, scaled, transform, original.size());
    expect(!anchor.observe(scaled).valid, "明显尺度变化不当成平移成功");
    expect(anchor.observe(original).valid, "失败观测不悄悄换模板，终止本轮由协调器负责");

    auto invalid = config;
    invalid.background_roi = invalid.target_roi;
    expect(!anchor.initialize(original, invalid, error), "两块ROI重叠拒绝伪独立核对");
    expect(!anchor.observe(original).valid, "重新初始化失败撤销旧参考");
    invalid = config;
    invalid.max_relative_shift_normalized = 0;
    expect(!anchor.initialize(original, invalid, error), "一致性容差必须显式预冻结");
    invalid.max_relative_shift_normalized = std::numeric_limits<double>::quiet_NaN();
    expect(!anchor.initialize(original, invalid, error), "非有限容差拒绝");
    invalid = config;
    invalid.target_roi.x = std::numeric_limits<int>::max();
    expect(!anchor.initialize(original, invalid, error), "ROI整数溢出拒绝");
    expect(!anchor.initialize(cv::Mat(original.size(), CV_8UC1, cv::Scalar(80)), config, error),
        "平坦模板初始化时拒绝");
    auto blank_background = original.clone();
    blank_background(config.background_roi).setTo(80);
    expect(!anchor.initialize(blank_background, config, error) && error == "background_insufficient_texture",
        "背景模板必须独立具备纹理");
    expect(anchor.initialize(color, config, error) && anchor.observe(color).valid, "彩色原始帧可直接观测");
    return failures == 0 ? 0 : 1;
}
