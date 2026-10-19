#ifndef MOVEMENT_TIMELINE_H
#define MOVEMENT_TIMELINE_H

#include "movement/movement.h"
#include <nlohmann/json_fwd.hpp>
#include <vector>

// 调试编辑器和后续地图入口共用的数据与同步执行契约；调用方负责设备独占和线程。
namespace movement::timeline {
enum class ActionKind { KEY, TURN, MASK, MOVE, WHEEL };
enum class Key { W, A, S, D, SPACE, CTRL };
struct Action {
    ActionKind kind = ActionKind::KEY;
    Key key = Key::W;
    int start_ms = 0;
    int duration_ms = 100;
    double angle_degrees = 25.0; // 负数左转，正数右转。
    bool physical = false; // 录制物理按键轨道，先扣除同键屏蔽区间再合并软件轨道。
    int dx = 0, dy = 0, wheel = 0;
};
struct Mask {
    bool enabled = true;
    int wasd = 15;
    int start_ms = -1; // -1：首次脚本空格；没有空格则不屏蔽。
    int duration_ms = 0; // 0：持续到动作结束。
};
struct Plan {
    std::string name = "新身法方案", map, location, notes, voice_command;
    std::vector<Action> actions;
    Mask mask;
};
bool validate_plan(const Plan&, std::string& error) noexcept;
bool parse_plan(const nlohmann::json&, Plan&, std::string& error) noexcept;
nlohmann::json plan_json(const Plan&);
int duration_ms(const Plan&) noexcept;
struct Progress { int elapsed_ms = 0, total_ms = 0, sent_dx = 0; };
struct Callbacks {
    std::function<bool()> permitted;
    std::function<bool()> canceled;
    std::function<void(const Progress&)> progress;
};
enum class Outcome { COMPLETED, CANCELED, FAILED };
struct Result {
    Outcome outcome = Outcome::FAILED;
    bool cleanup_known = true;
    int sent_dx = 0;
    double elapsed_ms = 0; // 包含设备确认和最终清理的实际墙钟耗时。
    std::string error;
};
// 不创建线程、不订阅物理输入；每次发送前重查许可，任何键盘尝试之后都承担清理责任。
Result run_plan(IMouseController&, const Plan&, const movement::Config&, const Callbacks&) noexcept;
}
#endif
