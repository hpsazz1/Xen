#ifndef MOVEMENT_INTERNAL_H
#define MOVEMENT_INTERNAL_H
#include <cstdint>
namespace movement::detail {
// 处理时刻已过保护期仍须拒绝期内采集的旧事件，边界本身允许新事件。
inline bool trigger_guard_finished(std::int64_t received_ns, std::int64_t now_ns,
                                   std::int64_t deadline_ns) noexcept {
    return received_ns >= deadline_ns && now_ns >= deadline_ns;
}
}
#endif
