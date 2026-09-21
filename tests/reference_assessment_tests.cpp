#include "reference_assessment/reference_assessment.h"
#include <cmath>
#include <iostream>
#include <limits>

using namespace xen::reference_assessment;
namespace {
int failures = 0;
void expect(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << message << '\n'; }
}
void near(double actual, double expected, const char* message) {
    expect(std::abs(actual - expected) < 0.000001, message);
}
void event(Engine& e, TimeMs t, Key key, bool down) {
    expect(e.handle_event({t, key, down}).accepted, "合法事件被拒绝");
}
Shot fire(Engine& e, TimeMs t) {
    event(e, t, Key::Fire, true);
    auto out = e.tick(t + 19);
    expect(out.shots.size() == 1, "延迟首样本数量错误");
    return out.shots.empty() ? Shot{} : out.shots.front();
}
}
int main() {
    // 对应上游 assessment_engine 的七个固定向量，额外核验精确有符号差。
    for (const int diff : {-15, 1, 8, 15, 400}) {
        Engine e;
        event(e, 0, Key::Left, true);
        Output out;
        if (diff < 0) {
            event(e, 100, Key::Right, true);
            out = e.handle_event({115, Key::Left, false});
        } else {
            event(e, 100, Key::Left, false);
            out = e.handle_event({100.0 + diff, Key::Right, true});
        }
        if (diff == 400) expect(out.assessments.empty(), "极端时间差未过滤");
        else {
            expect(out.assessments.size() == 1, "换向记录缺失");
            if (!out.assessments.empty()) {
                const auto& a = out.assessments.front();
                near(a.diff_ms, diff, "有符号时间差不正确");
                expect(a.perfect == (std::abs(diff) <= 2), "完美边界不正确");
                expect(a.success == (std::abs(diff) <= 10), "成功边界不正确");
            }
        }
    }
    {
        Engine e;
        event(e, 0, Key::Left, true); event(e, 100, Key::Left, false);
        event(e, 115, Key::Right, true); event(e, 120, Key::Right, false);
        expect(e.handle_event({130, Key::Left, true}).assessments.empty(), "50ms去重失效");
    }
    {
        Engine e;
        event(e, 0, Key::Forward, true); event(e, 100, Key::Forward, false);
        auto out = e.handle_event({110, Key::Back, true});
        expect(out.assessments.size() == 1 && out.assessments[0].success, "WS边界应成功");
    }
    {
        Engine e;
        event(e, 0, Key::Right, true); e.tick(50);
        near(e.snapshot().velocity_x, 0.275, "50ms加速向量错误");
        e.tick(200); near(e.snapshot().velocity_x, 1, "满速模型向量错误");
        auto shot = fire(e, 210);
        near(shot.speed_ratio, 2.941, "持续方向速度比错误");
        expect(!shot.stable && shot.reason == "single_direction_held", "持续方向应模型跑打");
    }
    {
        Engine e;
        event(e, 0, Key::Left, true); e.tick(200);
        event(e, 200, Key::Right, true); e.tick(250);
        near(e.snapshot().velocity_x, -0.3, "重叠时应按最新反向键制动");
        auto shot = fire(e, 250);
        expect(shot.axis_conflict && shot.counter_strafe, "重叠制动状态缺失");
    }
    {
        Engine e;
        event(e, 0, Key::Right, true); e.tick(200);
        event(e, 200, Key::Right, false); e.tick(300);
        near(e.snapshot().velocity_x, 0.75, "自然减速向量错误");
        e.tick(600); near(e.snapshot().velocity_x, 0, "松键未归零");
        auto shot = fire(e, 620);
        expect(shot.stable && shot.reason == "no_movement", "静止模型应稳定");
    }
    {
        Engine e;
        event(e, 0, Key::Right, true); event(e, 0, Key::Crouch, true);
        auto shot = fire(e, 200);
        expect(shot.stable && shot.crouching && shot.reason == "crouching", "蹲射上游语义错误");
        near(shot.speed_ratio, 0, "蹲射应覆盖模型速度比");
        expect(shot.estimated_speed > 0, "蹲射不应删除估计速度");
    }
    {
        Engine e;
        event(e, 0, Key::Crouch, true); event(e, 100, Key::Crouch, false);
        auto shot = fire(e, 110);
        expect(shot.crouch_grace && !shot.stable && shot.reason == "crouch_grace", "蹲起宽限不能标作稳定");
    }
    {
        Engine e;
        event(e, 0, Key::Fire, true);
        expect(e.snapshot().next_sample_ms == 18, "默认首采样延迟不是18ms");
        auto out = e.handle_event({5, Key::Fire, false});
        expect(out.shots.size() == 1 && out.shots[0].time_ms == 18, "短tap必须保留上游前推采样语义");
        expect(e.tick(200).shots.empty(), "释放后不应继续采样");
    }
    {
        Engine e;
        event(e, 0, Key::Fire, true); event(e, 5, Key::Fire, true);
        expect(e.tick(18).shots.size() == 1, "pending首样本前重复DOWN不能重启");
        auto out = e.tick(318);
        expect(out.shots.size() == 3, "迟到tick应排空118/218/318ms样本");
        if (out.shots.size() == 3) {
            expect(out.shots[0].held && out.shots[0].sequence_index == 2, "连发序号错误");
            expect(out.shots[2].time_ms == 318, "不能把到期时间换成处理时间");
        }
        event(e, 319, Key::Fire, false);
        expect(e.tick(500).shots.empty(), "UP未取消连发");
    }
    {
        Engine e;
        event(e, 0, Key::Fire, true);
        unsigned count = 0;
        for (TimeMs t = 20; t <= 35000; t += 10) count += static_cast<unsigned>(e.tick(t).shots.size());
        expect(count > 300 && e.snapshot().next_sample_ms.has_value(), "持续持键保护应重启而非永久停采样");
        expect(e.snapshot().shots.size() == 300, "历史应有界");
    }
    {
        Engine e;
        event(e, 0, Key::Fire, true);
        expect(e.reset(10), "gap reset失败");
        expect(e.tick(100).shots.empty() && !e.snapshot().fire_pressed, "gap后不得继承开火");
        event(e, 100, Key::Left, true); e.tick(200);
        const auto before = e.snapshot();
        expect(!e.tick(199).accepted, "倒退时间未拒绝");
        near(e.snapshot().velocity_x, before.velocity_x, "拒绝时间仍修改模型");
        expect(!e.reset(-1), "负时间reset未拒绝");
    }
    {
        // runtime先tick后事件；直接engine调用保留上游事件先改键态的不同结果。
        Engine direct, runtime;
        event(direct, 0, Key::Right, true); event(runtime, 0, Key::Right, true);
        event(direct, 100, Key::Right, false);
        runtime.tick(100); event(runtime, 100, Key::Right, false);
        near(direct.snapshot().velocity_x, 0, "直接engine顺序偏离上游");
        near(runtime.snapshot().velocity_x, 0.55, "runtime应先积分旧键态");
    }
    {
        Engine e;
        event(e, 0, Key::Shift, true); event(e, 0, Key::Space, true);
        auto shot = fire(e, 10);
        expect(shot.stable && e.snapshot().shift_pressed && e.snapshot().space_pressed, "Basic modifier仅记录、不推断物理状态");
    }
    {
        Engine e;
        event(e, 0, Key::Left, true); event(e, 100, Key::Left, false);
        auto out = e.handle_event({102.1, Key::Right, true});
        expect(out.assessments.size() == 1 && !out.assessments[0].perfect, "2.1ms不得截断成完美");
        if (!out.assessments.empty()) near(out.assessments[0].diff_ms, 2.1, "小数毫秒精度丢失");
        expect(!e.tick(std::numeric_limits<double>::quiet_NaN()).accepted, "NaN未拒绝");
        expect(!e.tick(std::numeric_limits<double>::infinity()).accepted, "无穷时间未拒绝");
    }
    // 原Rust核心8条黄金轨迹：逐项核对数值与状态，排除墙钟字段。
    { Engine e;
        e.tick(0); event(e, 0, Key::Left, true);
        e.tick(500); event(e, 500, Key::Left, false);
        e.tick(505); event(e, 505, Key::Right, true);
        e.tick(530); event(e, 530, Key::Fire, true);
        e.tick(535); event(e, 535, Key::Fire, false);
        e.tick(700);
        const auto snapshot = e.snapshot();
        expect(snapshot.shots.size() == 1, "黄金样本数量 release_then_opposite_5ms");
        if (snapshot.shots.size() > 0) {
            near(snapshot.shots[0].error, 0, "黄金字段 release_then_opposite_5ms error");
            near(snapshot.shots[0].timing_diff_ms, -13.382352941176311, "黄金字段 release_then_opposite_5ms timingDiffMs");
            near(snapshot.shots[0].speed_ratio, 1, "黄金字段 release_then_opposite_5ms speedRatio");
            near(snapshot.shots[0].sequence_index, 1, "黄金字段 release_then_opposite_5ms shotSequenceIndex");
            near(snapshot.shots[0].stop_success_age_ms, 0, "黄金字段 release_then_opposite_5ms stopSuccessAgeMs");
            near(snapshot.shots[0].estimated_speed, 0.385, "黄金字段 release_then_opposite_5ms estimatedSpeed");
            near(snapshot.shots[0].movement_keys_down, 1, "黄金字段 release_then_opposite_5ms movementKeysDown");
            near(snapshot.shots[0].accuracy_threshold, 0.34, "黄金字段 release_then_opposite_5ms accuracyThreshold");
            expect(snapshot.shots[0].crouching == false, "黄金状态 release_then_opposite_5ms crouching");
            expect(snapshot.shots[0].held == false, "黄金状态 release_then_opposite_5ms fireHeld");
            expect(snapshot.shots[0].crouch_grace == false, "黄金状态 release_then_opposite_5ms crouchGraceActive");
            expect(snapshot.shots[0].counter_strafe == true, "黄金状态 release_then_opposite_5ms counterStrafeActive");
            expect(snapshot.shots[0].delayed == true, "黄金状态 release_then_opposite_5ms fireSampleDelayed");
            expect(snapshot.shots[0].axis_conflict == false, "黄金状态 release_then_opposite_5ms axisConflict");
            expect(snapshot.shots[0].stable == true, "黄金状态 release_then_opposite_5ms isStable");
        }
        expect(snapshot.assessments.size() == 1, "黄金换向数量 release_then_opposite_5ms");
        if(snapshot.assessments.size() > 0) near(snapshot.assessments[0].diff_ms, 5, "黄金换向时间差");
    }
    { Engine e;
        e.tick(0); event(e, 0, Key::Left, true);
        e.tick(500); event(e, 500, Key::Right, true);
        e.tick(515); event(e, 515, Key::Left, false);
        e.tick(550); event(e, 550, Key::Fire, true);
        e.tick(555); event(e, 555, Key::Fire, false);
        e.tick(700);
        const auto snapshot = e.snapshot();
        expect(snapshot.shots.size() == 1, "黄金样本数量 overlap_15ms");
        if (snapshot.shots.size() > 0) {
            near(snapshot.shots[0].error, 0, "黄金字段 overlap_15ms error");
            near(snapshot.shots[0].timing_diff_ms, 20.9, "黄金字段 overlap_15ms timingDiffMs");
            near(snapshot.shots[0].speed_ratio, 0.141, "黄金字段 overlap_15ms speedRatio");
            near(snapshot.shots[0].sequence_index, 1, "黄金字段 overlap_15ms shotSequenceIndex");
            near(snapshot.shots[0].stop_success_age_ms, 20.9, "黄金字段 overlap_15ms stopSuccessAgeMs");
            near(snapshot.shots[0].estimated_speed, 0.048, "黄金字段 overlap_15ms estimatedSpeed");
            near(snapshot.shots[0].movement_keys_down, 1, "黄金字段 overlap_15ms movementKeysDown");
            near(snapshot.shots[0].accuracy_threshold, 0.34, "黄金字段 overlap_15ms accuracyThreshold");
            expect(snapshot.shots[0].crouching == false, "黄金状态 overlap_15ms crouching");
            expect(snapshot.shots[0].held == false, "黄金状态 overlap_15ms fireHeld");
            expect(snapshot.shots[0].crouch_grace == false, "黄金状态 overlap_15ms crouchGraceActive");
            expect(snapshot.shots[0].counter_strafe == true, "黄金状态 overlap_15ms counterStrafeActive");
            expect(snapshot.shots[0].delayed == true, "黄金状态 overlap_15ms fireSampleDelayed");
            expect(snapshot.shots[0].axis_conflict == false, "黄金状态 overlap_15ms axisConflict");
            expect(snapshot.shots[0].stable == true, "黄金状态 overlap_15ms isStable");
        }
        expect(snapshot.assessments.size() == 1, "黄金换向数量 overlap_15ms");
        if(snapshot.assessments.size() > 0) near(snapshot.assessments[0].diff_ms, -15, "黄金换向时间差");
    }
    { Engine e;
        e.tick(0); event(e, 0, Key::Left, true);
        e.tick(500); event(e, 500, Key::Left, false);
        e.tick(500); event(e, 500, Key::Right, true);
        e.tick(550); event(e, 550, Key::Fire, true);
        e.tick(555); event(e, 555, Key::Fire, false);
        e.tick(700);
        const auto snapshot = e.snapshot();
        expect(snapshot.shots.size() == 1, "黄金样本数量 same_time_swap");
        if (snapshot.shots.size() > 0) {
            near(snapshot.shots[0].error, 0, "黄金字段 same_time_swap error");
            near(snapshot.shots[0].timing_diff_ms, 20.9, "黄金字段 same_time_swap timingDiffMs");
            near(snapshot.shots[0].speed_ratio, 0.141, "黄金字段 same_time_swap speedRatio");
            near(snapshot.shots[0].sequence_index, 1, "黄金字段 same_time_swap shotSequenceIndex");
            near(snapshot.shots[0].stop_success_age_ms, 20.9, "黄金字段 same_time_swap stopSuccessAgeMs");
            near(snapshot.shots[0].estimated_speed, 0.048, "黄金字段 same_time_swap estimatedSpeed");
            near(snapshot.shots[0].movement_keys_down, 1, "黄金字段 same_time_swap movementKeysDown");
            near(snapshot.shots[0].accuracy_threshold, 0.34, "黄金字段 same_time_swap accuracyThreshold");
            expect(snapshot.shots[0].crouching == false, "黄金状态 same_time_swap crouching");
            expect(snapshot.shots[0].held == false, "黄金状态 same_time_swap fireHeld");
            expect(snapshot.shots[0].crouch_grace == false, "黄金状态 same_time_swap crouchGraceActive");
            expect(snapshot.shots[0].counter_strafe == true, "黄金状态 same_time_swap counterStrafeActive");
            expect(snapshot.shots[0].delayed == true, "黄金状态 same_time_swap fireSampleDelayed");
            expect(snapshot.shots[0].axis_conflict == false, "黄金状态 same_time_swap axisConflict");
            expect(snapshot.shots[0].stable == true, "黄金状态 same_time_swap isStable");
        }
        expect(snapshot.assessments.size() == 1, "黄金换向数量 same_time_swap");
        if(snapshot.assessments.size() > 0) near(snapshot.assessments[0].diff_ms, 0, "黄金换向时间差");
    }
    { Engine e;
        e.tick(0); event(e, 0, Key::Left, true);
        e.tick(500); event(e, 500, Key::Fire, true);
        e.tick(505); event(e, 505, Key::Fire, false);
        e.tick(700);
        const auto snapshot = e.snapshot();
        expect(snapshot.shots.size() == 1, "黄金样本数量 moving_short_tap");
        if (snapshot.shots.size() > 0) {
            near(snapshot.shots[0].error, 1, "黄金字段 moving_short_tap error");
            near(snapshot.shots[0].timing_diff_ms, -194.1176470588235, "黄金字段 moving_short_tap timingDiffMs");
            near(snapshot.shots[0].speed_ratio, 2.941, "黄金字段 moving_short_tap speedRatio");
            near(snapshot.shots[0].sequence_index, 1, "黄金字段 moving_short_tap shotSequenceIndex");
            near(snapshot.shots[0].stop_success_age_ms, 0, "黄金字段 moving_short_tap stopSuccessAgeMs");
            near(snapshot.shots[0].estimated_speed, 1, "黄金字段 moving_short_tap estimatedSpeed");
            near(snapshot.shots[0].movement_keys_down, 1, "黄金字段 moving_short_tap movementKeysDown");
            near(snapshot.shots[0].accuracy_threshold, 0.34, "黄金字段 moving_short_tap accuracyThreshold");
            expect(snapshot.shots[0].crouching == false, "黄金状态 moving_short_tap crouching");
            expect(snapshot.shots[0].held == false, "黄金状态 moving_short_tap fireHeld");
            expect(snapshot.shots[0].crouch_grace == false, "黄金状态 moving_short_tap crouchGraceActive");
            expect(snapshot.shots[0].counter_strafe == false, "黄金状态 moving_short_tap counterStrafeActive");
            expect(snapshot.shots[0].delayed == true, "黄金状态 moving_short_tap fireSampleDelayed");
            expect(snapshot.shots[0].axis_conflict == false, "黄金状态 moving_short_tap axisConflict");
            expect(snapshot.shots[0].stable == false, "黄金状态 moving_short_tap isStable");
        }
        expect(snapshot.assessments.size() == 0, "黄金换向数量 moving_short_tap");
    }
    { Engine e;
        e.tick(100); event(e, 100, Key::Fire, true);
        e.tick(105); event(e, 105, Key::Fire, false);
        e.tick(300);
        const auto snapshot = e.snapshot();
        expect(snapshot.shots.size() == 1, "黄金样本数量 stationary_short_tap");
        if (snapshot.shots.size() > 0) {
            near(snapshot.shots[0].error, 0, "黄金字段 stationary_short_tap error");
            near(snapshot.shots[0].timing_diff_ms, 13, "黄金字段 stationary_short_tap timingDiffMs");
            near(snapshot.shots[0].speed_ratio, 0, "黄金字段 stationary_short_tap speedRatio");
            near(snapshot.shots[0].sequence_index, 1, "黄金字段 stationary_short_tap shotSequenceIndex");
            near(snapshot.shots[0].stop_success_age_ms, 13, "黄金字段 stationary_short_tap stopSuccessAgeMs");
            near(snapshot.shots[0].estimated_speed, 0, "黄金字段 stationary_short_tap estimatedSpeed");
            near(snapshot.shots[0].movement_keys_down, 0, "黄金字段 stationary_short_tap movementKeysDown");
            near(snapshot.shots[0].accuracy_threshold, 0.34, "黄金字段 stationary_short_tap accuracyThreshold");
            expect(snapshot.shots[0].crouching == false, "黄金状态 stationary_short_tap crouching");
            expect(snapshot.shots[0].held == false, "黄金状态 stationary_short_tap fireHeld");
            expect(snapshot.shots[0].crouch_grace == false, "黄金状态 stationary_short_tap crouchGraceActive");
            expect(snapshot.shots[0].counter_strafe == false, "黄金状态 stationary_short_tap counterStrafeActive");
            expect(snapshot.shots[0].delayed == true, "黄金状态 stationary_short_tap fireSampleDelayed");
            expect(snapshot.shots[0].axis_conflict == false, "黄金状态 stationary_short_tap axisConflict");
            expect(snapshot.shots[0].stable == true, "黄金状态 stationary_short_tap isStable");
        }
        expect(snapshot.assessments.size() == 0, "黄金换向数量 stationary_short_tap");
    }
    { Engine e;
        e.tick(0); event(e, 0, Key::Left, true);
        e.tick(500); event(e, 500, Key::Fire, true);
        e.tick(600); event(e, 600, Key::Left, false);
        e.tick(1000); event(e, 1000, Key::Fire, false);
        e.tick(1200);
        const auto snapshot = e.snapshot();
        expect(snapshot.shots.size() == 5, "黄金样本数量 moving_held_fire_then_release");
        if (snapshot.shots.size() > 0) {
            near(snapshot.shots[0].error, 1, "黄金字段 moving_held_fire_then_release error");
            near(snapshot.shots[0].timing_diff_ms, -194.1176470588235, "黄金字段 moving_held_fire_then_release timingDiffMs");
            near(snapshot.shots[0].speed_ratio, 2.941, "黄金字段 moving_held_fire_then_release speedRatio");
            near(snapshot.shots[0].sequence_index, 1, "黄金字段 moving_held_fire_then_release shotSequenceIndex");
            near(snapshot.shots[0].stop_success_age_ms, 0, "黄金字段 moving_held_fire_then_release stopSuccessAgeMs");
            near(snapshot.shots[0].estimated_speed, 1, "黄金字段 moving_held_fire_then_release estimatedSpeed");
            near(snapshot.shots[0].movement_keys_down, 1, "黄金字段 moving_held_fire_then_release movementKeysDown");
            near(snapshot.shots[0].accuracy_threshold, 0.34, "黄金字段 moving_held_fire_then_release accuracyThreshold");
            expect(snapshot.shots[0].crouching == false, "黄金状态 moving_held_fire_then_release crouching");
            expect(snapshot.shots[0].held == false, "黄金状态 moving_held_fire_then_release fireHeld");
            expect(snapshot.shots[0].crouch_grace == false, "黄金状态 moving_held_fire_then_release crouchGraceActive");
            expect(snapshot.shots[0].counter_strafe == false, "黄金状态 moving_held_fire_then_release counterStrafeActive");
            expect(snapshot.shots[0].delayed == true, "黄金状态 moving_held_fire_then_release fireSampleDelayed");
            expect(snapshot.shots[0].axis_conflict == false, "黄金状态 moving_held_fire_then_release axisConflict");
            expect(snapshot.shots[0].stable == false, "黄金状态 moving_held_fire_then_release isStable");
        }
        if (snapshot.shots.size() > 1) {
            near(snapshot.shots[1].error, 1, "黄金字段 moving_held_fire_then_release error");
            near(snapshot.shots[1].timing_diff_ms, -180.88235294117646, "黄金字段 moving_held_fire_then_release timingDiffMs");
            near(snapshot.shots[1].speed_ratio, 2.809, "黄金字段 moving_held_fire_then_release speedRatio");
            near(snapshot.shots[1].sequence_index, 2, "黄金字段 moving_held_fire_then_release shotSequenceIndex");
            near(snapshot.shots[1].stop_success_age_ms, 0, "黄金字段 moving_held_fire_then_release stopSuccessAgeMs");
            near(snapshot.shots[1].estimated_speed, 0.955, "黄金字段 moving_held_fire_then_release estimatedSpeed");
            near(snapshot.shots[1].movement_keys_down, 0, "黄金字段 moving_held_fire_then_release movementKeysDown");
            near(snapshot.shots[1].accuracy_threshold, 0.34, "黄金字段 moving_held_fire_then_release accuracyThreshold");
            expect(snapshot.shots[1].crouching == false, "黄金状态 moving_held_fire_then_release crouching");
            expect(snapshot.shots[1].held == true, "黄金状态 moving_held_fire_then_release fireHeld");
            expect(snapshot.shots[1].crouch_grace == false, "黄金状态 moving_held_fire_then_release crouchGraceActive");
            expect(snapshot.shots[1].counter_strafe == false, "黄金状态 moving_held_fire_then_release counterStrafeActive");
            expect(snapshot.shots[1].delayed == false, "黄金状态 moving_held_fire_then_release fireSampleDelayed");
            expect(snapshot.shots[1].axis_conflict == false, "黄金状态 moving_held_fire_then_release axisConflict");
            expect(snapshot.shots[1].stable == false, "黄金状态 moving_held_fire_then_release isStable");
        }
        if (snapshot.shots.size() > 2) {
            near(snapshot.shots[2].error, 1, "黄金字段 moving_held_fire_then_release error");
            near(snapshot.shots[2].timing_diff_ms, -107.35294117647061, "黄金字段 moving_held_fire_then_release timingDiffMs");
            near(snapshot.shots[2].speed_ratio, 2.074, "黄金字段 moving_held_fire_then_release speedRatio");
            near(snapshot.shots[2].sequence_index, 3, "黄金字段 moving_held_fire_then_release shotSequenceIndex");
            near(snapshot.shots[2].stop_success_age_ms, 0, "黄金字段 moving_held_fire_then_release stopSuccessAgeMs");
            near(snapshot.shots[2].estimated_speed, 0.705, "黄金字段 moving_held_fire_then_release estimatedSpeed");
            near(snapshot.shots[2].movement_keys_down, 0, "黄金字段 moving_held_fire_then_release movementKeysDown");
            near(snapshot.shots[2].accuracy_threshold, 0.34, "黄金字段 moving_held_fire_then_release accuracyThreshold");
            expect(snapshot.shots[2].crouching == false, "黄金状态 moving_held_fire_then_release crouching");
            expect(snapshot.shots[2].held == true, "黄金状态 moving_held_fire_then_release fireHeld");
            expect(snapshot.shots[2].crouch_grace == false, "黄金状态 moving_held_fire_then_release crouchGraceActive");
            expect(snapshot.shots[2].counter_strafe == false, "黄金状态 moving_held_fire_then_release counterStrafeActive");
            expect(snapshot.shots[2].delayed == false, "黄金状态 moving_held_fire_then_release fireSampleDelayed");
            expect(snapshot.shots[2].axis_conflict == false, "黄金状态 moving_held_fire_then_release axisConflict");
            expect(snapshot.shots[2].stable == false, "黄金状态 moving_held_fire_then_release isStable");
        }
        if (snapshot.shots.size() > 3) {
            near(snapshot.shots[3].error, 0.676, "黄金字段 moving_held_fire_then_release error");
            near(snapshot.shots[3].timing_diff_ms, -33.82352941176474, "黄金字段 moving_held_fire_then_release timingDiffMs");
            near(snapshot.shots[3].speed_ratio, 1.338, "黄金字段 moving_held_fire_then_release speedRatio");
            near(snapshot.shots[3].sequence_index, 4, "黄金字段 moving_held_fire_then_release shotSequenceIndex");
            near(snapshot.shots[3].stop_success_age_ms, 0, "黄金字段 moving_held_fire_then_release stopSuccessAgeMs");
            near(snapshot.shots[3].estimated_speed, 0.455, "黄金字段 moving_held_fire_then_release estimatedSpeed");
            near(snapshot.shots[3].movement_keys_down, 0, "黄金字段 moving_held_fire_then_release movementKeysDown");
            near(snapshot.shots[3].accuracy_threshold, 0.34, "黄金字段 moving_held_fire_then_release accuracyThreshold");
            expect(snapshot.shots[3].crouching == false, "黄金状态 moving_held_fire_then_release crouching");
            expect(snapshot.shots[3].held == true, "黄金状态 moving_held_fire_then_release fireHeld");
            expect(snapshot.shots[3].crouch_grace == false, "黄金状态 moving_held_fire_then_release crouchGraceActive");
            expect(snapshot.shots[3].counter_strafe == false, "黄金状态 moving_held_fire_then_release counterStrafeActive");
            expect(snapshot.shots[3].delayed == false, "黄金状态 moving_held_fire_then_release fireSampleDelayed");
            expect(snapshot.shots[3].axis_conflict == false, "黄金状态 moving_held_fire_then_release axisConflict");
            expect(snapshot.shots[3].stable == false, "黄金状态 moving_held_fire_then_release isStable");
        }
        if (snapshot.shots.size() > 4) {
            near(snapshot.shots[4].error, 0, "黄金字段 moving_held_fire_then_release error");
            near(snapshot.shots[4].timing_diff_ms, 54, "黄金字段 moving_held_fire_then_release timingDiffMs");
            near(snapshot.shots[4].speed_ratio, 0.603, "黄金字段 moving_held_fire_then_release speedRatio");
            near(snapshot.shots[4].sequence_index, 5, "黄金字段 moving_held_fire_then_release shotSequenceIndex");
            near(snapshot.shots[4].stop_success_age_ms, 54, "黄金字段 moving_held_fire_then_release stopSuccessAgeMs");
            near(snapshot.shots[4].estimated_speed, 0.205, "黄金字段 moving_held_fire_then_release estimatedSpeed");
            near(snapshot.shots[4].movement_keys_down, 0, "黄金字段 moving_held_fire_then_release movementKeysDown");
            near(snapshot.shots[4].accuracy_threshold, 0.34, "黄金字段 moving_held_fire_then_release accuracyThreshold");
            expect(snapshot.shots[4].crouching == false, "黄金状态 moving_held_fire_then_release crouching");
            expect(snapshot.shots[4].held == true, "黄金状态 moving_held_fire_then_release fireHeld");
            expect(snapshot.shots[4].crouch_grace == false, "黄金状态 moving_held_fire_then_release crouchGraceActive");
            expect(snapshot.shots[4].counter_strafe == false, "黄金状态 moving_held_fire_then_release counterStrafeActive");
            expect(snapshot.shots[4].delayed == false, "黄金状态 moving_held_fire_then_release fireSampleDelayed");
            expect(snapshot.shots[4].axis_conflict == false, "黄金状态 moving_held_fire_then_release axisConflict");
            expect(snapshot.shots[4].stable == true, "黄金状态 moving_held_fire_then_release isStable");
        }
        expect(snapshot.assessments.size() == 0, "黄金换向数量 moving_held_fire_then_release");
    }
    { Engine e;
        e.tick(0); event(e, 0, Key::Left, true);
        e.tick(500); event(e, 500, Key::Crouch, true);
        e.tick(510); event(e, 510, Key::Fire, true);
        e.tick(515); event(e, 515, Key::Fire, false);
        e.tick(600); event(e, 600, Key::Crouch, false);
        e.tick(610); event(e, 610, Key::Fire, true);
        e.tick(615); event(e, 615, Key::Fire, false);
        e.tick(680); event(e, 680, Key::Fire, true);
        e.tick(685); event(e, 685, Key::Fire, false);
        e.tick(900);
        const auto snapshot = e.snapshot();
        expect(snapshot.shots.size() == 3, "黄金样本数量 crouch_and_exit");
        if (snapshot.shots.size() > 0) {
            near(snapshot.shots[0].error, 0, "黄金字段 crouch_and_exit error");
            near(snapshot.shots[0].timing_diff_ms, -194.1176470588235, "黄金字段 crouch_and_exit timingDiffMs");
            near(snapshot.shots[0].speed_ratio, 0, "黄金字段 crouch_and_exit speedRatio");
            near(snapshot.shots[0].sequence_index, 1, "黄金字段 crouch_and_exit shotSequenceIndex");
            near(snapshot.shots[0].stop_success_age_ms, 0, "黄金字段 crouch_and_exit stopSuccessAgeMs");
            near(snapshot.shots[0].estimated_speed, 1, "黄金字段 crouch_and_exit estimatedSpeed");
            near(snapshot.shots[0].movement_keys_down, 1, "黄金字段 crouch_and_exit movementKeysDown");
            near(snapshot.shots[0].accuracy_threshold, 0.34, "黄金字段 crouch_and_exit accuracyThreshold");
            expect(snapshot.shots[0].crouching == true, "黄金状态 crouch_and_exit crouching");
            expect(snapshot.shots[0].held == false, "黄金状态 crouch_and_exit fireHeld");
            expect(snapshot.shots[0].crouch_grace == false, "黄金状态 crouch_and_exit crouchGraceActive");
            expect(snapshot.shots[0].counter_strafe == false, "黄金状态 crouch_and_exit counterStrafeActive");
            expect(snapshot.shots[0].delayed == true, "黄金状态 crouch_and_exit fireSampleDelayed");
            expect(snapshot.shots[0].axis_conflict == false, "黄金状态 crouch_and_exit axisConflict");
            expect(snapshot.shots[0].stable == true, "黄金状态 crouch_and_exit isStable");
        }
        if (snapshot.shots.size() > 1) {
            near(snapshot.shots[1].error, 0, "黄金字段 crouch_and_exit error");
            near(snapshot.shots[1].timing_diff_ms, -194.1176470588235, "黄金字段 crouch_and_exit timingDiffMs");
            near(snapshot.shots[1].speed_ratio, 0, "黄金字段 crouch_and_exit speedRatio");
            near(snapshot.shots[1].sequence_index, 1, "黄金字段 crouch_and_exit shotSequenceIndex");
            near(snapshot.shots[1].stop_success_age_ms, 0, "黄金字段 crouch_and_exit stopSuccessAgeMs");
            near(snapshot.shots[1].estimated_speed, 1, "黄金字段 crouch_and_exit estimatedSpeed");
            near(snapshot.shots[1].movement_keys_down, 1, "黄金字段 crouch_and_exit movementKeysDown");
            near(snapshot.shots[1].accuracy_threshold, 0.34, "黄金字段 crouch_and_exit accuracyThreshold");
            expect(snapshot.shots[1].crouching == false, "黄金状态 crouch_and_exit crouching");
            expect(snapshot.shots[1].held == false, "黄金状态 crouch_and_exit fireHeld");
            expect(snapshot.shots[1].crouch_grace == true, "黄金状态 crouch_and_exit crouchGraceActive");
            expect(snapshot.shots[1].counter_strafe == false, "黄金状态 crouch_and_exit counterStrafeActive");
            expect(snapshot.shots[1].delayed == true, "黄金状态 crouch_and_exit fireSampleDelayed");
            expect(snapshot.shots[1].axis_conflict == false, "黄金状态 crouch_and_exit axisConflict");
            expect(snapshot.shots[1].stable == false, "黄金状态 crouch_and_exit isStable");
        }
        if (snapshot.shots.size() > 2) {
            near(snapshot.shots[2].error, 1, "黄金字段 crouch_and_exit error");
            near(snapshot.shots[2].timing_diff_ms, -194.1176470588235, "黄金字段 crouch_and_exit timingDiffMs");
            near(snapshot.shots[2].speed_ratio, 2.143, "黄金字段 crouch_and_exit speedRatio");
            near(snapshot.shots[2].sequence_index, 1, "黄金字段 crouch_and_exit shotSequenceIndex");
            near(snapshot.shots[2].stop_success_age_ms, 0, "黄金字段 crouch_and_exit stopSuccessAgeMs");
            near(snapshot.shots[2].estimated_speed, 1, "黄金字段 crouch_and_exit estimatedSpeed");
            near(snapshot.shots[2].movement_keys_down, 1, "黄金字段 crouch_and_exit movementKeysDown");
            near(snapshot.shots[2].accuracy_threshold, 0.34, "黄金字段 crouch_and_exit accuracyThreshold");
            expect(snapshot.shots[2].crouching == false, "黄金状态 crouch_and_exit crouching");
            expect(snapshot.shots[2].held == false, "黄金状态 crouch_and_exit fireHeld");
            expect(snapshot.shots[2].crouch_grace == false, "黄金状态 crouch_and_exit crouchGraceActive");
            expect(snapshot.shots[2].counter_strafe == false, "黄金状态 crouch_and_exit counterStrafeActive");
            expect(snapshot.shots[2].delayed == true, "黄金状态 crouch_and_exit fireSampleDelayed");
            expect(snapshot.shots[2].axis_conflict == false, "黄金状态 crouch_and_exit axisConflict");
            expect(snapshot.shots[2].stable == false, "黄金状态 crouch_and_exit isStable");
        }
        expect(snapshot.assessments.size() == 0, "黄金换向数量 crouch_and_exit");
    }
    { Engine e;
        e.tick(0); event(e, 0, Key::Left, true);
        e.tick(200); event(e, 200, Key::Right, true);
        e.tick(215); event(e, 215, Key::Left, false);
        e.tick(270); event(e, 270, Key::Fire, true);
        e.tick(275); event(e, 275, Key::Fire, false);
        e.tick(400); event(e, 400, Key::Left, true);
        e.tick(415); event(e, 415, Key::Right, false);
        e.tick(470); event(e, 470, Key::Fire, true);
        e.tick(475); event(e, 475, Key::Fire, false);
        e.tick(700);
        const auto snapshot = e.snapshot();
        expect(snapshot.shots.size() == 2, "黄金样本数量 adad_taps");
        if (snapshot.shots.size() > 0) {
            near(snapshot.shots[0].error, 0, "黄金字段 adad_taps error");
            near(snapshot.shots[0].timing_diff_ms, 40.9, "黄金字段 adad_taps timingDiffMs");
            near(snapshot.shots[0].speed_ratio, 0.268, "黄金字段 adad_taps speedRatio");
            near(snapshot.shots[0].sequence_index, 1, "黄金字段 adad_taps shotSequenceIndex");
            near(snapshot.shots[0].stop_success_age_ms, 40.9, "黄金字段 adad_taps stopSuccessAgeMs");
            near(snapshot.shots[0].estimated_speed, 0.091, "黄金字段 adad_taps estimatedSpeed");
            near(snapshot.shots[0].movement_keys_down, 1, "黄金字段 adad_taps movementKeysDown");
            near(snapshot.shots[0].accuracy_threshold, 0.34, "黄金字段 adad_taps accuracyThreshold");
            expect(snapshot.shots[0].crouching == false, "黄金状态 adad_taps crouching");
            expect(snapshot.shots[0].held == false, "黄金状态 adad_taps fireHeld");
            expect(snapshot.shots[0].crouch_grace == false, "黄金状态 adad_taps crouchGraceActive");
            expect(snapshot.shots[0].counter_strafe == false, "黄金状态 adad_taps counterStrafeActive");
            expect(snapshot.shots[0].delayed == true, "黄金状态 adad_taps fireSampleDelayed");
            expect(snapshot.shots[0].axis_conflict == false, "黄金状态 adad_taps axisConflict");
            expect(snapshot.shots[0].stable == true, "黄金状态 adad_taps isStable");
        }
        if (snapshot.shots.size() > 1) {
            near(snapshot.shots[1].error, 0, "黄金字段 adad_taps error");
            near(snapshot.shots[1].timing_diff_ms, 61.8, "黄金字段 adad_taps timingDiffMs");
            near(snapshot.shots[1].speed_ratio, 0.606, "黄金字段 adad_taps speedRatio");
            near(snapshot.shots[1].sequence_index, 1, "黄金字段 adad_taps shotSequenceIndex");
            near(snapshot.shots[1].stop_success_age_ms, 61.8, "黄金字段 adad_taps stopSuccessAgeMs");
            near(snapshot.shots[1].estimated_speed, 0.206, "黄金字段 adad_taps estimatedSpeed");
            near(snapshot.shots[1].movement_keys_down, 1, "黄金字段 adad_taps movementKeysDown");
            near(snapshot.shots[1].accuracy_threshold, 0.34, "黄金字段 adad_taps accuracyThreshold");
            expect(snapshot.shots[1].crouching == false, "黄金状态 adad_taps crouching");
            expect(snapshot.shots[1].held == false, "黄金状态 adad_taps fireHeld");
            expect(snapshot.shots[1].crouch_grace == false, "黄金状态 adad_taps crouchGraceActive");
            expect(snapshot.shots[1].counter_strafe == false, "黄金状态 adad_taps counterStrafeActive");
            expect(snapshot.shots[1].delayed == true, "黄金状态 adad_taps fireSampleDelayed");
            expect(snapshot.shots[1].axis_conflict == false, "黄金状态 adad_taps axisConflict");
            expect(snapshot.shots[1].stable == true, "黄金状态 adad_taps isStable");
        }
        expect(snapshot.assessments.size() == 2, "黄金换向数量 adad_taps");
        if(snapshot.assessments.size() > 0) near(snapshot.assessments[0].diff_ms, -15, "黄金换向时间差");
        if(snapshot.assessments.size() > 1) near(snapshot.assessments[1].diff_ms, -15, "黄金换向时间差");
    }
    if (failures == 0) std::cout << "参考评估核心向量全部通过\n";
    return failures ? 1 : 0;
}
