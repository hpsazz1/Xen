#include "recoil_tuner/recoil_tuner.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace recoil_tuner {
namespace {
bool finite(double value) { return std::isfinite(value); }
bool finite_pair(const std::array<double, 2>& value) { return finite(value[0]) && finite(value[1]); }
bool bounded_pair(const std::array<double, 2>& value) { return finite_pair(value) && std::abs(value[0]) <= 1e6 && std::abs(value[1]) <= 1e6; }
double norm(const std::array<double, 2>& value) { return std::hypot(value[0], value[1]); }
double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const auto n = values.size();
    return n % 2 ? values[n / 2] : values[n / 2 - 1] + (values[n / 2] - values[n / 2 - 1]) * 0.5;
}
double robust_center(const std::vector<const Trial*>& trials, int axis) {
    std::vector<double> values;
    for (const auto* trial : trials) values.push_back(trial->residual[axis]);
    double center = median(values);
    std::vector<double> deviations;
    for (double value : values) deviations.push_back(std::abs(value - center));
    const double scale = std::max(1e-9, 1.4826 * median(deviations));
    // Huber只降权，不删除异常Run；噪声尺度来自每个Trial的测量报告。
    for (int iteration = 0; iteration < 12; ++iteration) {
        double weighted = 0, weights = 0;
        for (const auto* trial : trials) {
            const double width = std::max(scale, trial->noise[axis]);
            const double residual = std::abs(trial->residual[axis] - center);
            const double weight = std::min(1.0, 1.345 * width / std::max(residual, 1e-12));
            weighted += weight * trial->residual[axis]; weights += weight;
        }
        const double next = weighted / weights;
        if (std::abs(next - center) < 1e-10) break;
        center = next;
    }
    return center;
}
Metrics metrics(const std::vector<const Trial*>& trials, std::array<double, 2> shift) {
    Metrics result;
    result.robust_center_error = std::hypot(robust_center(trials, 0) + shift[0], robust_center(trials, 1) + shift[1]);
    std::vector<double> norms;
    for (const auto* trial : trials) {
        const double x = trial->residual[0] + shift[0], y = trial->residual[1] + shift[1];
        norms.push_back(std::hypot(x, y));
        result.mean_absolute_axis[0] += std::abs(x) / trials.size();
        result.mean_absolute_axis[1] += std::abs(y) / trials.size();
    }
    std::sort(norms.begin(), norms.end());
    result.p95_error = norms[static_cast<std::size_t>(std::ceil(norms.size() * 0.95)) - 1];
    result.worst_error = norms.back();
    return result;
}
bool curve_valid(const std::vector<CurvePoint>& curve, const Request& request) {
    if (curve.size() < 2 || curve.size() > 100000 || curve[0].time_ms != 0 || curve[0].x_counts != 0 || curve[0].y_counts != 0) return false;
    for (std::size_t i = 0; i < curve.size(); ++i) {
        const auto& p = curve[i];
        if (!finite(p.time_ms) || !finite(p.x_counts) || !finite(p.y_counts) || p.time_ms < 0 || p.time_ms > 60000 ||
            std::abs(p.x_counts) > request.max_curve_absolute_counts || std::abs(p.y_counts) > request.max_curve_absolute_counts) return false;
        if (i && (p.time_ms <= curve[i - 1].time_ms || std::hypot(p.x_counts - curve[i - 1].x_counts,
            p.y_counts - curve[i - 1].y_counts) > request.max_curve_increment_counts)) return false;
    }
    return true;
}
std::array<double, 2> sample(const std::vector<CurvePoint>& curve, double t) {
    if (t <= 0) return {};
    if (t >= curve.back().time_ms) return {curve.back().x_counts, curve.back().y_counts};
    const auto right = std::upper_bound(curve.begin(), curve.end(), t, [](double value, const CurvePoint& p) { return value < p.time_ms; });
    const auto& b = *right; const auto& a = *(right - 1);
    const double weight = (t - a.time_ms) / (b.time_ms - a.time_ms);
    return {a.x_counts + (b.x_counts - a.x_counts) * weight, a.y_counts + (b.y_counts - a.y_counts) * weight};
}
bool phase_matches(const Dataset& data, const std::array<double, 2>& counts, double time) {
    auto low = sample(data.base_curve, time - data.phase_tolerance_ms);
    auto high = sample(data.base_curve, time + data.phase_tolerance_ms);
    for (int axis = 0; axis < 2; ++axis) if (low[axis] > high[axis]) std::swap(low[axis], high[axis]);
    const auto first = std::lower_bound(data.base_curve.begin(), data.base_curve.end(), time - data.phase_tolerance_ms,
        [](const CurvePoint& p, double value) { return p.time_ms < value; });
    for (auto it = first; it != data.base_curve.end() && it->time_ms <= time + data.phase_tolerance_ms; ++it) {
        low[0] = std::min(low[0], it->x_counts); high[0] = std::max(high[0], it->x_counts);
        low[1] = std::min(low[1], it->y_counts); high[1] = std::max(high[1], it->y_counts);
    }
    for (int axis = 0; axis < 2; ++axis) if (counts[axis] < low[axis] - 1.0 || counts[axis] > high[axis] + 1.0) return false;
    return true;
}
bool request_valid(const Request& r) {
    return r.generation != 0 && !r.candidate_revision.empty() && r.candidate_revision.size() <= 256 &&
        finite(r.max_axis_correction_counts) && r.max_axis_correction_counts > 0 && r.max_axis_correction_counts <= 10000 &&
        finite(r.max_total_correction_counts) && r.max_total_correction_counts > 0 && r.max_total_correction_counts <= 10000 &&
        finite(r.max_curve_absolute_counts) && r.max_curve_absolute_counts > 0 && r.max_curve_absolute_counts <= 1e7 &&
        finite(r.max_curve_increment_counts) && r.max_curve_increment_counts > 0 && r.max_curve_increment_counts <= 32767 &&
        finite(r.max_response_condition) && r.max_response_condition >= 1 && r.max_response_condition <= 1000 &&
        finite(r.min_excitation_to_noise) && r.min_excitation_to_noise >= 1 && r.min_excitation_to_noise <= 10000 &&
        finite(r.min_systematic_to_noise) && r.min_systematic_to_noise >= 1 && r.min_systematic_to_noise <= 100 &&
        finite(r.max_timing_uncertainty_ms) && r.max_timing_uncertainty_ms >= 0 && r.max_timing_uncertainty_ms <= 100 &&
        finite(r.min_relative_improvement) && r.min_relative_improvement > 0 && r.min_relative_improvement < 1 &&
        finite(r.max_axis_regression) && r.max_axis_regression >= 0 && r.min_fit_trials >= 3 && r.min_holdout_trials >= 2;
}
}

Report optimize(const Dataset& data, const Request& request, const CandidateValidator& validate) noexcept {
    Report report;
    try {
        const auto invalid = [&](const std::string& message) { report.messages.push_back(message); return report; };
        if (!request_valid(request) || data.schema_version != 1 || data.environment_fingerprint.empty() ||
            data.base_profile_revision.empty() || request.candidate_revision == data.base_profile_revision ||
            !finite(data.phase_tolerance_ms) || data.phase_tolerance_ms <= 0 || data.phase_tolerance_ms > 1000 ||
            !curve_valid(data.base_curve, request) || data.trials.size() > 10000 || data.response_experiments.size() > 10000)
            return invalid("数据版本、基线曲线或优化预算无效。");
        std::set<std::string> ids, hashes, firing_ids, trial_runs;
        std::vector<const Trial*> fit, holdout;
        for (const auto& trial : data.trials) {
            if (trial.id.empty() || trial.content_hash.empty() || trial.firing_id.empty() || trial.source_run.empty() ||
                !ids.insert(trial.id).second || !hashes.insert(trial.content_hash).second || !firing_ids.insert(trial.firing_id).second ||
                !trial_runs.insert(trial.source_run).second) return invalid("Trial身份、内容或射击会话重复，不能把同一次射击分作训练与留出。");
            if (trial.environment_fingerprint != data.environment_fingerprint || trial.executed_profile_revision != data.base_profile_revision ||
                trial.measurement_source.empty() || !trial.completed || !trial.independent_recoil || !trial.timing_valid || !trial.reference_confirmed ||
                !bounded_pair(trial.residual) || !bounded_pair(trial.noise) || !bounded_pair(trial.executed_counts) || trial.noise[0] <= 0 || trial.noise[1] <= 0 ||
                !finite(trial.measurement_ms) || trial.measurement_ms < data.base_curve.back().time_ms ||
                !finite(trial.timing_uncertainty_ms) || trial.timing_uncertainty_ms < 0 || trial.timing_uncertainty_ms > request.max_timing_uncertainty_ms)
                return invalid("Trial缺少匹配的执行环境、独立压枪、已确认测量参考或完整源时序。");
            if (trial.receipts.empty() || trial.receipts.size() > 100000) return invalid("Trial缺少有界的设备执行回执。");
            std::set<std::uint64_t> command_ids;
            double last_time = -1;
            std::array<double, 2> counts{};
            std::size_t vertex = 0;
            for (const auto& receipt : trial.receipts) {
                if (receipt.command_id == 0 || !command_ids.insert(receipt.command_id).second || receipt.state != ReceiptState::ACKNOWLEDGED ||
                    !finite(receipt.planned_ms) || receipt.planned_ms < 0 || receipt.planned_ms > receipt.completed_ms || receipt.completed_ms - receipt.planned_ms > data.phase_tolerance_ms ||
                    !finite(receipt.completed_ms) || receipt.completed_ms < 0 || receipt.completed_ms < last_time || receipt.completed_ms > trial.measurement_ms ||
                    !finite(receipt.x_counts) || !finite(receipt.y_counts) || std::abs(receipt.x_counts) > 32767 || std::abs(receipt.y_counts) > 32767 ||
                    std::trunc(receipt.x_counts) != receipt.x_counts || std::trunc(receipt.y_counts) != receipt.y_counts)
                    return invalid("回执缺失、重复、未知或时间不一致；禁止把未确认位移用于拟合。");
                while (vertex < data.base_curve.size() && data.base_curve[vertex].time_ms < receipt.completed_ms) {
                    if (!phase_matches(data, counts, data.base_curve[vertex++].time_ms)) return invalid("回执间存在未按基线执行的曲线阶段，不能只凭终点总量拟合。");
                }
                if (!phase_matches(data, counts, receipt.completed_ms)) return invalid("回执之前的累计位移超出已校准相位包络，可能迟到突发。");
                counts[0] += receipt.x_counts; counts[1] += receipt.y_counts; last_time = receipt.completed_ms;
                const auto planned = sample(data.base_curve, receipt.planned_ms);
                if (std::abs(counts[0] - planned[0]) >= 1 || std::abs(counts[1] - planned[1]) >= 1) return invalid("计划时点累计位移不匹配基线曲线。");
                if (!phase_matches(data, counts, receipt.completed_ms)) return invalid("回执之后的累计位移超出已校准相位包络，可能提前输出。");
            }
            while (vertex < data.base_curve.size()) {
                if (!phase_matches(data, counts, data.base_curve[vertex++].time_ms)) return invalid("末次回执后存在遗漏曲线阶段。");
            }
            if (counts != trial.executed_counts) return invalid("Trial汇总counts与实际回执不一致。");
            if (std::abs(counts[0] - data.base_curve.back().x_counts) >= 1.0 ||
                std::abs(counts[1] - data.base_curve.back().y_counts) >= 1.0)
                return invalid("完整Trial的已确认总量与基线曲线不符，不能用截断或限幅数据拟合完整曲线。");
            if (trial.use == TrialUse::HOLDOUT) {
                if (trial.previously_used_generation != 0) return invalid("留出Trial已经用于优化，须转开发数据并使用新的前瞻留出。");
                holdout.push_back(&trial);
            } else if (trial.use == TrialUse::FIT) fit.push_back(&trial);
            else return invalid("Trial用途无效。");
        }
        if (fit.size() < request.min_fit_trials || holdout.size() < request.min_holdout_trials) return invalid("独立训练或留出Trial数量不足。");
        std::vector<double> measurement_noise;
        for (const auto* trial : fit) measurement_noise.push_back(norm(trial->noise));
        if (std::hypot(robust_center(fit, 0), robust_center(fit, 1)) < request.min_systematic_to_noise * median(measurement_noise)) {
            report.status = Status::NO_IMPROVEMENT;
            return invalid("系统性偏差低于测量噪声预算，不为随机散布生成修正。");
        }
        report.status = Status::RESPONSE_UNIDENTIFIABLE;
        if (data.response_experiments.size() < 3) return invalid("缺少至少三组独立响应差异实验；不能把像素残差直接取反当counts。");
        cv::Mat u(static_cast<int>(data.response_experiments.size()), 2, CV_64F), e(u.rows, 2, CV_64F);
        std::set<std::string> experiment_ids, experiment_runs, experiment_hashes;
        double max_noise = 0;
        for (int i = 0; i < u.rows; ++i) {
            const auto& experiment = data.response_experiments[i];
            if (experiment.id.empty() || !experiment_ids.insert(experiment.id).second ||
                experiment.environment_fingerprint != data.environment_fingerprint || experiment.basis != "terminal_smoothstep_v1" ||
                experiment.baseline_run.empty() || experiment.changed_run.empty() || experiment.baseline_run == experiment.changed_run ||
                experiment.baseline_hash.empty() || experiment.changed_hash.empty() || experiment.baseline_hash == experiment.changed_hash ||
                !experiment_runs.insert(experiment.baseline_run).second || !experiment_runs.insert(experiment.changed_run).second ||
                !experiment_hashes.insert(experiment.baseline_hash).second || !experiment_hashes.insert(experiment.changed_hash).second ||
                trial_runs.count(experiment.baseline_run) || trial_runs.count(experiment.changed_run) ||
                hashes.count(experiment.baseline_hash) || hashes.count(experiment.changed_hash) ||
                !finite(experiment.baseline_timing_uncertainty_ms) || experiment.baseline_timing_uncertainty_ms < 0 || experiment.baseline_timing_uncertainty_ms > request.max_timing_uncertainty_ms ||
                !finite(experiment.changed_timing_uncertainty_ms) || experiment.changed_timing_uncertainty_ms < 0 || experiment.changed_timing_uncertainty_ms > request.max_timing_uncertainty_ms ||
                !experiment.acknowledged || !experiment.timing_valid || !bounded_pair(experiment.delta_counts) || !bounded_pair(experiment.delta_residual) ||
                !finite(experiment.noise) || experiment.noise <= 0 || norm(experiment.delta_counts) <= 0)
                return invalid("响应实验身份/基函数/环境/时序不完整，或与本轮训练留出数据泄漏。");
            max_noise = std::max(max_noise, experiment.noise);
            for (int axis = 0; axis < 2; ++axis) { u.at<double>(i, axis) = experiment.delta_counts[axis]; e.at<double>(i, axis) = experiment.delta_residual[axis]; }
        }
        cv::SVD input_svd(u, cv::SVD::NO_UV);
        const double smallest_input = input_svd.w.at<double>(1);
        if (smallest_input <= 1e-9 || input_svd.w.at<double>(0) / smallest_input > request.max_response_condition)
            return invalid("响应输入秩不足或条件数过大，不能分别识别X/Y修正。");
        cv::Mat h_transpose;
        if (!cv::solve(u, e, h_transpose, cv::DECOMP_SVD)) return invalid("响应矩阵求解失败。");
        cv::SVD response_svd(h_transpose, cv::SVD::NO_UV);
        const double smallest_response = response_svd.w.at<double>(1);
        report.response_condition = smallest_response > 1e-12 ? response_svd.w.at<double>(0) / smallest_response : 0;
        if (smallest_response <= 1e-12 || !finite(report.response_condition) || report.response_condition > request.max_response_condition ||
            smallest_response * smallest_input / max_noise < request.min_excitation_to_noise)
            return invalid("响应方向不可辨识或变化低于噪声预算，不能生成自动修正。");
        cv::Mat calibration_error = u * h_transpose - e;
        if (cv::norm(calibration_error, cv::NORM_INF) > 3.0 * max_noise) return invalid("响应实验与局部线性模型不相容，需先复核标定。");
        cv::Mat h = h_transpose.t();
        report.response_matrix = {h.at<double>(0, 0), h.at<double>(0, 1), h.at<double>(1, 0), h.at<double>(1, 1)};
        report.response_available = true;
        cv::Mat target = (cv::Mat_<double>(2, 1) << -robust_center(fit, 0), -robust_center(fit, 1));
        cv::Mat correction;
        if (!cv::solve(h, target, correction, cv::DECOMP_SVD)) return invalid("修正求解失败。");
        std::array<double, 2> delta{correction.at<double>(0), correction.at<double>(1)};
        if (!finite_pair(delta)) return invalid("修正结果非有限数值。");
        double scale = 1.0;
        for (double value : delta) if (std::abs(value) > request.max_axis_correction_counts) scale = std::min(scale, request.max_axis_correction_counts / std::abs(value));
        if (norm(delta) > request.max_total_correction_counts) scale = std::min(scale, request.max_total_correction_counts / norm(delta));
        for (double& value : delta) value *= scale;
        const std::array<double, 2> shift{report.response_matrix[0] * delta[0] + report.response_matrix[1] * delta[1],
            report.response_matrix[2] * delta[0] + report.response_matrix[3] * delta[1]};
        if (!finite_pair(shift)) return invalid("预测修正非有限数值。");
        report.fit_before = metrics(fit, {}); report.fit_predicted = metrics(fit, shift);
        report.holdout_before = metrics(holdout, {}); report.holdout_predicted = metrics(holdout, shift);
        const auto finite_metrics = [](const Metrics& m) { return finite(m.robust_center_error) && finite(m.p95_error) && finite(m.worst_error) && finite_pair(m.mean_absolute_axis); };
        if (!finite_metrics(report.fit_before) || !finite_metrics(report.fit_predicted) || !finite_metrics(report.holdout_before) || !finite_metrics(report.holdout_predicted)) return invalid("预测指标非有限数值。");
        report.predictions_available = true;
        for (const auto* trial : holdout) report.consumed_holdout_trial_ids.push_back(trial->id);
        report.status = Status::NO_IMPROVEMENT;
        if (report.holdout_before.robust_center_error <= 1e-9 ||
            report.holdout_predicted.robust_center_error > report.holdout_before.robust_center_error * (1.0 - request.min_relative_improvement) ||
            report.holdout_predicted.p95_error > report.holdout_before.p95_error || report.holdout_predicted.worst_error > report.holdout_before.worst_error)
            return invalid("留出预测未改善中心偏差或使尾部误差退化；保留原活动版本。");
        for (int axis = 0; axis < 2; ++axis) if (report.holdout_predicted.mean_absolute_axis[axis] > report.holdout_before.mean_absolute_axis[axis] + request.max_axis_regression)
            return invalid("候选导致另一轴退化；不生成可用版本。");
        Candidate candidate;
        candidate.revision = request.candidate_revision; candidate.parent_revision = data.base_profile_revision;
        candidate.environment_fingerprint = data.environment_fingerprint; candidate.generation = request.generation;
        candidate.correction_counts = delta; candidate.points = data.base_curve;
        for (auto& point : candidate.points) {
            const double t = point.time_ms / data.base_curve.back().time_ms;
            const double weight = t * t * (3.0 - 2.0 * t);
            point.x_counts += delta[0] * weight; point.y_counts += delta[1] * weight;
        }
        if (!curve_valid(candidate.points, request)) return invalid("修正曲线超出有限时长或动作预算。");
        report.status = Status::VALIDATOR_REQUIRED;
        if (!validate) return invalid("候选需通过生产曲线编译/执行契约验证；当前只完成拟合预测。");
        std::string error;
        if (!validate(candidate.points, error)) return invalid("生产曲线验证拒绝：" + error);
        for (const auto* trial : fit) candidate.fit_trial_ids.push_back(trial->id);
        for (const auto& trial : data.trials) candidate.evidence_hashes.push_back(trial.content_hash);
        for (const auto& experiment : data.response_experiments) {
            candidate.evidence_hashes.push_back(experiment.baseline_hash);
            candidate.evidence_hashes.push_back(experiment.changed_hash);
        }
        candidate.consumed_holdout_trial_ids = report.consumed_holdout_trial_ids;
        candidate.software_validated = true;
        report.candidate = std::move(candidate); report.status = Status::CANDIDATE_VALIDATED;
        report.messages.push_back("已生成独立软件/离线候选；未实测、未接受、未覆盖活动曲线。下一轮使用新的前瞻留出。");
        return report;
    } catch (const std::exception&) {
        report.status = Status::INVALID_DATA;
        report.candidate.reset();
        report.messages.push_back("优化失败：数据、内存或数值计算异常。");
        return report;
    } catch (...) {
        report.status = Status::INVALID_DATA; report.candidate.reset(); return report;
    }
}
} // namespace recoil_tuner
