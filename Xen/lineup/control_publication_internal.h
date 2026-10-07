#ifndef XEN_LINEUP_CONTROL_PUBLICATION_INTERNAL_H
#define XEN_LINEUP_CONTROL_PUBLICATION_INTERNAL_H
#include "lineup/control_ipc.h"

namespace lineup::detail {
// 准备首帧是尚无观察，不能冒充取消；空闲取消只发一次，实际心跳由 IPC Client 承担。
class ControlPublication {
    bool published_ = false;
    control::Mode mode_ = control::Mode::CANCEL;
    std::uint64_t sequence_ = 0, generation_ = 0;
public:
    void reset() noexcept { published_ = false; sequence_ = generation_ = 0; }
    bool wants(const control::Request &request, bool locating) const noexcept {
        if (request.mode == control::Mode::CANCEL)
            return !locating && (!published_ || mode_ != control::Mode::CANCEL);
        return request.mode == control::Mode::LOCATE || !published_ ||
            request.observation.sequence != sequence_ ||
            request.observation.identity.selection_generation != generation_;
    }
    void accepted(const control::Request &request) noexcept {
        published_ = true; mode_ = request.mode;
        sequence_ = request.observation.sequence;
        generation_ = request.observation.identity.selection_generation;
    }
};
}
#endif
