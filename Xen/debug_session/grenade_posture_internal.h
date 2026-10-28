#ifndef DEBUG_SESSION_GRENADE_POSTURE_INTERNAL_H
#define DEBUG_SESSION_GRENADE_POSTURE_INTERNAL_H

#include "mouse/mouse.h"
#include "source_context/source_context.h"
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

namespace debug_session::detail {
class GrenadePosture final {
public:
    using Source = std::function<source_context::SourceContextSnapshot()>;
    // 回调仅保有独立租约及许可，不借裸Session生命周期；归还回调在Ctrl释放之后运行。
    GrenadePosture(std::shared_ptr<IMouseController> device, Source source,
                   std::function<bool()> permitted, std::function<void(bool)> finish)
        : device_(std::move(device)), source_(std::move(source)),
          permitted_(std::move(permitted)), finish_(std::move(finish)) {}
    ~GrenadePosture() { finish(); }
    GrenadePosture(const GrenadePosture&) = delete;
    GrenadePosture& operator=(const GrenadePosture&) = delete;
    bool start() noexcept {
        try {
            std::lock_guard lock(mutex_);
            if (finished_) return false;
            if (started_) return valid_locked();
            if (attempted_) return false;
            source_context::SourceContextSnapshot source;
            if (!context_locked(source)) return false;
            source_session_ = source.session_id;
            source_epoch_ = source.control_safety_epoch;
            // 未确认的按下也可能已经生效，不能等ACK后才承担释放责任。
            attempted_ = true; cleanup_known_ = false;
            started_ = device_->set_left_ctrl_key(true).disposition == KeyboardDisposition::ACKNOWLEDGED;
            return started_ && valid_locked();
        } catch (...) { return false; }
    }
    bool valid() const noexcept {
        try {
            std::lock_guard lock(mutex_);
            return !finished_ && started_ && valid_locked();
        } catch (...) { return false; }
    }
    bool finish() noexcept {
        std::function<void(bool)> release;
        bool known = false;
        try {
            {
                std::lock_guard lock(mutex_);
                if (finished_) return cleanup_known_;
                finished_ = true; started_ = false;
                // 清理不再检查许可/焦点；只释放自己尝试按下的软件Ctrl，不动W/SPACE。
                cleanup_known_ = !attempted_ || (device_ &&
                    device_->set_left_ctrl_key(false).disposition == KeyboardDisposition::ACKNOWLEDGED);
                known = cleanup_known_;
                release = std::move(finish_);
            }
            // Runtime归还可能触发其他生命周期锁，不能携带姿态锁回调。
            if (release) release(known);
        } catch (...) { return false; }
        return known;
    }
    bool cleanup_known() const noexcept {
        try { std::lock_guard lock(mutex_); return cleanup_known_; }
        catch (...) { return false; }
    }
private:
    bool context_locked(source_context::SourceContextSnapshot& source) const {
        if (!device_ || !source_ || !permitted_ || !permitted_() ||
            !device_->supports_left_ctrl_key() || !device_->output_owner_exclusive() ||
            device_->status() != MouseStatus::READY) return false;
        InputSnapshot input;
        if (!device_->poll_input(input) || !input.state_valid) return false;
        source = source_();
        return source.available && source.focused && source.session_id != 0;
    }
    bool valid_locked() const {
        source_context::SourceContextSnapshot source;
        return context_locked(source) && source.session_id == source_session_ &&
            source.control_safety_epoch == source_epoch_;
    }
    std::shared_ptr<IMouseController> device_;
    Source source_;
    std::function<bool()> permitted_;
    std::function<void(bool)> finish_;
    mutable std::mutex mutex_;
    std::uint64_t source_session_ = 0, source_epoch_ = 0;
    bool attempted_ = false, started_ = false, finished_ = false, cleanup_known_ = true;
};
}
#endif
