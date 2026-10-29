// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef XEN_RECORDER_MARKER_INTERNAL_H
#define XEN_RECORDER_MARKER_INTERNAL_H
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

namespace xen::recorder::detail {
inline constexpr std::size_t kMarkerIdBytes = 97, kRenderQueueCapacity = 512;
using MarkerId = std::array<char,kMarkerIdBytes>;
inline bool valid_marker_id(std::string_view id) noexcept {
    if (id.empty() || id.size() >= kMarkerIdBytes) return false;
    for (const auto ch : id)
        if (!((ch>='a' && ch<='z') || (ch>='A' && ch<='Z') || (ch>='0' && ch<='9') ||
              ch=='-' || ch=='_' || ch=='.')) return false;
    return true;
}
struct MarkerFrame {
    bool draw = false, record = false, first = false, timed_out = false;
};
enum class FrameTimingStatus : std::uint8_t {
    VALID, NO_COMPLETED_FRAME, INVALID_CURRENT, DUPLICATE_CURRENT, ROLLBACK,
    INVALID_COMPLETION, COMPLETION_GAP
};
struct FrameTimingCandidate {
    std::uint64_t encoder_cts=0;
    FrameTimingStatus status=FrameTimingStatus::NO_COMPLETED_FRAME;
};
// 记录真实主合成回调，绝不按帧率减一帧。不同帧间隔照实映射，缺口明确未知。
class MainRenderClock {
public:
    void reset() noexcept { completed_=0; last_content_=0; completed_valid_=false; }
    void completed(std::uint64_t cts) noexcept {
        if (!cts) { completed_=0; completed_valid_=false; return; }
        const bool ordered=!completed_ || cts>completed_;
        completed_=cts; completed_valid_=ordered;
    }
    FrameTimingCandidate candidate(std::uint64_t content,bool gpu) noexcept {
        if (!content) { reset(); return {0,FrameTimingStatus::INVALID_CURRENT}; }
        if (last_content_ && content<last_content_) {
            reset(); last_content_=content; return {0,FrameTimingStatus::ROLLBACK};
        }
        if (last_content_==content) return {0,FrameTimingStatus::DUPLICATE_CURRENT};
        const bool missed=last_content_ && completed_<last_content_;
        last_content_=content;
        if (!gpu) return {content,FrameTimingStatus::VALID};
        if (!completed_) return {0,FrameTimingStatus::NO_COMPLETED_FRAME};
        if (!completed_valid_ || completed_>=content) return {0,FrameTimingStatus::INVALID_COMPLETION};
        if (missed) return {0,FrameTimingStatus::COMPLETION_GAP};
        return {completed_,FrameTimingStatus::VALID};
    }
private:
    std::uint64_t completed_=0,last_content_=0;
    bool completed_valid_=false;
};
// 时间由调用者注入；可见时长从第一次真实渲染开始，不从热键请求开始推测。
class MarkerLifecycle {
public:
    bool request(std::string_view id, std::int64_t now, std::uint32_t duration_ms) noexcept {
        if (waiting_ || visible_ || !valid_marker_id(id) || id==std::string_view(id_.data()) ||
            now<=0 || duration_ms<1 || duration_ms>10000) return false;
        id_.fill(0); std::memcpy(id_.data(),id.data(),id.size());
        requested_=now; duration_=static_cast<std::int64_t>(duration_ms)*1000000LL;
        started_=0; last_frame_=0; waiting_=true;
        return true;
    }
    MarkerFrame frame(std::int64_t now, std::uint64_t frame_time) noexcept {
        MarkerFrame result;
        if (waiting_) {
            if (now<requested_ || now-requested_>=2000000000LL) {
                waiting_=false; result.timed_out=true; return result;
            }
            if (!frame_time) return result;
            waiting_=false; visible_=true; started_=now; result.first=true;
        }
        if (!visible_) return result;
        if (now<started_ || now-started_>=duration_) { visible_=false; return result; }
        result.draw=true;
        if (frame_time && frame_time>last_frame_) { result.record=true; last_frame_=frame_time; }
        return result;
    }
    bool busy() const noexcept { return waiting_ || visible_; }
    const MarkerId& id() const noexcept { return id_; }
private:
    MarkerId id_{};
    bool waiting_=false, visible_=false;
    std::int64_t requested_=0, started_=0, duration_=0;
    std::uint64_t last_frame_=0;
};
struct RawRenderedMarker {
    MarkerId id{};
    std::int64_t begin_steady=0, end_steady=0, begin_system=0, end_system=0;
    std::uint64_t frame_time=0, encoder_frame_time=0;
    FrameTimingStatus timing_status=FrameTimingStatus::NO_COMPLETED_FRAME;
    bool first=false, timed_out=false;
};
class RenderQueue {
public:
    void push(const RawRenderedMarker& event) noexcept {
        if (size_==events_.size()) {
            begin_=(begin_+1)%events_.size(); --size_;
            if (dropped_!=std::numeric_limits<std::uint64_t>::max()) ++dropped_;
        }
        events_[(begin_+size_)%events_.size()]=event; ++size_;
    }
    bool pop(RawRenderedMarker& event) noexcept {
        if (!size_) return false;
        event=events_[begin_]; begin_=(begin_+1)%events_.size(); --size_; return true;
    }
    std::size_t size() const noexcept { return size_; }
    std::uint64_t dropped() const noexcept { return dropped_; }
private:
    std::array<RawRenderedMarker,kRenderQueueCapacity> events_{};
    std::size_t begin_=0,size_=0;
    std::uint64_t dropped_=0;
};
}
#endif
