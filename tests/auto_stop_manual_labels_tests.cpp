#include "auto_stop_probe/manual_labels_internal.h"
#include <iostream>
using namespace auto_stop_probe_detail;
int main() {
    try {
        const auto shot = [](int id, double speed) { return Json{{"ordinal",id},{"complete_hold",true},
            {"samples",Json::array({Json{{"valid",true},{"estimated_speed",speed}}})}}; };
        Json analysis{{"recording_id","test"},{"archive_complete",true},{"settings",{{"max_move_speed",1}}},
            {"shots",Json::array({shot(1,.25),shot(2,.3),shot(3,.4)})}};
        Json labels{{"schema_version",1},{"recording_id","test"},{"qualified_shot_ranges",Json::array({{1,2}})},
            {"rejected_shot_ranges",Json::array({{3,3}})}};
        const auto check = [](bool good) { if (!good) throw std::runtime_error("人工标签约束回归失败"); };
        auto result = apply_manual_labels(analysis,labels);
        check(result["calibration_envelope"]["fit_status"] == "FIXED_MODEL_THRESHOLD_BOUNDS");
        check(result["calibration_envelope"]["threshold_ratio_lower_bound"] == .3);
        check(result["calibration_envelope"]["threshold_ratio_upper_exclusive"] == .4);
        check(result["calibration_envelope"]["settings_applied"] == false);
        auto positive = labels; positive["rejected_shot_ranges"] = Json::array();
        check(apply_manual_labels(analysis,positive)["calibration_envelope"]["fit_status"] == "INSUFFICIENT_LABEL_CLASSES");
        auto conflict = analysis; conflict["shots"][2]["samples"][0]["estimated_speed"] = .2;
        check(apply_manual_labels(conflict,labels)["calibration_envelope"]["fit_status"] == "MODEL_OR_TIMING_CONFLICT");
        conflict = analysis; conflict["shots"][0]["atomic_ambiguous"] = true;
        check(apply_manual_labels(conflict,labels)["calibration_envelope"]["fit_status"] == "EVIDENCE_INCOMPLETE");
        for (int kind=0;kind<4;++kind) {
            auto bad=labels;
            if(kind==0) bad["recording_id"]="other";
            if(kind==1) bad["qualified_shot_ranges"]={{1,5}};
            if(kind==2) bad["rejected_shot_ranges"]={{2,3}};
            if(kind==3) bad["qualified_shot_ranges"]={{1,2},{2,2}};
            bool rejected=false;try{(void)apply_manual_labels(analysis,bad);}catch(...){rejected=true;}check(rejected);
        }
        std::cout << "人工标记范围、不可识别性与证据边界通过\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
