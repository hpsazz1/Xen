#include "runtime/aim_frame_internal.h"

#include <cmath>
#include <iostream>
#include <opencv2/imgproc.hpp>

int main() {
    int failures = 0;
    const auto expect = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::cerr << "[失败] " << message << '\n'; }
    };
    cv::Mat texture(320, 320, CV_8UC3);
    cv::RNG random(74521);
    random.fill(texture, cv::RNG::UNIFORM, 0, 255);
    runtime::detail::RuntimeObservationClock clock;
    runtime::detail::CameraMotionEstimator estimator;
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15;
    Aim aim(config);
    const auto start = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    for (int i = 0; i < 3; ++i) {
        CapturedFrame captured;
        captured.width = captured.height = 320;
        captured.source_width = 2560;
        captured.source_height = 1440;
        captured.roi_x = 1120;
        captured.roi_y = 560;
        captured.timing.sequence = 20 + i;
        captured.timing.captured_at = start + std::chrono::milliseconds(i * 4);
        const cv::Mat transform = (cv::Mat_<double>(2, 3) << 1, 0, i * 2, 0, 1, 0);
        cv::warpAffine(texture, captured.bgr, transform, texture.size(),
                       cv::INTER_LINEAR, cv::BORDER_REFLECT);
        auto prepared = runtime::detail::prepare_aim_frame(captured,
            {{150.0f + i * 2, 140.0f, 180.0f + i * 2, 200.0f, 0.95f, 0}},
            clock, estimator, true);
        expect(prepared.frame.control_center_x == 160 && prepared.frame.control_center_y == 160,
               "生产组装必须保留主机 FOV 到 ROI 的中心转换");
        expect(prepared.frame.control_at >= captured.timing.captured_at &&
                   prepared.background_motion_ms >= 0,
               "生产控制时间必须在图像观测之后取值");
        // 离线使用显式原控制时间；生产入口的取时先单独检查。
        prepared.frame.control_at = captured.timing.captured_at + std::chrono::milliseconds(2);
        if (prepared.reset_aim) aim.reset();
        const auto result = aim.process(prepared.frame);
        if (i == 0) {
            expect(prepared.frame.background_motion_x.status == AimBackgroundMotionStatus::WARMING,
                   "首张图像不能制造背景零位移");
        } else {
            expect(prepared.frame.background_motion_x.status == AimBackgroundMotionStatus::VALID,
                   "真实生产估计器必须提供有效背景位移");
            expect(result.control.background_motion_use_x == AimBackgroundMotionUse::CONSUMED,
                   "组装到公开 Aim 必须真正消费背景，漏接线应失败");
            expect(std::fabs(result.control.observer_camera_motion_x_source_pixels - 2.0f) < 0.2f,
                   "完整组装路径保持背景方向及像素单位");
        }
    }
    estimator.reset();
    expect(failures == 0, "Runtime/Aim 组装合同失败");
    return failures ? 1 : 0;
}
