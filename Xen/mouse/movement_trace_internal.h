#ifndef MOUSE_MOVEMENT_TRACE_INTERNAL_H
#define MOUSE_MOVEMENT_TRACE_INTERNAL_H

#include "mouse/mouse.h"
#include <algorithm>
#include <mutex>

namespace mouse::detail {
inline std::int64_t movement_trace_now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 生产者仅复制固定事件；内存只在开始分配，消费者分批复制后自行写盘。
template<std::size_t Capacity = 8192>
class MovementTraceBuffer {
public:
    static_assert(Capacity > 0);
    bool begin() noexcept {
        std::lock_guard lock(mutex_);
        if (active_) return false;
        try { if (!storage_) storage_ = std::make_unique<Storage>(); }
        catch (...) { return false; }
        head_ = count_ = 0;
        gap_ = false;
        active_ = true;
        return true;
    }
    bool active() const noexcept {
        std::lock_guard lock(mutex_);
        return active_;
    }
    void publish(const MovementTraceEvent& event) noexcept {
        std::lock_guard lock(mutex_);
        if (!active_) return;
        if (count_ == Capacity) { gap_ = true; return; }
        (*storage_)[(head_ + count_) % Capacity] = event;
        ++count_;
    }
    void end(bool interrupted = false) noexcept {
        std::lock_guard lock(mutex_);
        if (active_ && interrupted) gap_ = true;
        active_ = false;
    }
    bool read(MovementTraceBatch& batch) noexcept {
        batch.events.clear();
        batch.gap = false;
        try { batch.events.resize(std::min<std::size_t>(Capacity, 256)); }
        catch (...) { batch.gap = true; return false; }
        std::lock_guard lock(mutex_);
        batch.gap = gap_;
        const auto size = std::min(count_, batch.events.size());
        for (std::size_t i = 0; i < size; ++i)
            batch.events[i] = (*storage_)[(head_ + i) % Capacity];
        head_ = (head_ + size) % Capacity;
        count_ -= size;
        batch.events.resize(size);
        return storage_ != nullptr;
    }
private:
    using Storage = std::array<MovementTraceEvent, Capacity>;
    mutable std::mutex mutex_;
    std::unique_ptr<Storage> storage_;
    std::size_t head_ = 0, count_ = 0;
    bool active_ = false, gap_ = false;
};
}
#endif
