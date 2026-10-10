#ifndef INPUT_TRAINING_INTERNAL_H
#define INPUT_TRAINING_INTERNAL_H

#include "input_training/input_training.h"

#include <atomic>

namespace input_training::detail {
// 钩子仅由生命周期owner在start/load前设置；只控制窄边界，
// 不替代生产解析、线程构造、文件发布或评估。finished仅供观测收尾。
class SessionTestAccess {
public:
    static void before_replay_event(Session& session,
        std::function<void(const Event&, const std::atomic_bool&)> operation);
    static void before_worker_start(Session& session, std::function<void()> operation);
    static void before_manifest_commit(Session& session, std::function<void()> operation);
    static void after_manifest_commit_admitted(Session& session, std::function<void()> operation);
    static bool finished(const Session& session);
};
}

#endif
