#ifndef MOVEMENT_RECORDING_H
#define MOVEMENT_RECORDING_H
#include "movement/timeline.h"
#include <nlohmann/json.hpp>

namespace movement::recording {
// 只派生接收域候选；原始轨迹另存，不把报告解释选择当作设备标定。
struct Result {
    timeline::Plan plan;
    bool complete = false;
    std::string error;
};
nlohmann::json trace_json(const std::vector<MovementTraceEvent>&, std::int64_t begin_ns,
    std::int64_t end_ns, bool gap, int motion_mode);
Result derive(const std::vector<MovementTraceEvent>&, std::int64_t begin_ns,
    std::int64_t end_ns, bool gap, int motion_mode);
}
#endif
