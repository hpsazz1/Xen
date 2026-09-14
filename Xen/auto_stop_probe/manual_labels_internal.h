#ifndef AUTO_STOP_MANUAL_LABELS_INTERNAL_H
#define AUTO_STOP_MANUAL_LABELS_INTERNAL_H
#include "probe_internal.h"
#include <cmath>
#include <set>
#include <optional>

namespace auto_stop_probe_detail {
// 人工标签只指定接收记录中的样本范围；不从现有绿色评分推断标签。
inline Json apply_manual_labels(Json analysis, const Json& labels) {
    if (!labels.value("recording_usable", true))
        throw std::runtime_error("用户已排除此录制，不可用于参数校准或复测提案");
    if (!labels.is_object() || labels.value("schema_version", 0) != 1 ||
        labels.at("recording_id") != analysis.at("recording_id"))
        throw std::runtime_error("人工标签未绑定本次录制");
    std::set<std::uint64_t> available, qualified, rejected;
    for (const auto& shot : analysis.at("shots")) available.insert(shot.at("ordinal").get<std::uint64_t>());
    const auto ranges = [&](const char* key, std::set<std::uint64_t>& result) {
        const auto& rows = labels.at(key);
        if (!rows.is_array() || rows.size() > 300) throw std::runtime_error("人工范围过多");
        for (const auto& row : rows) {
            if (!row.is_array() || row.size() != 2 || !row[0].is_number_integer() || !row[1].is_number_integer() || row[0] < 1 || row[1] < row[0])
                throw std::runtime_error("人工范围无效");
            const auto first = row[0].get<std::uint64_t>(), last = row[1].get<std::uint64_t>();
            if (last - first >= 300) throw std::runtime_error("人工范围超过保留窗口");
            for (auto id = first; id <= last; ++id) {
                if (!available.contains(id) || !result.insert(id).second) throw std::runtime_error("人工范围缺失或重复");
                if (id == UINT64_MAX) break;
            }
        }
    };
    ranges("qualified_shot_ranges", qualified); ranges("rejected_shot_ranges", rejected);
    for (auto id : qualified) if (rejected.contains(id)) throw std::runtime_error("人工标签冲突");
    Json result{{"qualified_count", qualified.size()}, {"rejected_count", rejected.size()},
        {"labels_source", "USER_EXPLICIT_RANGES"}, {"settings_applied", false},
        {"fit_status", "WAITING_FOR_USER_LABELS"}, {"qualified_max_estimated_speed", nullptr},
        {"rejected_min_estimated_speed", nullptr}, {"threshold_ratio_lower_bound", nullptr},
        {"threshold_ratio_upper_exclusive", nullptr}, {"invalid_labeled_samples", 0},
        {"note", "只给固定移速模型与采样时机下的阈值约束，不唯一拟合加减速或延迟，不自动改参数"}};
    std::optional<double> high_good, low_bad;
    for (auto& shot : analysis["shots"]) {
        const auto id = shot.at("ordinal").get<std::uint64_t>();
        const bool good = qualified.contains(id), bad = rejected.contains(id);
        shot["human_label"] = good ? "QUALIFIED" : bad ? "REJECTED" : "UNLABELED";
        if (!good && !bad) continue;
        const auto& samples = shot.at("samples");
        if (samples.empty() || !samples[0].value("valid", false) || !shot.value("complete_hold", false) ||
            shot.value("atomic_ambiguous", false) || !samples[0]["estimated_speed"].is_number()) {
            result["invalid_labeled_samples"] = result["invalid_labeled_samples"].get<int>() + 1; continue;
        }
        const double speed = samples[0]["estimated_speed"].get<double>();
        if (!std::isfinite(speed)) throw std::runtime_error("标注样本数值无效");
        if (good) high_good = high_good ? std::max(*high_good, speed) : speed;
        if (bad) low_bad = low_bad ? std::min(*low_bad, speed) : speed;
    }
    if (!qualified.empty() || !rejected.empty()) result["fit_status"] = "INSUFFICIENT_LABEL_CLASSES";
    const double maximum = analysis.at("settings").at("max_move_speed").get<double>();
    if (maximum <= 0 || !std::isfinite(maximum)) throw std::runtime_error("标注模型参数无效");
    if (high_good) { result["qualified_max_estimated_speed"] = *high_good; result["threshold_ratio_lower_bound"] = *high_good / maximum; }
    if (low_bad) { result["rejected_min_estimated_speed"] = *low_bad; result["threshold_ratio_upper_exclusive"] = *low_bad / maximum; }
    if (high_good && low_bad) result["fit_status"] = *high_good < *low_bad ? "FIXED_MODEL_THRESHOLD_BOUNDS" : "MODEL_OR_TIMING_CONFLICT";
    if (result["invalid_labeled_samples"] != 0 || !analysis.value("archive_complete", false)) result["fit_status"] = "EVIDENCE_INCOMPLETE";
    analysis["human_labels"] = labels;
    analysis["calibration_envelope"] = std::move(result);
    return analysis;
}
}
#endif
