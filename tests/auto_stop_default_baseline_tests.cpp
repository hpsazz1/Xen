#include "auto_stop_probe/default_baseline_internal.h"
#include <iostream>
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
}
int main() {
    using namespace auto_stop_probe_detail;
    try {
        const auto report = derive_default_baseline();
        const auto p = parse_counterpulse_plan(report.at("candidate_plan"));
        require(p.move_ms == 182 && p.counter_hold_ms == 72 && p.shot_after_release_ms == 2 && p.fire_delay_ms == 13,
            "默认公式应生成182/72/2/13");
        require(!p.move_during_fire_delay && p.fire_interval_ms == 0 && p.direction == 2, "首评分前必须中性等待");
        auto v = sampling_detail::advance(0, 1, p.move_ms/1000.0);
        require(v == 1, "移动必须达到模型满速");
        v = sampling_detail::advance(v, -1, p.counter_hold_ms/1000.0);
        require(v < 0 && std::abs(v + .003142857142857) < 1e-12, "整数过冲必须正式积分保留");
        v = sampling_detail::advance(v, 0, p.shot_after_release_ms/1000.0);
        require(v == 0 && report["derivation"]["q_at_fire_down"] == 0 && report["derivation"]["q_at_first_sample"] == 0,
            "自然等待后必须模型回零");
        require(p.shot_hold_ms + p.fire_delay_ms >= 18, "下一移动不能早于首采样");
        sampling_detail::SamplingSettings custom; custom.max_move_speed = .5; custom.accel_per_sec = 10;
        custom.counter_strafe_accel_per_sec = 20; custom.fire_sample_delay_ms = 0;
        const auto c = parse_counterpulse_plan(derive_default_baseline(custom)["candidate_plan"]);
        require(c.move_ms == 50 && c.counter_hold_ms == 25 && c.shot_after_release_ms == 0 && c.fire_delay_ms == 0,
            "自定义有效参数必须重新推导");
        sampling_detail::SamplingSettings ratio; ratio.clean_shot_speed_ratio = .8;
        require(derive_default_baseline(ratio)["candidate_plan"] == report["candidate_plan"], "阈比不改变归零动作，不能倒推物理真值");
        sampling_detail::SamplingSettings incompatible; incompatible.max_move_speed = 2; incompatible.accel_per_sec = 1;
        bool rejected = false;
        try { (void)derive_default_baseline(incompatible); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "有效模型但超动作预算必须拒绝，不裁剪move");
        sampling_detail::SamplingSettings invalid; invalid.natural_decel_per_sec = 0;
        rejected = false;
        try { (void)derive_default_baseline(invalid); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "无效设置必须先由正式settings parser拒绝");
        require(report["source"] == "FORMULA_ONLY_NO_MANUAL_DATA" && !report["assumptions"].empty(), "假设及来源必须随报告保留");
        require(report["derivation"]["time_basis"] == "NOMINAL_ZERO_EXTRA_ACK_DELAY", "名义积分不能冒充实际ACK时序");
        auto delayed = sampling_detail::advance(1, -1, (p.counter_hold_ms + 1)/1000.0);
        delayed = sampling_detail::advance(delayed, 0, p.shot_after_release_ms/1000.0);
        require(std::abs(delayed) > 0, "额外1ms反向释放ACK延迟时，不能声称开火当刻仍严格归零");
        std::cout << "默认基线公式专项通过；未调用设备\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
