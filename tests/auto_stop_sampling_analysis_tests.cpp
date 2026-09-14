#include "auto_stop_probe/sampling_analysis_internal.h"
#include <iostream>

namespace {
using namespace auto_stop_probe_detail;
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
Json cmd(const char* kind, int value, std::int64_t ms) {
    const auto time = ms * 1000000;
    return {{"kind", kind}, {"value", value}, {"planned_ns", time - 100}, {"submit_ns", time - 100},
        {"ack_received_ns", time}, {"backend_completed_ns", time + 10}, {"returned_ns", time + 20},
        {"disposition", 2}};
}
Json report(int brake, int hold = 5) {
    return {{"success", true}, {"plan", {{"schema_version",2},{"baseline","counter"},{"direction",2},
        {"move_ms",100},{"counter_hold_ms",brake},{"counter_delay_ms",0},
        {"shot_after_release_ms",18},{"shot_hold_ms",hold},{"fire_interval_ms",300}}},
        {"commands", Json::array({cmd("wasd",0,1), cmd("left_button",0,2),cmd("wasd",2,10),
            cmd("wasd",0,110),cmd("wasd",8,110),cmd("wasd",0,110+brake),
            cmd("left_button",1,128+brake),cmd("left_button",0,128+brake+hold),cmd("wasd",0,400+hold)})}};
}
void tests() {
    const auto defaults = sampling_detail::parse_sampling_settings(Json::object());
    check(defaults.hud_enabled && sampling_detail::sampling_settings_json(defaults)["fire_sample_delay_ms"] == 18,
        "参数默认兼容并输出完整快照");
    const auto five = analyze_counterpulse_sampling(report(5));
    check(five["analysis_complete"] == true, "正式schema2数字direction=2可解析");
    check(five["shots"][0]["down_model"]["classification"] == "MICRO" &&
        five["shots"][0]["samples"][0]["classification"] == "MICRO", "5ms反向仍微动");
    const auto ten = analyze_counterpulse_sampling(report(10));
    check(ten["shots"][0]["down_model"]["within_model_threshold"] == false &&
        ten["shots"][0]["samples"][0]["within_model_threshold"] == true, "10ms仅延迟采样进入模型阈值");
    const auto fifteen = analyze_counterpulse_sampling(report(15));
    check(fifteen["shots"][0]["down_model"]["within_model_threshold"] == true, "15ms按火时已进模型阈值");
    check(five["first_sample_count"] == 1 && five["held_sample_count"] == 0 &&
        five["shots"][0]["samples"][0]["time_ns"].get<std::int64_t>() -
        five["shots"][0]["down_ack_ns"].get<std::int64_t>() == 18000000, "5ms短按仍18ms首样本");
    const auto long_hold = analyze_counterpulse_sampling(report(5,1000));
    check(long_hold["first_sample_count"] == 1 && long_hold["held_sample_count"] == 9 &&
        long_hold["samples_are_bullets"] == false && long_hold["physical_validation_passed"] == false,
        "长按产生模型样本而非十发子弹");
    auto broken = report(5); broken["commands"][4]["disposition"] = 3;
    auto invalid = analyze_counterpulse_sampling(broken);
    check(invalid["analysis_complete"] == false && invalid["shots"][0]["down_model"]["valid"] == false,
        "未知ACK后即使全松也不恢复可信速度");
    broken = report(5); broken["commands"].erase(7);
    invalid = analyze_counterpulse_sampling(broken);
    check(invalid["shots"][0]["complete_hold"] == false && invalid["shots"][0]["down_model"]["valid"] == false,
        "缺UP不得显示有效通过");
    auto limited = analyze_counterpulse_sampling(report(5,40000));
    check(limited["total_sample_count"] == 300 && limited["shots"][0]["sample_budget_exceeded"] == true,
        "长按模型样本预算有界且明确截断");
    broken = report(5); broken.erase("plan");
    invalid = analyze_counterpulse_sampling(broken);
    check(invalid["analysis_complete"] == false && invalid["shots"].size() == 1, "缺计划保留可分析前缀并标记");
    broken = report(5); broken["commands"][4]["value"] = 1;
    invalid = analyze_counterpulse_sampling(broken);
    check(invalid["analysis_complete"] == false && invalid["shots"][0]["down_model"]["valid"] == false,
        "不支持W/S明确使模型失效");
    broken = report(5); broken["commands"].erase(8);
    invalid = analyze_counterpulse_sampling(broken);
    check(invalid["shots"][0]["samples"][0]["extrapolated"] == true, "短按未来采样超末报告需标外推");
    broken = report(5); broken["commands"][8] = cmd("wasd",2,143);
    invalid = analyze_counterpulse_sampling(broken);
    check(invalid["shots"][0]["samples"][0]["estimated_speed"].get<double>() >
        five["shots"][0]["samples"][0]["estimated_speed"].get<double>(),
        "短按UP之后采样之前新按键按真实顺序影响模型");
    auto tuned = report(5);
    tuned["sampling_settings"] = {{"counter_strafe_accel_per_sec",40},{"fire_sample_delay_ms",30},
        {"auto_fire_interval_ms",200},{"hud_enabled",false}};
    auto custom = analyze_counterpulse_sampling(tuned);
    check(custom["shots"][0]["down_model"]["within_model_threshold"] == true &&
        custom["shots"][0]["samples"][0]["time_ns"].get<std::int64_t>() -
        custom["shots"][0]["down_ack_ns"].get<std::int64_t>() == 30000000 && custom["settings"]["hud_enabled"] == false,
        "可调反向模型与采样延迟真实生效");
    tuned = report(5,1000); tuned["sampling_settings"] = {{"auto_fire_interval_ms",200}};
    custom = analyze_counterpulse_sampling(tuned);
    check(custom["total_sample_count"] == 5, "采样间隔参数改变长按样本数");
    tuned = report(5); tuned["sampling_settings"] = {{"clean_shot_speed_ratio",.8}};
    custom = analyze_counterpulse_sampling(tuned);
    check(custom["shots"][0]["down_model"]["within_model_threshold"] == true,
        "可调稳定模型阈值真实生效但不改物理判定");
    for (const auto bad : {Json{{"accel_per_sec",0}},Json{{"fire_sample_delay_ms",1.5}},
        Json{{"unknown",1}},Json{{"hud_enabled",1}},Json{{"max_move_speed",std::nan("")}}}) {
        tuned["sampling_settings"] = bad;
        custom = analyze_counterpulse_sampling(tuned);
        check(custom["analysis_complete"] == false && custom["settings"].is_null(), "非法采样设置不能悄悄回默认");
    }
    sampling_detail::SamplingModel live;
    live.observe_wasd(1000000,0); live.observe_wasd(10000000,2);
    const auto before = live.model_at(110000000);
    live.model_at(900000000);
    live.observe_wasd(110000000,0);
    const auto after = live.model_at(110000000);
    check(before["estimated_speed"] == after["estimated_speed"], "实时未来查询不推进状态或污染较早ACK");
    live.invalidate(120000000); live.observe_wasd(130000000,0);
    check(live.model_at(200000000)["valid"] == false, "实时未知后全松不伪造速度复位");
    tuned = report(5); tuned["plan"]["baseline"] = "no_counter";
    tuned["commands"].erase(4); tuned["commands"].erase(4);
    custom = analyze_counterpulse_sampling(tuned);
    bool saw_move = false, saw_counter = false;
    for (const auto& stage : custom["shots"][0]["stages"]) {
        saw_move |= stage["name"] == "move_hold"; saw_counter |= stage["name"] == "counter_hold";
    }
    check(saw_move && !saw_counter, "no_counter移动不能误标反向阶段");
    tuned = report(5); tuned["commands"][4]["submit_ns"] = INT64_MIN;
    tuned["commands"][4]["planned_ns"] = INT64_MAX;
    tuned["commands"][4]["ack_received_ns"] = INT64_MAX;
    custom = analyze_counterpulse_sampling(tuned);
    check(custom["analysis_complete"] == false, "恶意时钟边界明确无效且差值计算无溢出");
    sampling_detail::SamplingModel unordered;
    unordered.observe_wasd(100,0); unordered.observe_wasd(99,2);
    check(unordered.model_at(101)["valid"] == false && unordered.model_at(INT64_MAX)["valid"] == false,
        "实时乱序与越界查询无效");
    auto live_report = report(5,1000);
    live_report["commands"].erase(8); live_report["commands"].erase(7);
    auto at90 = analyze_counterpulse_live_sampling(live_report, defaults, 223000000);
    auto at118 = analyze_counterpulse_live_sampling(live_report, defaults, 251000000);
    check(at90["total_sample_count"] == 1 && at118["total_sample_count"] == 2 &&
        at118["shots"][0]["samples"][1]["valid"] == true && at118["shots"][0]["complete_hold"] == false,
        "实时长按没有后续ACK仍按当前时间采样且不称已完成hold");
    auto tap_report = report(5); tap_report["commands"].erase(8);
    auto before_due = analyze_counterpulse_live_sampling(tap_report, defaults, 143000000);
    auto at_due = analyze_counterpulse_live_sampling(tap_report, defaults, 151000000);
    check(before_due["total_sample_count"] == 0 && at_due["total_sample_count"] == 1,
        "5ms短按实时不能提前显示18ms首样本");
    live_report["commands"].push_back(cmd("left_button",1,140));
    auto duplicate = analyze_counterpulse_live_sampling(live_report, defaults, 300000000);
    check(duplicate["analysis_complete"] == false && duplicate["shots"][1]["samples"][0]["valid"] == false,
        "实时重复DOWN使后续模型失效而非用query覆盖有效性");
    check(duplicate["current_model"]["valid"] == false && duplicate["current_model"]["classification"] == "UNAVAILABLE",
        "重复DOWN的实时当前速度与采样有效性一致");
    live_report = report(5); live_report["commands"].erase(8); live_report["commands"].erase(7);
    live_report["commands"].push_back(cmd("wasd",0,140));
    live_report["commands"].back()["disposition"] = 3;
    auto live_unknown = analyze_counterpulse_live_sampling(live_report, defaults, 300000000);
    check(live_unknown["shots"][0]["samples"][0]["valid"] == false,
        "实时未知ACK中断活动hold可信性");
}
}
int main() { try { tests(); std::cout << "采样分析测试通过\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; } }
