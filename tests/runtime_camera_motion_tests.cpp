#include "runtime/camera_motion_internal.h"

#include <cmath>
#include <iostream>
#include <string>

#include <opencv2/imgproc.hpp>

namespace {
int failures = 0;
void expect(bool value, const std::string& message) {
    if (!value) { ++failures; std::cerr << "[失败] " << message << '\n'; }
}
using Status = AimBackgroundMotionStatus;
using Estimator = runtime::detail::CameraMotionEstimator;

cv::Mat texture() {
    cv::Mat gray(320, 320, CV_8UC1);
    cv::RNG random(241099);
    random.fill(gray, cv::RNG::UNIFORM, 0, 256);
    cv::GaussianBlur(gray, gray, cv::Size(3, 3), 0.6);
    cv::Mat bgr;
    cv::cvtColor(gray, bgr, cv::COLOR_GRAY2BGR);
    return bgr;
}
cv::Mat translated(const cv::Mat& image, double dx, double dy = 0.0) {
    cv::Mat result;
    const cv::Mat transform = (cv::Mat_<double>(2, 3) << 1, 0, dx, 0, 1, dy);
    cv::warpAffine(image, result, transform, image.size(),
                   cv::INTER_LINEAR, cv::BORDER_REFLECT_101);
    return result;
}
CapturedFrame capture(const cv::Mat& image, std::uint64_t sequence) {
    CapturedFrame result;
    result.bgr = image;
    result.width = image.cols;
    result.height = image.rows;
    result.source_width = 2560;
    result.source_height = 1440;
    result.roi_x = 1120;
    result.roi_y = 560;
    result.timing.sequence = sequence;
    result.timing.captured_at = std::chrono::steady_clock::time_point(
        std::chrono::milliseconds(1000 + sequence * 4));
    return result;
}
AimFrame input(const CapturedFrame& captured) {
    AimFrame frame;
    frame.sequence = captured.timing.sequence;
    frame.captured_at = captured.timing.captured_at;
    frame.roi_width = captured.width;
    frame.roi_height = captured.height;
    frame.source_pixels_per_roi_pixel_x =
        static_cast<float>(captured.source_pixels_per_pixel_x);
    frame.source_pixels_per_roi_pixel_y =
        static_cast<float>(captured.source_pixels_per_pixel_y);
    return frame;
}
void test_translation_zero_and_owned_history() {
    const auto original = texture();
    for (double dx : {3.0, -4.0, 0.0}) {
        Estimator estimator;
        auto first = capture(original.clone(), 1);
        auto warm = estimator.observe(first, input(first));
        expect(warm.motion.status == Status::WARMING, "首帧须暖机");
        // 归还 Capture 后其存储可被改写，estimator 的上一帧不能跟着变。
        first.bgr.setTo(cv::Scalar::all(0));
        auto second = capture(translated(original, dx), 2);
        const auto result = estimator.observe(second, input(second));
        expect(result.motion.status == Status::VALID, "正负平移及真零应有效");
        expect(std::fabs(result.motion.dx_roi_pixels - dx) < 0.2,
               "真实生产估计应恢复背景平移而不借用旧帧");
        expect(result.motion.previous_sequence == 1 && result.motion.sequence == 2 &&
               result.motion.observation_epoch == warm.observation_epoch,
               "测量保留实际帧对及epoch");
        expect(result.motion.usable_patch_count == 2 && result.elapsed_ms >= 0,
               "两空间patch及耗时必须实际产生");
    }
}
void test_quality_and_foreground() {
    const auto original = texture();
    Estimator estimator;
    auto first = capture(cv::Mat(320, 320, CV_8UC3, cv::Scalar::all(128)), 1);
    estimator.observe(first, input(first));
    auto second = capture(first.bgr, 2);
    expect(estimator.observe(second, input(second)).motion.status == Status::LOW_TEXTURE,
           "低纹理真零不能伪装可靠测量");

    estimator.reset();
    first = capture(original, 3);
    auto frame = input(first);
    Detection obstruction;
    obstruction.x1 = 20; obstruction.y1 = 60;
    obstruction.x2 = 30; obstruction.y2 = 80;
    frame.detections.push_back(obstruction);
    estimator.observe(first, frame);
    second = capture(translated(original, 2), 4);
    expect(estimator.observe(second, input(second)).motion.status == Status::FOREGROUND,
           "上一帧任意检测遮挡也必须拒绝");
    auto third = capture(translated(original, 4), 5);
    frame = input(third);
    frame.detections.push_back(obstruction);
    expect(estimator.observe(third, frame).motion.status == Status::FOREGROUND,
           "当前帧任意检测遮挡必须拒绝");
    auto both = capture(translated(original, 6), 6);
    frame = input(both);
    frame.detections.push_back(obstruction);
    expect(estimator.observe(both, frame).motion.status == Status::FOREGROUND,
           "前后两帧同时遮挡也不得产出背景");

    estimator.reset();
    first = capture(original, 6);
    estimator.observe(first, input(first));
    cv::Mat different = translated(original, 3);
    const cv::Mat reversed = translated(original, -3);
    reversed(cv::Rect(208, 48, 96, 160)).copyTo(
        different(cv::Rect(208, 48, 96, 160)));
    second = capture(different, 7);
    expect(estimator.observe(second, input(second)).motion.status == Status::INCONSISTENT,
           "左右背景不同向不能平均成零");
}
void test_pair_geometry_reset_and_unsupported() {
    const auto original = texture();
    Estimator estimator;
    auto first = capture(original, 1);
    const auto warm = estimator.observe(first, input(first));
    auto second = capture(original, 2);
    second.roi_x += 1;
    const auto geometry = estimator.observe(second, input(second));
    expect(geometry.motion.status == Status::INVALID_GEOMETRY &&
           geometry.observation_epoch != warm.observation_epoch,
           "ROI改变必须隔离旧图像区间并换epoch");
    auto third = capture(original, 3);
    third.roi_x = second.roi_x;
    expect(estimator.observe(third, input(third)).motion.status == Status::VALID,
           "新几何下一对可重新有效");
    expect(estimator.observe(third, input(third)).motion.status == Status::INVALID_PAIR,
           "重复时间/sequence不得重复观测");
    auto fourth = capture(original, 4);
    expect(estimator.observe(fourth, input(fourth), true).motion.status == Status::WARMING,
           "源时钟reset必须暖机");
    estimator.reset();
    auto fifth = capture(original, 5);
    expect(estimator.observe(fifth, input(fifth)).motion.status == Status::WARMING,
           "检测失败或重载显式reset后不得跨缺口测量");
    auto gpu = capture({}, 6);
    gpu.storage = CapturedFrameStorage::D3D11_BGRA8;
    expect(estimator.observe(gpu, input(gpu)).motion.status == Status::UNSUPPORTED,
           "GPU-only明确不支持，不触发隐式D2H");
    auto seventh = capture(original, 7);
    expect(estimator.observe(seventh, input(seventh)).motion.status == Status::WARMING,
           "不支持路径重获CPU后须暖机");

    auto skipped = capture(translated(original, 2), 10);
    const auto skipped_result = estimator.observe(skipped, input(skipped));
    expect(skipped_result.motion.status == Status::VALID &&
           skipped_result.motion.previous_sequence == 7 &&
           skipped_result.motion.sequence == 10,
           "latest跳帧以真实帧对为准，不要求sequence加一");
    auto source = capture(original, 11);
    source.timing.source_time_timing_valid = true;
    source.timing.source_time_at = source.timing.captured_at;
    source.timing.source_clock_session_id = 7;
    source.timing.source_time_basis = SourceTimeBasis::NDI_SDK_SUBMISSION;
    auto source_result = estimator.observe(source, input(source));
    expect(source_result.motion.status == Status::WARMING &&
           source_result.observation_epoch != skipped_result.observation_epoch,
           "即使调用方漏传reset，源时基变化仍隔离");
    auto source_next = source;
    source_next.timing.sequence = 12;
    source_next.timing.captured_at += std::chrono::milliseconds(4);
    source_next.timing.source_time_at = source_next.timing.captured_at;
    source_next.timing.source_clock_session_id = 8;
    expect(estimator.observe(source_next, input(source_next)).motion.status == Status::WARMING,
           "源session改变须重新暖机");
    auto mismatch = input(source_next);
    mismatch.sequence = 13;
    expect(estimator.observe(source_next, mismatch).motion.status == Status::INVALID_PAIR,
           "Capture与Aim帧身份不同必须拒绝");
    auto scaled = capture(original, 14);
    estimator.observe(scaled, input(scaled));
    scaled.timing.sequence = 15;
    scaled.timing.captured_at += std::chrono::milliseconds(4);
    scaled.source_pixels_per_pixel_x = 2.0;
    expect(estimator.observe(scaled, input(scaled)).motion.status == Status::INVALID_GEOMETRY,
           "source比例变化不得跨几何补偿");
}
void test_stride_and_repeated_phase_inputs() {
    Estimator estimator;
    cv::Mat storage(330, 340, CV_8UC3);
    const auto original = texture();
    const cv::Rect crop(3, 5, 320, 320);
    for (std::uint64_t sequence = 1; sequence <= 5; ++sequence) {
        translated(original, static_cast<double>(sequence)).copyTo(storage(crop));
        const cv::Mat image = storage(crop);
        const cv::Mat before = image.clone();
        auto captured = capture(image, sequence);
        const auto result = estimator.observe(captured, input(captured));
        expect(cv::norm(image, before, cv::NORM_INF) == 0.0,
               "非连续Capture图像不得被灰度转换或phase乘窗修改");
        if (sequence > 1) {
            expect(result.motion.status == Status::VALID &&
                   std::fabs(result.motion.dx_roi_pixels - 1.0f) < 0.2f,
                   "连续多次phase不能把上次窗函数累积到下一帧");
        }
    }
}
} // namespace

int main() {
    test_translation_zero_and_owned_history();
    test_quality_and_foreground();
    test_pair_geometry_reset_and_unsupported();
    test_stride_and_repeated_phase_inputs();
    if (failures == 0) std::cout << "runtime camera motion tests passed\n";
    return failures == 0 ? 0 : 1;
}
