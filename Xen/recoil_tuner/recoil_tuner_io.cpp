#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include "recoil_tuner/recoil_tuner.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include "recoil/recoil.h"
#include <chrono>
#include <iterator>
#include <stdexcept>

namespace recoil_tuner {
namespace {
using Json = nlohmann::json;
bool write_new(const std::filesystem::path& path, const std::string& text, std::string& error) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { error = "输出已存在或无法独占创建；未覆盖原文件。"; return false; }
    DWORD written = 0;
    const bool okay = text.size() < MAXDWORD && WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
        written == text.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!okay) error = "写入未完成；保留未完成文件供检查，不标记成功。";
    return okay;
}
Json metrics_json(const Metrics& metrics) {
    return {{"robust_center_error", metrics.robust_center_error}, {"p95_error", metrics.p95_error},
        {"worst_error", metrics.worst_error}, {"mean_absolute_axis", metrics.mean_absolute_axis}};
}
const char* status_name(Status status) {
    switch (status) {
        case Status::INVALID_DATA: return "INVALID_DATA";
        case Status::RESPONSE_UNIDENTIFIABLE: return "RESPONSE_UNIDENTIFIABLE";
        case Status::NO_IMPROVEMENT: return "NO_IMPROVEMENT";
        case Status::VALIDATOR_REQUIRED: return "VALIDATOR_REQUIRED";
        case Status::CANDIDATE_VALIDATED: return "CANDIDATE_VALIDATED";
    }
    return "INVALID_DATA";
}
std::uint64_t unsigned_value(const Json& j) {
    if (!j.is_number_unsigned() && !(j.is_number_integer() && j.get<std::int64_t>() >= 0)) throw std::runtime_error("整数类型无效");
    return j.get<std::uint64_t>();
}
}

std::string environment_fingerprint(const RecoilProfile& p) {
    return Json{{"schema_version", 1}, {"weapon_id", p.weapon_id}, {"profile_id", p.id},
        {"unit", p.unit}, {"sample_semantics", p.sample_semantics}, {"fire_mode", p.fire_mode},
        {"game_build", p.calibration.game_build}, {"input_path", p.calibration.input_path},
        {"conditions", p.calibration.conditions},
        {"sensitivity", p.calibration.sensitivity ? Json(*p.calibration.sensitivity) : Json(nullptr)},
        {"source_sha256", p.source.sha256},
        {"phase_tolerance_ms", p.phase_tolerance_ms ? Json(*p.phase_tolerance_ms) : Json(nullptr)},
        {"recovery_ms", p.recovery_ms ? Json(*p.recovery_ms) : Json(nullptr)}}.dump();
}

Report optimize_profile_recorded(const Dataset& data, const Request& request, const RecoilProfile& base,
    const CandidateValidator& validate, const std::filesystem::path& test_directory) noexcept {
    const auto fail = [](const std::string& message) { Report r; r.messages.push_back(message); return r; };
    struct Lock { HANDLE file = INVALID_HANDLE_VALUE; ~Lock() { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); } } lock;
    try {
        std::string validation;
        if (!validate_recoil_profile(base, validation) || !base.phase_tolerance_ms ||
            data.phase_tolerance_ms != *base.phase_tolerance_ms || data.environment_fingerprint != environment_fingerprint(base) ||
            data.base_profile_revision != std::to_string(base.revision) || data.base_curve.size() != base.points.size())
            return fail("数据集环境、相位证据或版本与生产基线不一致。");
        for (size_t i = 0; i < base.points.size(); ++i)
            if (data.base_curve[i].time_ms != base.points[i].time_ms || data.base_curve[i].x_counts != base.points[i].x_counts || data.base_curve[i].y_counts != base.points[i].y_counts)
                return fail("数据集曲线与生产基线不一致。");
        auto directory = test_directory;
        if (directory.empty()) {
            wchar_t local[32768]; const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
            if (!n || n >= 32768) return fail("无法定位当前用户用途登记目录。");
            directory = std::filesystem::path(local) / "Xen" / "recoil-tuner-usage-v1";
        }
        std::filesystem::create_directories(directory);
        lock.file = CreateFileW((directory / "usage.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (lock.file == INVALID_HANDLE_VALUE) return fail("用途登记被另一分析占用或不可写；本轮未查看留出。");
        const auto path = directory / "usage.json";
        Json ledger = {{"schema_version", 1}, {"records", Json::array()}};
        if (std::filesystem::exists(path)) {
            if (std::filesystem::file_size(path) > 32 * 1024 * 1024) return fail("用途登记文件过大，拒绝继续。");
            std::ifstream input(path); input >> ledger;
            if (ledger.at("schema_version") != 1 || !ledger.at("records").is_array()) return fail("用途登记文件损坏。");
        }
        for (const auto& t : data.trials) if (t.use == TrialUse::HOLDOUT)
            for (const auto& used : ledger.at("records"))
                if (used.at("content_hash") == t.content_hash ||
                    (used.at("source_run") == t.source_run && (used.value("use", "") == "response" || used.at("firing_id") == t.firing_id)) ||
                    (used.at("environment_fingerprint") == data.environment_fingerprint && used.at("trial_id") == t.id))
                    return fail("留出数据已用于先前分析；必须采集新的独立留出。");
        auto report = optimize(data, request, validate);
        if (!report.predictions_available) return report;
        for (const auto& t : data.trials)
            ledger["records"].push_back({{"trial_id", t.id}, {"content_hash", t.content_hash}, {"source_run", t.source_run},
                {"firing_id", t.firing_id}, {"environment_fingerprint", data.environment_fingerprint}, {"generation", request.generation}, {"use", t.use == TrialUse::FIT ? "fit" : "holdout"}});
        for (const auto& experiment : data.response_experiments) {
            const auto record_response = [&](const std::string& run, const std::string& hash) {
                ledger["records"].push_back({{"trial_id", ""}, {"content_hash", hash}, {"source_run", run},
                    {"firing_id", ""}, {"environment_fingerprint", data.environment_fingerprint},
                    {"generation", request.generation}, {"use", "response"}});
            };
            record_response(experiment.baseline_run, experiment.baseline_hash);
            record_response(experiment.changed_run, experiment.changed_hash);
        }
        const auto temp = directory / ("usage-" + std::to_string(GetCurrentProcessId()) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".tmp");
        std::string error;
        if (!write_new(temp, ledger.dump(2), error) || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return fail("用途登记未持久保存，已隐藏本轮预测与候选；不得将其作为新留出结果。");
        return report;
    } catch (...) { return fail("基线绑定或用途登记失败，未返回预测和候选。"); }
}

bool load_dataset(const std::filesystem::path& path, Dataset& dataset, std::string& error) noexcept {
    try {
        const auto size = std::filesystem::file_size(path);
        if (size == 0 || size > 16 * 1024 * 1024) { error = "数据文件为空或超过16 MiB限制。"; return false; }
        std::ifstream input(path, std::ios::binary);
        if (!input) { error = "无法读取数据文件。"; return false; }
        const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        const auto j = Json::parse(bytes, [](int depth, Json::parse_event_t, Json&) {
            if (depth > 16) throw std::runtime_error("嵌套过深");
            return true;
        }, true, false);
        Dataset next;
        if (unsigned_value(j.at("schema_version")) != 1) { error = "数据schema版本不支持。"; return false; }
        next.environment_fingerprint = j.at("environment_fingerprint").get<std::string>();
        next.phase_tolerance_ms = j.at("phase_tolerance_ms");
        next.base_profile_revision = j.at("base_profile_revision").get<std::string>();
        for (const auto& p : j.at("base_curve")) next.base_curve.push_back({p.at("time_ms"), p.at("x_counts"), p.at("y_counts")});
        for (const auto& value : j.at("response_experiments")) {
            ResponseExperiment experiment;
            experiment.id = value.at("id"); experiment.environment_fingerprint = value.at("environment_fingerprint");
            experiment.baseline_run = value.at("baseline_run"); experiment.changed_run = value.at("changed_run");
            experiment.baseline_hash = value.at("baseline_hash"); experiment.changed_hash = value.at("changed_hash");
            experiment.basis = value.at("basis"); experiment.delta_counts = value.at("delta_counts").get<std::array<double, 2>>();
            experiment.delta_residual = value.at("delta_residual").get<std::array<double, 2>>(); experiment.noise = value.at("noise");
            experiment.baseline_timing_uncertainty_ms = value.at("baseline_timing_uncertainty_ms");
            experiment.changed_timing_uncertainty_ms = value.at("changed_timing_uncertainty_ms");
            experiment.acknowledged = value.at("acknowledged"); experiment.timing_valid = value.at("timing_valid");
            next.response_experiments.push_back(std::move(experiment));
        }
        for (const auto& value : j.at("trials")) {
            Trial trial;
            trial.id = value.at("id"); trial.content_hash = value.at("content_hash"); trial.firing_id = value.at("firing_id");
            trial.source_run = value.at("source_run"); trial.environment_fingerprint = value.at("environment_fingerprint");
            trial.executed_profile_revision = value.at("executed_profile_revision"); trial.measurement_source = value.at("measurement_source");
            const auto use = value.at("use").get<std::string>();
            if (use != "fit" && use != "holdout") { error = "Trial用途必须为fit或holdout。"; return false; }
            trial.use = use == "fit" ? TrialUse::FIT : TrialUse::HOLDOUT;
            trial.previously_used_generation = unsigned_value(value.at("previously_used_generation"));
            trial.completed = value.at("completed"); trial.independent_recoil = value.at("independent_recoil");
            trial.timing_valid = value.at("timing_valid"); trial.reference_confirmed = value.at("reference_confirmed");
            trial.measurement_ms = value.at("measurement_ms"); trial.timing_uncertainty_ms = value.at("timing_uncertainty_ms");
            trial.residual = value.at("residual").get<std::array<double, 2>>(); trial.noise = value.at("noise").get<std::array<double, 2>>();
            trial.executed_counts = value.at("executed_counts").get<std::array<double, 2>>();
            for (const auto& entry : value.at("receipts")) {
                Receipt receipt;
                receipt.command_id = unsigned_value(entry.at("command_id")); receipt.completed_ms = entry.at("completed_ms");
                receipt.planned_ms = entry.at("planned_ms");
                receipt.x_counts = entry.at("x_counts"); receipt.y_counts = entry.at("y_counts");
                const auto state = entry.at("state").get<std::string>();
                if (state != "acknowledged" && state != "not_sent" && state != "unknown") { error = "设备回执状态无效。"; return false; }
                receipt.state = state == "acknowledged" ? ReceiptState::ACKNOWLEDGED : state == "not_sent" ? ReceiptState::NOT_SENT : ReceiptState::UNKNOWN;
                trial.receipts.push_back(receipt);
            }
            next.trials.push_back(std::move(trial));
        }
        dataset = std::move(next); error.clear(); return true;
    } catch (...) { error = "数据文件缺字段、类型错误、无法读取或JSON损坏。"; return false; }
}

bool save_result(const std::filesystem::path& directory, const Report& report, std::string& error) noexcept {
    try {
        if (!std::filesystem::create_directory(directory)) { error = "结果目录已存在；拒绝覆盖。"; return false; }
        Json j{{"schema_version", 1}, {"status", status_name(report.status)}, {"messages", report.messages},
            {"consumed_holdout_trial_ids", report.consumed_holdout_trial_ids}, {"actual_candidate_measurement", nullptr},
            {"physically_accepted", false}, {"active_profile_changed", false}};
        j["response_condition"] = report.response_condition > 0 ? Json(report.response_condition) : Json(nullptr);
        j["response_matrix"] = report.response_available ? Json(report.response_matrix) : Json(nullptr);
        j["fit_before"] = report.predictions_available ? metrics_json(report.fit_before) : Json(nullptr);
        j["fit_predicted"] = report.predictions_available ? metrics_json(report.fit_predicted) : Json(nullptr);
        j["holdout_before"] = report.predictions_available ? metrics_json(report.holdout_before) : Json(nullptr);
        j["holdout_predicted"] = report.predictions_available ? metrics_json(report.holdout_predicted) : Json(nullptr);
        if (!write_new(directory / "report.json", j.dump(2), error)) return false;
        if (report.candidate && report.candidate->software_validated && report.status == Status::CANDIDATE_VALIDATED) {
            const auto& c = *report.candidate;
            Json candidate{{"schema_version", c.schema_version}, {"revision", c.revision}, {"parent_revision", c.parent_revision},
                {"environment_fingerprint", c.environment_fingerprint}, {"generation", c.generation}, {"basis", "terminal_smoothstep_v1"},
                {"correction_counts", c.correction_counts}, {"fit_trial_ids", c.fit_trial_ids},
                {"consumed_holdout_trial_ids", c.consumed_holdout_trial_ids}, {"evidence_hashes", c.evidence_hashes},
                {"state", "CANDIDATE_VALIDATED"}, {"physically_accepted", false}, {"points", Json::array()}};
            for (const auto& p : c.points) candidate["points"].push_back({{"time_ms", p.time_ms}, {"x_counts", p.x_counts}, {"y_counts", p.y_counts}});
            if (!write_new(directory / "candidate.json", candidate.dump(2), error)) return false;
        }
        if (!write_new(directory / "COMPLETE.json", "{\"complete\":true,\"active_profile_changed\":false}\n", error)) return false;
        error.clear(); return true;
    } catch (...) { error = "结果写入失败；没有修改活动曲线，缺少COMPLETE标记的目录不可作为完整结果。"; return false; }
}

bool save_image_measurement(const std::filesystem::path& path, const ImageMeasurement& m, std::string& error) noexcept {
    try {
        Json j{{"schema_version", 1}, {"valid", m.valid}, {"message", m.message}, {"requires_manual_confirmation", true},
            {"temporal_assignment_available", false}, {"reference", {m.reference.x, m.reference.y}},
            {"translation", {m.translation.x, m.translation.y}}, {"registration_response", m.registration_response}, {"candidates", Json::array()}};
        for (const auto& p : m.candidate_centers) j["candidates"].push_back({{"center", {p.x, p.y}},
            {"residual_pixels", {p.x - m.reference.x, p.y - m.reference.y}}, {"identity_confirmed", false}});
        return write_new(path, j.dump(2), error);
    } catch (...) { error = "测量候选写入失败。"; return false; }
}
} // namespace recoil_tuner
