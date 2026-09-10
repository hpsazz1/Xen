#include "recoil_tuner/recoil_tuner.h"
#include "recoil/recoil.h"
#include <nlohmann/json.hpp>
#include <opencv2/imgproc.hpp>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>

namespace {
using namespace recoil_tuner;
int failures = 0;
void expect(bool condition, const char* message) { if (!condition) { ++failures; std::cerr << message << '\n'; } }
Dataset fixture() {
    Dataset d; d.phase_tolerance_ms = 5;
    d.environment_fingerprint = "synthetic-game-input-conditions-v1"; d.base_profile_revision = "1";
    d.base_curve = {{0, 0, 0}, {50, 0, 5}, {100, 0, 10}};
    for (int i = 0; i < 5; ++i) {
        Trial t;
        const auto suffix = std::to_string(i);
        t.id = "trial-" + suffix; t.content_hash = "hash-" + suffix; t.firing_id = "firing-" + suffix; t.source_run = "run-" + suffix;
        t.environment_fingerprint = d.environment_fingerprint; t.executed_profile_revision = "1"; t.measurement_source = "synthetic-known-response";
        t.use = i < 3 ? TrialUse::FIT : TrialUse::HOLDOUT;
        t.completed = t.independent_recoil = t.timing_valid = t.reference_confirmed = true;
        t.measurement_ms = 110; t.timing_uncertainty_ms = 0.1;
        t.residual = {4.0 + (i - 2) * 0.02, -3.0 + (i - 2) * 0.01}; t.noise = {0.1, 0.1}; t.executed_counts = {0, 10};
        for (int k = 1; k <= 10; ++k) t.receipts.push_back({static_cast<std::uint64_t>(k), k * 10.0, k * 10.0, 0, 1, ReceiptState::ACKNOWLEDGED}); d.trials.push_back(t);
    }
    for (int i = 0; i < 3; ++i) {
        ResponseExperiment e;
        const auto suffix = std::to_string(i);
        e.id = "experiment-" + suffix; e.environment_fingerprint = d.environment_fingerprint;
        e.baseline_run = "cal-base-" + suffix; e.changed_run = "cal-changed-" + suffix;
        e.baseline_hash = "cal-base-hash-" + suffix; e.changed_hash = "cal-change-hash-" + suffix;
        e.acknowledged = e.timing_valid = true; e.noise = 0.1;
        e.delta_counts = i == 0 ? std::array<double, 2>{10, 0} : i == 1 ? std::array<double, 2>{0, 10} : std::array<double, 2>{10, 10};
        e.delta_residual = {2 * e.delta_counts[0], 3 * e.delta_counts[1]}; d.response_experiments.push_back(e);
    }
    return d;
}
Request request() { Request r; r.candidate_revision = "2"; return r; }
bool production_validate(const std::vector<CurvePoint>& points, std::string& error) {
    RecoilProfile p;
    p.id = "synthetic"; p.weapon_id = "synthetic-weapon"; p.state = RecoilProfileState::SCHEMA_VALID;
    for (const auto& v : points) p.points.push_back({v.time_ms, v.x_counts, v.y_counts});
    RecoilProfile out;
    return compile_recoil_profile(p, {}, out, error);
}
void test_identification_and_candidate() {
    auto d = fixture();
    auto report = optimize(d, request(), production_validate);
    expect(report.status == Status::CANDIDATE_VALIDATED && report.candidate.has_value(), "已知独立响应与重复Trial应生成软件候选");
    if (!report.candidate) { for (const auto& text : report.messages) std::cerr << text << '\n'; return; }
    expect(std::abs(report.response_matrix[0] - 2) < 1e-9 && std::abs(report.response_matrix[3] - 3) < 1e-9,
        "应恢复已知X/Y响应而非把像素当counts");
    const auto& c = *report.candidate;
    expect(std::abs(c.correction_counts[0] + 1.99) < 0.03 && std::abs(c.correction_counts[1] - 1.0033) < 0.03,
        "修正方向和尺度必须经H换算");
    expect(c.parent_revision == "1" && c.revision == "2" && c.points.front().x_counts == 0 && c.points.back().time_ms == 100,
        "候选独立版本、不改时间轴或曲线原点");
    expect(c.evidence_hashes.size() == 11 && c.consumed_holdout_trial_ids.size() == 2,
        "候选保存数据来源及本轮已消费留出身份");
    expect(report.holdout_predicted.robust_center_error < report.holdout_before.robust_center_error && d.base_curve.back().x_counts == 0,
        "预测改善不能修改输入基线");
    auto limited = request(); limited.max_total_correction_counts = 0.5;
    report = optimize(d, limited, production_validate);
    expect(report.candidate && std::hypot(report.candidate->correction_counts[0], report.candidate->correction_counts[1]) <= 0.5000001,
        "修正总量受预算约束，不无限追误差");
    report = optimize(d, request());
    expect(report.status == Status::VALIDATOR_REQUIRED && !report.candidate, "缺生产验证不发布软件已验证候选");
    report = optimize(d, request(), [](const auto&, std::string& error) { error = "执行预算拒绝"; return false; });
    expect(!report.candidate, "生产编译失败不能留可应用候选");
    auto outlier = d.trials.front();
    outlier.id = "outlier"; outlier.content_hash = "outlier-hash"; outlier.firing_id = "outlier-fire"; outlier.source_run = "outlier-run";
    outlier.residual = {100, -100}; d.trials.push_back(outlier);
    report = optimize(d, request(), production_validate);
    expect(report.candidate && std::abs(report.candidate->correction_counts[0]) < 3,
        "稳健拟合不能让单次异常Run主导整体修正");
    d = fixture(); for (auto& trial : d.trials) trial.noise = {100, 100};
    expect(!optimize(d, request(), production_validate).candidate, "缺少超出噪声的可重复偏差不产生修正");
}
void test_data_and_response_rejections() {
    for (int mutation = 0; mutation < 11; ++mutation) {
        auto d = fixture();
        switch (mutation) {
            case 0: d.trials[4].content_hash = d.trials[0].content_hash; break;
            case 1: d.trials[0].environment_fingerprint = "different"; break;
            case 2: d.trials[0].executed_profile_revision = "draft"; break;
            case 3: d.trials[0].receipts[0].state = ReceiptState::UNKNOWN; break;
            case 4: d.trials[0].executed_counts[1] = 11; break;
            case 5: d.trials[4].previously_used_generation = 1; break;
            case 6: d.trials[0].independent_recoil = false; break;
            case 7: d.trials[0].reference_confirmed = false; break;
            case 8: d.trials[0].measurement_ms = 50; break;
            case 9: d.trials[0].timing_uncertainty_ms = 100; break;
            case 10: d.trials[0].receipts.push_back(d.trials[0].receipts[0]); break;
        }
        expect(optimize(d, request(), production_validate).status == Status::INVALID_DATA, "坏身份/执行/测量/留出泄漏必须拒绝");
    }
    for (int mutation = 0; mutation < 5; ++mutation) {
        auto d = fixture();
        switch (mutation) {
            case 0: d.response_experiments.clear(); break;
            case 1: for (auto& e : d.response_experiments) { e.delta_counts = {10, 0}; e.delta_residual = {20, 0}; } break;
            case 2: for (auto& e : d.response_experiments) e.noise = 100; break;
            case 3: d.response_experiments[0].changed_hash = d.trials[0].content_hash; break;
            case 4: d.response_experiments[0].basis = "unrelated-input"; break;
        }
        const auto r = optimize(d, request(), production_validate);
        expect(r.status == Status::RESPONSE_UNIDENTIFIABLE && !r.candidate, "缺H/秩不足/噪声/标定泄漏不得自动修正");
    }
    auto d = fixture();
    d.trials[3].residual = {-4, 3}; d.trials[4].residual = {-4, 3};
    const auto report = optimize(d, request(), production_validate);
    expect(report.status == Status::NO_IMPROVEMENT && !report.candidate && report.consumed_holdout_trial_ids.size() == 2,
        "留出反向反例不能被训练改善掩盖；拒绝后也记录留出已消费");
}
void test_image_measurement() {
    cv::Mat before(256, 256, CV_8UC1);
    cv::RNG rng(12345); rng.fill(before, cv::RNG::UNIFORM, 90, 170);
    auto after = before.clone(); cv::circle(after, {150, 150}, 3, cv::Scalar(0), cv::FILLED);
    ImageRequest r; r.reference = {145, 145}; r.reference_confirmed = true;
    r.registration_roi = {16, 16, 64, 64}; r.measurement_roi = {100, 100, 100, 100};
    auto result = measure_image_pair(before, after, r);
    expect(result.valid && result.candidate_centers.size() == 1 && result.requires_manual_confirmation && !result.temporal_assignment_available,
        "暗斑测量只产生待人工确认候选，不生成逐发真值");
    if (result.valid) expect(cv::norm(result.candidate_centers.front() - cv::Point2d(150, 150)) < 1,
        "配准后中心应落在合成已知暗斑位置");
    r.reference_confirmed = false;
    expect(!measure_image_pair(before, after, r).valid, "缺人工锚点不输出可用测量");
    r.reference_confirmed = true;
    expect(!measure_image_pair(before, before, r).valid, "重复帧不能凭空生成新弹孔");
    after = before.clone(); after(r.measurement_roi).setTo(0);
    expect(!measure_image_pair(before, after, r).valid, "遮挡大变化不得误认弹孔");
    const cv::Mat flat(256, 256, CV_8UC1, cv::Scalar(128));
    expect(!measure_image_pair(flat, flat, r).valid, "无纹理不能宣称配准可信");
    r.measurement_roi = r.registration_roi;
    expect(!measure_image_pair(before, before, r).valid, "背景与测量ROI不能重合污染配准");
}
void test_result_files() {
    const auto root = std::filesystem::temp_directory_path() / ("xen-tuner-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(root);
    std::string error;
    const auto report = optimize(fixture(), request(), production_validate);
    expect(save_result(root / "result", report, error) && std::filesystem::exists(root / "result/COMPLETE.json"), "结果完整发布必须带完成标记");
    expect(!save_result(root / "result", report, error), "重复路径不得覆盖已有候选或基线");
    std::ifstream file(root / "result/report.json"); nlohmann::json json; file >> json;
    expect(json.at("actual_candidate_measurement").is_null() && json.at("physically_accepted") == false && json.at("active_profile_changed") == false,
        "离线报告缺实测为null且不得伪称已接受");
    const auto bad_path = root / "bad.json";
    { std::ofstream f(bad_path); f << "{\"schema_version\":4294967297}"; }
    Dataset unchanged = fixture();
    expect(!load_dataset(bad_path, unchanged, error) && unchanged.base_profile_revision == "1", "坏schema不得截断成有效版本或破坏调用方数据");
    std::error_code ignored; std::filesystem::remove_all(root, ignored);
}
}
void test_recording_and_numeric_limits() {
    auto d = fixture();
    d.trials[3].residual = {std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
    expect(optimize(d, request(), production_validate).status == Status::INVALID_DATA, "有限极值残差不得溢出并放行");
    d = fixture(); d.response_experiments[0].baseline_timing_uncertainty_ms = 100;
    expect(!optimize(d, request(), production_validate).candidate, "响应实验时间不确定度也须受预算约束");
    d = fixture(); d.response_experiments[1].baseline_run = d.response_experiments[0].changed_run;
    expect(!optimize(d, request(), production_validate).candidate, "响应实验不可复用Run");
    d = fixture(); d.trials[0].receipts = {{1, 100, 100, 0, 10, ReceiptState::ACKNOWLEDGED}};
    expect(optimize(d, request(), production_validate).status == Status::INVALID_DATA, "终点相同的延后突发必须拒绝");
    d = fixture(); d.trials[0].receipts = {{1, 0, 0, 0, 10, ReceiptState::ACKNOWLEDGED}};
    expect(optimize(d, request(), production_validate).status == Status::INVALID_DATA, "终点相同的提前突发必须拒绝");
    d = fixture();
    RecoilProfile base; base.id = "synthetic"; base.weapon_id = "synthetic-weapon";
    base.state = RecoilProfileState::SCHEMA_VALID; base.phase_tolerance_ms = 5;
    for (const auto& p : d.base_curve) base.points.push_back({p.time_ms, p.x_counts, p.y_counts});
    d.environment_fingerprint = environment_fingerprint(base);
    for (auto& t : d.trials) t.environment_fingerprint = d.environment_fingerprint;
    for (auto& e : d.response_experiments) e.environment_fingerprint = d.environment_fingerprint;
    const auto dir = std::filesystem::temp_directory_path() / ("xen-tuner-ledger-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto wrong = base; wrong.weapon_id = "other-weapon";
    expect(!optimize_profile_recorded(d, request(), wrong, production_validate, dir).predictions_available, "相同曲线不同武器不可误绑定");
    const auto first = optimize_profile_recorded(d, request(), base, production_validate, dir);
    expect(first.candidate.has_value(), "首次绑定分析可生成候选并登记用途");
    const auto again = optimize_profile_recorded(d, request(), base, production_validate, dir);
    expect(!again.predictions_available && !again.candidate, "重载原Dataset不能复用留出");
    auto relabeled = d;
    relabeled.trials[0].use = TrialUse::HOLDOUT; relabeled.trials[3].use = TrialUse::FIT;
    relabeled.trials[4].id += "fresh"; relabeled.trials[4].content_hash += "fresh"; relabeled.trials[4].source_run += "fresh"; relabeled.trials[4].firing_id += "fresh";
    expect(!optimize_profile_recorded(relabeled, request(), base, production_validate, dir).predictions_available, "已使用fit不得重贴fresh holdout");
    auto next = d;
    for (auto& t : next.trials) { t.id += "next"; t.content_hash += "next"; t.source_run += "next"; t.firing_id += "next"; }
    for (int source = 0; source < 2; ++source) {
        auto old_response = next;
        for (auto& e : old_response.response_experiments) {
            e.id += "next"; e.baseline_run += "next"; e.changed_run += "next";
            e.baseline_hash += "next"; e.changed_hash += "next";
        }
        if (source == 0) old_response.trials[3].source_run = d.response_experiments[0].baseline_run;
        else old_response.trials[3].content_hash = d.response_experiments[0].changed_hash;
        expect(optimize(old_response, request(), production_validate).candidate.has_value(), "旧响应数据重贴只跨轮冲突，单轮结构仍可拟合");
        const auto reuse = optimize_profile_recorded(old_response, request(), base, production_validate, dir);
        expect(!reuse.predictions_available && !reuse.candidate, "响应实验整Run或hash不得跨轮重贴为fresh holdout");
    }
    const auto rejected = optimize_profile_recorded(next, request(), base, [](const auto&, auto&) { return false; }, dir);
    expect(rejected.predictions_available && !rejected.candidate, "生产验证拒绝仍已查看留出");
    expect(!optimize_profile_recorded(next, request(), base, production_validate, dir).predictions_available, "失败但已查看的留出亦不可复用");
}

int main() {
    test_recording_and_numeric_limits();
    test_identification_and_candidate(); test_data_and_response_rejections(); test_image_measurement(); test_result_files();
    if (failures) return 1;
    std::cout << "弹道优化器数据、可辨识性、候选隔离与图像负例专项通过。\n";
    return 0;
}
