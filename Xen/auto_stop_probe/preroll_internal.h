#ifndef AUTO_STOP_COUNTERPULSE_PREROLL_INTERNAL_H
#define AUTO_STOP_COUNTERPULSE_PREROLL_INTERNAL_H
#include <cstdint>

namespace auto_stop_probe_detail {
enum class PrerollState { WAIT, READY, FAILED };
struct PrerollDecision { PrerollState state; const char* reason; };

class CounterpulsePrerollGate {
public:
    explicit CounterpulsePrerollGate(std::int64_t started_ns) : started_(started_ns), previous_(started_ns) {}
    // 只证明采集观察窗口，不证明人物静止。首帧等待和首帧后的观察分别有界。
    PrerollDecision evaluate(std::int64_t now_ns, std::int64_t first_frame_ns,
        std::int64_t latest_frame_ns, int frame_count, bool capture_failed, bool cancelled) {
        if (cancelled) return fail("USER_CANCELLED");
        if (failure_) return {PrerollState::FAILED, failure_};
        if (capture_failed) return fail("CAPTURE_FAILED");
        if (started_ < 0 || now_ns < previous_) return fail("CAPTURE_CLOCK_REGRESSION");
        previous_ = now_ns;
        if (frame_count < 0 || first_frame_ns < 0 || latest_frame_ns < 0 ||
            first_frame_ns > now_ns || latest_frame_ns > now_ns ||
            (first_frame_ns && (first_frame_ns < started_ || latest_frame_ns < first_frame_ns)) ||
            (!first_frame_ns && (latest_frame_ns || frame_count)) ||
            (first_frame_ns && !frame_count)) return fail("CAPTURE_TIMING_INVALID");
        if (first_ && (first_frame_ns != first_ || latest_frame_ns < latest_ || frame_count < count_))
            return fail("CAPTURE_EVIDENCE_REGRESSION");
        if (!first_frame_ns) {
            if (now_ns - started_ >= first_timeout_ns) return fail("CAPTURE_FIRST_FRAME_TIMEOUT");
            return {PrerollState::WAIT, "WAIT_FIRST_FRAME"};
        }
        if (first_frame_ns - started_ > first_timeout_ns) return fail("CAPTURE_FIRST_FRAME_TIMEOUT");
        first_ = first_frame_ns; latest_ = latest_frame_ns; count_ = frame_count;
        if (now_ns - latest_frame_ns > maximum_age_ns) return fail("CAPTURE_STALE");
        if (now_ns - first_frame_ns < observation_ns) return {PrerollState::WAIT, "CAPTURE_OBSERVING"};
        if (frame_count < 3) return fail("CAPTURE_INSUFFICIENT_FRAMES");
        return {PrerollState::READY, "CAPTURE_READY"};
    }
    static constexpr std::int64_t first_timeout_ns = 5000000000LL;
    static constexpr std::int64_t observation_ns = 300000000;
    static constexpr std::int64_t maximum_age_ns = 100000000;
private:
    PrerollDecision fail(const char* reason) { failure_ = reason; return {PrerollState::FAILED, reason}; }
    std::int64_t started_, previous_, first_ = 0, latest_ = 0;
    int count_ = 0;
    const char* failure_ = nullptr;
};
}
#endif
