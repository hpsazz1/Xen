#ifndef XEN_REFERENCE_ASSESSMENT_H
#define XEN_REFERENCE_ASSESSMENT_H

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace xen::reference_assessment {
// cs-match-hud e632605f8b6c20ac5ab8ac3284e4fdc33735d431 的 Basic 模型移植。
// 只接受离线事件；所有速度、稳定标签均为模型值，绝不产生控制许可或设备输出。
using TimeMs = double;
enum class Key { Forward, Back, Left, Right, Crouch, Fire, Shift, Space };
struct Event { TimeMs time_ms{}; Key key{}; bool down{}; };
struct Assessment {
    TimeMs time_ms{};
    Key from{}, to{};
    double diff_ms{};
    std::string timing;
    bool perfect{}, success{};
};
struct Shot {
    TimeMs time_ms{};
    double error{}, estimated_speed{}, accuracy_threshold{}, speed_ratio{};
    double stop_success_age_ms{}, timing_diff_ms{};
    std::string reason;
    unsigned sequence_index{}, movement_keys_down{};
    bool stable{}, crouching{}, crouch_grace{}, axis_conflict{}, counter_strafe{};
    bool held{}, delayed{};
};
struct Output {
    bool accepted{true};
    std::string error;
    std::vector<Assessment> assessments;
    std::vector<Shot> shots;
};
struct Snapshot {
    double velocity_x{}, velocity_y{};
    bool fire_pressed{}, shift_pressed{}, space_pressed{};
    std::optional<TimeMs> next_sample_ms;
    std::vector<Assessment> assessments;
    std::vector<Shot> shots;
};
class Engine {
public:
    // 外部时间为相对毫秒，必须有限、非负、单调；非法调用返回 accepted=false 且不改变状态。
    Output handle_event(Event event);
    Output tick(TimeMs time_ms);
    // gap 或上下文失效必须由调用方显式 reset；不把未知间隔当连续可信观测。
    bool reset(TimeMs time_ms);
    Snapshot snapshot() const;
private:
    struct AxisWait { bool waiting{}; Key released{}; TimeMs time_ms{}; };
    bool validate_time(TimeMs time_ms, Output& output);
    void assess(Event event, Output& output);
    void advance(TimeMs time_ms);
    std::optional<TimeMs> next_due() const;
    void fire_down(TimeMs time_ms);
    std::optional<Shot> process(TimeMs time_ms);
    Shot emit(TimeMs time_ms, bool held);
    std::array<bool, 4> movement_pressed_{};
    std::array<std::optional<TimeMs>, 4> press_times_{};
    std::array<TimeMs, 4> assessment_press_times_{};
    std::array<AxisWait, 2> waiting_{};
    std::vector<Assessment> assessments_;
    std::vector<Shot> shots_;
    TimeMs last_assessment_ms_{}, last_external_ms_{-1}, movement_time_ms_{-1};
    double velocity_x_{}, velocity_y_{};
    std::optional<double> stop_time_ms_;
    std::optional<TimeMs> crouch_release_ms_, last_movement_ms_;
    bool crouching_{}, fire_pressed_{}, shift_pressed_{}, space_pressed_{};
    bool axis_conflict_{}, counter_strafe_{}, scheduler_active_{}, first_emitted_{};
    TimeMs fire_down_ms_{}, first_due_ms_{}, last_sample_ms_{};
    unsigned sequence_index_{};
};
}
#endif
