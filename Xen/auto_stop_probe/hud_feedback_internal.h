#ifndef AUTO_STOP_HUD_FEEDBACK_INTERNAL_H
#define AUTO_STOP_HUD_FEEDBACK_INTERNAL_H
#include "auto_stop_probe/sampling_analysis_internal.h"

namespace auto_stop_probe_detail {
// 展示汇总复用已有模型样本；阈比参考线不意味着按默认参数重新运行了模型。
inline Json summarize_hud_feedback(const Json& analysis, const SamplingSettings& settings) {
    Json timing{{"count",0},{"valid_count",0},{"mean_ms",nullptr},{"stddev_ms",nullptr},
        {"min_ms",nullptr},{"max_ms",nullptr},{"excellent_percent",nullptr},{"habit","暂无"},
        {"latest_delta_ms",nullptr},{"latest_grade","暂无"}};
    std::vector<double> deltas;
    unsigned excellent=0;
    const auto grade = [](double d) -> const char* {
        return std::abs(d)<=2 ? "完美" : std::abs(d)<=10 ? "优秀" : d<0 ? "偏早" : "偏晚";
    };
    const auto& timings=analysis.value("timings",Json::array());
    timing["count"]=analysis.value("total_timing_count",analysis.value("total_timings",timings.size()));
    for(std::size_t i=timings.size()>300 ? timings.size()-300 : 0;i<timings.size();++i) {
        const auto& point=timings[i];
        timing["latest_delta_ms"]=nullptr; timing["latest_grade"]="不可评价";
        if(!point.contains("delta_ms") || !point["delta_ms"].is_number() || point.value("atomic_ambiguous",false)) continue;
        const double raw=point["delta_ms"].get<double>();
        if(!std::isfinite(raw) || std::abs(raw)>120) continue;
        const double d=std::round(raw*10)/10;
        deltas.push_back(d); if(std::abs(d)<=10) ++excellent;
        timing["latest_delta_ms"]=d; timing["latest_grade"]=grade(d);
    }
    timing["valid_count"]=deltas.size();
    if(!deltas.empty()) {
        double sum=0,variance=0; for(double d:deltas) sum+=d;
        const double mean=std::round(sum/deltas.size()*10)/10;
        for(double d:deltas) variance+=(d-mean)*(d-mean);
        timing["mean_ms"]=mean; timing["stddev_ms"]=std::sqrt(variance/deltas.size());
        timing["min_ms"]=*std::min_element(deltas.begin(),deltas.end());
        timing["max_ms"]=*std::max_element(deltas.begin(),deltas.end());
        timing["excellent_percent"]=100.0*excellent/deltas.size();
        timing["habit"]=mean < -5 ? "偏早" : mean > 5 ? "偏晚" : "正常";
    }
    const auto& shots=analysis.value("shots",Json::array());
    Json shooting{{"hold_count",analysis.value("shot_count",shots.size())},{"sample_count",0},
        {"valid_count",0},{"stable_percent",nullptr},{"mean_error",nullptr},
        {"latest_ratio",nullptr},{"latest_error",nullptr},{"latest_classification","暂无"}};
    std::size_t observed=0,valid=0,stable=0; double errors=0;
    for(std::size_t i=shots.size()>300 ? shots.size()-300 : 0;i<shots.size();++i) {
        const auto& shot=shots[i];
        // 最近按住无到期样本时显示暂无，不能借上一枪显示“稳定”。
        shooting["latest_ratio"]=nullptr; shooting["latest_error"]=nullptr;
        shooting["latest_classification"]="等待采样";
        if(!shot.contains("samples") || !shot["samples"].is_array()) continue;
        for(const auto& sample:shot["samples"]) {
            ++observed;
            shooting["latest_ratio"]=nullptr; shooting["latest_error"]=nullptr;
            shooting["latest_classification"]="不可评价";
            if(!sample.value("valid",false) || shot.value("atomic_ambiguous",false) ||
                !sample.contains("speed_ratio") || !sample["speed_ratio"].is_number()) continue;
            const double q=sample["speed_ratio"].get<double>();
            if(!std::isfinite(q) || q<0) continue;
            const double error=std::clamp((q-1)/.5,0.0,1.0);
            ++valid; if(q<=1) ++stable; errors+=error;
            shooting["latest_ratio"]=q; shooting["latest_error"]=error;
            shooting["latest_classification"]=q<=1 ? "阈值内" : q<=1.5 ? "微动" : "奔跑";
        }
    }
    shooting["sample_count"]=observed; shooting["valid_count"]=valid;
    if(valid) { shooting["stable_percent"]=100.0*stable/valid; shooting["mean_error"]=errors/valid; }
    return {{"timing",timing},{"shooting",shooting},{"summary_scope","最近300次保留记录中的有效样本"},
        {"baseline",{{"timing_zero_ms",0},{"perfect_ms",2},{"excellent_ms",10},
            {"default_sampling",sampling_settings_json(SamplingSettings{})},
            {"active_sampling",sampling_settings_json(settings)},
            {"active_threshold_q",1},{"default_threshold_q",SamplingSettings{}.clean_shot_speed_ratio/settings.clean_shot_speed_ratio}}}};
}
}
#endif
