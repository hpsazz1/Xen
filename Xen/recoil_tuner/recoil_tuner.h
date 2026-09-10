#ifndef RECOIL_TUNER_H
#define RECOIL_TUNER_H

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <opencv2/core.hpp>
struct RecoilProfile;

namespace recoil_tuner {

struct CurvePoint { double time_ms = 0, x_counts = 0, y_counts = 0; };
enum class TrialUse { FIT, HOLDOUT };
enum class ReceiptState { ACKNOWLEDGED, NOT_SENT, UNKNOWN };
struct Receipt {
    std::uint64_t command_id = 0;
    double completed_ms = 0;
    double planned_ms = 0;
    double x_counts = 0, y_counts = 0;
    ReceiptState state = ReceiptState::UNKNOWN;
};
struct Trial {
    std::string id, content_hash, firing_id, source_run, environment_fingerprint, executed_profile_revision;
    std::string measurement_source;
    TrialUse use = TrialUse::FIT;
    // 非零表示已经用于某轮选择/调整，不能重新标为独立留出。
    std::uint64_t previously_used_generation = 0;
    bool completed = false, independent_recoil = false, timing_valid = false, reference_confirmed = false;
    double measurement_ms = 0, timing_uncertainty_ms = 0;
    std::array<double, 2> residual{}, noise{}, executed_counts{};
    std::vector<Receipt> receipts;
};
// 差异实验的输入必须就是本模块 smoothstep 时间基函数的总counts变化。
// baseline 与 changed 均绑定完整、已结束的独立Run；不能用本轮fit/holdout自证H。
struct ResponseExperiment {
    std::string id, environment_fingerprint, baseline_run, changed_run, baseline_hash, changed_hash;
    std::string basis = "terminal_smoothstep_v1";
    std::array<double, 2> delta_counts{}, delta_residual{};
    double noise = 0;
    double baseline_timing_uncertainty_ms = 0, changed_timing_uncertainty_ms = 0;
    bool acknowledged = false, timing_valid = false;
};
struct Dataset {
    int schema_version = 1;
    std::string environment_fingerprint, base_profile_revision;
    double phase_tolerance_ms = 0;
    std::vector<CurvePoint> base_curve;
    std::vector<ResponseExperiment> response_experiments;
    std::vector<Trial> trials;
};
struct Request {
    std::uint64_t generation = 1;
    std::string candidate_revision;
    double max_axis_correction_counts = 5, max_total_correction_counts = 5;
    double max_curve_absolute_counts = 10000, max_curve_increment_counts = 100;
    double max_response_condition = 20, min_excitation_to_noise = 5;
    double min_systematic_to_noise = 2;
    double max_timing_uncertainty_ms = 5, min_relative_improvement = 0.05;
    double max_axis_regression = 0;
    std::size_t min_fit_trials = 3, min_holdout_trials = 2;
};
struct Metrics { double robust_center_error = 0, p95_error = 0, worst_error = 0; std::array<double, 2> mean_absolute_axis{}; };
struct Candidate {
    int schema_version = 1;
    std::string revision, parent_revision, environment_fingerprint;
    std::uint64_t generation = 0;
    std::array<double, 2> correction_counts{};
    std::vector<CurvePoint> points;
    std::vector<std::string> fit_trial_ids, consumed_holdout_trial_ids;
    std::vector<std::string> evidence_hashes;
    // 仅软件/离线验证，不代表实测或已接受。
    bool software_validated = false;
};
enum class Status { INVALID_DATA, RESPONSE_UNIDENTIFIABLE, NO_IMPROVEMENT, VALIDATOR_REQUIRED, CANDIDATE_VALIDATED };
struct Report {
    Status status = Status::INVALID_DATA;
    std::vector<std::string> messages;
    std::vector<std::string> consumed_holdout_trial_ids;
    double response_condition = 0;
    std::array<double, 4> response_matrix{};
    bool response_available = false, predictions_available = false;
    Metrics fit_before, fit_predicted, holdout_before, holdout_predicted;
    std::optional<Candidate> candidate;
};
using CandidateValidator = std::function<bool(const std::vector<CurvePoint>&, std::string&)>;
Report optimize(const Dataset& dataset, const Request& request, const CandidateValidator& validate = {}) noexcept;
std::string environment_fingerprint(const RecoilProfile& profile);
// UI/CLI生产入口：绑定真实基线并在展示预测之前持久登记留出用途。
Report optimize_profile_recorded(const Dataset& dataset, const Request& request, const RecoilProfile& base,
    const CandidateValidator& validate = {}, const std::filesystem::path& test_usage_directory = {}) noexcept;
bool load_dataset(const std::filesystem::path& path, Dataset& dataset, std::string& error) noexcept;
// 新建独立结果目录；已存在时拒绝，绝不覆盖活动配置或原始数据。
bool save_result(const std::filesystem::path& new_directory, const Report& report, std::string& error) noexcept;

struct ImageRequest {
    cv::Point2d reference;
    bool reference_confirmed = false;
    // 固定靶面平移配准：背景区与弹孔查找区必须互不相交。
    cv::Rect registration_roi, measurement_roi;
    double max_translation_pixels = 8, min_registration_response = 0.5;
    double threshold = 25, min_area = 3, max_area = 200;
    double max_changed_fraction = 0.05;
};
struct ImageMeasurement {
    bool valid = false;
    bool requires_manual_confirmation = true;
    bool temporal_assignment_available = false;
    cv::Point2d translation;
    cv::Point2d reference;
    double registration_response = 0;
    std::vector<cv::Point2d> candidate_centers;
    std::string message;
};
// 只输出经配准的差分候选；亮度/特效/弹孔身份仍需人工确认，不能直接充当Trial。
ImageMeasurement measure_image_pair(const cv::Mat& before, const cv::Mat& after, const ImageRequest& request) noexcept;
bool save_image_measurement(const std::filesystem::path& new_path, const ImageMeasurement& measurement, std::string& error) noexcept;

} // namespace recoil_tuner
#endif
