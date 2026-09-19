#ifndef RECOIL_WALL_CAPTURE_ANALYSIS_H
#define RECOIL_WALL_CAPTURE_ANALYSIS_H
#include "recoil_tuner/recoil_tuner.h"
#include "recoil/recoil.h"

namespace recoil_tuner {
struct WallFrame { double time_ms = 0; cv::Mat image; };
// 标定记录是独立已完成的两轴设备响应，绝非优化器的差异实验 H。
struct WallCalibrationSample {
    std::array<double, 2> counts{}, pixel_delta{};
    bool acknowledged = false;
    std::string evidence_id;
};
// 准备和分析共用：验证独立两轴、已确认来源及正反响应的一致性，不连接设备。
bool fit_wall_calibration(const std::vector<WallCalibrationSample>& samples,
    double max_condition, std::array<double,4>& pixel_response, std::string& error) noexcept;
struct WallCaptureRequest {
    enum class Mode { BULLET_MARKS, CAMERA_MOTION, FOLLOW_RECOIL };
    Mode mode = Mode::BULLET_MARKS;
    // 独立鼠标标定已检查准星屏幕中心保持不变，才能使用 H^-1((q-q0)-b)。
    bool follow_recoil_mouse_invariance_confirmed = false;
    ImageRequest image;
    std::vector<WallCalibrationSample> calibration;
    std::string environment_fingerprint, profile_id, weapon_id, source_hash;
    double sensitivity = 0, max_condition = 20, matching_radius_pixels = 2;
};
struct WallObservation {
    cv::Point2d center;
    cv::Point2d crosshair_center{}, background_translation{};
    double earliest_ms = 0, first_visible_ms = 0;
    std::array<double, 2> cumulative_counts{};
};
// 固定洋红实心小点或无间隙对称短十字；多解、缺臂、裁边均拒绝。
bool locate_follow_recoil_crosshair(const cv::Mat& image, cv::Point2d& center, std::string& error) noexcept;
struct WallCaptureReport {
    bool valid = false, requires_manual_confirmation = true;
    // 这些是图像首次可见区间；不提供真实逐发时间资格。
    bool shot_timing_available = false;
    std::string message, environment_fingerprint;
    std::array<double, 4> pixel_response{};
    cv::Point2d reference;
    std::vector<std::string> calibration_evidence;
    std::vector<WallObservation> observations;
    std::optional<RecoilProfile> candidate;
    WallCaptureRequest::Mode mode = WallCaptureRequest::Mode::BULLET_MARKS;
};
WallCaptureReport analyze_wall_capture(const std::vector<WallFrame>& frames, const WallCaptureRequest& request) noexcept;
// 独立端点测量入口：调用者必须提供真实Run/回执及人工确认；不修改用途、H或留出身份。
bool wall_measurement_to_trial(const WallCaptureReport& report, std::size_t observation_index,
    bool point_confirmed, const Trial& metadata, Trial& output, std::string& error) noexcept;
// fit和holdout由调用者明确划分并持久登记；每个报告必须人工核对，不能重用同一原始采集。
struct WallOptimizationRequest {
    // 仅约束基线已有时域的微调；新增时域按拟合组的边界相对残差建立，仍需独立验证。
    double max_axis_correction_counts = 5;
    double min_relative_improvement = 0.05;
    bool measurements_confirmed = false;
    double locked_prefix_ms = 0;
};
struct WallTrial {
    WallCaptureReport measurement;
    std::string source_run, executed_profile_json;
    bool confirmed = false;
    TrialUse use = TrialUse::FIT;
};
WallCaptureReport optimize_wall_trials_recorded(const RecoilProfile& base, const std::vector<WallTrial>& trials,
    const WallOptimizationRequest& request, const std::filesystem::path& usage_directory) noexcept;
WallCaptureReport optimize_wall_trials(const RecoilProfile& base,
    const std::vector<WallCaptureReport>& fit, const std::vector<WallCaptureReport>& holdout,
    const WallOptimizationRequest& request) noexcept;
bool save_wall_report(const std::filesystem::path& new_path, const WallCaptureReport& report, std::string& error) noexcept;
bool load_wall_report(const std::filesystem::path& path, WallCaptureReport& report, std::string& error) noexcept;
}
#endif
