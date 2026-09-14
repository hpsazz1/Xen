#ifndef AUTO_STOP_DEFAULT_BASELINE_INTERNAL_H
#define AUTO_STOP_DEFAULT_BASELINE_INTERNAL_H
#include "auto_stop_probe/sampling_analysis_internal.h"
#include "auto_stop_probe/counterpulse_internal.h"

namespace auto_stop_probe_detail {
// 只生成参考模型下的可审阅动作；不读取人工素材，不执行设备。
inline Json derive_default_baseline(const sampling_detail::SamplingSettings& input = {}) {
    const auto settings = sampling_detail::parse_sampling_settings(sampling_detail::sampling_settings_json(input));
    constexpr int shot_hold = 5;
    const auto move = static_cast<int>(std::ceil(1000 * settings.max_move_speed / settings.accel_per_sec));
    const auto reverse = static_cast<int>(std::ceil(1000 * settings.max_move_speed / settings.counter_strafe_accel_per_sec));
    const auto moved = sampling_detail::advance(0, 1, move / 1000.0, settings);
    const auto reversed = sampling_detail::advance(moved, -1, reverse / 1000.0, settings);
    const auto after = static_cast<int>(std::ceil(1000 * std::abs(reversed) / settings.natural_decel_per_sec));
    const auto rested = sampling_detail::advance(reversed, 0, after / 1000.0, settings);
    const auto sampled = sampling_detail::advance(rested, 0, settings.fire_sample_delay_ms / 1000.0, settings);
    const auto delay = std::max(0, settings.fire_sample_delay_ms - shot_hold);
    Json plan{{"schema_version",2},{"baseline","counter"},{"shots",8},{"capture_enabled",false},
        {"direction",2},{"move_ms",move},{"counter_delay_ms",0},{"counter_hold_ms",reverse},
        {"shot_after_release_ms",after},{"fire_delay_ms",delay},{"fire_interval_ms",0},
        {"move_during_fire_delay",false},{"shot_hold_ms",shot_hold},{"late_tolerance_ms",5}};
    CounterpulsePlan parsed;
    try { parsed = parse_counterpulse_plan(plan); }
    catch (const std::exception& e) { throw std::runtime_error(std::string("默认模型动作在正式 schema 2 中不可表达，未裁剪参数：") + e.what()); }
    const auto threshold = settings.max_move_speed * settings.clean_shot_speed_ratio;
    return Json{{"schema_version",1},{"source","FORMULA_ONLY_NO_MANUAL_DATA"},
        {"sampling_settings",sampling_detail::sampling_settings_json(settings)},
        {"candidate_plan",counterpulse_plan_json(parsed)},
        {"derivation",Json{{"time_basis","NOMINAL_ZERO_EXTRA_ACK_DELAY"},
            {"move_to_max_ms",1000 * settings.max_move_speed / settings.accel_per_sec},
            {"reverse_to_zero_ms",1000 * settings.max_move_speed / settings.counter_strafe_accel_per_sec},
            {"velocity_after_move",moved},{"velocity_after_reverse",reversed},
            {"natural_return_ms",1000 * std::abs(reversed) / settings.natural_decel_per_sec},
            {"velocity_at_fire_down",rested},{"q_at_fire_down",std::abs(rested)/threshold},
            {"q_at_first_sample",std::abs(sampled)/threshold},
            {"next_movement_earliest_after_fire_down_ms",shot_hold+delay},
            {"first_sample_after_fire_down_ms",settings.fire_sample_delay_ms},
            {"cycle_budget_ms",parsed.cycle_budget_ms()},
            {"formulas",Json::array({"move_ms=ceil(1000*V/a)","counter_hold_ms=ceil(1000*V/b)",
                "reverse_velocity=advance(advance(0,+1,move_ms/1000),-1,counter_hold_ms/1000)",
                "shot_after_release_ms=ceil(1000*abs(reverse_velocity)/n)",
                "fire_delay_ms=max(0,fire_sample_delay_ms-5)","q=abs(v)/(V*r)"})}}},
        {"assumptions",Json::array({"从模型零速开始；仅 A/D 单轴，方向对称。",
            "默认 V=1、a=5.5、n=2.5、b=14、r=.34、首采样18ms来自参考初值，未实测校准；自定义值同样仅为假设。",
            "shots=8、shot_hold_ms=5、late_tolerance_ms=5 是工具选择，不是运动方程推导。",
            "counter_delay_ms=0 是无空隙设计；fire_interval_ms=0 避免重复预算。",
            "先打一发种子单发，再执行7个移动周期；ACK计时不等同游戏生效时刻。",
            "积分假设无额外命令或ACK延迟；反向UP ACK延迟会延长实际保持，实际q应按Run时间戳重算，不能保证为零。",
            "q<=1 仅模型阈值内，不表示实际停稳或物理射击无误差。",
            "下一移动不早于首样本；同刻边沿依赖连续模型零瞬时增量，不提供实机时序余量。"})}};
}
}
#endif
