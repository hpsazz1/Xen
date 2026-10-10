#ifndef INPUT_TRAINING_INTERNAL_H
#define INPUT_TRAINING_INTERNAL_H

#include "input_training/input_training.h"

#include <atomic>

namespace input_training::detail {
// 仅在 load 前由生命周期 owner 设置；确定性控制事件交付边界，不替代解析与评估。
class SessionTestAccess {
public:
    static void before_replay_event(Session& session,
        std::function<void(const Event&, const std::atomic_bool&)> operation);
};
}

#endif
